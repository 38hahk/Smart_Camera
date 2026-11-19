/*
 * 旋转编码器（带按压开关）驱动 —— 相机 App 用来变焦 / 拍照
 *
 * 板端接线（扩展排针 H1，PZ254V-12-12P，默认接法）：
 *     编码器 CLK(A 相) → H1 pin 8  = GPIO1_B0  (sysfs 号 40)
 *     编码器 DT (B 相) → H1 pin 6  = GPIO1_B1  (sysfs 号 41)
 *     编码器 SW (按下) → H1 pin 10 = GPIO1_A2  (sysfs 号 34)  低电平有效
 *     VCC → pin 3 (VCC_3V3)，GND → pin 1
 *
 * 注意：GPIO1_B0/B1/A2 在原理图上同时是 BT_WAKE / BT_HOST_WAKE / BT_RST_N
 *       （通过 0Ω 电阻连到蓝牙）。本板只用 WiFi 不用蓝牙，所以可以借用；
 *       如果以后要开蓝牙，就得换脚（改下面三个宏）。
 *
 * 实现：
 *   1) 用 sysfs gpio 的 edge 中断（poll+POLLPRI），不用 while(1) 里死读，
 *      既省 CPU 又不会漏脉冲；
 *   2) A/B 相走标准的正交解码表，累计满 4 个跳变（EC11 一格）算一步；
 *   3) 回调运行在编码器线程里 —— 页面里只允许改 volatile 标志，不能碰 lv_ 对象。
 */

#include "encoder_manager.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>

#if LV_USE_SIMULATOR == 0

#define SYS_GPIO_DIR "/sys/class/gpio"

/* EC11 一格 = 4 个正交跳变；如果换的编码器一格只有 2 个跳变就改成 2 */
#define ENCODER_TICKS_PER_STEP  4
#define ENCODER_SW_DEBOUNCE_MS  60

static int  s_fd_a = -1, s_fd_b = -1, s_fd_sw = -1;
static pthread_t s_thread;
static volatile int s_running = 0;
static bool s_available = false;
static encoder_rotate_cb_t s_on_rotate = NULL;
static encoder_press_cb_t  s_on_press  = NULL;

/* ---------------- sysfs 小工具 ---------------- */

static int _write_file(const char *path, const char *value)
{
    FILE *f = fopen(path, "w");
    if (f == NULL) return -1;
    fputs(value, f);
    fclose(f);
    return 0;
}

static int _open_value(int gpio)
{
    char path[96];
    snprintf(path, sizeof(path), SYS_GPIO_DIR "/gpio%d/value", gpio);
    return open(path, O_RDONLY);
}

static int _read_value(int fd)
{
    char buf[8];
    if (fd < 0) return -1;
    if (lseek(fd, 0, SEEK_SET) < 0) return -1;
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    if (n <= 0) return -1;
    buf[n] = 0;
    return (buf[0] == '1') ? 1 : 0;
}

/* 导出 + 设为输入 + 设置中断边沿 */
static bool _setup_pin(int gpio, const char *edge)
{
    char path[96];
    char num[8];
    snprintf(num, sizeof(num), "%d", gpio);

    /* 已经导出过的话会返回 EBUSY，忽略即可 */
    if (_write_file(SYS_GPIO_DIR "/export", num) != 0) {
        printf("[encoder] export %d 失败（可能已被占用）\n", gpio);
    }
    usleep(30 * 1000);   /* udev/内核建目录要点时间 */

    snprintf(path, sizeof(path), SYS_GPIO_DIR "/gpio%d/direction", gpio);
    if (_write_file(path, "in") != 0) {
        printf("[encoder] GPIO%d 设成输入失败\n", gpio);
        return false;
    }
    snprintf(path, sizeof(path), SYS_GPIO_DIR "/gpio%d/edge", gpio);
    if (_write_file(path, edge) != 0) {
        printf("[encoder] GPIO%d 设置边沿 %s 失败\n", gpio, edge);
        return false;
    }
    return true;
}

/* 单调时钟毫秒数（用 time() 只有秒级精度，去抖会失效） */
static long long _now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* 标准正交解码表：index = (prev << 2) | cur，取值 ±1 表示一次跳变的方向 */
static int8_t _quad_step(int prev, int cur)
{
    static const int8_t tbl[16] = {
         0, -1,  1,  0,
         1,  0,  0, -1,
        -1,  0,  0,  1,
         0,  1, -1,  0
    };
    return tbl[((prev & 3) << 2) | (cur & 3)];
}

/* ---------------- 编码器线程 ---------------- */

static void *_encoder_loop(void *arg)
{
    (void)arg;

    int prev = (_read_value(s_fd_a) << 1) | _read_value(s_fd_b);
    int acc = 0;
    long long last_press_ms = 0;

    struct pollfd fds[3] = {
        { s_fd_a,  POLLPRI, 0 },
        { s_fd_b,  POLLPRI, 0 },
        { s_fd_sw, POLLPRI, 0 },
    };

    while (s_running) {
        int rc = poll(fds, 3, 200);
        if (rc < 0) {
            if (errno == EINTR) continue;
            printf("[encoder] poll 出错: %s\n", strerror(errno));
            break;
        }
        if (rc == 0) continue;

        /* --- A/B 相：正交解码 --- */
        if (fds[0].revents & POLLPRI || fds[1].revents & POLLPRI) {
            int a = _read_value(s_fd_a);
            int b = _read_value(s_fd_b);
            if (a >= 0 && b >= 0) {
                int cur = (a << 1) | b;
                acc += _quad_step(prev, cur);
                prev = cur;

                while (acc >= ENCODER_TICKS_PER_STEP) {
                    acc -= ENCODER_TICKS_PER_STEP;
                    if (s_on_rotate) s_on_rotate(+1);
                }
                while (acc <= -ENCODER_TICKS_PER_STEP) {
                    acc += ENCODER_TICKS_PER_STEP;
                    if (s_on_rotate) s_on_rotate(-1);
                }
            }
        }

        /* --- 按键：下降沿（低电平有效） --- */
        if (fds[2].revents & POLLPRI) {
            int v = _read_value(s_fd_sw);
            if (v == 0) {
                long long now = _now_ms();
                if (now - last_press_ms > ENCODER_SW_DEBOUNCE_MS) {
                    last_press_ms = now;
                    if (s_on_press) s_on_press();
                }
            }
        }
    }

    printf("[encoder] 线程退出\n");
    return NULL;
}

/* ---------------- 对外接口 ---------------- */

bool encoder_init(encoder_rotate_cb_t on_rotate, encoder_press_cb_t on_press)
{
    if (s_running) return true;

    s_on_rotate = on_rotate;
    s_on_press  = on_press;

    /* 引脚被别的驱动占用（比如蓝牙）时这里会失败 */
    if (!_setup_pin(ENCODER_GPIO_A, "both") ||
        !_setup_pin(ENCODER_GPIO_B, "both") ||
        !_setup_pin(ENCODER_GPIO_SW, "falling")) {
        s_available = false;
        return false;
    }

    s_fd_a  = _open_value(ENCODER_GPIO_A);
    s_fd_b  = _open_value(ENCODER_GPIO_B);
    s_fd_sw = _open_value(ENCODER_GPIO_SW);
    if (s_fd_a < 0 || s_fd_b < 0 || s_fd_sw < 0) {
        printf("[encoder] 打开 value 节点失败（A=%d B=%d SW=%d）\n",
               s_fd_a, s_fd_b, s_fd_sw);
        if (s_fd_a >= 0)  close(s_fd_a);
        if (s_fd_b >= 0)  close(s_fd_b);
        if (s_fd_sw >= 0) close(s_fd_sw);
        s_fd_a = s_fd_b = s_fd_sw = -1;
        s_available = false;
        return false;
    }

    /* 起始电平先读一次，避免第一次转动算错方向 */
    (void)_read_value(s_fd_a);
    (void)_read_value(s_fd_b);

    s_running = 1;
    if (pthread_create(&s_thread, NULL, _encoder_loop, NULL) != 0) {
        printf("[encoder] 创建线程失败\n");
        s_running = 0;
        close(s_fd_a); close(s_fd_b); close(s_fd_sw);
        s_fd_a = s_fd_b = s_fd_sw = -1;
        s_available = false;
        return false;
    }

    s_available = true;
    printf("[encoder] 已启动（A=%d B=%d SW=%d）\n",
           ENCODER_GPIO_A, ENCODER_GPIO_B, ENCODER_GPIO_SW);
    return true;
}

void encoder_deinit(void)
{
    if (s_running) {
        s_running = 0;
        pthread_join(s_thread, NULL);
    }
    if (s_fd_a >= 0)  { close(s_fd_a);  s_fd_a = -1; }
    if (s_fd_b >= 0)  { close(s_fd_b);  s_fd_b = -1; }
    if (s_fd_sw >= 0) { close(s_fd_sw); s_fd_sw = -1; }

    s_available = false;
    s_on_rotate = NULL;
    s_on_press  = NULL;
    printf("[encoder] deinit 完成\n");
}

bool encoder_available(void)
{
    return s_available;
}

#else  /* LV_USE_SIMULATOR != 0：仿真机没有 gpio，直接当不可用 */

bool encoder_init(encoder_rotate_cb_t on_rotate, encoder_press_cb_t on_press)
{
    (void)on_rotate;
    (void)on_press;
    return false;
}

void encoder_deinit(void) { }

bool encoder_available(void) { return false; }

#endif
