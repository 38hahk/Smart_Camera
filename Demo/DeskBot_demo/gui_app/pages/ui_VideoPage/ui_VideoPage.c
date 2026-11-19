/*
 * 视频 App：读取 SD 卡里的 mp4（H.264）并显示在 LCD 上
 *
 * 结构：
 *   - 播放线程：ffmpeg 解码 → RGB565 帧写进 frame_staging
 *   - LVGL 定时器（UI 线程）：把 frame_staging 拷进 canvas 缓冲区并 invalidate
 *     （一定要在 UI 线程里 invalidate，LVGL 不是线程安全的）
 *
 * 路径：优先读环境变量 ECHO_VIDEO；否则用 /root/bin/video/demo.mp4
 *       （目录里如果有多个 mp4，取第一个）
 */

#include "ui_VideoPage.h"
#include "media/ui_video_decode.h"
#include "media/ui_audio_out.h"

#if defined(HAVE_ICONV)
#include <iconv.h>          /* SD 卡上的中文文件名可能是 GBK，显示前转成 UTF-8 */
#endif
#if defined(HAVE_FFMPEG)
#include <libavformat/avformat.h>   /* 只读容器头，拿视频时长 */
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/stat.h>

///////////////////// VARIABLES ////////////////////

#define VIDEO_DEFAULT_PATH "/root/bin/video/demo.mp4"
#define VIDEO_TARGET_FPS   12      /* 单核 A7 + SPI 屏，10~12fps 比较稳 */

/*
 * 没设 ECHO_VIDEO 时按这个顺序找视频（第一个有视频的胜出）。
 * ⭐ 顺序是「**SD 卡优先、固件自带垫底**」：卡里插上就能放卡上的（换片不用重烧），
 *    卡上没有才用固件自带的 /root/video。音乐页也是这个思路（那边卡排在前面）。
 *    注意探测目录只看该目录**自己**有没有视频文件，不看子目录。
 */
static const char * const video_search_paths[] = {
    "/mnt/media/video",         /* SD 卡上的 video 子目录（卡上有就优先播卡上的） */
    "/mnt/media",               /* SD 卡根目录 */
    "/mnt/video",               /* 手动挂到别处的卡 */
    "/root/video",              /* 固件 overlay 自带的（垫底：卡上没有才用） */
    "/root/bin/video",          /* 早期版本用的位置 */
    VIDEO_DEFAULT_PATH,         /* 老默认值：单个文件 */
};

static uint8_t video_canvas_buf[UI_SCREEN_WIDTH * UI_SCREEN_HEIGHT * 2];  /* 给 LVGL 画 */
static uint8_t frame_staging[UI_SCREEN_WIDTH * UI_SCREEN_HEIGHT * 2];     /* 给播放线程写 */

static lv_obj_t *video_canvas;
static lv_obj_t *status_label;
static lv_timer_t *video_timer;

static pthread_t        player_tid;
static pthread_mutex_t  frame_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile int     player_run   = 0;
static volatile int     frame_ready  = 0;
static volatile int     player_error = 0;
static volatile int     paused       = 0;

static char video_path[256];
static int  video_w = 0, video_h = 0;
static int  frames_shown = 0;
static double cur_fps = 0;

/*
 * 声音比画面晚多少毫秒（提前垫静音的量）。
 * 为什么需要它：视频是「每解出一帧才顺手喂一次音频」，12fps 下就是每 83ms 才
 * 突发一批音频，而声卡是按 11ms 一小块连续要数据的 —— 中间那段没数据就成了
 * 欠载（听感上是周期性的咔哒声）。垫一段静音把缓冲垫起来就不会欠载了。
 * 代价就是声音整体比画面晚这么点时间（12fps 的画面本身就很顿，120ms 基本同步）。
 * 调法：看日志里的 under>0 就调大，嫌口型不同步就调小。
 */
static int audio_prebuf_ms(void)
{
    const char *e = getenv("ECHO_VIDEO_AUBUF");
    int ms = (e && e[0]) ? atoi(e) : 120;
    if (ms < 0)   ms = 0;
    if (ms > 1000) ms = 1000;
    return ms;
}

/* 解码线程拿到 PCM 就丢进音频环形缓冲 */
static void on_pcm(void *user, const void *pcm, int bytes)
{
    (void)user;
    ui_audio_out_write(pcm, bytes);
}

/* 播放列表：目录里多个视频可以来回切 */
#define VIDEO_MAX_FILES 32
#define VIDEO_NAME_LEN  160
static char playlist[VIDEO_MAX_FILES][256];
static char playlist_name[VIDEO_MAX_FILES][VIDEO_NAME_LEN];
static int  playlist_count = 0;
static int  playlist_index = 0;

/* UI -> 播放线程的请求（都是单写单读的简单标志） */
static volatile int    req_switch   = -1;   /* >=0 表示要切到第几个 */
static volatile double req_seek     = -1;   /* >=0 表示要跳到第几秒 */
static volatile int    loop_enable  = 1;    /* 循环播放 */
static volatile double cur_pos      = 0;    /* 当前播放位置（秒） */
static volatile double cur_dur      = 0;    /* 总时长（秒，0 = 未知） */
static volatile int    user_seeking = 0;    /* 用户正在拖进度条 */

/* UI 控件 */
static lv_obj_t *loop_label;
static lv_obj_t *pause_label;
static lv_obj_t *slider;
static lv_obj_t *time_label;
static lv_obj_t *time_dur_label;          /* 总时长（右下角） */
static lv_obj_t *file_label;

/* 顶部银白 bar / 底部控制卡片 / 点画面用的透明层 */
static lv_obj_t *ctrl_bar, *ctrl_card, *tap_area;
static int       ctrl_visible = 1;        /* 控制条是否显示（5 秒无操作自动收起） */
static lv_obj_t *menu_btn;                /* 播放列表（子界面入口） */

/* ---------- 播放列表子界面（叠在本页上的全屏浮层，切集不会中断播放）---------- */
#define VLIST_ROW_H  44
static lv_obj_t *list_overlay, *list_box;
static lv_obj_t *list_rows[VIDEO_MAX_FILES];
static lv_obj_t *list_title_labels[VIDEO_MAX_FILES];
static lv_obj_t *list_sub_labels[VIDEO_MAX_FILES];
static lv_obj_t *list_badges[VIDEO_MAX_FILES];
static char     list_dur[VIDEO_MAX_FILES][16];     /* "3:25"，空 = 还没读 */
static unsigned char dur_loaded[VIDEO_MAX_FILES];
static lv_timer_t *list_job_timer;
static int list_built = 0;
static int list_job  = 0;

/* 按钮（自检要用到，就放文件作用域） */
static lv_obj_t *back_btn;
static lv_obj_t *loop_btn;
static lv_obj_t *pause_btn;
static lv_obj_t *prev_btn;
static lv_obj_t *next_btn;

///////////////////// 路径处理 ////////////////////

/* 判断文件名是不是视频 */
static int is_video_name(const char *n)
{
    size_t l = strlen(n);
    if (l < 5) return 0;
    const char *ext = n + l - 4;
    return !strcasecmp(ext, ".mp4") || !strcasecmp(ext, ".avi") ||
           !strcasecmp(ext, ".mov") || !strcasecmp(ext, ".mkv");
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

/* 传目录就收集里面的视频（最多 32 个，按名字排序）；传文件就是单条 */
static int scan_playlist(const char *src)
{
    playlist_count = 0;
    if (!src || !src[0]) return 0;

    DIR *dir = opendir(src);
    if (!dir) {                        /* 单个文件 */
        snprintf(playlist[0], sizeof(playlist[0]), "%s", src);
        base_name(playlist_name[0], VIDEO_NAME_LEN, src);
        playlist_count = 1;
        return 1;
    }

    static char tmp[VIDEO_MAX_FILES][256];
    int n = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL && n < VIDEO_MAX_FILES) {
        if (!is_video_name(ent->d_name)) continue;
        snprintf(tmp[n], sizeof(tmp[0]), "%s%s%s", src,
                 (src[strlen(src) - 1] == '/') ? "" : "/", ent->d_name);
        n++;
    }
    closedir(dir);

    qsort(tmp, n, sizeof(tmp[0]), name_cmp);   /* 排序，保证上一集/下一集顺序稳定 */
    for (int i = 0; i < n; i++) {
        snprintf(playlist[i], sizeof(playlist[i]), "%s", tmp[i]);
        base_name(playlist_name[i], VIDEO_NAME_LEN, tmp[i]);
    }
    playlist_count = n;
    return n;
}

int ui_VideoPage_set_file(const char *path)
{
    if (!path || !path[0]) return -1;
    if (scan_playlist(path) <= 0) {
        printf("[VideoPage] 没找到视频文件: %s\n", path);
        return -1;
    }
    playlist_index = 0;
    snprintf(video_path, sizeof(video_path), "%s", playlist[0]);
    printf("[VideoPage] 播放列表 %d 个，当前: %s\n", playlist_count, video_path);
    return 0;
}

const char *ui_VideoPage_get_file(void) { return video_path; }

/* 这个路径里真的有视频吗（目录要非空，文件要是视频扩展名） */
static int probe_video_path(const char *p)
{
    if (!p || access(p, F_OK) != 0) return 0;

    struct stat st;
    if (stat(p, &st) != 0) return 0;
    if (S_ISREG(st.st_mode)) return is_video_name(p) ? 1 : 0;
    if (!S_ISDIR(st.st_mode)) return 0;
    return scan_playlist(p) > 0;     /* 目录：扫一遍看有没有视频 */
}

///////////////////// 播放线程 ////////////////////

static void sleep_until(struct timespec *t0, double seconds)
{
    double target = t0->tv_sec + t0->tv_nsec / 1e9 + seconds;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    double cur = now.tv_sec + now.tv_nsec / 1e9;
    if (target > cur) {
        struct timespec sl;
        double d = target - cur;
        sl.tv_sec  = (time_t)d;
        sl.tv_nsec = (long)((d - sl.tv_sec) * 1e9);
        nanosleep(&sl, NULL);
    }
}

static void *video_player_thread(void *arg)
{
    (void)arg;

    while (player_run) {
        if (playlist_count <= 0) break;
        if (playlist_index < 0 || playlist_index >= playlist_count) playlist_index = 0;

        ui_video_decoder_t *dec = ui_video_open(playlist[playlist_index],
                                                UI_SCREEN_WIDTH, UI_SCREEN_HEIGHT);
        if (!dec) { player_error = 1; break; }

        video_w = ui_video_width(dec);
        video_h = ui_video_height(dec);
        cur_dur = ui_video_duration(dec);
        cur_pos = 0;
        snprintf(video_path, sizeof(video_path), "%s", playlist[playlist_index]);

        int fps = (int)(ui_video_fps(dec) + 0.5f);
        if (fps < 1 || fps > 30) fps = VIDEO_TARGET_FPS;
        double interval = 1.0 / fps;
        printf("[VideoPage] 播放 %d/%d %s  %dx%d @%dfps  %.1fs\n",
               playlist_index + 1, playlist_count, playlist_name[playlist_index],
               video_w, video_h, fps, cur_dur);

        /* ---------- 音频：打开输出 + 注册 PCM 回调 ---------- */
        if (ui_video_has_audio(dec)) {
            int ar = ui_video_audio_rate(dec), ac = ui_video_audio_channels(dec);
            const char *er = getenv("ECHO_VIDEO_AURATE");
            const char *ec = getenv("ECHO_VIDEO_AUCH");
            if (er && er[0]) ar = atoi(er);
            if (ec && ec[0]) ac = atoi(ec);
            if (ar <= 0) ar = 44100;
            if (ac <= 0) ac = 1;
            if (ar != ui_video_audio_rate(dec) || ac != ui_video_audio_channels(dec))
                ui_video_set_audio_format(dec, ar, ac);

            if (ui_audio_out_open(ar, ac, audio_prebuf_ms()) != 0) {
                /* 设备不吃文件自带的采样率/声道数 → 降级到板子上验证过的 16kHz 单声道 */
                if (ar != 16000 || ac != 1) {
                    printf("[VideoPage] 音频降级到 16000Hz 单声道\n");
                    if (ui_video_set_audio_format(dec, 16000, 1) == 0) {
                        ar = 16000; ac = 1;
                        ui_audio_out_open(ar, ac, audio_prebuf_ms());
                    }
                }
            }
            ui_video_set_pcm_callback(dec, on_pcm, NULL);
            printf("[VideoPage] 音频%s: %dHz %dch 垫%dms\n",
                   ui_audio_out_is_open() ? "已开" : "不可用(静音)",
                   ar, ac, audio_prebuf_ms());
        } else {
            printf("[VideoPage] 这个文件没有音频流\n");
        }
        int audio_paused = 0;

        struct timespec t0, t1;
        double stats_t0 = 0; int stats_n = 0;
        double pos_acc = 0; int pos_n = 0;
        int need_reopen = 0;
        clock_gettime(CLOCK_MONOTONIC, &t0);

        while (player_run && !need_reopen) {
            /* 切上一集/下一集 */
            if (req_switch >= 0) {
                playlist_index = req_switch;
                req_switch = -1;
                need_reopen = 1;
                break;
            }
            /* 拖动进度条后跳转 */
            if (req_seek >= 0) {
                ui_video_seek(dec, req_seek);
                ui_audio_out_flush();       /* 丢掉旧位置还没放出来的声音 */
                req_seek = -1;
                clock_gettime(CLOCK_MONOTONIC, &t0);
            }
            /* 暂停/恢复：音频跟着静音/重新垫缓冲 */
            if (paused != audio_paused) {
                ui_audio_out_set_paused(paused);
                audio_paused = paused;
            }
            if (paused) { usleep(20000); clock_gettime(CLOCK_MONOTONIC, &t0); continue; }

            clock_gettime(CLOCK_MONOTONIC, &t0);

            pthread_mutex_lock(&frame_lock);
            int r = ui_video_read_frame(dec, frame_staging);
            if (r == 1) frame_ready = 1;
            pthread_mutex_unlock(&frame_lock);

            if (r == 0) {                 /* 一轮播完 */
                ui_video_rewind(dec);     /* 回到开头，不循环时按 Play 就从头发 */
                ui_audio_out_flush();
                if (!loop_enable) paused = 1;
                continue;
            }
            if (r < 0) { player_error = 1; break; }

            /* 播放位置：每 4 帧采样一次 */
            pos_acc += ui_video_position(dec); pos_n++;
            if (pos_n >= 4) { cur_pos = pos_acc / pos_n; pos_acc = 0; pos_n = 0; }

            /* 帧率统计（按墙钟时间，每秒打印一次） */
            clock_gettime(CLOCK_MONOTONIC, &t1);
            if (stats_t0 == 0) stats_t0 = t1.tv_sec + t1.tv_nsec / 1e9;
            stats_n++;
            double now = t1.tv_sec + t1.tv_nsec / 1e9;
            if (now - stats_t0 >= 1.0) {
                cur_fps = stats_n / (now - stats_t0);
                /* 音频统计一起打印：buf=环形缓冲里还有多少字节，
                 * under=真欠载（会累积延迟，一直涨就把 ECHO_VIDEO_AUBUF 调大），
                 * skip=trylock冲突的单次静音（11ms，偶发无所谓）*/
                printf("[VideoPage] decode fps = %.1f  pos=%.1fs  audio buf=%dB under=%d skip=%d\n",
                       cur_fps, cur_pos, ui_audio_out_buffered_bytes(),
                       ui_audio_out_underruns(), ui_audio_out_lockskips());
                stats_t0 = now; stats_n = 0;
            }

            sleep_until(&t0, interval);
        }

        ui_audio_out_close();
        ui_video_close(dec);
    }

    printf("[VideoPage] 播放线程退出\n");
    return NULL;
}

///////////////////// UI ////////////////////

/* 显示/收起 顶部 bar + 底部控制卡片（点画面切换）*/
static void ctrl_show(int show)
{
    if (ctrl_visible == (show ? 1 : 0)) return;
    ctrl_visible = show ? 1 : 0;
    if (!ctrl_bar || !ctrl_card) return;
    if (show) {
        lv_obj_remove_flag(ctrl_bar, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(ctrl_card, LV_OBJ_FLAG_HIDDEN);
        lv_display_trigger_activity(NULL);       /* 重新开始计 5 秒 */
    } else {
        lv_obj_add_flag(ctrl_bar, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ctrl_card, LV_OBJ_FLAG_HIDDEN);
    }
    printf("[VideoPage] 控制条%s\n", show ? "显示" : "收起");
}

/* 点画面：有控制条就收起，没有就唤出 */
static void ui_event_tap_area(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    ctrl_show(!ctrl_visible);
}

///////////////////// 播放列表子界面 ////////////////////

/* 合法 UTF-8 判断（SD 卡上的中文文件名可能是 GBK，内核没开 NLS_UTF8） */
static int video_utf8_valid(const unsigned char *s, size_t n)
{
    size_t i = 0;
    while (i < n) {
        unsigned char c = s[i];
        int len;
        if (c < 0x80)                { i++;     continue; }
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

/* 不是 UTF-8 → 按 GBK 系转一次；都不行就原样留着（顶多显示乱码，不会崩） */
static void video_text_fix_utf8(char *s, size_t cap)
{
    size_t n = strlen(s);
    if (n == 0 || video_utf8_valid((const unsigned char *)s, n)) return;
#if defined(HAVE_ICONV)
    static const char *charsets[] = { "GBK", "GB18030", "GB2312" };
    for (unsigned i = 0; i < sizeof(charsets) / sizeof(charsets[0]); i++) {
        iconv_t cd = iconv_open("UTF-8", charsets[i]);
        if (cd == (iconv_t)-1) continue;

        char *out = malloc(cap);
        if (!out) { iconv_close(cd); return; }

        char  *inp = s;
        size_t inleft = n;
        char  *outp = out;
        size_t outleft = cap - 1;
        size_t r = iconv(cd, &inp, &inleft, &outp, &outleft);
        iconv_close(cd);

        if (r != (size_t)-1 && inleft == 0) {
            size_t outlen = (size_t)(outp - out);
            memcpy(s, out, outlen);
            s[outlen] = 0;
            free(out);
            printf("[VideoPage] 文件名按 %s 转成 UTF-8\n", charsets[i]);
            return;
        }
        free(out);
    }
#else
    (void)cap;
#endif
}

/* 界面上显示的名字：文件名去扩展名（必要时转码） */
static void video_display_name(int idx, char *out, size_t n)
{
    if (n == 0) return;
    out[0] = 0;
    if (idx < 0 || idx >= playlist_count) return;

    snprintf(out, n, "%s", playlist_name[idx]);
    char *dot = strrchr(out, '.');
    if (dot && dot != out) *dot = 0;
    video_text_fix_utf8(out, n);
}

/* 用 ffmpeg 读容器头拿时长（只读头，不解码），格式化成 m:ss */
static void video_read_duration(int idx, char *out, size_t n)
{
    out[0] = 0;
#if defined(HAVE_FFMPEG)
    if (idx < 0 || idx >= playlist_count) return;

    AVFormatContext *fmt = NULL;
    if (avformat_open_input(&fmt, playlist[idx], NULL, NULL) != 0) return;

    /* 很多容器（mp4 也是）open 完之后 duration 还是 AV_NOPTS_VALUE，
     * 要先读一遍流信息才有；实在没有就退回视频流的 duration */
    avformat_find_stream_info(fmt, NULL);
    int64_t dur = fmt->duration;
    if (dur == AV_NOPTS_VALUE || dur <= 0) {
        dur = AV_NOPTS_VALUE;
        for (unsigned i = 0; i < fmt->nb_streams; i++) {
            AVStream *st = fmt->streams[i];
            if (st->codecpar->codec_type != AVMEDIA_TYPE_VIDEO) continue;
            if (st->duration != AV_NOPTS_VALUE && st->duration > 0) {
                dur = av_rescale_q(st->duration, st->time_base, AV_TIME_BASE_Q);
                break;
            }
        }
    }
    if (dur != AV_NOPTS_VALUE && dur > 0) {
        int secs = (int)(dur / AV_TIME_BASE);
        snprintf(out, n, "%d:%02d", secs / 60, secs % 60);
    }
    avformat_close_input(&fmt);
#else
    (void)idx; (void)n;
#endif
}

/* 把「正在播放」那一行刷成绿色标题 + 深色底 + 播放徽标 */
static void list_update_current(void)
{
    for (int i = 0; i < playlist_count && i < VIDEO_MAX_FILES; i++) {
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

/* 点某一行：切到这一集并关掉列表 */
static void list_row_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;

    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx >= 0 && idx < playlist_count) {
        req_switch = idx;
        paused = 0;
        printf("[VideoPage] 列表选片 -> %d/%d %s\n",
               idx + 1, playlist_count, playlist_name[idx]);
    }
    if (list_overlay) lv_obj_add_flag(list_overlay, LV_OBJ_FLAG_HIDDEN);
}

static void list_back_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (list_overlay) lv_obj_add_flag(list_overlay, LV_OBJ_FLAG_HIDDEN);
}

/* 后台补时长：60ms 一拍读一个文件（只读容器头，不阻塞界面） */
static void list_job_cb(lv_timer_t *t)
{
    (void)t;
    if (!list_overlay || lv_obj_has_flag(list_overlay, LV_OBJ_FLAG_HIDDEN)) return;

    if (list_job >= playlist_count || list_job >= VIDEO_MAX_FILES) {
        lv_timer_delete(list_job_timer);
        list_job_timer = NULL;
        printf("[VideoPage] 列表时长读取完成\n");
        return;
    }

    int i = list_job++;
    if (dur_loaded[i]) return;
    dur_loaded[i] = 1;
    video_read_duration(i, list_dur[i], sizeof(list_dur[i]));
    if (list_sub_labels[i]) {
        lv_label_set_text(list_sub_labels[i], list_dur[i][0] ? list_dur[i] : "-");
    }
}

/* 建列表（只建一次）：顶栏 + 每个视频一行 */
static void list_build(void)
{
    if (list_built || !list_box) return;
    list_built = 1;
    lv_obj_clean(list_box);

    char buf[64];
    char name_buf[VIDEO_NAME_LEN];

    lv_obj_t *title = lv_label_create(list_overlay);
    snprintf(buf, sizeof(buf), "共 %d 个视频", playlist_count);
    lv_label_set_text(title, buf);
    lv_obj_set_style_text_font(title, &ui_font_app16, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0x1F242B), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 48, 8);

    lv_obj_t *back = lv_btn_create(list_overlay);
    lv_obj_set_size(back, 44, 32);
    lv_obj_set_pos(back, 0, 0);
    lv_obj_set_style_bg_opa(back, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(back, 0, 0);
    lv_obj_set_style_shadow_width(back, 0, 0);
    lv_obj_set_style_pad_all(back, 0, 0);
    lv_obj_t *al = lv_label_create(back);
    lv_label_set_text(al, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(al, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(al, lv_color_hex(0x3A3F46), 0);
    lv_obj_align(al, LV_ALIGN_LEFT_MID, 10, 0);
    lv_obj_add_event_cb(back, list_back_cb, LV_EVENT_ALL, NULL);

    for (int i = 0; i < playlist_count && i < VIDEO_MAX_FILES; i++) {
        lv_obj_t *row = lv_obj_create(list_box);
        lv_obj_set_size(row, 300, VLIST_ROW_H);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_radius(row, 8, 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, list_row_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        list_rows[i] = row;

        /* 小图标框（没有视频缩略图，用 ▶ 图标占位） */
        lv_obj_t *icon = lv_obj_create(row);
        lv_obj_set_size(icon, 32, 32);
        lv_obj_set_pos(icon, 8, 6);
        lv_obj_set_style_radius(icon, 6, 0);
        lv_obj_set_style_bg_color(icon, lv_color_hex(0xDCE0E5), 0);
        lv_obj_set_style_border_width(icon, 0, 0);
        lv_obj_set_style_pad_all(icon, 0, 0);
        lv_obj_remove_flag(icon, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(icon, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t *il = lv_label_create(icon);
        lv_label_set_text(il, LV_SYMBOL_VIDEO);
        lv_obj_set_style_text_font(il, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(il, lv_color_hex(0x3A3F46), 0);
        lv_obj_center(il);

        /* 正在播放的圆形播放徽标 */
        lv_obj_t *badge = lv_obj_create(row);
        lv_obj_set_size(badge, 18, 18);
        lv_obj_set_pos(badge, 15, 13);
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

        /* 名字（去扩展名） */
        video_display_name(i, name_buf, sizeof(name_buf));
        lv_obj_t *tt = lv_label_create(row);
        lv_label_set_text(tt, name_buf);
        lv_obj_set_style_text_font(tt, &ui_font_app16, 0);
        lv_obj_set_style_text_color(tt, lv_color_hex(0x22262B), 0);
        lv_obj_set_width(tt, 236);
        lv_label_set_long_mode(tt, LV_LABEL_LONG_DOT);
        lv_obj_set_pos(tt, 50, 3);
        list_title_labels[i] = tt;

        /* 时长（先显示 -，由 list_job_cb 逐行补） */
        lv_obj_t *sub = lv_label_create(row);
        lv_label_set_text(sub, list_dur[i][0] ? list_dur[i] : "-");
        lv_obj_set_style_text_font(sub, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(sub, lv_color_hex(0x8A9099), 0);
        lv_obj_set_pos(sub, 50, 24);
        list_sub_labels[i] = sub;
    }

    list_job = 0;
    if (!list_job_timer) list_job_timer = lv_timer_create(list_job_cb, 60, NULL);
}

/* 打开列表：滚到当前集、收起控制条 */
static void list_show(void)
{
    if (!list_overlay) return;
    if (!list_built) list_build();

    list_update_current();
    lv_obj_remove_flag(list_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(list_overlay);

    if (playlist_index >= 0 && playlist_index < VIDEO_MAX_FILES && list_rows[playlist_index]) {
        lv_obj_scroll_to_view(list_rows[playlist_index], LV_ANIM_OFF);
    }
    printf("[VideoPage] 打开播放列表（%d 个）\n", playlist_count);
}

static void ui_event_menu_btn(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    list_show();
}

/* 无屏调试钩子：ECHO_VIDEO_LIST=1 开机直接打开播放列表 */
static void list_auto_cb(lv_timer_t *t)
{
    lv_timer_delete(t);
    list_show();
}

/* LVGL 线程里跑：搬帧 + 更新进度/时间/文件名（标签内容变了才写，省 SPI 带宽） */
static void video_refresh_cb(lv_timer_t *timer)
{
    LV_UNUSED(timer);

    if (player_error && player_run == 0) {
        if (status_label) lv_label_set_text(status_label, "Decode failed");
        lv_timer_delete(video_timer);
        video_timer = NULL;
        return;
    }

    char buf[160];
    /* 每次重新进页面（frames_shown 归零）都把"上次显示过什么"重置掉 */
    static int last_paused = -1, last_slider = -1, last_sec = -1, last_idx = -2;
    if (frames_shown == 0) { last_paused = -1; last_slider = -1; last_sec = -1; last_idx = -2; }

    /* 暂停按钮：图标和音乐页一致 */
    if (pause_label && paused != last_paused) {
        lv_label_set_text(pause_label, paused ? LV_SYMBOL_PLAY : LV_SYMBOL_PAUSE);
        last_paused = paused;
    }

    /* 进度条（用户正在拖就不回写，免得打架） */
    if (slider && cur_dur > 0.05 && !user_seeking) {
        int v = (int)(cur_pos / cur_dur * 1000.0);
        if (v < 0) v = 0;
        if (v > 1000) v = 1000;
        if (v != last_slider) { lv_slider_set_value(slider, v, LV_ANIM_OFF); last_slider = v; }
    }

    /* 时间：秒数变了才更新（左右两边分开显示，和音乐页一致） */
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

    /* 标题 = 文件名（去扩展名），右侧 = 第几集 */
    if (file_label && playlist_index != last_idx) {
        snprintf(buf, sizeof(buf), "%s",
                 (playlist_count > 0) ? playlist_name[playlist_index] : "-");
        char *dot = strrchr(buf, '.');
        if (dot && dot != buf) *dot = 0;
        lv_label_set_text(file_label, buf);
        if (status_label) {
            snprintf(buf, sizeof(buf), "%d / %d", playlist_index + 1,
                     playlist_count > 0 ? playlist_count : 1);
            lv_label_set_text(status_label, buf);
        }
        list_update_current();          /* 列表浮层开着时高亮跟着走 */
        last_idx = playlist_index;
    }

    if (pthread_mutex_trylock(&frame_lock) != 0) return;
    if (frame_ready) {
        memcpy(video_canvas_buf, frame_staging, sizeof(video_canvas_buf));
        frame_ready = 0;
        lv_obj_invalidate(video_canvas);
        frames_shown++;
    }
    pthread_mutex_unlock(&frame_lock);

    /* 控制条 5 秒没操作就收起，全屏看片不被挡；点画面再唤出 */
    if (ctrl_visible && lv_display_get_inactive_time(NULL) > 5000) ctrl_show(0);
}

static void ui_event_back_btn(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;

    /* 看板模式（开机直接进视频页）时视频页就是根页面，没"上一页"可返回 ——
     * 那种情况下直接回桌面，否则按返回毫无反应，人就被卡在视频页里出不去。*/
    if (page_manager.cur_depth <= 1) {
        static char home_name[] = "HomePage";   /* OpenPage 要 char*，不能用字符串字面量 */
        lv_lib_pm_OpenPage(&page_manager, NULL, home_name);
    } else {
        lv_lib_pm_OpenPrePage(&page_manager);
    }
}

static void ui_event_playpause_btn(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    paused = !paused;
    /* 按钮文字由刷新定时器统一同步 */
}

static void ui_event_loop_btn(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    loop_enable = !loop_enable;
    if (loop_label) {
        /* 图标不变，只靠颜色区分开关（蓝=开 / 灰=关），和音乐页一致 */
        lv_obj_set_style_text_color(loop_label,
            lv_color_hex(loop_enable ? 0x2F7BEA : 0xB4BAC2), 0);
    }
    printf("[VideoPage] 循环 = %s\n", loop_enable ? "on" : "off");
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

/* 进度条：按住了先不让定时器回写，松手才真正 seek */
static void ui_event_slider(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED || code == LV_EVENT_PRESSING) {
        user_seeking = 1;
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_CLICKED) {
        if (cur_dur > 0.05) {
            double want = (double)lv_slider_get_value(slider) / 1000.0 * cur_dur;
            req_seek = want;
            printf("[VideoPage] seek -> %.1fs\n", want);
        }
        user_seeking = 0;
    }
}

/* 自检：依次演练各个控件（用 LVGL 事件，等于模拟触摸） */
static void selftest_cb(lv_timer_t *t)
{
    static int step = 0;
    switch (step++) {
    case 1:
        printf("[selftest] 点 Next\n");
        lv_obj_send_event(next_btn, LV_EVENT_CLICKED, NULL);
        break;
    case 3:
        printf("[selftest] 把进度条拖到 60%% 并松手\n");
        lv_obj_send_event(slider, LV_EVENT_PRESSED, NULL);
        lv_slider_set_value(slider, 600, LV_ANIM_OFF);
        lv_obj_send_event(slider, LV_EVENT_RELEASED, NULL);
        break;
    case 5:
        printf("[selftest] 点 Loop\n");
        lv_obj_send_event(loop_btn, LV_EVENT_CLICKED, NULL);
        break;
    case 7:
        printf("[selftest] 点 Pause\n");
        lv_obj_send_event(pause_btn, LV_EVENT_CLICKED, NULL);
        break;
    case 9:
        printf("[selftest] 再点 Pause（继续）\n");
        lv_obj_send_event(pause_btn, LV_EVENT_CLICKED, NULL);
        break;
    case 11:
        printf("[selftest] 点 Prev\n");
        lv_obj_send_event(prev_btn, LV_EVENT_CLICKED, NULL);
        break;
    case 13:
        printf("[selftest] 点画面（控制条应该已自动收起，这一步重新唤出）\n");
        lv_obj_send_event(tap_area, LV_EVENT_CLICKED, NULL);
        break;
    case 15:
        printf("[selftest] 点菜单，打开播放列表\n");
        lv_obj_send_event(menu_btn, LV_EVENT_CLICKED, NULL);
        break;
    case 17:
        printf("[selftest] 点列表第 2 行\n");
        if (playlist_count > 1 && list_rows[1])
            lv_obj_send_event(list_rows[1], LV_EVENT_CLICKED, NULL);
        break;
    case 19:
        printf("[selftest] 结束\n");
        lv_timer_delete(t);
        break;
    }
}

void ui_VideoPage_init(void)
{
    lv_obj_t *VideoPage = lv_obj_create(NULL);

    frames_shown = 0;
    player_error = 0;
    frame_ready  = 0;
    paused       = 0;
    cur_fps      = 0;
    req_switch   = -1;
    req_seek     = -1;
    cur_pos      = 0;
    cur_dur      = 0;
    user_seeking = 0;

    /* 底色：和音乐页一致的浅色（控制条收起时视频铺满整屏，基本看不到） */
    lv_obj_set_style_bg_color(VideoPage, lv_color_hex(0xFBFDFF), 0);
    lv_obj_remove_flag(VideoPage, LV_OBJ_FLAG_SCROLLABLE);

    /* 画布：整屏，直接喂 RGB565 */
    memset(video_canvas_buf, 0, sizeof(video_canvas_buf));
    video_canvas = lv_canvas_create(VideoPage);
    lv_canvas_set_buffer(video_canvas, video_canvas_buf,
                         UI_SCREEN_WIDTH, UI_SCREEN_HEIGHT, LV_COLOR_FORMAT_NATIVE);
    lv_obj_center(video_canvas);

    /* 透明点按层：点画面显示/收起控制条。它在 bar/card 下面，不影响按钮点击 */
    tap_area = lv_obj_create(VideoPage);
    lv_obj_set_size(tap_area, UI_SCREEN_WIDTH, UI_SCREEN_HEIGHT);
    lv_obj_set_pos(tap_area, 0, 0);
    lv_obj_set_style_bg_opa(tap_area, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(tap_area, 0, 0);
    lv_obj_set_style_pad_all(tap_area, 0, 0);
    lv_obj_remove_flag(tap_area, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(tap_area, ui_event_tap_area, LV_EVENT_CLICKED, NULL);

    /* ---------- 顶部：银白 bar（返回 / 文件名 / 第几集）---------- */
    ctrl_bar = lv_obj_create(VideoPage);
    lv_obj_set_size(ctrl_bar, UI_SCREEN_WIDTH, 32);
    lv_obj_set_pos(ctrl_bar, 0, 0);
    lv_obj_set_style_radius(ctrl_bar, 0, 0);
    lv_obj_set_style_bg_color(ctrl_bar, lv_color_hex(0xEFF2F6), 0);      /* 银白 */
    lv_obj_set_style_bg_opa(ctrl_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(ctrl_bar, 1, 0);
    lv_obj_set_style_border_side(ctrl_bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(ctrl_bar, lv_color_hex(0xDCE2EA), 0);
    lv_obj_set_style_pad_all(ctrl_bar, 0, 0);
    lv_obj_remove_flag(ctrl_bar, LV_OBJ_FLAG_SCROLLABLE);

    back_btn = lv_btn_create(ctrl_bar);
    lv_obj_set_size(back_btn, 44, 32);
    lv_obj_set_pos(back_btn, 0, 0);
    lv_obj_set_style_bg_opa(back_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(back_btn, 0, 0);
    lv_obj_set_style_shadow_width(back_btn, 0, 0);
    lv_obj_set_style_pad_all(back_btn, 0, 0);
    lv_obj_t *back_label = lv_label_create(back_btn);
    lv_label_set_text(back_label, LV_SYMBOL_LEFT);      /* 和音乐页一致的返回图标 */
    lv_obj_set_style_text_font(back_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(back_label, lv_color_hex(0x3A3F46), 0);
    lv_obj_align(back_label, LV_ALIGN_LEFT_MID, 10, 0);
    lv_obj_add_event_cb(back_btn, ui_event_back_btn, LV_EVENT_ALL, NULL);

    file_label = lv_label_create(ctrl_bar);             /* 曲名 = 文件名（去扩展名） */
    lv_label_set_text(file_label, "");
    /* ⚠️ 这里必须用带汉字的字体：montserrat 系列一个汉字都没有，
     * 中文文件名会整串变成方块 */
    lv_obj_set_style_text_font(file_label, &ui_font_app16, 0);
    lv_obj_set_style_text_color(file_label, lv_color_hex(0x1F242B), 0);
    lv_obj_set_width(file_label, 176);
    lv_label_set_long_mode(file_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(file_label, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_pos(file_label, 48, 8);

    status_label = lv_label_create(ctrl_bar);           /* 第几集 / 共几集（也用于报错） */
    lv_label_set_text(status_label, "- / -");
    lv_obj_set_style_text_font(status_label, &ui_font_app16, 0);
    lv_obj_set_style_text_color(status_label, lv_color_hex(0x8A9099), 0);
    lv_obj_align(status_label, LV_ALIGN_RIGHT_MID, -8, 0);

    /* ---------- 底部：浅色控制卡片（上一集/播放/下一集/循环 + 进度条 + 时间）---------- */
    ctrl_card = lv_obj_create(VideoPage);
    lv_obj_set_size(ctrl_card, 304, 68);
    lv_obj_set_pos(ctrl_card, 8, 166);
    lv_obj_set_style_radius(ctrl_card, 12, 0);
    lv_obj_set_style_bg_color(ctrl_card, lv_color_hex(0xFBFCFE), 0);
    lv_obj_set_style_bg_opa(ctrl_card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(ctrl_card, 1, 0);
    lv_obj_set_style_border_color(ctrl_card, lv_color_hex(0xE2E7EE), 0);
    lv_obj_set_style_pad_all(ctrl_card, 0, 0);
    lv_obj_remove_flag(ctrl_card, LV_OBJ_FLAG_SCROLLABLE);

    /* 5 个按钮以卡片中心（x=152）左右对称：↕菜单 · 上一集 · 播放 · 下一集 · 循环 */
    menu_btn = lv_btn_create(ctrl_card);
    lv_obj_set_size(menu_btn, 38, 30);
    lv_obj_set_pos(menu_btn, 21, 5);
    lv_obj_set_style_bg_opa(menu_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(menu_btn, 0, 0);
    lv_obj_set_style_shadow_width(menu_btn, 0, 0);
    lv_obj_set_style_pad_all(menu_btn, 0, 0);
    lv_obj_t *menu_label = lv_label_create(menu_btn);
    lv_label_set_text(menu_label, LV_SYMBOL_LIST);
    lv_obj_set_style_text_font(menu_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(menu_label, lv_color_hex(0x3A3F46), 0);
    lv_obj_center(menu_label);
    lv_obj_add_event_cb(menu_btn, ui_event_menu_btn, LV_EVENT_ALL, NULL);

    prev_btn = lv_btn_create(ctrl_card);
    lv_obj_set_size(prev_btn, 38, 30);
    lv_obj_set_pos(prev_btn, 77, 5);
    lv_obj_set_style_bg_opa(prev_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(prev_btn, 0, 0);
    lv_obj_set_style_shadow_width(prev_btn, 0, 0);
    lv_obj_set_style_pad_all(prev_btn, 0, 0);
    lv_obj_t *prev_label = lv_label_create(prev_btn);
    lv_label_set_text(prev_label, LV_SYMBOL_PREV);
    lv_obj_set_style_text_font(prev_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(prev_label, lv_color_hex(0x3A3F46), 0);
    lv_obj_center(prev_label);
    lv_obj_add_event_cb(prev_btn, ui_event_prev_btn, LV_EVENT_ALL, NULL);

    pause_btn = lv_btn_create(ctrl_card);               /* 圆形蓝色主按钮 */
    lv_obj_set_size(pause_btn, 40, 40);
    lv_obj_set_pos(pause_btn, 132, 0);
    lv_obj_set_style_radius(pause_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(pause_btn, lv_color_hex(0x2F7BEA), 0);
    lv_obj_set_style_bg_opa(pause_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(pause_btn, 0, 0);
    lv_obj_set_style_shadow_width(pause_btn, 0, 0);
    lv_obj_set_style_pad_all(pause_btn, 0, 0);
    pause_label = lv_label_create(pause_btn);
    lv_label_set_text(pause_label, LV_SYMBOL_PAUSE);
    lv_obj_set_style_text_font(pause_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(pause_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(pause_label);
    lv_obj_add_event_cb(pause_btn, ui_event_playpause_btn, LV_EVENT_ALL, NULL);

    next_btn = lv_btn_create(ctrl_card);
    lv_obj_set_size(next_btn, 38, 30);
    lv_obj_set_pos(next_btn, 189, 5);
    lv_obj_set_style_bg_opa(next_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(next_btn, 0, 0);
    lv_obj_set_style_shadow_width(next_btn, 0, 0);
    lv_obj_set_style_pad_all(next_btn, 0, 0);
    lv_obj_t *next_label = lv_label_create(next_btn);
    lv_label_set_text(next_label, LV_SYMBOL_NEXT);
    lv_obj_set_style_text_font(next_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(next_label, lv_color_hex(0x3A3F46), 0);
    lv_obj_center(next_label);
    lv_obj_add_event_cb(next_btn, ui_event_next_btn, LV_EVENT_ALL, NULL);

    loop_btn = lv_btn_create(ctrl_card);
    lv_obj_set_size(loop_btn, 38, 30);
    lv_obj_set_pos(loop_btn, 245, 5);
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

    /* 进度条：和音乐页同一套配色，旋钮小圆点 */
    slider = lv_slider_create(ctrl_card);
    lv_obj_set_size(slider, 280, 6);
    lv_obj_set_pos(slider, 12, 42);
    lv_slider_set_range(slider, 0, 1000);
    lv_slider_set_value(slider, 0, LV_ANIM_OFF);
    lv_obj_set_ext_click_area(slider, 8);               /* 手指容易点中 */
    lv_obj_set_style_bg_color(slider, lv_color_hex(0xD5DDE7), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, lv_color_hex(0x2F7BEA), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, lv_color_hex(0x2F7BEA), LV_PART_KNOB);
    lv_obj_set_style_pad_all(slider, 2, LV_PART_KNOB);
    lv_obj_set_style_shadow_width(slider, 0, LV_PART_KNOB);
    lv_obj_add_event_cb(slider, ui_event_slider, LV_EVENT_ALL, NULL);

    time_label = lv_label_create(ctrl_card);            /* 已播（左下） */
    lv_label_set_text(time_label, "0:00");
    lv_obj_set_style_text_font(time_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(time_label, lv_color_hex(0x8A9099), 0);
    lv_obj_set_pos(time_label, 12, 50);

    time_dur_label = lv_label_create(ctrl_card);        /* 总长（右下） */
    lv_label_set_text(time_dur_label, "0:00");
    lv_obj_set_style_text_font(time_dur_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(time_dur_label, lv_color_hex(0x8A9099), 0);
    lv_obj_align(time_dur_label, LV_ALIGN_BOTTOM_RIGHT, -12, -6);

    /* 刚进页面先显示控制条：把"最后操作时间"重置成现在，否则开机后从没摸过屏幕，
     * lv_display_get_inactive_time() 会很大 → 控制条一进来就被自动收起 */
    lv_display_trigger_activity(NULL);

    /* ---------- 播放列表浮层（全屏，默认隐藏，点菜单按钮打开）---------- */
    list_built = 0;
    list_job   = 0;
    list_job_timer = NULL;
    for (int i = 0; i < VIDEO_MAX_FILES; i++) {
        list_rows[i]         = NULL;
        list_title_labels[i] = NULL;
        list_sub_labels[i]   = NULL;
        list_badges[i]       = NULL;
        list_dur[i][0]       = 0;
        dur_loaded[i]        = 0;
    }

    list_overlay = lv_obj_create(VideoPage);
    lv_obj_set_size(list_overlay, UI_SCREEN_WIDTH, UI_SCREEN_HEIGHT);
    lv_obj_set_pos(list_overlay, 0, 0);
    lv_obj_set_style_radius(list_overlay, 0, 0);
    lv_obj_set_style_bg_color(list_overlay, lv_color_hex(0xF2F4F7), 0);
    lv_obj_set_style_bg_opa(list_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(list_overlay, 0, 0);
    lv_obj_set_style_pad_all(list_overlay, 0, 0);
    lv_obj_remove_flag(list_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(list_overlay, LV_OBJ_FLAG_HIDDEN);

    list_box = lv_obj_create(list_overlay);          /* 可滚动的列表 */
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

    /* 刷新定时器（UI 线程，30ms 一次足够） */
    video_timer = lv_timer_create(video_refresh_cb, 30, NULL);

    /* 决定播哪个文件：先看 ECHO_VIDEO，没设就按候选列表挨个找 */
    const char *env = getenv("ECHO_VIDEO");
    int found = 0;
    if (env && env[0]) {
        found = (ui_VideoPage_set_file(env) == 0);
        if (!found) printf("[VideoPage] ECHO_VIDEO=%s 里没找到视频\n", env);
    } else {
        for (unsigned i = 0; i < sizeof(video_search_paths) / sizeof(video_search_paths[0]); i++) {
            if (!probe_video_path(video_search_paths[i])) {
                printf("[VideoPage] %s 里没有视频\n", video_search_paths[i]);
                continue;
            }
            printf("[VideoPage] 自动找到视频：%s\n", video_search_paths[i]);
            found = (ui_VideoPage_set_file(video_search_paths[i]) == 0);
            break;
        }
    }
    if (!found) {
        printf("[VideoPage] 所有候选路径都没有视频\n");
        lv_label_set_text(status_label, "No video file");
        lv_scr_load_anim(VideoPage, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 100, 0, true);
        return;
    }
    if (!ui_video_is_supported()) {
        lv_label_set_text(status_label, "ffmpeg not built in");
        lv_scr_load_anim(VideoPage, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 100, 0, true);
        return;
    }

    /* 起播放线程 */
    player_run = 1;
    if (pthread_create(&player_tid, NULL, video_player_thread, NULL) != 0) {
        player_run = 0;
        lv_label_set_text(status_label, "pthread create failed");
        lv_scr_load_anim(VideoPage, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 100, 0, true);
        return;
    }

    /* 自检模式：ECHO_VIDEO_SELFTEST=1 时自动依次演练 Next / seek / Loop / Pause / Prev，
     * 只用看日志就能确认控件接线正常（板子上没屏时特别有用）*/
    const char *st = getenv("ECHO_VIDEO_SELFTEST");
    if (st && st[0] == '1') {
        printf("[VideoPage] 自检开始\n");
        lv_timer_create(selftest_cb, 2000, NULL);
    }

    const char *lsenv = getenv("ECHO_VIDEO_LIST");
    if (lsenv && lsenv[0] == '1') lv_timer_create(list_auto_cb, 1500, NULL);

    /* 挂上屏幕（本项目其它页面都是在自己 init 末尾做这一步）*/
    lv_scr_load_anim(VideoPage, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 100, 0, true);
}

void ui_VideoPage_deinit(void)
{
    player_run = 0;
    req_switch = -1;
    req_seek   = -1;
    user_seeking = 0;
    if (video_timer) { lv_timer_delete(video_timer); video_timer = NULL; }
    if (list_job_timer) { lv_timer_delete(list_job_timer); list_job_timer = NULL; }
    if (player_tid) {
        pthread_join(player_tid, NULL);
        player_tid = 0;
    }
    ui_audio_out_close();     /* 线程里已经关过一次，这里兜底 */
    player_error = 0;
    frame_ready  = 0;
    video_canvas = NULL;
    status_label = NULL;
    time_label   = NULL;
    time_dur_label = NULL;
    file_label   = NULL;
    slider       = NULL;
    pause_label  = NULL;
    loop_label   = NULL;
    back_btn     = NULL;
    prev_btn     = NULL;
    next_btn     = NULL;
    pause_btn    = NULL;
    loop_btn     = NULL;
    ctrl_bar     = NULL;
    ctrl_card    = NULL;
    tap_area     = NULL;
    ctrl_visible = 1;
    menu_btn     = NULL;
    list_overlay = NULL;
    list_box     = NULL;
    list_built   = 0;
    list_job     = 0;
    for (int i = 0; i < VIDEO_MAX_FILES; i++) {
        list_rows[i]         = NULL;
        list_title_labels[i] = NULL;
        list_sub_labels[i]   = NULL;
        list_badges[i]       = NULL;
    }
    printf("[VideoPage] deinit 完成，共显示 %d 帧\n", frames_shown);
}
