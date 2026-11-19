/*
 * 相机 App
 *
 * 入口：桌面 «相机» 图标（ui_HomePage.c 的 ui_CameraBtn → "CameraPage"）
 *
 * 操作（板端）：
 *   旋转编码器  → 数字变焦 1.0x ~ 5.0x（0.5 一档）
 *   按下编码器  → 拍照，存进 SD 卡 /mnt/media/photos/IMG_YYYYMMDD_HHMMSS.jpg
 *                 （卡没插/没挂上时退回 /root/photos）
 *   返回按钮 / 右滑 → 退出
 *
 * 编码器接线见 common/encoder_manager/encoder_manager.h：
 *   A=GPIO1_B0(H1 pin8)、B=GPIO1_B1(pin6)、SW=GPIO1_A2(pin10)
 *
 * ⚠️ 线程模型：
 *   - 采集线程在 cam_capture.cc 里，负责 cap>>frame；
 *   - 编码器线程的回调**只改 volatile 标志**，LVGL 这边 33ms 定时器去消费；
 *   - 相机打开要 1~5 秒（要试分辨率），也放后台线程，页面不会卡住。
 *
 * ⚠️ 相机只在板端有：x86 仿真时 cam_start 直接失败，页面显示提示，UI 照样能截图验证。
 */

#include "ui_CameraPage.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/statvfs.h>

#include "../../../common/encoder_manager/encoder_manager.h"

#if LV_USE_SIMULATOR == 0
    #include "../../../../yolov5_demo/cpp/cam_capture.h"
#else
    /* 仿真机没有摄像头：给一套空实现，UI 仍然能跑 */
    static int  cam_start(int w, int h) { (void)w; (void)h; return -1; }
    static void cam_stop(void) { }
    static int  cam_get_preview(uint8_t *p, int w, int h, float z)
    { (void)p; (void)w; (void)h; (void)z; return -1; }
    static int  cam_save_jpeg(const char *p, float z, int m)
    { (void)p; (void)z; (void)m; return -1; }
#endif

///////////////////// 参数 ////////////////////

#define CAM_W            320        //摄像头原始输出宽度
#define CAM_H            240        //摄像头原始输出高度
#define CAM_ZOOM_MIN     1.0f       //最小变焦倍数
#define CAM_ZOOM_MAX     5.0f       //最大变焦倍数
#define CAM_ZOOM_STEP    0.5f       //每次变焦增长倍数
#define CAM_MIN_PHOTO_W  640      /* 变焦后区域太小就放大到这个宽度再存 */

#define CAM_STATE_IDLE     0      /* 没开始 */
#define CAM_STATE_STARTING 1      /* 正在打开 */
#define CAM_STATE_READY    2      /* 有画面了 */
#define CAM_STATE_FAILED   3      /* 打不开 */

///////////////////// 状态 ////////////////////

static lv_obj_t *cam_page;          //相机页面根容器
static lv_obj_t *cam_img;           //实时预览图像控件
static lv_obj_t *cam_zoom_label;    //变焦倍数文本
static lv_obj_t *cam_hint;          //常驻提示次
static lv_obj_t *cam_toast;         //提示外框（里面装 label）
static lv_obj_t *cam_flash;         //闪光灯开关按钮
static lv_obj_t *cam_back_btn;      //退出按钮

static lv_timer_t *cam_timer;       //摄像头定时器
static uint8_t *cam_buf;           /* RGB565 预览缓冲（只分配一次，页面反复进出复用） */
//预览画面数据结构体
static lv_image_dsc_t cam_dsc = {
    .header.w  = CAM_W,
    .header.h  = CAM_H,
    .data_size = CAM_W * CAM_H * 2,
    .header.cf = LV_COLOR_FORMAT_RGB565,   //颜色格式
    .data      = NULL,
};

static float cam_zoom = CAM_ZOOM_MIN; //初始化变焦倍数为最小倍数

/* 编码器线程 → LVGL 线程的通信（只允许 volatile 标志） */
static volatile int cam_rot_acc  = 0;   //旋转编码器累加值
static volatile int cam_shot_req = 0;   //拍照请求标志

static volatile int cam_start_state = CAM_STATE_IDLE;   //相机启动状态
static volatile int cam_cancel      = 0;   /* 页面已退出，启动完就立刻关掉相机 */

static int      cam_ui_state    = -1;   //UI本地状态
static uint32_t cam_toast_until = 0;    //Toast到期时间戳
static uint32_t cam_flash_until = 0;    //闪光灯保持时间戳

///////////////////// 编码器回调（在编码器线程里执行，不能碰 LVGL） ////////////////////

//编码器旋转 旋转编码器变焦标志位 累加 
static void _enc_on_rotate(int dir)
{
    cam_rot_acc += dir;
}

//编码器按下 旋转编码器按下标志位置为1
static void _enc_on_press(void)
{
    cam_shot_req = 1;
}

///////////////////// 拍照 ////////////////////

//测试xx路径有没有写权限
static bool _dir_writable(const char *p)
{
    return access(p, W_OK) == 0;
}

//设置照片储存地址
static const char *_photo_dir(void)
{
    static char dir[64];

    /* S99 开机脚本会把 SD 卡挂到 /mnt/media；卡不在就退回内部存储 */
    if (_dir_writable("/mnt/media"))    //如果sd卡被挂在到/mnt/media，并且有写权限
    {
        snprintf(dir, sizeof(dir), "/mnt/media/photos");   //将/mnt/media/photos这个路径写入dir中
        mkdir(dir, 0777);   //创建dir文件、可读可写可执行
        if (_dir_writable(dir)) return dir;
    }
    snprintf(dir, sizeof(dir), "/root/photos");
    mkdir(dir, 0777);
    return dir;
}

//设置xx提示函数
static void _toast(const char *text)
{
    if (cam_toast == NULL) return;
    lv_obj_t *label = lv_obj_get_child(cam_toast, 0);
    if (label) lv_label_set_text(label, text);
    lv_obj_remove_flag(cam_toast, LV_OBJ_FLAG_HIDDEN);
    cam_toast_until = lv_tick_get() + 2500;
}

//
static void _take_photo(void)
{
    char path[160];
    char name[64];
    time_t now = time(NULL);            //定义时间变量
    struct tm *lt = localtime(&now);    //将当前时间存入now，并用指针指向now

    //如果lt不为空，就将lt指向的时间结构体中的数据拼接成照片名 存入name中
    if (lt) strftime(name, sizeof(name), "IMG_%Y%m%d_%H%M%S.jpg", lt);
    else    snprintf(name, sizeof(name), "IMG_%ld.jpg", (long)now);

    //获取设置好的照片的存储路径
    const char *dir = _photo_dir();
    snprintf(path, sizeof(path), "%s/%s", dir, name);   //将路径和照片名拼接存入path中

    /* SD 卡满了要直接说清楚，别等到写一半失败 */
    struct statvfs vfs;
    if (statvfs(dir, &vfs) == 0)    //POSIX 接口，查询文件系统（SD 卡）容量信息，存入vfs中
    {
        unsigned long long free_b = (unsigned long long)vfs.f_bavail * vfs.f_frsize;
        if (free_b < 256ULL * 1024)    //如果容量小于256kb，就提示储存空间不足
        {
            printf("[CameraPage] %s 剩余空间不足：%llu 字节\n", dir, free_b);
            _toast("存储空间不足，请清理 SD 卡");
            return;
        }
    }

    /* 如果闪光灯打开，快门闪白 */
    if (cam_flash) {
        lv_obj_set_style_bg_opa(cam_flash, 190, 0);     //将背景透明度设为190
        cam_flash_until = lv_tick_get() + 90;           //UI保持点亮90ms
    }

    int rc = cam_save_jpeg(path, cam_zoom, CAM_MIN_PHOTO_W);    //照片保存入口函数
    if (rc == 0) {
        printf("[CameraPage] 拍照成功: %s\n", path);
        char msg[96];
        snprintf(msg, sizeof(msg), "已保存 %s", name);
        _toast(msg);
    } else {
        printf("[CameraPage] 拍照失败 rc=%d\n", rc);
        _toast("保存失败(检查 SD 卡)");
    }
}

///////////////////// 定时器 ////////////////////

//根据状态设置不同常驻提示词
static void _update_hint_for_state(void)
{
    if (cam_hint == NULL) return;

    switch (cam_start_state) {
    case CAM_STATE_STARTING:
        lv_label_set_text(cam_hint, "相机启动中...");
        break;
    case CAM_STATE_FAILED:
        lv_label_set_text(cam_hint, "相机不可用(仅开发板支持)");
        break;
    case CAM_STATE_READY:
        if (encoder_available()) lv_label_set_text(cam_hint, "旋转变焦  按下拍照");
        else                     lv_label_set_text(cam_hint, "编码器未接 无法变焦/拍照");
        break;
    default:
        break;
    }
}

//UI相机界面定时回调
static void _camera_tick(lv_timer_t *t)
{
    (void)t;

    /*如果ui层相机状态和相机状态不同。更新UI提示词*/
    if (cam_start_state != cam_ui_state) {
        cam_ui_state = cam_start_state;
        _update_hint_for_state();
    }

    /* 1. 编码器：旋转 → 变焦 */
    int rot = cam_rot_acc;
    if (rot != 0)   //如果编码器旋转标志位发生变化
    {
        cam_rot_acc = 0;    //将标志位置为0
        cam_zoom += CAM_ZOOM_STEP * rot;    //计算出变化的焦距
        if (cam_zoom < CAM_ZOOM_MIN) cam_zoom = CAM_ZOOM_MIN;
        if (cam_zoom > CAM_ZOOM_MAX) cam_zoom = CAM_ZOOM_MAX;
        if (cam_zoom_label)     //显示变化后的焦距
        {
            char buf[16];
            snprintf(buf, sizeof(buf), "%.1fx", cam_zoom);
            lv_label_set_text(cam_zoom_label, buf);
        }
    }

    /* 2. 编码器：按下 → 拍照 */
    if (cam_shot_req) {
        cam_shot_req = 0;
        if (cam_start_state == CAM_STATE_READY) _take_photo();
        else                                    _toast("相机还没准备好");
    }

    /* 3. 快门闪白衰减 / 提示自动消失 */
    if (cam_flash && cam_flash_until && lv_tick_get() > cam_flash_until) {
        lv_obj_set_style_bg_opa(cam_flash, 0, 0);
        cam_flash_until = 0;
    }
    if (cam_toast && cam_toast_until && lv_tick_get() > cam_toast_until) {
        lv_obj_add_flag(cam_toast, LV_OBJ_FLAG_HIDDEN);
        cam_toast_until = 0;
    }

    /* 4. 刷新预览 */
    if (cam_start_state == CAM_STATE_READY && cam_buf && cam_img) {
        if (cam_get_preview(cam_buf, CAM_W, CAM_H, cam_zoom) == 0) {
            lv_obj_invalidate(cam_img);
        }
    }
}

///////////////////// 相机启动线程 ////////////////////

static void *_cam_start_thread(void *arg)
{
    (void)arg;
    /* 640x480：本板（SC3336 只有 2304x1296 原生模式）唯一验证可靠的采集尺寸。
       请求 1280x720 会协商失败但 opencv 仍按 1280 建 Mat → 画面显示成"两份"。
       想试别的尺寸可以在启动前设 ECHO_CAM_SIZE=1280x720。 */
    int rc = cam_start(640, 480);
    if (cam_cancel) {
        if (rc == 0) cam_stop();            /* 页面已经退出，别占着摄像头 */
        cam_start_state = CAM_STATE_IDLE;
        return NULL;
    }
    cam_start_state = (rc == 0) ? CAM_STATE_READY : CAM_STATE_FAILED;
    printf("[CameraPage] 相机%s\n", rc == 0 ? "就绪" : "启动失败");
    return NULL;
}

///////////////////// 事件 ////////////////////

static void _back_cb(lv_event_t *e)
{
    (void)e;
    lv_lib_pm_OpenPrePage(&page_manager);
}

static void _gesture_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_GESTURE) return;
    if (lv_indev_get_gesture_dir(lv_indev_active()) == LV_DIR_RIGHT) {
        lv_lib_pm_OpenPrePage(&page_manager);
    }
}

///////////////////// 小工具 ////////////////////

/* 半透明黑色圆角小底板 */
static lv_obj_t *_pill(lv_obj_t *parent)
{
    lv_obj_t *p = lv_obj_create(parent);
    lv_obj_set_size(p, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(p, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(p, 130, 0);
    lv_obj_set_style_radius(p, 8, 0);
    lv_obj_set_style_border_opa(p, 0, 0);
    lv_obj_set_style_pad_all(p, 4, 0);
    lv_obj_remove_flag(p, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return p;
}

///////////////////// 自检（无屏/无编码器时验证用） ////////////////////

static lv_timer_t *cam_selftest_timer;

static void _selftest_cb(lv_timer_t *t)
{
    static int step = 0;
    switch (step) {
    case 0: printf("[selftest] 顺时针 2 格 -> 期望 2.0x\n");  cam_rot_acc = +2;  break;
    case 1: printf("[selftest] 再转 8 格 -> 期望卡在 5.0x\n"); cam_rot_acc = +8;  break;
    case 2: printf("[selftest] 按下编码器 -> 拍照\n");          cam_shot_req = 1;  break;
    case 3: printf("[selftest] 逆时针 10 格 -> 期望卡在 1.0x\n"); cam_rot_acc = -10; break;
    case 4:
        printf("[selftest] 结束\n");
        lv_timer_delete(t);
        cam_selftest_timer = NULL;
        break;
    }
    step++;
}

///////////////////// 页面 ////////////////////

void ui_CameraPage_init(void)
{
    cam_zoom        = CAM_ZOOM_MIN;
    cam_rot_acc     = 0;
    cam_shot_req    = 0;
    cam_cancel      = 0;
    cam_ui_state    = -1;
    cam_toast_until = 0;
    cam_flash_until = 0;

    cam_page = lv_obj_create(NULL);
    lv_obj_remove_flag(cam_page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(cam_page, lv_color_black(), 0);

    /* ---------- 预览画面（缓冲只分配一次，反复进出页面复用） ---------- */
    if (cam_buf == NULL) {
        cam_buf = (uint8_t *)malloc(CAM_W * CAM_H * 2);
        if (cam_buf) memset(cam_buf, 0, CAM_W * CAM_H * 2);
    }
    cam_dsc.data = cam_buf;

    cam_img = lv_img_create(cam_page);
    lv_obj_set_pos(cam_img, 0, 0);
    lv_obj_set_size(cam_img, CAM_W, CAM_H);
    if (cam_buf) lv_img_set_src(cam_img, &cam_dsc);

    /* ---------- 顶部：返回 + 变焦倍数 ---------- */
    lv_obj_t *top = lv_obj_create(cam_page);
    lv_obj_set_size(top, 320, 34);
    lv_obj_align(top, LV_ALIGN_TOP_MID, 0, 4);
    lv_obj_set_style_bg_opa(top, 0, 0);
    lv_obj_set_style_border_opa(top, 0, 0);
    lv_obj_set_style_pad_all(top, 0, 0);
    lv_obj_remove_flag(top, LV_OBJ_FLAG_SCROLLABLE);

    cam_back_btn = lv_button_create(top);
    lv_obj_set_size(cam_back_btn, 58, 28);
    lv_obj_align(cam_back_btn, LV_ALIGN_LEFT_MID, 2, 0);
    lv_obj_set_style_radius(cam_back_btn, 8, 0);
    lv_obj_set_style_bg_color(cam_back_btn, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(cam_back_btn, 130, 0);
    lv_obj_t *bl = lv_label_create(cam_back_btn);
    lv_label_set_text(bl, "返回");
    lv_obj_set_style_text_font(bl, &ui_font_app16, 0);
    lv_obj_center(bl);
    lv_obj_add_event_cb(cam_back_btn, _back_cb, LV_EVENT_CLICKED, NULL);

    cam_zoom_label = lv_label_create(top);
    lv_label_set_text(cam_zoom_label, "1.0x");
    lv_obj_set_style_text_font(cam_zoom_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(cam_zoom_label, lv_color_white(), 0);
    lv_obj_set_style_bg_color(cam_zoom_label, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(cam_zoom_label, 130, 0);
    lv_obj_set_style_radius(cam_zoom_label, 8, 0);
    lv_obj_set_style_pad_hor(cam_zoom_label, 6, 0);
    lv_obj_set_style_pad_ver(cam_zoom_label, 2, 0);
    lv_obj_align(cam_zoom_label, LV_ALIGN_RIGHT_MID, -4, 0);

    /* ---------- 底部操作提示 ---------- */
    lv_obj_t *hpill = _pill(cam_page);
    lv_obj_align(hpill, LV_ALIGN_BOTTOM_MID, 0, -6);
    cam_hint = lv_label_create(hpill);
    lv_label_set_text(cam_hint, "相机启动中...");
    lv_obj_set_style_text_font(cam_hint, &ui_font_app16, 0);
    lv_obj_set_style_text_color(cam_hint, lv_color_white(), 0);

    /* ---------- 保存提示（默认隐藏） ---------- */
    cam_toast = _pill(cam_page);
    lv_obj_align(cam_toast, LV_ALIGN_CENTER, 0, 40);
    lv_obj_add_flag(cam_toast, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *tl = lv_label_create(cam_toast);
    lv_label_set_text(tl, "");
    lv_obj_set_style_text_font(tl, &ui_font_app16, 0);
    lv_obj_set_style_text_color(tl, lv_color_hex(0x8CFFB0), 0);

    /* ---------- 快门闪白（最后创建 = 盖在最上层） ---------- */
    cam_flash = lv_obj_create(cam_page);
    lv_obj_set_size(cam_flash, CAM_W, CAM_H);
    lv_obj_set_pos(cam_flash, 0, 0);
    lv_obj_set_style_bg_color(cam_flash, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(cam_flash, 0, 0);
    lv_obj_set_style_border_opa(cam_flash, 0, 0);
    lv_obj_remove_flag(cam_flash, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    /* 右滑返回（触摸屏也能退） */
    lv_obj_add_event_cb(cam_page, _gesture_cb, LV_EVENT_GESTURE, NULL);

    /* ---------- 编码器 ---------- */
    if (encoder_init(_enc_on_rotate, _enc_on_press)) {
        printf("[CameraPage] 编码器已就绪（旋转=变焦，按下=拍照）\n");
    } else {
        printf("[CameraPage] 编码器不可用（未接线 / 引脚被占用 / 仿真）\n");
    }

    /* ---------- 相机（后台打开，避免卡住 UI） ---------- */
    cam_start_state = CAM_STATE_STARTING;
    pthread_t th;
    if (pthread_create(&th, NULL, _cam_start_thread, NULL) == 0) {
        pthread_detach(th);
    } else {
        cam_start_state = CAM_STATE_FAILED;
    }

    cam_timer = lv_timer_create(_camera_tick, 33, NULL);

    const char *st = getenv("ECHO_CAM_SELFTEST");
    if (st && st[0] == '1') {
        printf("[CameraPage] 自检开始\n");
        cam_selftest_timer = lv_timer_create(_selftest_cb, 1200, NULL);
    }

    printf("[CameraPage] 进入相机页\n");

    /* ⚠️ 页面管理器不会帮你切屏，每个页面 init 末尾都要自己 load */
    lv_scr_load_anim(cam_page, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 100, 0, true);
}

void ui_CameraPage_deinit(void)
{
    /* 定时器先删，之后不会再有任何回调碰界面 */
    if (cam_timer) {
        lv_timer_delete(cam_timer);
        cam_timer = NULL;
    }
    if (cam_selftest_timer) {
        lv_timer_delete(cam_selftest_timer);
        cam_selftest_timer = NULL;
    }

    encoder_deinit();

    /* 相机启动线程可能还在跑：标记取消，由它自己收尾；
       已经就绪的直接关掉（不然 YOLO 页会打不开摄像头） */
    cam_cancel = 1;
    if (cam_start_state == CAM_STATE_READY) cam_stop();
    cam_start_state = CAM_STATE_IDLE;

    /* ⚠️ cam_buf 不 free：旧屏幕要等新页面动画播完才被 LVGL 删掉，
       这段时间里 lv_img 还可能去读这块内存。150KB 留着复用更安全。 */

    cam_page       = NULL;
    cam_img        = NULL;
    cam_zoom_label = NULL;
    cam_hint       = NULL;
    cam_toast      = NULL;
    cam_flash      = NULL;
    cam_back_btn   = NULL;

    printf("[CameraPage] deinit 完成\n");
}
