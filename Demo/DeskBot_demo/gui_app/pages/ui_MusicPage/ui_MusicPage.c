/*
 * 音乐播放页（独立一页，不依赖视频）
 *
 * 和视频页的区别（也是这一页存在的理由）：
 *   - **不需要屏幕也能用**：插上耳机/音箱就能听，屏坏了照样听歌。
 *   - 不用解码视频、不用刷 SPI 屏，单核 A7 上功耗和负载都低得多。
 *   - 解码比播放快几十倍，所以这里必须**限速**（等音频缓冲消费掉再解下一段），
 *     不像视频页那样能靠「帧率」自然控速。
 *
 * 曲目来源：优先 SD 卡（S99 挂到 /mnt/media），没有就用固件自带的目录。
 * 路径：
 *   目录 → 扫里面所有音频文件（最多 64 个，按名字排序）
 *   环境变量 ECHO_MUSIC 可以指定文件或目录；设了它开机还会直接进这一页。
 *
 * 歌词：音频旁边放一个同主名的 .lrc（song.mp3 → song.lrc，也认 song.mp3.lrc），
 *       播放时按 [mm:ss.xx] 时间标签同步显示：左侧 5 行歌词（上两句/当前句/下两句，
 *       当前句蓝色高亮），右侧一个半遮出屏的蓝色唱片，底部图标控制条 + 进度条。
 *       编码 UTF-8（可带 BOM）优先，不是 UTF-8 就按 GBK/GB18030 用 iconv 转一次。
 *
 * 音量：点底部音量图标弹出竖条（0~100，值存在 system_para.conf 的 sound），
 *       松手时才调 sys_set_volume() —— 板端每次调用都要开关一次 ALSA 混音器。
 *
 * 播放列表：点底部列表图标弹出一个全屏浮层（不切页面，所以不会打断播放）：
 *       顶栏是返回箭头 + "共 N 首歌曲"，下面是可滚动的歌曲行（小封面 / 曲名 / 歌手，
 *       正在播放那一行绿色标题 + 深色底 + 播放徽标）。点某一行就切到那首。
 *       歌手名用 ffmpeg 读容器标签；没有标签显示"未知歌手"。
 *       小封面的解码放在 60ms 定时器里一首一首做，进列表不会卡。
 *
 * 无屏自检：ECHO_MUSIC_SELFTEST=1（2 秒一步：下一曲 / 拖进度 / 循环 / 暂停 / 继续 /
 *           上一曲 / 打开列表 / 点第 3 行 / 打开音量 / 把音量设为 80）
 * 调试钩子：ECHO_MUSIC_LIST=1 开机直接打开列表；ECHO_MUSIC_VOL=1 直接弹音量条；
 *           ECHO_MUSIC_TRACK=N 从第 N 首开始播
 * PCM 落盘：ECHO_VIDEO_AUDWAV=/tmp/x.wav（复用音频输出模块的钩子，用来验证真的出声了）
 */

#include "ui_MusicPage.h"
#include "media/ui_video_decode.h"
#include "media/ui_audio_out.h"

/* 播放列表小封面要自己解码成 RGB565（LV_CACHE_DEF_SIZE=0，用文件路径会每帧重解码），
 * lv_image_decoder_dsc_t 在本版 LVGL 里是私有结构体，必须包这个头（和相册页一样） */
#include "../../lvgl/src/draw/lv_image_decoder_private.h"

#if defined(HAVE_FFMPEG)
#include <libavformat/avformat.h>   /* 读 ID3/容器里的歌手名 */
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/stat.h>

#if defined(HAVE_ICONV)
#include <iconv.h>          /* 歌词可能是 GBK 编码，用它转 UTF-8 */
#endif

///////////////////// 参数 ////////////////////

#define MUSIC_MAX_FILES    64
#define MUSIC_NAME_LEN     160

/* 起播前先垫多少毫秒静音：避免刚开头就欠载（咔哒声） */
#define MUSIC_PREBUF_MS    300
/* 缓冲里超过这么多毫秒的音频就先别解了 —— 解码比播放快得多，
 * 不限速的话环形缓冲会被填满，ui_audio_out 就会开始丢最旧的数据（声音就断了）*/
#define MUSIC_HIGH_MS      1200

/* 设备不接受文件自带格式时的降级目标（板子上验证过的组合） */
#define MUSIC_FB_RATE      16000
#define MUSIC_FB_CH        1

/* 歌词：最多多少行 / 单行最长字节数 / .lrc 文件最多读多少字节 */
#define LRC_MAX_LINES      200
#define LRC_TEXT_LEN       96
#define LRC_FILE_MAX       (64 * 1024)

/* 没设 ECHO_MUSIC 时按这个顺序找（第一个有曲子的胜出） */
static const char * const music_search_paths[] = {
    "/mnt/media/music",         /* SD 卡上的音乐目录（推荐） */
    "/mnt/media",               /* SD 卡根目录 */
    "/root/music",              /* 固件 overlay 里的音乐目录 */
    "/root/bin/music",
};

///////////////////// 状态 ////////////////////

static pthread_t        player_tid;
static volatile int     player_run   = 0;
static volatile int     player_error = 0;
static volatile int     paused       = 0;

static char playlist[MUSIC_MAX_FILES][256];
static char playlist_name[MUSIC_MAX_FILES][MUSIC_NAME_LEN];
static int  playlist_count = 0;
static int  playlist_index = 0;

static volatile int    req_switch  = -1;
static volatile double req_seek    = -1;
static volatile int    loop_enable = 1;    /* 1 = 播完自动下一首（列表循环）；0 = 播完停 */
static volatile double cur_pos     = 0;
static volatile double cur_dur     = 0;
static volatile int    user_seeking = 0;
static volatile int    out_rate = 0, out_ch = 0;

/* UI 控件 */
static lv_obj_t *title_label;
static lv_obj_t *status_label;
static lv_obj_t *pause_label;
static lv_obj_t *loop_label;
static lv_obj_t *slider;
static lv_obj_t *time_label;
static lv_timer_t *music_timer;
static lv_obj_t *back_btn, *loop_btn, *pause_btn, *prev_btn, *next_btn;
static lv_obj_t *menu_btn, *vol_btn;         /* 播放列表按钮 / 音量按钮 */
static lv_obj_t *music_icon;                 /* 音符图标：没有封面时当唱片中心的占位 */
static lv_obj_t *lrc_prev2_label, *lrc_prev1_label, *lrc_cur_label;
static lv_obj_t *lrc_next_label, *lrc_next2_label;   /* 下两句 */
static lv_obj_t *time_dur_label;             /* 总时长（右下角） */

/* 封面：唱片圆盘的圆心部分（等比缩放到盖满盘心） */
static lv_obj_t *cover_box, *cover_img;
static lv_obj_t *disc_hole;                  /* 深色盘心，也是封面的圆形容器 */
static lv_obj_t *bg_img;                     /* 同一张图铺满整页当背景（默认关） */
static int       cover_idx = -2;             /* 当前封面属于哪一首（-2 = 还没加载） */

/* 背景图不透明度：0 = 完全透明，255 = 完全不透明。
 * 数值越小 → 透明度越高。想要更淡就改这里。 */
#define MUSIC_BG_OPA   LV_OPA_30

/* ---------- 音量弹出条 ---------- */
static lv_obj_t *vol_popup, *vol_slider, *vol_value_label;

/* ---------- 播放列表子界面（叠在本页上的全屏浮层，切歌不会中断播放）---------- */
#define LIST_ROW_H     48
#define LIST_THUMB_PX  36                 /* 小封面边长 */

static lv_obj_t *list_overlay, *list_box;
static lv_obj_t *list_rows[MUSIC_MAX_FILES];
static lv_obj_t *list_title_labels[MUSIC_MAX_FILES];
static lv_obj_t *list_artist_labels[MUSIC_MAX_FILES];
static lv_obj_t *list_covers[MUSIC_MAX_FILES];         /* 真实封面（解码好之后才显示） */
static lv_obj_t *list_badges[MUSIC_MAX_FILES];         /* 正在播放的圆形播放徽标 */
static uint16_t *list_thumbs[MUSIC_MAX_FILES];         /* LIST_THUMB_PX^2 RGB565 */
static lv_image_dsc_t list_thumb_dsc[MUSIC_MAX_FILES];
static char list_artist[MUSIC_MAX_FILES][64];
static char list_title[MUSIC_MAX_FILES][128];           /* ID3 标题（有就用它当歌名） */
static unsigned char meta_loaded[MUSIC_MAX_FILES];      /* 1 = 已经读过标签 */
static lv_timer_t *list_job_timer;
static int list_built = 0;                             /* 行是否已经建好 */
static int list_job = 0;                               /* 下一个要补歌手/封面的行 */

/* 唱片旋转：80ms 一帧（12.5fps），每帧 4.8°，共 75 帧 = 6 秒一圈。
 * 不用无限 lv_anim 的原因是它跟着显示器刷新率跑（板子是 fbtft SPI 屏，
 * 每帧都要重绘 170x170 区域 ≈ 57KB），单核 A7 上会白白吃掉一半 CPU。*/
#define DISC_SPIN_STEP_MS   80
#define DISC_SPIN_STEP_DEG  48                 /* 单位 0.1 度 */
static lv_timer_t *disc_timer = NULL;
static int32_t disc_angle    = 0;              /* 当前角度（0.1 度） */

/* 定义在后面，这里先声明（music_refresh_cb 会用到） */
static void cover_set_for_track(int idx);
static void list_update_current(void);
static void music_display_name(int idx, char *out, size_t n);
static void disc_spin_start(void);
static void disc_spin_stop(void);

/* 页面代次：每进一次页面 +1，用来让刷新回调重置"上次显示过什么"的静态变量 */
static volatile int page_gen = 0;

///////////////////// 歌词（LRC） ////////////////////
/*
 * 音频旁边放一个同主名的 .lrc 就能显示歌词（song.mp3 → song.lrc，也认 song.mp3.lrc）。
 * 按 [mm:ss.xx] 时间标签跟播放位置同步，页面显示 4 行：上上句/上一句/当前句/下一句。
 * 编码：UTF-8（可带 BOM）优先，不是 UTF-8 就按 GBK/GB18030 转一次（需要 iconv）。
 * 换曲时由 UI 线程顺手读文件（很小，几毫秒），所以这里不需要额外线程和锁。
 */

typedef struct {
    double t;                       /* 这一行从第几秒开始 */
    char   text[LRC_TEXT_LEN];
} lrc_line_t;

static lrc_line_t lrc_lines[LRC_MAX_LINES];
static int lrc_count        = 0;
static int lrc_offset_ms    = 0;    /* .lrc 里的 [offset:±ms] */
static int lrc_loaded_index = -1;   /* 已经给哪一首找过歌词了（-1 = 还没找） */

/* 去掉首尾空白（空格/\t/\r）：UTF-8 汉字的字节都 >= 0x80，不会被误伤 */
static void lrc_trim(char *s)
{
    size_t n = strlen(s);
    while (n > 0 && (unsigned char)s[n - 1] <= ' ') s[--n] = 0;
    char *p = s;
    while (*p && (unsigned char)*p <= ' ') p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
}

static int lrc_utf8_valid(const unsigned char *s, size_t n)
{
    size_t i = 0;
    while (i < n) {
        unsigned char c = s[i];
        int len;
        if (c < 0x80)              { i++;     continue; }
        else if ((c & 0xE0) == 0xC0) len = 2;
        else if ((c & 0xF0) == 0xE0) len = 3;
        else if ((c & 0xF8) == 0xF0) len = 4;
        else return 0;
        if (i + (size_t)len > n) return 0;
        for (int k = 1; k < len; k++)
            if ((s[i + k] & 0xC0) != 0x80) return 0;
        i += (size_t)len;
    }
    return 1;
}

#if defined(HAVE_ICONV)
/* 不是 UTF-8 → 按 GBK 系编码转一遍；都不行就原样留着（顶多显示乱码，不会崩） */
static void lrc_recode(char *buf, size_t *len, size_t cap)
{
    static const char *charsets[] = { "GBK", "GB18030", "GB2312" };

    for (unsigned i = 0; i < sizeof(charsets) / sizeof(charsets[0]); i++) {
        iconv_t cd = iconv_open("UTF-8", charsets[i]);
        if (cd == (iconv_t)-1) continue;

        char *out = malloc(cap);
        if (!out) { iconv_close(cd); return; }

        char  *inp     = buf;
        size_t inleft  = *len;
        char  *outp    = out;
        size_t outleft = cap - 1;
        size_t r = iconv(cd, &inp, &inleft, &outp, &outleft);
        iconv_close(cd);

        if (r != (size_t)-1 && inleft == 0) {          /* 整段都转成功才算 */
            size_t outlen = (size_t)(outp - out);
            memcpy(buf, out, outlen);
            buf[outlen] = 0;
            *len = outlen;
            free(out);
            printf("[MusicPage] 文本按 %s 转成 UTF-8\n", charsets[i]);
            return;
        }
        free(out);
    }
    printf("[MusicPage] 文本不是 UTF-8 且转码失败 → 可能显示乱码\n");
}
#endif

/* 把一段可能是 GBK 的文本就地转成 UTF-8：
 * SD 卡上的中文文件名（内核没开 NLS_UTF8）和没标编码的 ID3 都是这种。
 * 不是合法 UTF-8 才转，已经是 UTF-8 就原样不动。*/
static void text_fix_utf8(char *s, size_t cap)
{
    size_t n = strlen(s);
    if (n == 0 || lrc_utf8_valid((const unsigned char *)s, n)) return;
#if defined(HAVE_ICONV)
    lrc_recode(s, &n, cap);
#else
    (void)cap;
#endif
}

/* 中文 mp3 的 ID3 常把 GBK 字节标成 ISO-8859-1，ffmpeg 会当成 Latin-1 转成 UTF-8，
 * 结果是 "ÇçÌì" 这种乱码（全是 U+0080~U+00FF 字符）。
 * 这里认出这种"伪 Latin-1"后还原成原始字节、再按 GBK 解一次；
 * 只有解出来真的带汉字才采用，否则原样保留（免得把 Björk 这种真西文搞坏）。*/
static void text_fix_mojibake(char *s, size_t cap)
{
    size_t n = strlen(s);
    char raw[256];
    size_t rl = 0;
    int has_non_ascii = 0;

    for (size_t i = 0; i < n; ) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x80) {
            if (rl < sizeof(raw)) raw[rl++] = (char)c;
            i++;
            continue;
        }
        int len;
        unsigned cp;
        if ((c & 0xE0) == 0xC0)      { len = 2; cp = c & 0x1F; }
        else if ((c & 0xF0) == 0xE0) { len = 3; cp = c & 0x0F; }
        else if ((c & 0xF8) == 0xF0) { len = 4; cp = c & 0x07; }
        else return;                              /* 不是合法 UTF-8，别管 */
        if (i + (size_t)len > n) return;
        for (int k = 1; k < len; k++) {
            if (((unsigned char)s[i + k] & 0xC0) != 0x80) return;
            cp = (cp << 6) | ((unsigned char)s[i + k] & 0x3F);
        }
        if (cp > 0xFF) return;                    /* 已经是真中文/其它 → 不用还原 */
        if (rl < sizeof(raw)) raw[rl++] = (char)(cp & 0xFF);
        has_non_ascii = 1;
        i += (size_t)len;
    }
    if (!has_non_ascii || rl == 0) return;

#if defined(HAVE_ICONV)
    static const char *charsets[] = { "GBK", "GB18030" };
    for (unsigned cs = 0; cs < sizeof(charsets) / sizeof(charsets[0]); cs++) {
        iconv_t cd = iconv_open("UTF-8", charsets[cs]);
        if (cd == (iconv_t)-1) continue;

        char *out = malloc(cap);
        if (!out) { iconv_close(cd); return; }

        char  *ip = raw;
        size_t il = rl;
        char  *op = out;
        size_t ol = cap - 1;
        size_t r  = iconv(cd, &ip, &il, &op, &ol);
        iconv_close(cd);

        if (r != (size_t)-1 && il == 0 && op > out) {
            *op = 0;
            /* 解出来必须真的有汉字才算数 */
            int has_cjk = 0;
            for (const unsigned char *p = (const unsigned char *)out; *p; ) {
                unsigned c2 = 0;
                int l2 = 0;
                if ((*p & 0xE0) == 0xC0)      { c2 = *p & 0x1F; l2 = 2; }
                else if ((*p & 0xF0) == 0xE0) { c2 = *p & 0x0F; l2 = 3; }
                else if ((*p & 0xF8) == 0xF0) { c2 = *p & 0x07; l2 = 4; }
                else { p++; continue; }
                for (int k = 1; k < l2; k++) c2 = (c2 << 6) | (p[k] & 0x3F);
                if (c2 >= 0x4E00 && c2 <= 0x9FFF) { has_cjk = 1; break; }
                p += l2;
            }
            if (has_cjk) {
                snprintf(s, cap, "%s", out);
                free(out);
                printf("[MusicPage] 标签按 %s 还原成 UTF-8\n", charsets[cs]);
                return;
            }
        }
        free(out);
    }
#else
    (void)cap;
#endif
}

/* 统一入口：先把不合法 UTF-8 当 GBK 转码，再看是不是"伪 Latin-1"的 GBK */
static void text_normalize(char *s, size_t cap)
{
    size_t n = strlen(s);
    if (n == 0) return;

    if (!lrc_utf8_valid((const unsigned char *)s, n)) {
        text_fix_utf8(s, cap);
        return;
    }
    text_fix_mojibake(s, cap);
}

static int lrc_add(double t, const char *text)
{
    if (lrc_count >= LRC_MAX_LINES) return 0;
    lrc_lines[lrc_count].t = t;
    snprintf(lrc_lines[lrc_count].text, LRC_TEXT_LEN, "%s", text);
    lrc_count++;
    return 1;
}

static int lrc_line_cmp(const void *a, const void *b)
{
    double x = ((const lrc_line_t *)a)->t, y = ((const lrc_line_t *)b)->t;
    return (x < y) ? -1 : (x > y) ? 1 : 0;
}

/* 解析 [ ] 里的时间标签：支持 [mm:ss] [mm:ss.xx] [mm:ss:cc]（cc = 百分秒）。
 * LRC 里没有 hh:mm:ss，所以 3 段就按 mm:ss:cc 算。不是时间标签返回 0。 */
static int lrc_parse_time(const char *tag, double *out)
{
    char buf[24];
    size_t n = strlen(tag);
    if (n == 0 || n >= sizeof(buf)) return 0;
    for (size_t i = 0; i < n; i++) {
        char c = tag[i];
        if (!isdigit((unsigned char)c) && c != ':' && c != '.' && c != ',') return 0;
        buf[i] = (c == ',') ? '.' : c;
    }
    buf[n] = 0;

    double v[3] = { 0, 0, 0 };
    int np = 0;
    char *save = NULL;
    for (char *tok = strtok_r(buf, ":", &save); tok && np < 3; tok = strtok_r(NULL, ":", &save))
        v[np++] = atof(tok);

    if (np == 2)      *out = v[0] * 60.0 + v[1];
    else if (np == 3) *out = v[0] * 60.0 + v[1] + v[2] / 100.0;
    else return 0;
    return 1;
}

/* 解析已经转成 UTF-8 的歌词文本（NUL 结尾），返回行数 */
static int lrc_parse(const char *data)
{
    lrc_count     = 0;
    lrc_offset_ms = 0;

    const char *p = data;
    while (*p && lrc_count < LRC_MAX_LINES) {
        const char *eol = strchr(p, '\n');
        size_t llen = eol ? (size_t)(eol - p) : strlen(p);
        char line[512];
        if (llen >= sizeof(line)) llen = sizeof(line) - 1;
        memcpy(line, p, llen);
        line[llen] = 0;

        char *q = line;
        while (*q == ' ' || *q == '\t') q++;

        double times[8];
        int ntimes = 0;
        while (*q == '[') {                     /* 一行可以有多个时间标签 */
            char *close = strchr(q, ']');
            if (!close) break;
            *close = 0;
            double t;
            if (ntimes < 8 && lrc_parse_time(q + 1, &t)) {
                times[ntimes++] = t;
            } else if (!strncasecmp(q + 1, "offset:", 7)) {
                lrc_offset_ms = atoi(q + 8);    /* [offset:±ms] 元数据 */
            }
            q = close + 1;
        }

        lrc_trim(q);
        if (*q) {                               /* 空行（间奏）直接跳过，保持上一句不闪 */
            for (int i = 0; i < ntimes; i++) lrc_add(times[i], q);
        }

        if (!eol) break;
        p = eol + 1;
    }

    if (lrc_offset_ms) {
        for (int i = 0; i < lrc_count; i++) lrc_lines[i].t -= lrc_offset_ms / 1000.0;
    }
    qsort(lrc_lines, lrc_count, sizeof(lrc_lines[0]), lrc_line_cmp);
    return lrc_count;
}

static int lrc_load_file(const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) return 0;

    char *buf = malloc(LRC_FILE_MAX);
    if (!buf) { fclose(fp); return 0; }

    size_t n = fread(buf, 1, LRC_FILE_MAX - 1, fp);
    fclose(fp);
    buf[n] = 0;

    if (n >= 3 && (unsigned char)buf[0] == 0xEF &&
                  (unsigned char)buf[1] == 0xBB && (unsigned char)buf[2] == 0xBF) {
        memmove(buf, buf + 3, n - 3 + 1);       /* 去掉 UTF-8 BOM，否则第一次比较会带上它 */
        n -= 3;
    }
    if (!lrc_utf8_valid((const unsigned char *)buf, n)) {
#if defined(HAVE_ICONV)
        lrc_recode(buf, &n, LRC_FILE_MAX);
#else
        printf("[MusicPage] %s 不是 UTF-8（没编进 iconv，请把歌词存成 UTF-8）\n", path);
#endif
    }

    int cnt = lrc_parse(buf);
    free(buf);
    return cnt;
}

/* 找同目录同主名的 .lrc 并加载；返回行数（0 = 这首歌没有歌词） */
static int lrc_load_for_track(int idx)
{
    if (idx < 0 || idx >= playlist_count) return 0;

    char base[300];
    snprintf(base, sizeof(base), "%s", playlist[idx]);
    char *dot = strrchr(base, '.');
    if (dot) *dot = 0;                          /* 去掉扩展名 */

    char path[320];
    /* 大小写都要试：FAT 卡不区分大小写，但 ubifs/ext4 区分 */
    const char *exts[] = { ".lrc", ".LRC", NULL };
    for (int i = 0; exts[i]; i++) {
        snprintf(path, sizeof(path), "%s%s", base, exts[i]);
        if (access(path, R_OK) != 0) continue;
        int n = lrc_load_file(path);
        if (n > 0) { printf("[MusicPage] 歌词 %d 行: %s\n", n, path); return n; }
    }
    snprintf(path, sizeof(path), "%s.lrc", playlist[idx]);   /* song.mp3.lrc 这种写法 */
    if (access(path, R_OK) == 0) {
        int n = lrc_load_file(path);
        if (n > 0) { printf("[MusicPage] 歌词 %d 行: %s\n", n, path); return n; }
    }

    printf("[MusicPage] 没有歌词文件（试过 %s.lrc / %s.LRC / %s.lrc）\n", base, base, playlist[idx]);
    return 0;
}

/* 二分：找最后一个 "时间 <= pos" 的行号；还没到第一行返回 -1 */
static int lrc_index_at(double pos)
{
    int lo = 0, hi = lrc_count - 1, r = -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (lrc_lines[mid].t <= pos) { r = mid; lo = mid + 1; }
        else hi = mid - 1;
    }
    return r;
}

static const char *lrc_text_at(int i)
{
    if (i < 0 || i >= lrc_count) return "";
    return lrc_lines[i].text;
}

/* 只在内容变化时才写标签：省 SPI 刷屏带宽（和进度条/时间一样的做法） */
static void lrc_set_label(lv_obj_t *l, const char *t)
{
    if (!l) return;
    const char *cur = lv_label_get_text(l);
    if (cur && strcmp(cur, t) == 0) return;
    lv_label_set_text(l, t);
}

/* 显示当前句（蓝色高亮）和前后各两句；还没到第一句时上面两行留空 */
static void lrc_refresh_view(int ci)
{
    if (lrc_count <= 0) return;
    lrc_set_label(lrc_prev2_label, lrc_text_at(ci - 2));
    lrc_set_label(lrc_prev1_label, lrc_text_at(ci - 1));
    lrc_set_label(lrc_cur_label,   lrc_text_at(ci));
    lrc_set_label(lrc_next_label,  lrc_text_at(ci + 1));
    lrc_set_label(lrc_next2_label, lrc_text_at(ci + 2));
}

/* 5 行歌词整块显示/隐藏；没有 .lrc 时只在当前句位置显示"无歌词" */
static void lrc_update_visibility(void)
{
    int has = (lrc_count > 0);

    lv_obj_t *rows[5] = { lrc_prev2_label, lrc_prev1_label, lrc_cur_label,
                          lrc_next_label, lrc_next2_label };
    for (int i = 0; i < 5; i++) {
        if (!rows[i]) continue;
        if (has) lv_obj_remove_flag(rows[i], LV_OBJ_FLAG_HIDDEN);
        else     lv_obj_add_flag(rows[i], LV_OBJ_FLAG_HIDDEN);
    }

    if (!lrc_cur_label) return;

    if (!has) {
        lv_obj_remove_flag(lrc_cur_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_text_color(lrc_cur_label, lv_color_hex(0x9AA0A6), 0);
        lv_label_set_text(lrc_cur_label, "无歌词");
    } else {
        lv_obj_set_style_text_color(lrc_cur_label, lv_color_hex(0x1F6FE5), 0);
    }
}

/* 歌词行：左对齐，宽度固定（超长自动截断），位置写死在这块歌词区里 */
#define LRC_X        14
#define LRC_W        208

static lv_obj_t *lrc_label_create(lv_obj_t *parent, int y, uint32_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, "");
    lv_obj_set_style_text_font(l, &ui_font_app16, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_set_width(l, LRC_W);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);      /* 本版本用 LONG_DOT，太长的截断 */
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_align(l, LV_ALIGN_TOP_LEFT, LRC_X, y);
    lv_obj_add_flag(l, LV_OBJ_FLAG_HIDDEN);            /* 找到歌词之前不显示 */
    return l;
}

///////////////////// 工具 ////////////////////

static int is_audio_name(const char *n)
{
    static const char *exts[] = { ".mp3", ".flac", ".wav", ".m4a", ".aac",
                                  ".ogg", ".opus", ".mka", ".wma", NULL };
    size_t l = strlen(n);
    if (l < 5) return 0;
    const char *ext = n + l - 4;
    for (int i = 0; exts[i]; i++)
        if (!strcasecmp(ext, exts[i])) return 1;
    return 0;
}

static void base_name(char *dst, size_t n, const char *path)
{
    const char *s = strrchr(path, '/');
    s = s ? s + 1 : path;
    snprintf(dst, n, "%s", s);
}

static int name_cmp(const void *a, const void *b)
{
    return strcmp((const char *)a, (const char *)b);
}

static int scan_playlist(const char *src)
{
    playlist_count = 0;
    if (!src || !src[0]) return 0;

    DIR *dir = opendir(src);
    if (!dir) {                       /* 单个文件 */
        snprintf(playlist[0], sizeof(playlist[0]), "%s", src);
        base_name(playlist_name[0], MUSIC_NAME_LEN, src);
        playlist_count = 1;
        return 1;
    }

    static char tmp[MUSIC_MAX_FILES][256];
    int n = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL && n < MUSIC_MAX_FILES) {
        if (!is_audio_name(ent->d_name)) continue;
        snprintf(tmp[n], sizeof(tmp[0]), "%s%s%s", src,
                 (src[strlen(src) - 1] == '/') ? "" : "/", ent->d_name);
        n++;
    }
    closedir(dir);

    qsort(tmp, n, sizeof(tmp[0]), name_cmp);
    for (int i = 0; i < n; i++) {
        snprintf(playlist[i], sizeof(playlist[i]), "%s", tmp[i]);
        base_name(playlist_name[i], MUSIC_NAME_LEN, tmp[i]);
    }
    playlist_count = n;
    return n;
}

static int probe_music_path(const char *p)
{
    if (!p || access(p, F_OK) != 0) return 0;
    struct stat st;
    if (stat(p, &st) != 0) return 0;
    if (S_ISREG(st.st_mode)) return is_audio_name(p) ? 1 : 0;
    if (!S_ISDIR(st.st_mode)) return 0;
    return scan_playlist(p) > 0;
}

static int set_playlist(const char *path)
{
    if (!path || !path[0]) return -1;
    if (scan_playlist(path) <= 0) return -1;
    playlist_index = 0;
    return 0;
}

///////////////////// 播放线程 ////////////////////

/* 解码线程拿到 PCM 就丢进音频环形缓冲 */
static void on_pcm(void *user, const void *pcm, int bytes)
{
    (void)user;
    ui_audio_out_write(pcm, bytes);
}

static void *music_player_thread(void *arg)
{
    (void)arg;

    while (player_run) {
        if (playlist_count <= 0) break;
        if (playlist_index < 0 || playlist_index >= playlist_count) playlist_index = 0;

        ui_video_decoder_t *dec = ui_audio_open(playlist[playlist_index], 0, 0);
        if (!dec) { player_error = 1; break; }

        int rate = ui_video_audio_rate(dec);
        int ch   = ui_video_audio_channels(dec);
        if (rate <= 0) rate = 44100;
        if (ch   <= 0) ch   = 2;

        cur_dur = ui_video_duration(dec);
        cur_pos = 0;
        printf("[MusicPage] 播放 %d/%d %s  %dHz %dch  %.1fs\n",
               playlist_index + 1, playlist_count, playlist_name[playlist_index],
               rate, ch, cur_dur);

        ui_video_set_pcm_callback(dec, on_pcm, NULL);

        if (ui_audio_out_open(rate, ch, MUSIC_PREBUF_MS) != 0) {
            /* 设备不接受文件自带的采样率/声道数 → 降级到板子上验证过的 16k 单声道 */
            if (rate != MUSIC_FB_RATE || ch != MUSIC_FB_CH) {
                printf("[MusicPage] 音频降级到 %dHz %dch\n", MUSIC_FB_RATE, MUSIC_FB_CH);
                if (ui_video_set_audio_format(dec, MUSIC_FB_RATE, MUSIC_FB_CH) == 0) {
                    rate = MUSIC_FB_RATE;
                    ch   = MUSIC_FB_CH;
                    ui_audio_out_open(rate, ch, MUSIC_PREBUF_MS);
                }
            }
        }
        out_rate = rate;
        out_ch   = ch;
        printf("[MusicPage] 音频输出%s: %dHz %dch\n",
               ui_audio_out_is_open() ? "已开" : "不可用(静音)", rate, ch);

        int audio_paused = 0;
        int need_reopen  = 0;
        int high_bytes   = MUSIC_HIGH_MS * rate * ch * 2 / 1000;
        double stats_t0 = 0;

        while (player_run && !need_reopen) {
            /* 切上一曲/下一曲 */
            if (req_switch >= 0) {
                playlist_index = req_switch;
                req_switch = -1;
                need_reopen = 1;
                break;
            }
            /* 拖了进度条 */
            if (req_seek >= 0) {
                ui_video_seek(dec, req_seek);
                ui_audio_out_flush();
                req_seek = -1;
            }
            /* 暂停/恢复 */
            if (paused != audio_paused) {
                ui_audio_out_set_paused(paused);
                audio_paused = paused;
            }
            if (paused) { usleep(20000); continue; }

            /* 限速：解码比播放快几十倍，缓冲里够听了就先歇会儿。
             * 不限速的话环形缓冲被填满 → ui_audio_out 丢最旧的数据 → 听感上就是跳音。*/
            if (ui_audio_out_buffered_bytes() > high_bytes) {
                usleep(50000);
                continue;
            }

            int r = ui_audio_read(dec);
            if (r < 0) { player_error = 1; break; }
            if (r == 0) {                       /* 这首放完了 */
                if (loop_enable) {
                    req_switch = (playlist_index + 1) % playlist_count;   /* 自动下一首 */
                } else {
                    paused = 1;                 /* 循环关：放完这首就停 */
                    ui_video_rewind(dec);
                    ui_audio_out_flush();
                }
                continue;
            }

            cur_pos = ui_video_position(dec);

            struct timespec t1;
            clock_gettime(CLOCK_MONOTONIC, &t1);
            double now = t1.tv_sec + t1.tv_nsec / 1e9;
            if (stats_t0 == 0) stats_t0 = now;
            if (now - stats_t0 >= 5.0) {       /* 音乐不用每秒刷，5 秒一行够了 */
                printf("[MusicPage] pos=%.1fs/%.1fs  buf=%dms under=%d skip=%d\n",
                       cur_pos, cur_dur,
                       ui_audio_out_buffered_bytes() * 1000 / (rate * ch * 2),
                       ui_audio_out_underruns(), ui_audio_out_lockskips());
                stats_t0 = now;
            }
        }

        ui_audio_out_close();
        ui_video_close(dec);
    }

    if (player_error) player_run = 0;   /* 出错时清掉运行标志，刷新定时器靠它显示提示 */
    printf("[MusicPage] 播放线程退出\n");
    return NULL;
}

///////////////////// UI ////////////////////

static void music_refresh_cb(lv_timer_t *timer)
{
    LV_UNUSED(timer);

    if (player_error && player_run == 0) {
        if (status_label) lv_label_set_text(status_label, "打不开");
        lv_timer_delete(music_timer);
        music_timer = NULL;
        return;
    }

    char buf[192];
    /* 这一页可能被反复进出，静态变量要跟着页面代次重置，否则重新进来时标签不刷新 */
    static int last_paused = -1, last_slider = -1, last_sec = -1, last_idx = -2, last_lrc = -2;
    static int seen_gen = -1;
    if (seen_gen != page_gen) {
        seen_gen = page_gen;
        last_paused = -1; last_slider = -1; last_sec = -1; last_idx = -2; last_lrc = -2;
    }

    if (pause_label && paused != last_paused) {
        lv_label_set_text(pause_label, paused ? LV_SYMBOL_PLAY : LV_SYMBOL_PAUSE);
        last_paused = paused;
    }

    if (slider && cur_dur > 0.05 && !user_seeking) {
        int v = (int)(cur_pos / cur_dur * 1000.0);
        if (v < 0) v = 0;
        if (v > 1000) v = 1000;
        if (v != last_slider) { lv_slider_set_value(slider, v, LV_ANIM_OFF); last_slider = v; }
    }

    if (time_label) {
        int ps = (int)cur_pos, ds = (int)(cur_dur + 0.5);
        if (ps != last_sec) {
            snprintf(buf, sizeof(buf), "%d:%02d", ps / 60, ps % 60);
            lv_label_set_text(time_label, buf);
            if (time_dur_label) {
                snprintf(buf, sizeof(buf), "%d:%02d", ds / 60, ds % 60);
                lv_label_set_text(time_dur_label, buf);
            }
            last_sec = ps;
        }
    }

    /* 曲名 + 第几首（换曲了才更新） */
    if (title_label && playlist_index != last_idx) {
        if (playlist_count > 0) {
            /* 有 ID3 标题用标题，否则用文件名；非 UTF-8（SD 卡 GBK 文件名）也会转码 */
            char name[MUSIC_NAME_LEN];
            music_display_name(playlist_index, name, sizeof(name));
            lv_label_set_text(title_label, name);
            if (status_label) {
                snprintf(buf, sizeof(buf), "%d / %d", playlist_index + 1, playlist_count);
                lv_label_set_text(status_label, buf);
            }
        }
        list_update_current();          /* 列表浮层开着时保持高亮跟着走 */
        last_idx = playlist_index;
    }

    /* 封面：换曲了才换图（<歌名>.png，否则 default.png） */
    if (playlist_count > 0 && playlist_index != cover_idx) {
        cover_set_for_track(playlist_index);
    }

    /* 歌词：换曲了就重新找这一首的 .lrc，然后按播放位置推进当前句 */
    if (playlist_index != lrc_loaded_index) {
        lrc_loaded_index = playlist_index;
        lrc_count = (playlist_count > 0) ? lrc_load_for_track(playlist_index) : 0;
        lrc_update_visibility();
        last_lrc = -2;
    }
    if (lrc_count > 0) {
        int ci = lrc_index_at(cur_pos);
        if (ci != last_lrc) {
            lrc_refresh_view(ci);
            last_lrc = ci;
        }
    }

    /* 唱片旋转：播放时顺时针转；暂停 / 出错 / 列表浮层打开时停在当前角度 */
    {
        int list_open = (list_overlay && !lv_obj_has_flag(list_overlay, LV_OBJ_FLAG_HIDDEN));
        int playing   = (!paused && !player_error && playlist_count > 0 && !list_open);
        if (playing) disc_spin_start();
        else         disc_spin_stop();
    }
}

static void ui_event_back_btn(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;

    /* 看板模式（开机直接进音乐页）时这一页是根页面，没有"上一页" → 回桌面，
     * 否则按返回毫无反应，人就被卡在这一页里 */
    if (page_manager.cur_depth <= 1) {
        static char home_name[] = "HomePage";
        lv_lib_pm_OpenPage(&page_manager, NULL, home_name);
    } else {
        lv_lib_pm_OpenPrePage(&page_manager);
    }
}

static void ui_event_playpause_btn(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    paused = !paused;
}

static void ui_event_loop_btn(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    loop_enable = !loop_enable;
    if (loop_label) {
        /* 图标不变，只靠颜色区分开关（蓝色开 / 灰色关） */
        lv_obj_set_style_text_color(loop_label,
            lv_color_hex(loop_enable ? 0x2F7BEA : 0xB4BAC2), 0);
    }
    printf("[MusicPage] 循环 = %s\n", loop_enable ? "on(自动下一首)" : "off(放完停)");
}

static void ui_event_prev_btn(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED || playlist_count <= 0) return;
    int idx = playlist_index - 1;
    if (idx < 0) idx = playlist_count - 1;
    req_switch = idx;
    paused = 0;
}

static void ui_event_next_btn(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED || playlist_count <= 0) return;
    int idx = playlist_index + 1;
    if (idx >= playlist_count) idx = 0;
    req_switch = idx;
    paused = 0;
}

static void ui_event_slider(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED || code == LV_EVENT_PRESSING) {
        user_seeking = 1;
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_CLICKED) {
        if (cur_dur > 0.05) {
            double want = (double)lv_slider_get_value(slider) / 1000.0 * cur_dur;
            req_seek = want;
            printf("[MusicPage] seek -> %.1fs\n", want);
        }
        user_seeking = 0;
    }
}

/* 自检：依次演练各控件（无屏时看日志就能确认接线） */
static void selftest_cb(lv_timer_t *t)
{
    static int step = 0;
    switch (step++) {
    case 1:
        printf("[selftest] 点 下一曲\n");
        lv_obj_send_event(next_btn, LV_EVENT_CLICKED, NULL);
        break;
    case 3:
        printf("[selftest] 把进度条拖到 50%% 并松手\n");
        lv_obj_send_event(slider, LV_EVENT_PRESSED, NULL);
        lv_slider_set_value(slider, 500, LV_ANIM_OFF);
        lv_obj_send_event(slider, LV_EVENT_RELEASED, NULL);
        break;
    case 5:
        printf("[selftest] 点 循环\n");
        lv_obj_send_event(loop_btn, LV_EVENT_CLICKED, NULL);
        break;
    case 7:
        printf("[selftest] 点 暂停\n");
        lv_obj_send_event(pause_btn, LV_EVENT_CLICKED, NULL);
        break;
    case 9:
        printf("[selftest] 再点 暂停（继续）\n");
        lv_obj_send_event(pause_btn, LV_EVENT_CLICKED, NULL);
        break;
    case 11:
        printf("[selftest] 点 上一曲\n");
        lv_obj_send_event(prev_btn, LV_EVENT_CLICKED, NULL);
        break;
    case 13:
        printf("[selftest] 点 播放列表\n");
        lv_obj_send_event(menu_btn, LV_EVENT_CLICKED, NULL);
        break;
    case 15:
        printf("[selftest] 点列表第 3 行\n");
        if (playlist_count > 2 && list_rows[2])
            lv_obj_send_event(list_rows[2], LV_EVENT_CLICKED, NULL);
        break;
    case 17:
        printf("[selftest] 点 音量\n");
        lv_obj_send_event(vol_btn, LV_EVENT_CLICKED, NULL);
        break;
    case 19:
        printf("[selftest] 把音量设为 80\n");
        if (vol_slider) {
            lv_slider_set_value(vol_slider, 80, LV_ANIM_OFF);
            lv_obj_send_event(vol_slider, LV_EVENT_RELEASED, NULL);   /* 模拟松手 */
        }
        break;
    case 21:
        printf("[selftest] 结束\n");
        lv_timer_delete(t);
        break;
    }
}

///////////////////// 页面 ////////////////////

///////////////////// 封面 ////////////////////

/* 封面就是盘心那一块：118 = 深色盘心直径（见 init 里的 disc_hole），
 * 圆角给成直径的一半 + 容器开 clip_corner，方图四角才会被裁成圆 */
#define COVER_W   118
#define COVER_H   118

/* 唱片圆盘：右半边出屏，只露出左侧一段弧（参考常见音乐 App 的歌词页） */
#define DISC_D    170

/* ⚠️ 16 位色深的 PNG 会让本版 LVGL 的 lodepng 解码把 TLSF 堆写坏
 *   （日志 lv_tlsf.c:690 block_merge_prev Asserted，进程直接挂掉）。
 *   这里先读 IHDR 判断位深，> 8 位就当没有封面，宁可显示占位图标。 */
static int png_bit_depth_ok(const char *path)
{
    static const unsigned char sig[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    unsigned char h[33];
    FILE *fp = fopen(path, "rb");
    if (!fp) return 0;
    size_t n = fread(h, 1, sizeof(h), fp);
    fclose(fp);
    if (n < sizeof(h) || memcmp(h, sig, sizeof(sig)) != 0) return 0;
    /* 8..11 长度, 12..15 "IHDR", 16..19 宽, 20..23 高, 24 位深, 25 颜色类型 */
    return h[24] <= 8;
}

/* 找这一首的封面文件：<歌名>.png → <歌名>.PNG → <目录>/default.png → <目录>/default.PNG
 * 找到返回 1 并把路径写进 out（列表缩略图也用它），找不到返回 0 */
static int music_cover_path(int idx, char *out, size_t n)
{
    out[0] = 0;
    if (idx < 0 || idx >= playlist_count) return 0;

    static char base[300];
    snprintf(base, sizeof(base), "%s", playlist[idx]);
    char *dot = strrchr(base, '.');
    if (dot) *dot = 0;                       /* 去掉扩展名，得到 <目录>/<歌名> */

    int dirlen = 0;
    const char *slash = strrchr(base, '/');
    if (slash) dirlen = (int)(slash - base);

    static const char *exts[] = { ".png", ".PNG", NULL };
    for (int i = 0; exts[i]; i++) {
        snprintf(out, n, "%s%s", base, exts[i]);
        if (access(out, R_OK) == 0 && png_bit_depth_ok(out)) return 1;
    }
    if (dirlen > 0) {
        for (int i = 0; exts[i]; i++) {
            snprintf(out, n, "%.*s/default%s", dirlen, base, exts[i]);
            if (access(out, R_OK) == 0 && png_bit_depth_ok(out)) return 1;
        }
    }
    out[0] = 0;
    return 0;
}

/* 每首歌的封面：<歌名>.png（与音乐同目录），找不到就用同目录的 default.png。
 * 两个都没有时把占位图标显示出来。
 * 只在换曲时调用（music_refresh_cb 里发现 playlist_index 变了）。 */
static void cover_set_for_track(int idx)
{
    if (!cover_img) return;

    if (idx < 0 || idx >= playlist_count) {
        lv_obj_add_flag(cover_img, LV_OBJ_FLAG_HIDDEN);
        if (disc_hole)   lv_obj_set_style_clip_corner(disc_hole, false, 0);
        if (bg_img)      lv_obj_add_flag(bg_img, LV_OBJ_FLAG_HIDDEN);
        if (music_icon)  lv_obj_remove_flag(music_icon, LV_OBJ_FLAG_HIDDEN);
        cover_idx = idx;
        return;
    }

    /* ECHO_MUSIC_BG=1 才把封面铺满当背景（新歌词页默认浅色，盖上去会发脏） */
    static int bg_enabled = -1;
    if (bg_enabled < 0) {
        const char *e = getenv("ECHO_MUSIC_BG");
        bg_enabled = (e && e[0] == '1');
    }

    static char found_path[320];
    if (!music_cover_path(idx, found_path, sizeof(found_path))) {
        lv_obj_add_flag(cover_img, LV_OBJ_FLAG_HIDDEN);
        if (disc_hole)   lv_obj_set_style_clip_corner(disc_hole, false, 0);
        if (bg_img)      lv_obj_add_flag(bg_img, LV_OBJ_FLAG_HIDDEN);
        if (music_icon) lv_obj_remove_flag(music_icon, LV_OBJ_FLAG_HIDDEN);
        printf("[MusicPage] 没有封面（%s 旁边没有 .png）\n", playlist[idx]);
        cover_idx = idx;
        return;
    }

    /* LVGL 只保存路径字符串指针，所以必须放静态缓冲；换图前先丢掉上一张的缓存 */
    static char lv_src[340];
    if (lv_src[0]) lv_image_cache_drop(lv_src);
    snprintf(lv_src, sizeof(lv_src), "A:%s", found_path);
    lv_image_set_src(cover_img, lv_src);

    /* 等比缩放到"盖满"盘心（取较大的那个比例，多出来的部分被盘心的圆形裁剪掉），
     * 这样方图、长方图都能把黑色圆填满，不会露出黑边。
     * ⚠️ 目标尺寸故意多给 2px：LVGL 的缩放是整数定点（scale 以 1/256 为单位）+
     *    离屏图层取整，正好 118 时会差 1px，转起来盘心边缘会露出黑色月牙 */
    lv_image_header_t hdr;
    if (lv_image_decoder_get_info(lv_src, &hdr) == LV_RESULT_OK && hdr.w > 0 && hdr.h > 0) {
        uint32_t zx = (uint32_t)((uint64_t)(COVER_W + 2) * LV_SCALE_NONE / hdr.w);
        uint32_t zy = (uint32_t)((uint64_t)(COVER_H + 2) * LV_SCALE_NONE / hdr.h);
        uint32_t z  = (zx > zy) ? zx : zy;
        if (z == 0) z = 1;
        lv_image_set_scale(cover_img, z);
    } else {
        lv_image_set_scale(cover_img, LV_SCALE_NONE);
    }
    lv_obj_center(cover_img);

    /* 同一张图铺满整页当背景（半透明，靠 MUSIC_BG_OPA 控制） */
    if (bg_img) {
        if (!bg_enabled) {
            lv_obj_add_flag(bg_img, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_image_set_src(bg_img, lv_src);
            if (hdr.w > 0 && hdr.h > 0) {
                uint32_t bx = (uint32_t)((uint64_t)UI_SCREEN_WIDTH  * LV_SCALE_NONE / hdr.w);
                uint32_t by = (uint32_t)((uint64_t)UI_SCREEN_HEIGHT * LV_SCALE_NONE / hdr.h);
                uint32_t bz = (bx > by) ? bx : by;      /* 取大的：铺满，多余部分被裁掉 */
                if (bz == 0) bz = 1;
                lv_image_set_scale(bg_img, bz);
            } else {
                lv_image_set_scale(bg_img, LV_SCALE_NONE);
            }
            lv_obj_center(bg_img);
            lv_obj_remove_flag(bg_img, LV_OBJ_FLAG_HIDDEN);
        }
    }

    lv_obj_remove_flag(cover_img, LV_OBJ_FLAG_HIDDEN);
    if (music_icon) lv_obj_add_flag(music_icon, LV_OBJ_FLAG_HIDDEN);
    if (disc_hole)  lv_obj_set_style_clip_corner(disc_hole, true, 0);   /* 方图四角裁成圆 */
    printf("[MusicPage] 封面: %s\n", found_path);
    cover_idx = idx;
}

///////////////////// 唱片旋转 ////////////////////
/*
 * 播放时唱片顺时针转，暂停/停止就停在当前角度（而不是"跳回 0°"）。
 * 做法是每 80ms 给 cover_box 加一点 transform_rotation：
 *   - 轴心设在圆盘中心（LV_PCT(50)），否则会绕着左上角甩。
 *   - 角度单位是 0.1 度，3600 = 一圈；用 % 3600 让它一直累加不跳变。
 *   - 圆盘上的蓝色渐变 + 内圈细线 + 中心封面/音符会一起转，转动才看得出来。
 *   - 用定时器（而非无限 lv_anim）是为了限帧：板子 SPI 屏每帧重绘这块要传 57KB，
 *     跟显示器刷新率跑会把单核 A7 吃满；80ms 一帧（12.5fps）观感够、压力小很多。
 */

static void disc_timer_cb(lv_timer_t *t)
{
    LV_UNUSED(t);
    if (!cover_box) return;
    disc_angle = (disc_angle + DISC_SPIN_STEP_DEG) % 3600;
    lv_obj_set_style_transform_rotation(cover_box, disc_angle, 0);
}

static void disc_spin_start(void)
{
    if (!cover_box || disc_timer) return;
    disc_timer = lv_timer_create(disc_timer_cb, DISC_SPIN_STEP_MS, NULL);
}

static void disc_spin_stop(void)
{
    if (disc_timer) {
        lv_timer_delete(disc_timer);
        disc_timer = NULL;
    }
    /* cover_box 保留当前角度，下次播放从原角度接着转 */
}

///////////////////// 音量弹出条 ////////////////////

static void vol_popup_refresh(void)
{
    if (!vol_slider) return;
    int v = (int)ui_system_para.sound;
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    lv_slider_set_value(vol_slider, v, LV_ANIM_OFF);
    if (vol_value_label) {
        char b[8];
        snprintf(b, sizeof(b), "%d", v);
        lv_label_set_text(vol_value_label, b);
    }
}

/* 拖动过程中只改参数和数字（lv_bar_set_value 不会发 VALUE_CHANGED，
 * 所以这里全靠自己同步）；真正下发到声卡放到松手时做，
 * 因为板端 sys_set_volume() 每次都要开关一次 ALSA 混音器，拖拽时调会卡。*/
static void vol_sync_value(void)
{
    if (!vol_slider) return;
    int v = (int)lv_slider_get_value(vol_slider);
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    ui_system_para.sound = (uint16_t)v;
    if (vol_value_label) {
        char b[8];
        snprintf(b, sizeof(b), "%d", v);
        lv_label_set_text(vol_value_label, b);
    }
}

/* 点到竖条哪个高度就直接跳那个音量（LVGL 的 slider 默认只认旋钮附近） */
static void vol_set_from_point(const lv_point_t *p)
{
    if (!vol_slider) return;
    lv_area_t a;
    lv_obj_get_coords(vol_slider, &a);
    int h = lv_area_get_height(&a);
    if (h <= 0) return;

    int v = (int)((long)(a.y2 - p->y) * 100 / h);
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    lv_slider_set_value(vol_slider, v, LV_ANIM_OFF);
}

static void vol_slider_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_PRESSED || code == LV_EVENT_PRESSING) {
        lv_indev_t *indev = lv_indev_active();
        if (indev) {
            lv_point_t p;
            lv_indev_get_point(indev, &p);
            vol_set_from_point(&p);
        }
        vol_sync_value();
    } else if (code == LV_EVENT_VALUE_CHANGED) {
        vol_sync_value();
    } else if (code == LV_EVENT_RELEASED) {
        vol_sync_value();
        sys_set_volume(ui_system_para.sound);   /* 板端走 ALSA 混音器；仿真里是空实现 */
        sys_save_system_parameters(sys_config_path, &ui_system_para);
        printf("[MusicPage] 音量 = %d\n", (int)ui_system_para.sound);
    }
}

/* 点音量按钮：在按钮上方弹出/收起竖条音量条 */
static void ui_event_volume_btn(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED || !vol_popup) return;
    if (lv_obj_has_flag(vol_popup, LV_OBJ_FLAG_HIDDEN)) {
        vol_popup_refresh();
        lv_obj_remove_flag(vol_popup, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(vol_popup);
    } else {
        lv_obj_add_flag(vol_popup, LV_OBJ_FLAG_HIDDEN);
    }
}

///////////////////// 播放列表子界面 ////////////////////

/* 读容器标签：标题 + 歌手（mp3 的 ID3 / flac 的 vorbis comment 都走 metadata）。
 * 每首只读一次，结果缓存到 list_title[] / list_artist[]。
 * 标签里的中文如果不是 UTF-8（有些 mp3 用 GBK 存却没标编码）也顺手转码。 */
static void music_load_meta(int idx)
{
    if (idx < 0 || idx >= playlist_count || idx >= MUSIC_MAX_FILES) return;
    if (meta_loaded[idx]) return;
    meta_loaded[idx] = 1;

    list_title[idx][0]  = 0;
    list_artist[idx][0] = 0;

#if defined(HAVE_FFMPEG)
    AVFormatContext *fmt = NULL;
    if (avformat_open_input(&fmt, playlist[idx], NULL, NULL) != 0) return;

    AVDictionaryEntry *tag = av_dict_get(fmt->metadata, "title", NULL, 0);
    if (tag && tag->value) snprintf(list_title[idx], sizeof(list_title[idx]), "%s", tag->value);

    tag = av_dict_get(fmt->metadata, "artist", NULL, 0);
    if (!tag) tag = av_dict_get(fmt->metadata, "album_artist", NULL, 0);
    if (tag && tag->value) snprintf(list_artist[idx], sizeof(list_artist[idx]), "%s", tag->value);

    avformat_close_input(&fmt);

    text_normalize(list_title[idx], sizeof(list_title[idx]));
    text_normalize(list_artist[idx], sizeof(list_artist[idx]));
#else
    (void)idx;
#endif
}

/* 文件名（去掉扩展名）；不是 UTF-8 就按 GBK 转一次。
 * 不读标签，所以建列表时用它（读标签交给 list_job_cb 逐行做） */
static void music_file_name(int idx, char *out, size_t n)
{
    if (n == 0) return;
    out[0] = 0;
    if (idx < 0 || idx >= playlist_count) return;

    snprintf(out, n, "%s", playlist_name[idx]);
    char *dot = strrchr(out, '.');
    if (dot && dot != out) *dot = 0;
    text_normalize(out, n);
}

/* 界面上显示的曲名：有 ID3 标题就用标题，否则用文件名。
 * 中文文件名在 SD 卡上常常是 GBK（内核没开 CONFIG_NLS_UTF8）→ 显示成方块/问号，
 * 所以这里统一过一遍 text_fix_utf8()。 */
static void music_display_name(int idx, char *out, size_t n)
{
    if (n == 0) return;
    out[0] = 0;
    if (idx < 0 || idx >= playlist_count) return;

    music_load_meta(idx);
    if (list_title[idx][0]) {
        snprintf(out, n, "%s", list_title[idx]);
        return;
    }
    music_file_name(idx, out, n);
}

/* 单像素 → RGB565。注意不同解码器给的通道顺序不一样：
 *   RGB888      内存里 R,G,B
 *   ARGB8888    LVGL 里内存序是 B,G,R,A
 *   RGB565      小端 16 位 */
static uint16_t music_px_565(const uint8_t *p, lv_color_format_t cf)
{
    switch (cf) {
    case LV_COLOR_FORMAT_RGB888:
        return (uint16_t)(((p[0] & 0xF8) << 8) | ((p[1] & 0xFC) << 3) | (p[2] >> 3));
    case LV_COLOR_FORMAT_ARGB8888:
        return (uint16_t)(((p[2] & 0xF8) << 8) | ((p[1] & 0xFC) << 3) | (p[0] >> 3));
    case LV_COLOR_FORMAT_RGB565: {
        uint16_t v;
        memcpy(&v, p, sizeof(v));
        return v;
    }
    default:
        return 0xE73D;                       /* 认不出的格式就填浅灰 */
    }
}

/* 把封面 PNG 解码成 LIST_THUMB_PX 见方的 RGB565 缩略图（一次，之后直接复用）。
 * LV_CACHE_DEF_SIZE = 0，直接用文件路径当图像源会每次重绘都重新解码，滚动会卡。 */
static int music_make_thumb(int idx)
{
    if (idx < 0 || idx >= playlist_count || list_thumbs[idx]) return 0;

    char path[320];
    if (!music_cover_path(idx, path, sizeof(path))) return 0;

    char lv_src[340];
    snprintf(lv_src, sizeof(lv_src), "A:%s", path);

    lv_image_decoder_dsc_t dsc;
    if (lv_image_decoder_open(&dsc, lv_src, NULL) != LV_RESULT_OK) {
        printf("[MusicPage] 封面解码失败：%s\n", path);
        return 0;
    }

    int iw = (int)dsc.header.w, ih = (int)dsc.header.h;
    const uint8_t *src = (dsc.decoded && dsc.decoded->data) ? dsc.decoded->data : NULL;
    lv_color_format_t cf = dsc.decoded ? dsc.decoded->header.cf : dsc.header.cf;
    int stride = dsc.decoded ? (int)dsc.decoded->header.stride : 0;

    int bpp = (cf == LV_COLOR_FORMAT_RGB888) ? 3 :
              (cf == LV_COLOR_FORMAT_RGB565) ? 2 : 4;

    if (!src || iw <= 0 || ih <= 0 || iw > 4096 || ih > 4096 || stride < iw * bpp) {
        printf("[MusicPage] 封面格式不支持：%s cf=%d %dx%d stride=%d\n",
               path, (int)cf, iw, ih, stride);
        lv_image_decoder_close(&dsc);
        return 0;
    }

    uint16_t *dst = (uint16_t *)malloc((size_t)LIST_THUMB_PX * LIST_THUMB_PX * 2);
    if (!dst) { lv_image_decoder_close(&dsc); return 0; }

    int tw = LIST_THUMB_PX, th = LIST_THUMB_PX;
    if ((long)iw * LIST_THUMB_PX > (long)ih * LIST_THUMB_PX) th = (int)((long)ih * LIST_THUMB_PX / iw);
    else                                                       tw = (int)((long)iw * LIST_THUMB_PX / ih);
    if (tw < 1) tw = 1;
    if (th < 1) th = 1;
    int ox = (LIST_THUMB_PX - tw) / 2, oy = (LIST_THUMB_PX - th) / 2;

    for (int i = 0; i < LIST_THUMB_PX * LIST_THUMB_PX; i++) dst[i] = 0xE73D;

    for (int y = 0; y < th; y++) {
        int sy = (int)((long)y * ih / th);
        const uint8_t *srow = src + (size_t)sy * stride;
        for (int x = 0; x < tw; x++) {
            int sx = (int)((long)x * iw / tw);
            dst[(size_t)(y + oy) * LIST_THUMB_PX + (x + ox)] =
                music_px_565(srow + (size_t)sx * bpp, cf);
        }
    }

    lv_image_decoder_close(&dsc);

    list_thumb_dsc[idx].header.w     = LIST_THUMB_PX;
    list_thumb_dsc[idx].header.h     = LIST_THUMB_PX;
    list_thumb_dsc[idx].header.cf    = LV_COLOR_FORMAT_RGB565;
    list_thumb_dsc[idx].header.magic = LV_IMAGE_HEADER_MAGIC;
    list_thumb_dsc[idx].data_size    = (uint32_t)LIST_THUMB_PX * LIST_THUMB_PX * 2;
    list_thumb_dsc[idx].data         = (const uint8_t *)dst;
    list_thumbs[idx] = dst;
    return 1;
}

/* 把「正在播放」那一行刷成绿色标题 + 深色底 + 播放徽标 */
static void list_update_current(void)
{
    for (int i = 0; i < playlist_count && i < MUSIC_MAX_FILES; i++) {
        if (!list_rows[i]) continue;
        int cur = (i == playlist_index);
        lv_obj_set_style_bg_color(list_rows[i], lv_color_hex(cur ? 0xE7EAEE : 0xFFFFFF), 0);
        if (list_title_labels[i]) {
            lv_obj_set_style_text_color(list_title_labels[i],
                lv_color_hex(cur ? 0x18A058 : 0x22262B), 0);
        }
        if (list_badges[i]) {
            if (cur) lv_obj_remove_flag(list_badges[i], LV_OBJ_FLAG_HIDDEN);
            else     lv_obj_add_flag(list_badges[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

/* 点某一行：切到这首并关掉列表 */
static void list_row_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;

    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx >= 0 && idx < playlist_count) {
        req_switch = idx;
        paused = 0;
        printf("[MusicPage] 列表选曲 -> %d/%d %s\n",
               idx + 1, playlist_count, playlist_name[idx]);
    }
    if (list_overlay) lv_obj_add_flag(list_overlay, LV_OBJ_FLAG_HIDDEN);
}

/* 后台补数据：一个 60ms 定时器一拍处理一首（读歌手 + 解一张小封面），
 * 不阻塞界面，列表进来时先显示「未知歌手」，随后逐行补上。 */
static void list_job_cb(lv_timer_t *t)
{
    (void)t;
    if (!list_overlay || lv_obj_has_flag(list_overlay, LV_OBJ_FLAG_HIDDEN)) return;

    if (list_job >= playlist_count || list_job >= MUSIC_MAX_FILES) {
        lv_timer_delete(list_job_timer);
        list_job_timer = NULL;
        printf("[MusicPage] 播放列表信息读取完成\n");
        return;
    }

    int i = list_job++;

    music_load_meta(i);            /* 已读过的直接返回 */

    if (list_artist_labels[i]) {
        lv_label_set_text(list_artist_labels[i],
                          list_artist[i][0] ? list_artist[i] : "未知歌手");
    }
    /* 有 ID3 标题就把行标题换成标题（建列表时先用文件名垫着） */
    if (list_title_labels[i] && list_title[i][0]) {
        lv_label_set_text(list_title_labels[i], list_title[i]);
    }

    if (list_covers[i] && !list_thumbs[i]) {
        if (music_make_thumb(i)) {
            lv_image_set_src(list_covers[i], &list_thumb_dsc[i]);
            lv_obj_remove_flag(list_covers[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

/* 建列表（只建一次）：顶栏 + 每首歌一行 */
static void list_back_cb(lv_event_t *e);

static void list_build(void)
{
    if (list_built || !list_box) return;
    list_built = 1;
    lv_obj_clean(list_box);

    char buf[64];
    char name_buf[MUSIC_NAME_LEN];
    char cover_path[320];
    lv_obj_t *title = lv_label_create(list_overlay);
    snprintf(buf, sizeof(buf), "共 %d 首歌曲", playlist_count);
    lv_label_set_text(title, buf);
    lv_obj_set_style_text_font(title, &ui_font_app16, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0x1F242B), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 34, 4);

    lv_obj_t *back = lv_btn_create(list_overlay);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_size(back, 30, 26);
    lv_obj_set_style_bg_opa(back, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(back, 0, 0);
    lv_obj_set_style_shadow_width(back, 0, 0);
    lv_obj_set_style_pad_all(back, 0, 0);
    lv_obj_t *al = lv_label_create(back);
    lv_label_set_text(al, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(al, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(al, lv_color_hex(0x3A3F46), 0);
    lv_obj_center(al);
    lv_obj_add_event_cb(back, list_back_cb, LV_EVENT_ALL, NULL);

    for (int i = 0; i < playlist_count && i < MUSIC_MAX_FILES; i++) {
        /* 行 */
        lv_obj_t *row = lv_obj_create(list_box);
        lv_obj_set_size(row, 300, LIST_ROW_H);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_radius(row, 8, 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, list_row_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        list_rows[i] = row;

        /* 封面框（占位底色） */
        lv_obj_t *cv = lv_obj_create(row);
        lv_obj_set_size(cv, 38, 38);
        lv_obj_set_pos(cv, 6, 5);
        lv_obj_set_style_radius(cv, 6, 0);
        lv_obj_set_style_bg_color(cv, lv_color_hex(0xDCE0E5), 0);
        lv_obj_set_style_border_width(cv, 0, 0);
        lv_obj_set_style_pad_all(cv, 0, 0);
        lv_obj_remove_flag(cv, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(cv, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t *ph = lv_image_create(cv);          /* 没封面时的音符占位 */
        lv_image_set_src(ph, &ui_img_music_data);
        lv_image_set_scale(ph, 128);                 /* 256 = 原始大小 */
        lv_obj_center(ph);

        if (music_cover_path(i, cover_path, sizeof(cover_path))) {
            lv_obj_t *im = lv_image_create(cv);      /* 解码好之后再显示 */
            lv_obj_center(im);
            lv_obj_add_flag(im, LV_OBJ_FLAG_HIDDEN);
            list_covers[i] = im;
        }

        /* 正在播放的圆形播放徽标（盖在封面上） */
        lv_obj_t *badge = lv_obj_create(row);
        lv_obj_set_size(badge, 20, 20);
        lv_obj_set_pos(badge, 15, 14);
        lv_obj_set_style_radius(badge, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(badge, lv_color_hex(0x101418), 0);
        lv_obj_set_style_bg_opa(badge, LV_OPA_70, 0);
        lv_obj_set_style_border_width(badge, 0, 0);
        lv_obj_set_style_pad_all(badge, 0, 0);
        lv_obj_remove_flag(badge, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(badge, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t *tri = lv_label_create(badge);
        lv_label_set_text(tri, LV_SYMBOL_PLAY);
        lv_obj_set_style_text_font(tri, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(tri, lv_color_hex(0xFFFFFF), 0);
        lv_obj_center(tri);
        lv_obj_add_flag(badge, LV_OBJ_FLAG_HIDDEN);
        list_badges[i] = badge;

        /* 曲名：元数据已读到的直接用（如正在播的那首），否则先用文件名
         * （不读标签，建表才快），ID3 标题由 list_job_cb 逐行补 */
        if (meta_loaded[i] && list_title[i][0]) {
            snprintf(name_buf, sizeof(name_buf), "%s", list_title[i]);
        } else {
            music_file_name(i, name_buf, sizeof(name_buf));
        }

        lv_obj_t *tt = lv_label_create(row);
        lv_label_set_text(tt, name_buf);
        lv_obj_set_style_text_font(tt, &ui_font_app16, 0);
        lv_obj_set_style_text_color(tt, lv_color_hex(0x22262B), 0);
        lv_obj_set_width(tt, 236);
        lv_label_set_long_mode(tt, LV_LABEL_LONG_DOT);
        lv_obj_set_pos(tt, 52, 4);
        list_title_labels[i] = tt;

        /* 歌手（先占位，由 list_job_cb 逐行补） */
        lv_obj_t *ar = lv_label_create(row);
        lv_label_set_text(ar, list_artist[i][0] ? list_artist[i] : "未知歌手");
        lv_obj_set_style_text_font(ar, &ui_font_app16, 0);
        lv_obj_set_style_text_color(ar, lv_color_hex(0x8A9099), 0);
        lv_obj_set_width(ar, 236);
        lv_label_set_long_mode(ar, LV_LABEL_LONG_DOT);
        lv_obj_set_pos(ar, 52, 25);
        list_artist_labels[i] = ar;
    }

    list_job = 0;
    if (!list_job_timer) list_job_timer = lv_timer_create(list_job_cb, 60, NULL);
}

/* 打开/关闭列表：打开时滚到正在播放的那首，音量弹窗顺便收起来 */
static void list_show(void)
{
    if (!list_overlay) return;
    if (!list_built) list_build();

    list_update_current();
    lv_obj_remove_flag(list_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(list_overlay);

    if (playlist_index >= 0 && playlist_index < MUSIC_MAX_FILES && list_rows[playlist_index]) {
        lv_obj_scroll_to_view(list_rows[playlist_index], LV_ANIM_OFF);
    }
    if (vol_popup) lv_obj_add_flag(vol_popup, LV_OBJ_FLAG_HIDDEN);
    printf("[MusicPage] 打开播放列表（%d 首）\n", playlist_count);

}

static void ui_event_menu_btn(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    list_show();
}

static void list_back_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (list_overlay) lv_obj_add_flag(list_overlay, LV_OBJ_FLAG_HIDDEN);
}

/* 无屏调试钩子：ECHO_MUSIC_LIST=1 开机直接打开列表；ECHO_MUSIC_VOL=1 直接弹音量条 */
static void list_auto_cb(lv_timer_t *t)
{
    lv_timer_delete(t);
    list_show();
}

static void vol_auto_cb(lv_timer_t *t)
{
    lv_timer_delete(t);
    if (!vol_popup) return;
    vol_popup_refresh();
    lv_obj_remove_flag(vol_popup, LV_OBJ_FLAG_HIDDEN);
}

void ui_MusicPage_init(void)
{
    lv_obj_t *page = lv_obj_create(NULL);

    /* 唱片故意超出屏幕右边（半遮效果）→ 屏幕会变成可滚动并画出滚动条，
     * 所以这里直接把屏幕的滚动、滚动条都关掉 */
    lv_obj_remove_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(page, LV_SCROLLBAR_MODE_OFF);

    page_gen++;
    player_error = 0;
    paused       = 0;
    req_switch   = -1;
    req_seek     = -1;
    cur_pos      = 0;
    cur_dur      = 0;
    user_seeking = 0;
    lrc_count        = 0;      /* 歌词等定时器发现当前曲目号后自己去加载 */
    lrc_loaded_index = -1;     /* -1 保证进页面后第一拍一定会去读一次 .lrc */
    cover_idx        = -2;     /* 同理：保证进页面后一定会给当前曲目加载封面 */

    /* 浅色渐变背景（上白下淡蓝），类似常见音乐 App 的歌词页 */
    lv_obj_set_style_bg_color(page, lv_color_hex(0xFBFDFF), 0);
    lv_obj_set_style_bg_grad_color(page, lv_color_hex(0xDCE8F6), 0);
    lv_obj_set_style_bg_grad_dir(page, LV_GRAD_DIR_VER, 0);

    /* ---------- 全屏背景（当前曲目封面，默认关；ECHO_MUSIC_BG=1 打开）---------- */
    bg_img = lv_image_create(page);
    lv_obj_set_size(bg_img, UI_SCREEN_WIDTH, UI_SCREEN_HEIGHT);
    lv_obj_center(bg_img);
    lv_obj_set_style_image_opa(bg_img, MUSIC_BG_OPA, 0);
    lv_obj_remove_flag(bg_img, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(bg_img, LV_OBJ_FLAG_HIDDEN);

    /* ---------- 顶部：返回箭头 / 歌名 / 第几首 ---------- */
    back_btn = lv_btn_create(page);
    lv_obj_align(back_btn, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_size(back_btn, 40, 26);
    lv_obj_set_style_bg_opa(back_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(back_btn, 0, 0);
    lv_obj_set_style_shadow_width(back_btn, 0, 0);
    lv_obj_set_style_pad_all(back_btn, 0, 0);
    lv_obj_t *l = lv_label_create(back_btn);
    lv_label_set_text(l, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(0x3A3F46), 0);
    lv_obj_center(l);
    lv_obj_add_event_cb(back_btn, ui_event_back_btn, LV_EVENT_ALL, NULL);

    title_label = lv_label_create(page);
    lv_label_set_text(title_label, "没有找到音乐");
    lv_obj_set_style_text_font(title_label, &ui_font_app16, 0);
    lv_obj_set_style_text_color(title_label, lv_color_hex(0x1F242B), 0);
    lv_obj_set_width(title_label, 180);
    lv_label_set_long_mode(title_label, LV_LABEL_LONG_DOT);
    lv_obj_align(title_label, LV_ALIGN_TOP_LEFT, 34, 4);

    status_label = lv_label_create(page);          /* 第几首 / 共几首（也用于报错） */
    lv_label_set_text(status_label, "- / -");
    lv_obj_set_style_text_font(status_label, &ui_font_app16, 0);
    lv_obj_set_style_text_color(status_label, lv_color_hex(0x8A9099), 0);
    lv_obj_align(status_label, LV_ALIGN_TOP_LEFT, 34, 24);

    /* ---------- 右侧：半遮的蓝色唱片（圆盘右侧出屏）---------- */
    cover_box = lv_obj_create(page);               /* 唱片本体 */
    lv_obj_set_size(cover_box, DISC_D, DISC_D);
    lv_obj_align(cover_box, LV_ALIGN_TOP_RIGHT, 77, -15);
    lv_obj_set_style_radius(cover_box, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(cover_box, lv_color_hex(0x9CC8F2), 0);
    lv_obj_set_style_bg_grad_color(cover_box, lv_color_hex(0x2E6FD0), 0);
    lv_obj_set_style_bg_grad_dir(cover_box, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_border_width(cover_box, 0, 0);
    lv_obj_set_style_pad_all(cover_box, 0, 0);
    lv_obj_remove_flag(cover_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(cover_box, LV_OBJ_FLAG_CLICKABLE);
    /* 旋转轴心放圆盘中心（默认是左上角，不改会绕着角甩） */
    lv_obj_set_style_transform_pivot_x(cover_box, LV_PCT(50), 0);
    lv_obj_set_style_transform_pivot_y(cover_box, LV_PCT(50), 0);

    lv_obj_t *disc_ring = lv_obj_create(cover_box);     /* 内圈细线（唱片槽） */
    lv_obj_set_size(disc_ring, DISC_D - 26, DISC_D - 26);
    lv_obj_set_style_radius(disc_ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(disc_ring, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(disc_ring, 2, 0);
    lv_obj_set_style_border_color(disc_ring, lv_color_hex(0xBFDCF7), 0);
    lv_obj_set_style_pad_all(disc_ring, 0, 0);
    lv_obj_remove_flag(disc_ring, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(disc_ring, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(disc_ring);

    disc_hole = lv_obj_create(cover_box);           /* 深色盘心（也当封面的圆形容器）*/
    lv_obj_set_size(disc_hole, COVER_W, COVER_H);
    lv_obj_set_style_radius(disc_hole, LV_RADIUS_CIRCLE, 0);
    /* 裁剪只在有封面时开（见 cover_set_for_track）：没封面时盘心不需要遮罩图层，
     * 转起来省一整个 layer 的 CPU */
    lv_obj_set_style_clip_corner(disc_hole, false, 0);
    lv_obj_set_style_bg_color(disc_hole, lv_color_hex(0x11151B), 0);
    lv_obj_set_style_border_width(disc_hole, 0, 0);
    lv_obj_set_style_pad_all(disc_hole, 0, 0);
    lv_obj_remove_flag(disc_hole, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(disc_hole, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(disc_hole);

    /* 封面：放在盘心里、缩放到盘心尺寸，正好盖满黑色区域（四角被上面的圆角裁掉） */
    cover_img = lv_image_create(disc_hole);
    lv_obj_center(cover_img);
    lv_obj_add_flag(cover_img, LV_OBJ_FLAG_HIDDEN);

    music_icon = lv_image_create(cover_box);       /* 没有封面时的占位图标 */
    lv_image_set_src(music_icon, &ui_img_music_data);
    lv_image_set_scale(music_icon, 128);           /* 256 = 原始大小，128 = 缩一半 */
    lv_obj_center(music_icon);

    /* ---------- 歌词：5 行，当前句蓝色高亮 ---------- */
    lrc_prev2_label = lrc_label_create(page, 44,  0xB6BCC4);
    lrc_prev1_label = lrc_label_create(page, 70,  0x7C838C);
    lrc_cur_label   = lrc_label_create(page, 96,  0x1F6FE5);
    lrc_next_label  = lrc_label_create(page, 122, 0x7C838C);
    lrc_next2_label = lrc_label_create(page, 148, 0xB6BCC4);

    /* ---------- 底部：播放列表 / 循环 / 上一曲 / 播放 / 下一曲 / 音量 ---------- */
    menu_btn = lv_btn_create(page);                /* 播放列表（子界面入口） */
    lv_obj_align(menu_btn, LV_ALIGN_TOP_LEFT, 8, 174);
    lv_obj_set_size(menu_btn, 36, 32);
    lv_obj_set_style_bg_opa(menu_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(menu_btn, 0, 0);
    lv_obj_set_style_shadow_width(menu_btn, 0, 0);
    lv_obj_set_style_pad_all(menu_btn, 0, 0);
    l = lv_label_create(menu_btn);
    lv_label_set_text(l, LV_SYMBOL_LIST);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(0x3A3F46), 0);
    lv_obj_center(l);
    lv_obj_add_event_cb(menu_btn, ui_event_menu_btn, LV_EVENT_ALL, NULL);

    loop_btn = lv_btn_create(page);
    lv_obj_align(loop_btn, LV_ALIGN_TOP_LEFT, 48, 174);
    lv_obj_set_size(loop_btn, 36, 32);
    lv_obj_set_style_bg_opa(loop_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(loop_btn, 0, 0);
    lv_obj_set_style_shadow_width(loop_btn, 0, 0);
    lv_obj_set_style_pad_all(loop_btn, 0, 0);
    loop_label = lv_label_create(loop_btn);
    lv_label_set_text(loop_label, LV_SYMBOL_LOOP);
    lv_obj_set_style_text_font(loop_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(loop_label, lv_color_hex(loop_enable ? 0x2F7BEA : 0xB4BAC2), 0);
    lv_obj_center(loop_label);
    lv_obj_add_event_cb(loop_btn, ui_event_loop_btn, LV_EVENT_ALL, NULL);

    prev_btn = lv_btn_create(page);
    lv_obj_align(prev_btn, LV_ALIGN_TOP_LEFT, 98, 174);
    lv_obj_set_size(prev_btn, 38, 32);
    lv_obj_set_style_bg_opa(prev_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(prev_btn, 0, 0);
    lv_obj_set_style_shadow_width(prev_btn, 0, 0);
    lv_obj_set_style_pad_all(prev_btn, 0, 0);
    l = lv_label_create(prev_btn);
    lv_label_set_text(l, LV_SYMBOL_PREV);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(0x3A3F46), 0);
    lv_obj_center(l);
    lv_obj_add_event_cb(prev_btn, ui_event_prev_btn, LV_EVENT_ALL, NULL);

    pause_btn = lv_btn_create(page);               /* 圆形蓝色主按钮 */
    lv_obj_align(pause_btn, LV_ALIGN_TOP_LEFT, 140, 170);
    lv_obj_set_size(pause_btn, 40, 40);
    lv_obj_set_style_radius(pause_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(pause_btn, lv_color_hex(0x2F7BEA), 0);
    lv_obj_set_style_bg_opa(pause_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(pause_btn, 0, 0);
    lv_obj_set_style_shadow_width(pause_btn, 10, 0);
    lv_obj_set_style_shadow_color(pause_btn, lv_color_hex(0x2F7BEA), 0);
    lv_obj_set_style_shadow_opa(pause_btn, LV_OPA_30, 0);
    lv_obj_set_style_pad_all(pause_btn, 0, 0);
    pause_label = lv_label_create(pause_btn);
    lv_label_set_text(pause_label, LV_SYMBOL_PAUSE);
    lv_obj_set_style_text_font(pause_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(pause_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(pause_label);
    lv_obj_add_event_cb(pause_btn, ui_event_playpause_btn, LV_EVENT_ALL, NULL);

    next_btn = lv_btn_create(page);
    lv_obj_align(next_btn, LV_ALIGN_TOP_LEFT, 186, 174);
    lv_obj_set_size(next_btn, 38, 32);
    lv_obj_set_style_bg_opa(next_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(next_btn, 0, 0);
    lv_obj_set_style_shadow_width(next_btn, 0, 0);
    lv_obj_set_style_pad_all(next_btn, 0, 0);
    l = lv_label_create(next_btn);
    lv_label_set_text(l, LV_SYMBOL_NEXT);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(0x3A3F46), 0);
    lv_obj_center(l);
    lv_obj_add_event_cb(next_btn, ui_event_next_btn, LV_EVENT_ALL, NULL);

    vol_btn = lv_btn_create(page);                 /* 音量：点一下弹出竖条 */
    lv_obj_align(vol_btn, LV_ALIGN_TOP_RIGHT, -14, 174);
    lv_obj_set_size(vol_btn, 36, 32);
    lv_obj_set_style_bg_opa(vol_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(vol_btn, 0, 0);
    lv_obj_set_style_shadow_width(vol_btn, 0, 0);
    lv_obj_set_style_pad_all(vol_btn, 0, 0);
    l = lv_label_create(vol_btn);
    lv_label_set_text(l, LV_SYMBOL_VOLUME_MAX);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(0x3A3F46), 0);
    lv_obj_center(l);
    lv_obj_add_event_cb(vol_btn, ui_event_volume_btn, LV_EVENT_ALL, NULL);

    /* ---------- 音量弹出条（竖条，默认隐藏）---------- */
    vol_popup = lv_obj_create(page);
    lv_obj_set_size(vol_popup, 48, 132);
    lv_obj_align(vol_popup, LV_ALIGN_TOP_RIGHT, -8, 36);
    lv_obj_set_style_radius(vol_popup, 12, 0);
    lv_obj_set_style_bg_color(vol_popup, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(vol_popup, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(vol_popup, 0, 0);
    lv_obj_set_style_pad_all(vol_popup, 0, 0);
    lv_obj_set_style_shadow_width(vol_popup, 14, 0);
    lv_obj_set_style_shadow_color(vol_popup, lv_color_hex(0x8FA6BF), 0);
    lv_obj_set_style_shadow_opa(vol_popup, LV_OPA_40, 0);
    lv_obj_remove_flag(vol_popup, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(vol_popup, LV_OBJ_FLAG_HIDDEN);

    vol_value_label = lv_label_create(vol_popup);
    lv_label_set_text(vol_value_label, "0");
    lv_obj_set_style_text_font(vol_value_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(vol_value_label, lv_color_hex(0x3A3F46), 0);
    lv_obj_align(vol_value_label, LV_ALIGN_TOP_MID, 0, 6);

    vol_slider = lv_slider_create(vol_popup);      /* 高 > 宽 时 LVGL 自动按竖条处理 */
    lv_obj_set_size(vol_slider, 8, 86);
    lv_obj_align(vol_slider, LV_ALIGN_TOP_MID, 0, 28);
    lv_slider_set_range(vol_slider, 0, 100);
    lv_slider_set_value(vol_slider, (int)ui_system_para.sound, LV_ANIM_OFF);
    lv_obj_set_ext_click_area(vol_slider, 16);     /* 点轨道两侧也能拖 */
    lv_obj_set_style_bg_color(vol_slider, lv_color_hex(0xD5DDE7), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(vol_slider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(vol_slider, lv_color_hex(0x2F7BEA), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(vol_slider, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(vol_slider, lv_color_hex(0x2F7BEA), LV_PART_KNOB);
    lv_obj_set_style_pad_all(vol_slider, 3, LV_PART_KNOB);
    lv_obj_set_style_shadow_width(vol_slider, 0, LV_PART_KNOB);
    lv_obj_add_event_cb(vol_slider, vol_slider_cb, LV_EVENT_ALL, NULL);

    /* ---------- 底部：进度条 / 两侧时间 ---------- */
    slider = lv_slider_create(page);
    lv_obj_set_size(slider, 292, 6);
    lv_obj_align(slider, LV_ALIGN_TOP_LEFT, 14, 212);
    lv_slider_set_range(slider, 0, 1000);
    lv_slider_set_value(slider, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(slider, lv_color_hex(0xD5DDE7), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, lv_color_hex(0x2F7BEA), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, lv_color_hex(0x2F7BEA), LV_PART_KNOB);
    lv_obj_set_style_pad_all(slider, 2, LV_PART_KNOB);   /* 旋钮大小 */
    lv_obj_set_style_shadow_width(slider, 0, LV_PART_KNOB);
    lv_obj_set_style_bg_opa(slider, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_add_event_cb(slider, ui_event_slider, LV_EVENT_ALL, NULL);

    time_label = lv_label_create(page);
    lv_label_set_text(time_label, "0:00");
    lv_obj_set_style_text_font(time_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(time_label, lv_color_hex(0x8A9099), 0);
    lv_obj_align(time_label, LV_ALIGN_TOP_LEFT, 14, 222);

    time_dur_label = lv_label_create(page);
    lv_label_set_text(time_dur_label, "0:00");
    lv_obj_set_style_text_font(time_dur_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(time_dur_label, lv_color_hex(0x8A9099), 0);
    lv_obj_align(time_dur_label, LV_ALIGN_TOP_RIGHT, -14, 222);

    /* ---------- 播放列表子界面（全屏浮层，默认隐藏，点菜单按钮打开）---------- */
    list_built = 0;
    list_job   = 0;
    list_job_timer = NULL;                 /* deinit 里删过，这里清指针 */
    for (int i = 0; i < MUSIC_MAX_FILES; i++) {
        list_rows[i]          = NULL;
        list_title_labels[i]  = NULL;
        list_artist_labels[i] = NULL;
        list_covers[i]        = NULL;
        list_badges[i]        = NULL;
        list_thumbs[i]        = NULL;
        list_artist[i][0]     = 0;
        list_title[i][0]      = 0;
        meta_loaded[i]        = 0;
        memset(&list_thumb_dsc[i], 0, sizeof(list_thumb_dsc[i]));
    }
    disc_spin_stop();          /* 兜底：上一页 deinit 已经停过 */
    disc_angle = 0;

    list_overlay = lv_obj_create(page);
    lv_obj_set_size(list_overlay, UI_SCREEN_WIDTH, UI_SCREEN_HEIGHT);
    lv_obj_set_pos(list_overlay, 0, 0);
    lv_obj_set_style_radius(list_overlay, 0, 0);
    lv_obj_set_style_bg_color(list_overlay, lv_color_hex(0xF2F4F7), 0);
    lv_obj_set_style_bg_opa(list_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(list_overlay, 0, 0);
    lv_obj_set_style_pad_all(list_overlay, 0, 0);
    lv_obj_remove_flag(list_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(list_overlay, LV_OBJ_FLAG_HIDDEN);

    list_box = lv_obj_create(list_overlay);          /* 可滚动的歌曲列表 */
    lv_obj_set_size(list_box, UI_SCREEN_WIDTH, UI_SCREEN_HEIGHT - 34);
    lv_obj_set_pos(list_box, 0, 34);
    lv_obj_set_style_radius(list_box, 0, 0);
    lv_obj_set_style_bg_opa(list_box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list_box, 0, 0);
    lv_obj_set_style_pad_top(list_box, 2, 0);
    lv_obj_set_style_pad_bottom(list_box, 6, 0);
    lv_obj_set_style_pad_left(list_box, 8, 0);
    lv_obj_set_style_pad_right(list_box, 8, 0);
    lv_obj_set_style_pad_row(list_box, 4, 0);
    lv_obj_set_flex_flow(list_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(list_box, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list_box, LV_SCROLLBAR_MODE_OFF);

    /* 刷新定时器：音乐不需要高频刷新，300ms 足够（省 CPU） */
    music_timer = lv_timer_create(music_refresh_cb, 300, NULL);

    /* ---------- 找曲子 ---------- */
    const char *env = getenv("ECHO_MUSIC");
    int found = 0;
    if (env && env[0]) {
        found = (set_playlist(env) == 0);
        if (!found) printf("[MusicPage] ECHO_MUSIC=%s 里没找到音乐\n", env);
    } else {
        for (unsigned i = 0; i < sizeof(music_search_paths) / sizeof(music_search_paths[0]); i++) {
            if (!probe_music_path(music_search_paths[i])) {
                printf("[MusicPage] %s 里没有音乐\n", music_search_paths[i]);
                continue;
            }
            printf("[MusicPage] 自动找到音乐：%s\n", music_search_paths[i]);
            found = (set_playlist(music_search_paths[i]) == 0);
            break;
        }
    }
    if (!found) {
        printf("[MusicPage] 所有候选路径都没有音乐\n");
        lv_label_set_text(status_label, "No music");
        lv_scr_load_anim(page, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 100, 0, true);
        return;
    }
    if (!ui_video_is_supported()) {
        lv_label_set_text(status_label, "ffmpeg not built in");
        lv_scr_load_anim(page, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 100, 0, true);
        return;
    }

    printf("[MusicPage] 播放列表 %d 首，当前: %s\n", playlist_count, playlist[0]);

    /* 调试：ECHO_MUSIC_TRACK=N 从第 N 首（0 开始）开始播 */
    const char *trk = getenv("ECHO_MUSIC_TRACK");
    if (trk && trk[0]) {
        int n = atoi(trk);
        if (n >= 0 && n < playlist_count) playlist_index = n;
    }

    player_run = 1;
    if (pthread_create(&player_tid, NULL, music_player_thread, NULL) != 0) {
        player_run = 0;
        lv_label_set_text(status_label, "thread failed");
        lv_scr_load_anim(page, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 100, 0, true);
        return;
    }

    const char *st = getenv("ECHO_MUSIC_SELFTEST");
    if (st && st[0] == '1') {
        printf("[MusicPage] 自检开始\n");
        lv_timer_create(selftest_cb, 2000, NULL);
    }

    const char *lsenv = getenv("ECHO_MUSIC_LIST");
    if (lsenv && lsenv[0] == '1') lv_timer_create(list_auto_cb, 1200, NULL);
    const char *vlenv = getenv("ECHO_MUSIC_VOL");
    if (vlenv && vlenv[0] == '1') lv_timer_create(vol_auto_cb, 1200, NULL);

    lv_scr_load_anim(page, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 100, 0, true);
}

void ui_MusicPage_deinit(void)
{
    player_run = 0;
    req_switch = -1;
    req_seek   = -1;
    user_seeking = 0;
    if (music_timer) { lv_timer_delete(music_timer); music_timer = NULL; }
    if (list_job_timer) { lv_timer_delete(list_job_timer); list_job_timer = NULL; }
    if (player_tid) {
        pthread_join(player_tid, NULL);
        player_tid = 0;
    }
    ui_audio_out_close();      /* 线程里关过，这里兜底 */
    disc_spin_stop();          /* 先把旋转动画挂掉，后面 cover_box 会被删 */
    player_error = 0;
    title_label  = NULL;
    status_label = NULL;
    time_label   = NULL;
    time_dur_label = NULL;
    slider       = NULL;
    music_icon   = NULL;
    cover_box    = NULL;
    cover_img    = NULL;
    disc_hole    = NULL;
    bg_img       = NULL;
    cover_idx    = -2;
    lrc_prev2_label = NULL;
    lrc_prev1_label = NULL;
    lrc_cur_label   = NULL;
    lrc_next_label  = NULL;
    lrc_next2_label = NULL;
    menu_btn = NULL;
    vol_btn  = NULL;
    vol_popup = NULL;
    vol_slider = NULL;
    vol_value_label = NULL;
    list_overlay = NULL;
    list_box = NULL;
    list_built = 0;
    list_job = 0;
    for (int i = 0; i < MUSIC_MAX_FILES; i++) {
        list_rows[i]          = NULL;
        list_title_labels[i]  = NULL;
        list_artist_labels[i] = NULL;
        list_covers[i]        = NULL;
        list_badges[i]        = NULL;
        list_artist[i][0]     = 0;
        list_title[i][0]      = 0;
        meta_loaded[i]        = 0;
        if (list_thumbs[i]) { free(list_thumbs[i]); list_thumbs[i] = NULL; }
        memset(&list_thumb_dsc[i], 0, sizeof(list_thumb_dsc[i]));
    }
    printf("[MusicPage] deinit 完成\n");
}
