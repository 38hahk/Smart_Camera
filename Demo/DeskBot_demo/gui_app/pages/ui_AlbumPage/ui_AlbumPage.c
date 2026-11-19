/*
 * 相册 App
 *
 * 入口：桌面 «相册» 图标（ui_HomePage.c 的 ui_AlbumBtn → "AlbumPage"）
 *
 * 数据来源：SD 卡（S99 开机挂到 /mnt/media）里的照片目录
 *   依次尝试：$ECHO_ALBUM_DIR（调试用）→ /mnt/media/photos → /root/photos → /mnt/media
 *   只认 .jpg/.jpeg（相机 App 存的就是 JPEG），**按拍摄时间倒序**
 *   （优先用文件名里的 IMG_YYYYMMDD_HHMMSS，解析不出就用文件 mtime），
 *   最近拍的在最上面。
 *
 * 两个状态（同一页里切控件，不重建页面）：
 *   列表态：返回 / 相册 / N 张 + 缩略图网格（3 列，可上下滚动）
 *   查看态：返回 / 文件名 / i/N + 全屏照片（左右滑动切换上一张/下一张）
 *
 * ⚠️ 为什么自己解码，而不是直接 lv_image_set_src("A:/xxx.jpg")：
 *   1. lv_conf.h 里 LV_CACHE_DEF_SIZE = 0 → LVGL 不缓存解码结果，
 *      滚动/重绘时会**每帧重新解一次 JPEG**，A7 上直接卡死；
 *   2. 本版 LVGL 的 tjpgd 解码器是按 MCU 条带输出的（一次只给一小条），
 *      没有现成的「整图解码后缩放到任意尺寸」接口。
 *   所以这里用 lv_image_decoder 的 get_area 把条带拼成整图 RGB888，
 *   再自己最近邻缩放成 RGB565 缩略图（96x72）缓存起来；
 *   解码放在 60ms 的定时器里一张一张做，进页面不会卡住。
 */

#include "ui_AlbumPage.h"

/* ⚠️ 本版 LVGL（9.2）的 lv_image_decoder_dsc_t 只在 lv_types.h 里前向声明，
 *    结构体本体在私有头里；要用 lv_image_decoder_open/get_area 就必须包含它。
 *    这里用相对路径（gui_app 没把 lvgl 根目录加进 -I，都是靠 ui.h 里的 ../lvgl/ 相对引用） */
#include "../../lvgl/src/draw/lv_image_decoder_private.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <dirent.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>

///////////////////// 参数 ////////////////////

#define ALBUM_SCAN_MAX  64          /* 最多收集多少张（排完序再截到 ALBUM_MAX） */
#define ALBUM_MAX       24          /* 网格里最多显示多少张 */
#define ALBUM_PATH_LEN  192
#define THUMB_W         96
#define THUMB_H         72
#define VIEW_W          320
#define VIEW_H          240

///////////////////// 状态 ////////////////////

typedef struct {
    char path[ALBUM_PATH_LEN];          /* 真实路径（opendir/stat 用） */
    char lv_path[ALBUM_PATH_LEN + 4];   /* LVGL 路径：前面加 "A:" */
    long long sort_ts;                  /* 排序键：拍摄时间（秒） */
    uint16_t *thumb;                    /* THUMB_W*THUMB_H RGB565，解码后才有 */
    lv_obj_t *img;                      /* 缩略图控件 */
    lv_image_dsc_t dsc;
} album_item_t;

static album_item_t items[ALBUM_SCAN_MAX];
static int item_count = 0;
static int thumb_next = 0;              /* 下一个待解码的缩略图下标 */

static lv_obj_t *album_page;
static lv_obj_t *list_bar, *list_box, *list_hint, *list_count;
static lv_obj_t *view_bar, *view_img, *view_title, *view_hint;
static lv_obj_t *view_del_btn;          /* 查看态的删除按钮（自检用） */
static lv_obj_t *del_dlg_ok;            /* 确认框里的「删除」（自检用） */

static lv_timer_t *album_timer;
static lv_timer_t *album_selftest_timer;

static uint16_t *view_buf;              /* VIEW_W*VIEW_H RGB565 */
static lv_image_dsc_t view_dsc;
static int  view_index = -1;            /* -1 = 当前在列表态 */
static char album_dir_used[ALBUM_PATH_LEN];

/* 前置声明（这几个函数互相调用） */
static void album_update_count(void);
static void album_build_grid(void);
static void album_show_view(int idx);
static void album_show_list(void);

///////////////////// JPEG 解码（LVGL 解码器 → RGB888） ////////////////////

/* 把一个 JPEG 解成整张 RGB888（自己把 MCU 条带拼起来）；失败返回 NULL */
static uint8_t *album_decode_jpeg(const char *lv_path, int *out_w, int *out_h)
{
    lv_image_decoder_dsc_t dsc;

    if (lv_image_decoder_open(&dsc, lv_path, NULL) != LV_RESULT_OK) {
        return NULL;
    }

    int iw = (int)dsc.header.w;
    int ih = (int)dsc.header.h;
    if (iw <= 0 || ih <= 0 || iw > 4096 || ih > 4096) {
        lv_image_decoder_close(&dsc);
        return NULL;
    }

    uint8_t *buf = (uint8_t *)malloc((size_t)iw * ih * 3);
    if (buf == NULL) {
        lv_image_decoder_close(&dsc);
        return NULL;
    }

    lv_area_t full = { 0, 0, iw - 1, ih - 1 };
    lv_area_t area;
    lv_memzero(&area, sizeof(area));
    area.y1 = LV_COORD_MIN;             /* 首次调用按解码器约定这样初始化 */

    /* guard：万一解码器不推进区域，也别在这里死循环 */
    for (int guard = 0; guard < 65536; guard++) {
        if (lv_image_decoder_get_area(&dsc, &full, &area) != LV_RESULT_OK) break;

        int aw = area.x2 - area.x1 + 1;
        int ah = area.y2 - area.y1 + 1;
        if (aw > 0 && ah > 0 && dsc.decoded && dsc.decoded->data) {
            const uint8_t *src = (const uint8_t *)dsc.decoded->data;
            for (int y = 0; y < ah; y++) {
                int dy = area.y1 + y;
                if (dy < 0 || dy >= ih) continue;
                if (area.x1 < 0) continue;
                int cw = aw;
                if (area.x1 + cw > iw) cw = iw - area.x1;
                memcpy(buf + (size_t)dy * iw * 3 + (size_t)area.x1 * 3,
                       src + (size_t)y * aw * 3,
                       (size_t)cw * 3);
            }
        }
        if (area.y2 >= ih - 1) break;
    }

    lv_image_decoder_close(&dsc);

    *out_w = iw;
    *out_h = ih;
    return buf;
}

/* RGB888 → RGB565，等比缩放并居中（四周补黑） */
static void album_fit_rgb565(const uint8_t *src, int sw, int sh,
                             uint16_t *dst, int dw, int dh)
{
    int tw = dw, th = dh;
    if ((long)sw * dh > (long)sh * dw) th = (int)((long)sh * dw / sw);
    else                               tw = (int)((long)sw * dh / sh);
    if (tw < 1) tw = 1;
    if (th < 1) th = 1;
    int ox = (dw - tw) / 2;
    int oy = (dh - th) / 2;

    if (ox || oy || tw != dw || th != dh) {
        memset(dst, 0, (size_t)dw * dh * 2);
    }

    for (int y = 0; y < th; y++) {
        int sy = (int)((long)y * sh / th);
        const uint8_t *srow = src + (size_t)sy * sw * 3;
        uint16_t *drow = dst + (size_t)(y + oy) * dw;
        for (int x = 0; x < tw; x++) {
            int sx = (int)((long)x * sw / tw);
            const uint8_t *p = srow + (size_t)sx * 3;
            /* ⚠️ LVGL 的 tjpgd 解码器输出顺序是 B,G,R（源码 tjpgd.c 里逐字节写的是
             *   B、G、R，虽然它把 header.cf 标成了 RGB888）。按 RGB 打包会导致
             *   红蓝互换 —— 照片"红变蓝、黄变青"就是这个原因，预览不受影响。 */
            drow[x + ox] = (uint16_t)(((p[2] & 0xF8) << 8) |     /* R */
                                      ((p[1] & 0xFC) << 3) |     /* G */
                                       (p[0] >> 3));             /* B */
        }
    }
}

///////////////////// 目录扫描 ////////////////////

/* 从文件名里取拍摄时间：IMG_20261029_143012.jpg → epoch 秒；解析不出返回 0 */
static long long _album_name_time(const char *name)
{
    int Y, M, D, h, m, s;
    if (sscanf(name, "IMG_%4d%2d%2d_%2d%2d%2d", &Y, &M, &D, &h, &m, &s) != 6) return 0;
    if (Y < 2000 || Y > 2100 || M < 1 || M > 12 || D < 1 || D > 31) return 0;

    struct tm tmv;
    memset(&tmv, 0, sizeof(tmv));
    tmv.tm_year = Y - 1900;
    tmv.tm_mon  = M - 1;
    tmv.tm_mday = D;
    tmv.tm_hour = h;
    tmv.tm_min  = m;
    tmv.tm_sec  = s;
    tmv.tm_isdst = -1;
    return (long long)mktime(&tmv);
}

static int _item_cmp_desc(const void *a, const void *b)
{
    const album_item_t *x = (const album_item_t *)a;
    const album_item_t *y = (const album_item_t *)b;

    /* 拍摄时间倒序 = 最近拍的在最上面；同一秒的再按文件名倒序 */
    if (x->sort_ts != y->sort_ts) return (x->sort_ts < y->sort_ts) ? 1 : -1;
    return strcmp(y->path, x->path);
}

/* 同名文件是否已经收过了（SD 卡上的优先，内部存储的同名忽略） */
static bool _album_has_name(const char *name)
{
    for (int i = 0; i < item_count; i++) {
        const char *n = strrchr(items[i].path, '/');
        if (n && strcmp(n + 1, name) == 0) return true;
    }
    return false;
}

static int album_scan_dir(const char *dir)
{
    DIR *d = opendir(dir);
    if (d == NULL) return 0;

    int added = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && item_count < ALBUM_SCAN_MAX) {
        if (e->d_name[0] == '.') continue;

        const char *ext = strrchr(e->d_name, '.');
        if (ext == NULL) continue;
        if (strcasecmp(ext, ".jpg") != 0 && strcasecmp(ext, ".jpeg") != 0) continue;

        if (_album_has_name(e->d_name)) continue;

        album_item_t *it = &items[item_count];
        memset(it, 0, sizeof(*it));
        /* 用显式精度避免 -Wformat-truncation 警告（文件名最长 255 字节） */
        snprintf(it->path, sizeof(it->path), "%.*s/%.*s",
                 (int)(sizeof(it->path) / 2), dir,
                 (int)sizeof(e->d_name), e->d_name);
        snprintf(it->lv_path, sizeof(it->lv_path), "A:%s", it->path);

        /* 排序键：文件名里的拍摄时间优先，其次文件修改时间 */
        long long name_ts = _album_name_time(e->d_name);
        struct stat st;
        long long mtime_ts = (stat(it->path, &st) == 0) ? (long long)st.st_mtime : 0;
        it->sort_ts = (name_ts > 0) ? name_ts : mtime_ts;

        item_count++;
        added++;
    }
    closedir(d);

    return added;
}

/* 依次尝试几个目录，返回找到的照片数
 *
 * ⚠️ **两个 photos 目录都要扫**：相机存图时如果 SD 卡没挂上/不可写，会退到
 *    /root/photos；原来“找到第一个非空目录就停”会导致「新拍的照片看不见」。
 *    同名以先扫到的（SD 卡）为准。*/
static int album_scan(void)
{
    item_count = 0;
    thumb_next = 0;

    /* 调试用：ECHO_ALBUM_DIR=/tmp/albumtest */
    const char *env = getenv("ECHO_ALBUM_DIR");
    if (env && env[0]) {
        album_scan_dir(env);
        if (item_count == 0) {
            printf("[AlbumPage] %s 里没有 jpg\n", env);
            return 0;
        }
        snprintf(album_dir_used, sizeof(album_dir_used), "%s", env);
    } else {
        album_scan_dir("/mnt/media/photos");
        int sd_cnt = item_count;
        album_scan_dir("/root/photos");
        if (item_count == 0) album_scan_dir("/mnt/media");   /* 照片直接放卡根目录的情况 */
        printf("[AlbumPage] SD %d 张 + 内部 %d 张\n", sd_cnt, item_count - sd_cnt);
        snprintf(album_dir_used, sizeof(album_dir_used), "SD+内部");
    }

    if (item_count > 1) {
        qsort(items, item_count, sizeof(items[0]), _item_cmp_desc);
    }

    /* 先排好序再截断，保证「最新的 24 张」而不是「先扫到的 24 张」 */
    if (item_count > ALBUM_MAX) item_count = ALBUM_MAX;

    if (item_count > 0) {
        printf("[AlbumPage] 最新一张：%s（ts=%lld）\n",
               items[0].path, items[0].sort_ts);
    }
    return item_count;
}

///////////////////// 缩略图 ////////////////////

static void album_update_count(void)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%d 张", item_count);
    if (list_count) lv_label_set_text(list_count, buf);
}

static void album_make_thumb(int i)
{
    int w = 0, h = 0;
    uint8_t *rgb = NULL;

    /* 失败的再试一次（SD 卡偶尔读失败，重开一次解码器通常就好了） */
    for (int attempt = 0; attempt < 2 && rgb == NULL; attempt++) {
        rgb = album_decode_jpeg(items[i].lv_path, &w, &h);
        if (rgb == NULL) usleep(30 * 1000);
    }

    if (rgb == NULL) {
        struct stat st;
        long sz = (stat(items[i].path, &st) == 0) ? (long)st.st_size : -1;
        printf("[AlbumPage] 解码失败：%s（%ld 字节）\n", items[i].path, sz);
        return;
    }

    if (items[i].thumb == NULL) {
        items[i].thumb = (uint16_t *)malloc((size_t)THUMB_W * THUMB_H * 2);
    }
    if (items[i].thumb == NULL) {
        free(rgb);
        return;
    }

    album_fit_rgb565(rgb, w, h, items[i].thumb, THUMB_W, THUMB_H);
    free(rgb);

    items[i].dsc.header.w     = THUMB_W;
    items[i].dsc.header.h     = THUMB_H;
    items[i].dsc.header.cf    = LV_COLOR_FORMAT_RGB565;
    items[i].dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    items[i].dsc.data_size    = (uint32_t)THUMB_W * THUMB_H * 2;
    items[i].dsc.data         = (const uint8_t *)items[i].thumb;

    if (items[i].img) lv_image_set_src(items[i].img, &items[i].dsc);
}

///////////////////// 查看态 ////////////////////

static void album_show_view(int idx)
{
    if (idx < 0) idx = 0;
    if (idx >= item_count) idx = item_count - 1;
    if (idx < 0) return;

    view_index = idx;

    int w = 0, h = 0;
    uint8_t *rgb = album_decode_jpeg(items[idx].lv_path, &w, &h);
    if (rgb == NULL) {
        ui_msgbox_info("提示", "这张照片打不开");
        view_index = -1;
        return;
    }

    if (view_buf == NULL) {
        view_buf = (uint16_t *)malloc((size_t)VIEW_W * VIEW_H * 2);
    }
    if (view_buf) {
        album_fit_rgb565(rgb, w, h, view_buf, VIEW_W, VIEW_H);
        view_dsc.data = (const uint8_t *)view_buf;      /* ← 必须绑上，否则是空图 */
        lv_image_set_src(view_img, &view_dsc);
    }
    free(rgb);

    /* 标题只显示文件名 */
    const char *name = strrchr(items[idx].path, '/');
    lv_label_set_text(view_title, name ? name + 1 : items[idx].path);

    /* 序号放到下边的提示行，顶栏空间留给文件名和删除按钮 */
    char buf[64];
    snprintf(buf, sizeof(buf), "%d/%d  左右滑动切换", idx + 1, item_count);
    lv_label_set_text(view_hint, buf);

    lv_obj_add_flag(list_bar, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(list_box, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(list_hint, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(view_bar, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(view_img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(view_hint, LV_OBJ_FLAG_HIDDEN);
}

static void album_show_list(void)
{
    view_index = -1;

    lv_obj_remove_flag(list_bar, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(list_box, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(view_bar, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(view_img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(view_hint, LV_OBJ_FLAG_HIDDEN);

    if (item_count == 0) lv_obj_remove_flag(list_hint, LV_OBJ_FLAG_HIDDEN);
}

///////////////////// 事件 ////////////////////

static void album_back_cb(lv_event_t *e)
{
    (void)e;
    lv_lib_pm_OpenPrePage(&page_manager);
}

/* 图标区左上角的返回：查看态先回列表，列表态再回桌面 */
static void album_back_smart_cb(lv_event_t *e)
{
    (void)e;
    if (view_index >= 0) album_show_list();
    else                 lv_lib_pm_OpenPrePage(&page_manager);
}

static void album_thumb_click_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= item_count) return;
    album_show_view(idx);
}

static void album_prev_cb(lv_event_t *e)
{
    (void)e;
    if (view_index > 0) album_show_view(view_index - 1);
}

static void album_next_cb(lv_event_t *e)
{
    (void)e;
    if (view_index >= 0 && view_index < item_count - 1) album_show_view(view_index + 1);
}

static void album_gesture_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_GESTURE) return;

    lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());
    if (view_index >= 0) {
        if (dir == LV_DIR_LEFT)       album_next_cb(e);
        else if (dir == LV_DIR_RIGHT) album_prev_cb(e);
    } else {
        if (dir == LV_DIR_RIGHT) lv_lib_pm_OpenPrePage(&page_manager);
    }
}

///////////////////// 删除 ////////////////////

/* 删除当前查看的这张；删完把数组前移、重建网格、跳到下一张 */
static void album_delete_current(void)
{
    int idx = view_index;
    if (idx < 0 || idx >= item_count) return;

    printf("[AlbumPage] 删除：%s\n", items[idx].path);
    if (unlink(items[idx].path) != 0) {
        perror("[AlbumPage] unlink");
        ui_msgbox_info("删除失败", "文件删不掉(只读/卡 已拔)");
        return;
    }

    if (items[idx].thumb) {
        free(items[idx].thumb);
        items[idx].thumb = NULL;
    }
    for (int i = idx; i < item_count - 1; i++) {
        items[i] = items[i + 1];
    }
    memset(&items[item_count - 1], 0, sizeof(items[0]));
    item_count--;
    thumb_next = 0;                 /* 已解码的会跳过，没解码的继续补 */

    album_update_count();
    album_build_grid();

    if (item_count == 0) {
        album_show_list();
        lv_label_set_text(list_hint, "没有找到照片\n(插 SD 卡, 用相机拍几张)");
        lv_obj_remove_flag(list_hint, LV_OBJ_FLAG_HIDDEN);
    } else {
        album_show_view(idx >= item_count ? item_count - 1 : idx);
    }
}

static void _del_ok_cb(lv_event_t *e)
{
    lv_obj_t *mb = (lv_obj_t *)lv_event_get_user_data(e);
    if (mb) lv_msgbox_close_async(mb);
    album_delete_current();
}

static void _del_cancel_cb(lv_event_t *e)
{
    lv_obj_t *mb = (lv_obj_t *)lv_event_get_user_data(e);
    if (mb) lv_msgbox_close_async(mb);
}

static void album_delete_cb(lv_event_t *e)
{
    (void)e;
    if (view_index < 0 || view_index >= item_count) return;

    const char *name = strrchr(items[view_index].path, '/');
    name = name ? name + 1 : items[view_index].path;

    char text[128];
    /* ⚠️ 别用中文全角标点（ui_font_app16 只含 GB2312 一级汉字，不含「？」等） */
    snprintf(text, sizeof(text), "确定删除这张照片?\n%.100s", name);

    lv_obj_t *mb = lv_msgbox_create(NULL);
    lv_obj_t *t  = lv_msgbox_add_title(mb, "删除照片");
    lv_obj_t *x  = lv_msgbox_add_text(mb, text);
    lv_obj_set_style_text_font(t, &ui_font_app16, 0);
    lv_obj_set_style_text_font(x, &ui_font_app16, 0);

    lv_obj_t *btn_no = lv_msgbox_add_footer_button(mb, "取消");
    lv_obj_t *btn_ok = lv_msgbox_add_footer_button(mb, "删除");
    lv_obj_set_style_text_font(lv_obj_get_child(btn_no, 0), &ui_font_app16, 0);
    lv_obj_set_style_text_font(lv_obj_get_child(btn_ok, 0), &ui_font_app16, 0);
    lv_obj_set_style_bg_color(btn_ok, lv_color_hex(0xB83B5E), 0);
    del_dlg_ok = btn_ok;

    lv_obj_add_event_cb(btn_no, _del_cancel_cb, LV_EVENT_CLICKED, mb);
    lv_obj_add_event_cb(btn_ok, _del_ok_cb,     LV_EVENT_CLICKED, mb);
}

/* 用当前 items[] 重建网格（初始化 / 删除后都用它） */
static void album_build_grid(void)
{
    if (list_box == NULL) return;
    lv_obj_clean(list_box);

    for (int i = 0; i < item_count; i++) {
        lv_obj_t *cell = lv_button_create(list_box);
        lv_obj_set_size(cell, THUMB_W, THUMB_H);
        lv_obj_set_style_radius(cell, 6, 0);
        lv_obj_set_style_bg_color(cell, lv_color_hex(0x1E2836), 0);
        lv_obj_set_style_border_opa(cell, 0, 0);
        lv_obj_set_style_pad_all(cell, 0, 0);
        lv_obj_remove_flag(cell, LV_OBJ_FLAG_SCROLLABLE);

        items[i].img = lv_image_create(cell);
        lv_obj_set_size(items[i].img, THUMB_W, THUMB_H);
        lv_obj_center(items[i].img);

        if (items[i].thumb) {                       /* 已经解码过的直接用 */
            lv_image_set_src(items[i].img, &items[i].dsc);
        }

        lv_obj_add_event_cb(cell, album_thumb_click_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);
    }
}

///////////////////// 定时器 ////////////////////

static void album_tick_cb(lv_timer_t *t)
{
    (void)t;

    /* 缩略图一张一张解码，避免进页面时长时间卡顿 */
    if (thumb_next < item_count) {
        if (items[thumb_next].thumb == NULL) {
            album_make_thumb(thumb_next);
        }
        thumb_next++;

        if (thumb_next >= item_count && list_hint) {
            lv_obj_add_flag(list_hint, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

///////////////////// 自检（无屏/仿真用） ////////////////////

static void album_selftest_cb(lv_timer_t *t)
{
    static int step = 0;
    switch (step) {
    case 0:
        printf("[selftest] 打开第 1 张\n");
        if (item_count > 0) album_show_view(0);
        break;
    case 1:
        printf("[selftest] 点删除 -> 确认框\n");
        if (view_del_btn) lv_obj_send_event(view_del_btn, LV_EVENT_CLICKED, NULL);
        break;
    case 2:
        printf("[selftest] 确认删除\n");
        if (del_dlg_ok) lv_obj_send_event(del_dlg_ok, LV_EVENT_CLICKED, NULL);
        break;
    case 3:
        printf("[selftest] 下一张\n");
        album_next_cb(NULL);
        break;
    case 4:
        printf("[selftest] 返回列表（剩 %d 张）\n", item_count);
        album_show_list();
        break;
    case 5:
        printf("[selftest] 结束\n");
        lv_timer_delete(t);
        album_selftest_timer = NULL;
        break;
    }
    step++;
}

///////////////////// 小工具 ////////////////////

static lv_obj_t *album_top_bar(lv_obj_t *parent)
{
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_set_size(bar, 320, 34);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 3);
    lv_obj_set_style_bg_opa(bar, 0, 0);
    lv_obj_set_style_border_opa(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    return bar;
}

static lv_obj_t *album_top_btn(lv_obj_t *bar, lv_align_t align, lv_coord_t x,
                               const char *txt, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_button_create(bar);
    lv_obj_set_size(btn, 58, 28);
    lv_obj_align(btn, align, x, 0);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x1E2836), 0);
    lv_obj_t *l = lv_label_create(btn);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, &ui_font_app16, 0);
    lv_obj_center(l);
    if (cb) lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    return btn;
}

///////////////////// 页面 ////////////////////

void ui_AlbumPage_init(void)
{
    view_index = -1;

    album_page = lv_obj_create(NULL);
    lv_obj_remove_flag(album_page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(album_page, lv_color_hex(0x101418), 0);

    /* ---------- 列表态顶栏 ---------- */
    list_bar = album_top_bar(album_page);
    album_top_btn(list_bar, LV_ALIGN_LEFT_MID, 3, "返回", album_back_cb);

    lv_obj_t *title = lv_label_create(list_bar);
    lv_label_set_text(title, "相册");
    lv_obj_set_style_text_font(title, &ui_font_app16, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, 0);

    list_count = lv_label_create(list_bar);
    lv_label_set_text(list_count, "0 张");
    lv_obj_set_style_text_font(list_count, &ui_font_app16, 0);
    lv_obj_set_style_text_color(list_count, lv_color_hex(0x8FA4B4), 0);
    lv_obj_align(list_count, LV_ALIGN_RIGHT_MID, -6, 0);

    /* ---------- 缩略图网格 ---------- */
    list_box = lv_obj_create(album_page);
    lv_obj_set_size(list_box, 316, 176);
    lv_obj_align(list_box, LV_ALIGN_TOP_MID, 0, 58);
    lv_obj_set_style_bg_opa(list_box, 0, 0);
    lv_obj_set_style_border_opa(list_box, 0, 0);
    lv_obj_set_style_pad_all(list_box, 2, 0);
    lv_obj_set_style_pad_row(list_box, 6, 0);
    lv_obj_set_style_pad_column(list_box, 6, 0);
    lv_obj_set_flex_flow(list_box, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_scroll_dir(list_box, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list_box, LV_SCROLLBAR_MODE_ACTIVE);

    list_hint = lv_label_create(album_page);
    lv_label_set_text(list_hint, "正在读取...");
    lv_obj_set_style_text_font(list_hint, &ui_font_app16, 0);
    lv_obj_set_style_text_color(list_hint, lv_color_hex(0x8FA4B4), 0);
    lv_obj_set_style_text_align(list_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(list_hint, LV_ALIGN_CENTER, 0, 20);

    /* ---------- 查看态：全屏照片 ---------- */
    view_img = lv_image_create(album_page);
    lv_obj_set_pos(view_img, 0, 0);
    lv_obj_set_size(view_img, VIEW_W, VIEW_H);
    lv_obj_add_flag(view_img, LV_OBJ_FLAG_HIDDEN);

    view_bar = album_top_bar(album_page);
    lv_obj_add_flag(view_bar, LV_OBJ_FLAG_HIDDEN);
    album_top_btn(view_bar, LV_ALIGN_LEFT_MID, 3, "返回", album_back_smart_cb);

    view_title = lv_label_create(view_bar);
    lv_obj_set_width(view_title, 160);
    lv_obj_set_style_text_align(view_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(view_title, LV_LABEL_LONG_DOT);
    lv_label_set_text(view_title, "");
    lv_obj_set_style_text_font(view_title, &ui_font_app16, 0);
    lv_obj_set_style_text_color(view_title, lv_color_white(), 0);
    lv_obj_align(view_title, LV_ALIGN_CENTER, 0, 0);

    /* 删除按钮（红）：删当前这张 */
    view_del_btn = album_top_btn(view_bar, LV_ALIGN_RIGHT_MID, -2, "删除",
                                 album_delete_cb);
    lv_obj_set_style_bg_color(view_del_btn, lv_color_hex(0xB83B5E), 0);

    view_hint = lv_label_create(album_page);
    lv_label_set_text(view_hint, "左右滑动切换");
    lv_obj_set_style_text_font(view_hint, &ui_font_app16, 0);
    lv_obj_set_style_text_color(view_hint, lv_color_white(), 0);
    lv_obj_set_style_bg_color(view_hint, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(view_hint, 110, 0);
    lv_obj_set_style_radius(view_hint, 8, 0);
    lv_obj_set_style_pad_all(view_hint, 4, 0);
    lv_obj_align(view_hint, LV_ALIGN_BOTTOM_MID, 0, -6);
    lv_obj_add_flag(view_hint, LV_OBJ_FLAG_HIDDEN);

    /* 查看态图片压在顶栏下面，所以要在 view_bar 之后创建图片层？
       → 不行，那样顶栏会被盖住。这里把图片先创建，顶栏后创建 = 顶栏在上 ✓ */

    view_dsc.header.w     = VIEW_W;
    view_dsc.header.h     = VIEW_H;
    view_dsc.header.cf    = LV_COLOR_FORMAT_RGB565;
    view_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    view_dsc.data_size    = (uint32_t)VIEW_W * VIEW_H * 2;
    view_dsc.data         = NULL;

    /* ---------- 扫描 + 建网格 ---------- */
    int n = album_scan();
    album_update_count();

    if (n == 0) {
        lv_label_set_text(list_hint, "没有找到照片\n(插 SD 卡, 用相机拍几张)");
    } else {
        lv_obj_add_flag(list_hint, LV_OBJ_FLAG_HIDDEN);
        album_build_grid();
        printf("[AlbumPage] 从 %s 找到 %d 张照片\n", album_dir_used, n);
    }

    lv_obj_add_event_cb(album_page, album_gesture_cb, LV_EVENT_GESTURE, NULL);

    album_timer = lv_timer_create(album_tick_cb, 60, NULL);

    const char *st = getenv("ECHO_ALBUM_SELFTEST");
    if (st && st[0] == '1' && n > 0) {
        printf("[AlbumPage] 自检开始\n");
        album_selftest_timer = lv_timer_create(album_selftest_cb, 2500, NULL);
    }

    printf("[AlbumPage] 进入相册页\n");

    /* ⚠️ 页面管理器不会帮你切屏，每个页面 init 末尾都要自己 load */
    lv_scr_load_anim(album_page, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 100, 0, true);
}

void ui_AlbumPage_deinit(void)
{
    if (album_timer) {
        lv_timer_delete(album_timer);
        album_timer = NULL;
    }
    if (album_selftest_timer) {
        lv_timer_delete(album_selftest_timer);
        album_selftest_timer = NULL;
    }

    for (int i = 0; i < ALBUM_SCAN_MAX; i++) {
        if (items[i].thumb) {
            free(items[i].thumb);
            items[i].thumb = NULL;
        }
        items[i].img = NULL;
        memset(&items[i].dsc, 0, sizeof(items[i].dsc));
    }
    if (view_buf) {
        free(view_buf);
        view_buf = NULL;
    }
    view_dsc.data = NULL;

    item_count = 0;
    thumb_next = 0;
    view_index = -1;

    album_page  = NULL;
    list_bar    = NULL;
    list_box    = NULL;
    list_hint   = NULL;
    list_count  = NULL;
    view_bar    = NULL;
    view_img    = NULL;
    view_title  = NULL;
    view_hint   = NULL;

    printf("[AlbumPage] deinit 完成\n");
}
