#include "lvgl/lvgl.h"
// 注：lvgl/demos 目录在本仓库里已经不存在（CMakeLists.txt 里也关掉了 demos/examples 目标），
//     不要再 include lv_demos.h；下面那两行 demo 调用本来就是注释掉的。
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <linux/input.h>

#include "gui_app/common/lv_lib.h"
#include "gui_app/ui.h"

static const char *getenv_default(const char *name, const char *dflt)
{
    return getenv(name) ? : dflt;
}

/*=====================================================================
 * 卡死自检（排查“屏幕卡住、怎么滑都没用”）
 *
 *   [slow]  单次 lv_timer_handler 超过 250ms —— UI 线程被某个回调阻塞了
 *           （例如换曲时在 UI 线程读 ID3 / 解码封面 PNG / 读 .lrc）
 *   [stall] 主循环超过 3 秒没有心跳 —— UI 线程死循环或被永久阻塞。
 *           同时打印当前页面、每个线程的状态与内核等待点、LVGL 堆占用。
 *   [stall] 超过 20 秒仍未恢复 —— 打完现场 _exit(9)，
 *           由 /etc/init.d/S99start_echo_defconfig 的守护循环重新拉起 main。
 *
 *   判读方法：
 *     - 有 [stall]，main 线程 state=R 且 CPU 100%  → 自己死循环
 *     - 有 [stall]，main 线程 state=S/D + wchan    → 卡在系统调用（读 SD / 触摸 / 帧缓冲）
 *     - 没有 [stall]、日志还在走但屏幕不动        → LVGL 正常，触摸驱动（fts）挂了
 *
 *   环境变量：ECHO_UI_WATCHDOG=0 关闭自检；ECHO_STALL_EXIT=0 只报不退出。
 *===================================================================*/
static volatile unsigned s_beat       = 0;   /* 主循环心跳，每圈 +1 */
static volatile int      s_in_handler = 0;   /* 1 = 正卡在 lv_timer_handler 里 */
static int               s_wd_enable  = 1;
static int               s_wd_exit    = 1;
static int               s_verbose    = 0;   /* ECHO_DEBUG=1 才输 [touch]/[slow] 诊断 */

static unsigned long ms_now(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (unsigned long)tv.tv_sec * 1000UL + (unsigned long)(tv.tv_usec / 1000);
}

static long mem_available_kb(void)
{
    FILE *fp = fopen("/proc/meminfo", "r");
    if (!fp) return -1;

    char line[128];
    long kb = -1;
    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "MemAvailable:", 13) == 0) {
            kb = strtol(line + 13, NULL, 10);
            break;
        }
    }
    fclose(fp);
    return kb;
}

static const char *cur_page_name(void)
{
    lv_lib_pm_page_t *p = lv_lib_pm_GetCurrentPage(&page_manager);
    return (p && p->name) ? p->name : "?";
}

/* 打出每个线程的状态 / 内核等待点 / 正在做的系统调用 */
static void dump_threads(void)
{
    DIR *d = opendir("/proc/self/task");
    if (!d) return;

    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;

        char path[320], buf[512];
        char comm[32] = "?";
        char state = '?';
        char wchan[64] = "-";
        char syscall[64] = "-";

        snprintf(path, sizeof(path), "/proc/self/task/%s/stat", e->d_name);
        FILE *fp = fopen(path, "r");
        if (fp) {
            if (fgets(buf, sizeof(buf), fp)) {
                char *lp = strchr(buf, '(');
                char *rp = strrchr(buf, ')');
                if (lp && rp && rp > lp) {
                    size_t n = (size_t)(rp - lp - 1);
                    if (n >= sizeof(comm)) n = sizeof(comm) - 1;
                    memcpy(comm, lp + 1, n);
                    comm[n] = '\0';
                    if (rp[1] == ' ' && rp[2]) state = rp[2];   /* R/S/D/Z/T */
                }
            }
            fclose(fp);
        }

        snprintf(path, sizeof(path), "/proc/self/task/%s/wchan", e->d_name);
        fp = fopen(path, "r");
        if (fp) {
            if (fgets(wchan, sizeof(wchan), fp)) wchan[strcspn(wchan, "\n")] = '\0';
            fclose(fp);
        }

        snprintf(path, sizeof(path), "/proc/self/task/%s/syscall", e->d_name);
        fp = fopen(path, "r");
        if (fp) {
            if (fgets(syscall, sizeof(syscall), fp)) syscall[strcspn(syscall, "\n")] = '\0';
            fclose(fp);
        }

        printf("[stall]   tid=%s state=%c comm=%s wchan=%s syscall=%s\n",
               e->d_name, state, comm, wchan, syscall);
    }
    closedir(d);
}

static void *_ui_watchdog(void *arg)
{
    (void)arg;
    unsigned long last = (unsigned long)-1;
    int stall = 0;

    for (;;) {
        sleep(1);

        unsigned long b = s_beat;
        if (b != last) {
            if (stall >= 3) printf("[stall] UI 恢复，共卡了 %d 秒\n", stall);
            last  = b;
            stall = 0;
            continue;
        }

        stall++;
        if (stall == 3) {
            lv_mem_monitor_t mon;
            lv_mem_monitor(&mon);
            printf("\n[stall] UI 已卡住 %d 秒：页面=%s in_handler=%d "
                   "LVGL堆=%u/%u B(峰值%u) 碎片%u%% MemAvailable=%ldkB\n",
                   stall, cur_page_name(), s_in_handler,
                   (unsigned)(mon.total_size - mon.free_size),
                   (unsigned)mon.total_size, (unsigned)mon.max_used,
                   (unsigned)mon.frag_pct, mem_available_kb());
        }
        /* 之后每 2 秒刷一次现场 */
        if (stall >= 3 && (stall % 2 == 1)) dump_threads();

        if (stall >= 20 && s_wd_exit) {
            printf("[stall] 20 秒仍未恢复，退出等待守护进程重启 main\n");
            fflush(stdout);
            _exit(9);
        }
    }
    return NULL;
}

#if LV_USE_EVDEV
/*---------------------------------------------------------------------
 * 触摸：设备信息打印 + 显式校准
 *
 * 坑：LVGL 9 的 lv_evdev 只用 EVIOCGABS(ABS_X)/ABS_Y 取校准范围，
 *     而 focaltech 驱动（MT protocol B）只注册 ABS_MT_POSITION_X/Y，
 *     所以 LVGL 拿到的 min=max=0 → 既不缩放也不镜像，只做 clamp。
 *     一旦驱动上报的范围和 320x240 不一致，落点就会整体偏掉。
 *
 * 因此这里主动读 ABS_MT_POSITION_X/Y 的范围并显式校准，同时打印出来，
 * 让日志能直接说明“驱动上的是什么范围”。
 *
 * 免重编调参（排查用，可直接改完在板上跑）：
 *   ECHO_TOUCH_DEV=/dev/input/event1
 *   ECHO_TOUCH_CAL="0,0,320,240"     # xmin,ymin,xmax,ymax
 *   ECHO_TOUCH_SWAP=1                 # 交换 X/Y
 *-------------------------------------------------------------------*/
static void touch_dump_range(int fd, int code, const char *name)
{
    if (!s_verbose) return;

    struct input_absinfo ai;
    if (ioctl(fd, EVIOCGABS(code), &ai) == 0) {
        printf("[touch]   %s: min=%d max=%d\n", name, ai.minimum, ai.maximum);
    }
    else {
        printf("[touch]   %s: 读不到（%s）\n", name, strerror(errno));
    }
}

static void lv_linux_indev_init(void)
{
    const char *dev = getenv("ECHO_TOUCH_DEV") ? : "/dev/input/event0";

    int fd = open(dev, O_RDONLY | O_NOCTTY);
    int mt_x_min = 0, mt_y_min = 0, mt_x_max = 0, mt_y_max = 0;

    if (fd < 0) {
        printf("[touch] 打开 %s 失败：%s\n", dev, strerror(errno));
    }
    else {
        char name[80] = "?";
        ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name);
        if (s_verbose) printf("[touch] %s name=\"%s\"\n", dev, name);
        touch_dump_range(fd, ABS_X, "ABS_X");
        touch_dump_range(fd, ABS_Y, "ABS_Y");
        touch_dump_range(fd, ABS_MT_POSITION_X, "ABS_MT_POSITION_X");
        touch_dump_range(fd, ABS_MT_POSITION_Y, "ABS_MT_POSITION_Y");

        struct input_absinfo ax, ay;
        if (ioctl(fd, EVIOCGABS(ABS_MT_POSITION_X), &ax) == 0 &&
            ioctl(fd, EVIOCGABS(ABS_MT_POSITION_Y), &ay) == 0 &&
            ax.maximum > ax.minimum && ay.maximum > ay.minimum) {
            mt_x_min = ax.minimum;
            mt_y_min = ay.minimum;
            mt_x_max = ax.maximum;
            mt_y_max = ay.maximum;
        }
        close(fd);
    }

    lv_indev_t * touch = lv_evdev_create(LV_INDEV_TYPE_POINTER, dev);
    if (touch == NULL) {
        printf("[touch] lv_evdev_create 失败，触摸不可用\n");
        return;
    }

    /* 用驱动声明的 MT 范围做校准（范围无效时保持 LVGL 默认行为） */
    if (mt_x_max > 0 && mt_y_max > 0) {
        lv_evdev_set_calibration(touch, mt_x_min, mt_y_min, mt_x_max, mt_y_max);
        if (s_verbose) {
            printf("[touch] 校准范围 = (%d,%d)..(%d,%d)\n",
                   mt_x_min, mt_y_min, mt_x_max, mt_y_max);
        }
    }

    /* 手工覆盖，便于不重编就能试各种组合 */
    const char *cal = getenv("ECHO_TOUCH_CAL");
    if (cal != NULL) {
        int a = 0, b = 0, c = 0, d = 0;
        if (sscanf(cal, "%d,%d,%d,%d", &a, &b, &c, &d) == 4) {
            lv_evdev_set_calibration(touch, a, b, c, d);
            printf("[touch] 改用 ECHO_TOUCH_CAL 校准 = (%d,%d)..(%d,%d)\n", a, b, c, d);
        }
    }
    const char *swap = getenv("ECHO_TOUCH_SWAP");
    if (swap != NULL) {
        lv_evdev_set_swap_axes(touch, atoi(swap) != 0);
        printf("[touch] ECHO_TOUCH_SWAP=%s（交换 X/Y）\n", swap);
    }
}
#endif

#if LV_USE_LINUX_FBDEV
static void lv_linux_disp_init(void)
{
    const char *device = getenv_default("LV_LINUX_FBDEV_DEVICE", "/dev/fb0");
    lv_display_t * disp = lv_linux_fbdev_create();

    lv_linux_fbdev_set_file(disp, device);
}
#elif LV_USE_LINUX_DRM
static void lv_linux_disp_init(void)
{
    const char *device = getenv_default("LV_LINUX_DRM_CARD", "/dev/dri/card0");
    lv_display_t * disp = lv_linux_drm_create();

    lv_linux_drm_set_file(disp, device, -1);
}
#elif LV_USE_SDL
static void lv_linux_disp_init(void)
{
    const int width = atoi(getenv("LV_SDL_VIDEO_WIDTH") ? : "320");
    const int height = atoi(getenv("LV_SDL_VIDEO_HEIGHT") ? : "240");

    lv_sdl_window_create(width, height);
}

static void lv_linux_indev_init(void)
{
    lv_sdl_mouse_create();
}

#else
#error Unsupported configuration
#endif

int main(void)
{
    /* 日志立刻落地，崩溃/卡死前的最后几行不会烂在缓冲区里 */
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    if (getenv("ECHO_UI_WATCHDOG") && atoi(getenv("ECHO_UI_WATCHDOG")) == 0) s_wd_enable = 0;
    if (getenv("ECHO_STALL_EXIT") && atoi(getenv("ECHO_STALL_EXIT")) == 0) s_wd_exit = 0;
    if (getenv("ECHO_DEBUG")) s_verbose = atoi(getenv("ECHO_DEBUG")) != 0;

    lv_init();

    /*Linux display device init*/
    lv_linux_disp_init();

    lv_linux_indev_init();

    /*Create a Demo*/
    // lv_demo_widgets();
    // lv_demo_widgets_start_slideshow();
    ui_init();
    if (s_verbose) printf("[boot] ui_init 完成，MemAvailable=%ldkB\n", mem_available_kb());

    if (s_wd_enable) {
        pthread_t tid;
        if (pthread_create(&tid, NULL, _ui_watchdog, NULL) == 0) {
            pthread_detach(tid);
            if (s_verbose) printf("[boot] 卡死自检已启动（ECHO_UI_WATCHDOG=0 可关）\n");
        }
    }

    /*Handle LVGL tasks*/
    while(1) {
        unsigned long t0 = ms_now();
        s_in_handler = 1;
        lv_timer_handler();
        s_in_handler = 0;

        unsigned long dt = ms_now() - t0;
        if (dt > 250 && s_verbose) {
            printf("[slow] lv_timer_handler 用了 %lu ms（页面=%s）\n", dt, cur_page_name());
        }

        s_beat++;
        usleep(1000);
    }

    return 0;
}
