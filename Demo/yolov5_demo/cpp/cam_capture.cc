/*
 * 相机 App 采集后端实现
 *
 * 线程模型：
 *   采集线程：cap >> frame（阻塞）→ 加锁存到 s_latest
 *   调用线程：cam_get_preview/cam_save_jpeg 加锁拷一份出来再慢慢处理，
 *             处理过程不拿锁，避免拖慢采集。
 *
 * 数字变焦：从画面中心裁出 (w/zoom, h/zoom) 的区域，再缩放到目标尺寸。
 *           预览缩放到 320x240，拍照缩放到「裁剪尺寸」（必要时放大到 min_w）。
 */

#include "cam_capture.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/stat.h>

#include <opencv2/core/core.hpp>
#include <opencv2/imgproc/imgproc.hpp>
#include <opencv2/highgui/highgui.hpp>   /* VideoCapture 在这个库里（libopencv_highgui.a） */

#include "turbojpeg.h"

static cv::VideoCapture s_cap;              //OpenCV视频采集句柄，打开/dev/videoX
static cv::Mat          s_latest;           //是 OpenCV 的图像容器，缓存最新BGR帧，读写必须拿s_lock锁，不能无锁直接访问
static pthread_mutex_t  s_lock = PTHREAD_MUTEX_INITIALIZER;     //pthread互斥锁，保护s_latest这个Mat，防止线程读写同时操作崩溃
static pthread_t        s_thread;           //采集线程ID
static volatile int     s_running = 0;      //采集线程正在工作标志位
static volatile int     s_stop    = 0;      //停止标志位
static int              s_width   = 0;      //摄像头原生画面宽度
static int              s_height  = 0;      //摄像头原生多画面高度
static volatile int     s_frames  = 0;   /* 累计收到多少帧，用来判断有没有画面 */
static volatile int     s_exited  = 0;   /* 采集线程退出标志位（防止 join 卡死） */

/* ===================== 颜色处理 =====================
 * 万一换个摄像头/改驱动后真的偏色，可以用模式微调（不用重编）：
 *   export ECHO_CAM_COLOR=0|1|2   或   写文件 /root/bin/cam_color（内容一个数字）
 *   0 = 不处理（默认）
 *   1 = 交换 R/B
 *   2 = U/V（Cb/Cr）互换校正矩阵
 */
static int s_color_mode = 0;

//设置 相机开始时的相机输出色彩模式
static void _cam_load_color_mode(void)
{
    int mode = 0;
    const char *env = getenv("ECHO_CAM_COLOR");     //读取环境变量ECHO_CAM_COLOR，用来动态选择相机输出色彩模式
    if (env && env[0])      //如果获取的模式不为空，就将获取的字符串转成int整数
    {
        mode = atoi(env);
    } 
    else    //如果获取的环境变量为空，就打开/root/bin/cam_color，获取相机输出色彩模式，如果不为1，就将相机输出色彩模式置为0
    {
        FILE *f = fopen("/root/bin/cam_color", "r");
        if (f) 
        {
            if (fscanf(f, "%d", &mode) != 1) mode = 0;
            fclose(f);
        }
    }
    if (mode < 0 || mode > 2) mode = 0; //模式越界判断
    s_color_mode = mode;    //将相机输出色彩模式保存到全局变量中
    printf("[cam] 颜色模式 = %d（0=不处理 1=R/B互换 2=U/V互换校正）\n", mode);
}

//相机颜色输出修复
static void _cam_color_fix(cv::Mat &m)
{
    if (s_color_mode == 0 || m.empty() || m.type() != CV_8UC3) return;

    //如果相机输出颜色模式为1，就交换R 和 B 的数值，因为OpenCV BGR 顺序：`p[0]=B, p[1]=G, p[2]=R`，不交换的话蓝色和红色会反
    if (s_color_mode == 1) {                 
        cv::cvtColor(m, m, cv::COLOR_RGB2BGR);
        return;
    }

    /* mode 2：U/V 互换校正（Q10 定点，线性矩阵） */
    for (int y = 0; y < m.rows; y++) 
    {
        uint8_t *p = m.ptr<uint8_t>(y);
        for (int x = 0; x < m.cols; x++, p += 3) 
        {
            int B = p[0], G = p[1], R = p[2];
            int r = (  64 * R + 126 * G + 835 * B) >> 10;
            int g = ( 253 * R + 991 * G - 220 * B) >> 10;
            int b = (1213 * R - 159 * G -  31 * B) >> 10;
            p[2] = (uint8_t)(r < 0 ? 0 : (r > 255 ? 255 : r));
            p[1] = (uint8_t)(g < 0 ? 0 : (g > 255 ? 255 : g));
            p[0] = (uint8_t)(b < 0 ? 0 : (b > 255 ? 255 : b));
        }
    }
}

//图像采集函数
static void *_capture_loop(void *arg)
{
    (void)arg;
    cv::Mat frame;      //OpenCV图像容器

    //如果停止标志位为0，即没有停止
    while (!s_stop) 
    {
        s_cap >> frame;           //`>>` 是 `cv::VideoCapture` 重载的抽取运算符，用来从摄像头流读取一帧图像到 frame
        if (frame.empty())        //如果Mat没有有效图像数据，就休眠20ms
        {
            usleep(20 * 1000);
            continue;
        }
        /* 兜底：万一后端给的不是 8UC3，先转成 BGR888，后面的算法都按 BGR 走 */
        if (frame.depth() != CV_8U)         //如果图像像素通道的位深类型不是CV_8U(8位)
        {
            frame.convertTo(frame, CV_8U);  //将图像像素通道的位深类型转成CV_8U(8位)
        }
        if (frame.channels() == 4)          //如果图像像素通道是通道4，就设置3、4通道颜色顺序为B、G、R
        {
            cv::cvtColor(frame, frame, cv::COLOR_BGRA2BGR);
        } 
        else if (frame.channels() == 1) //如果图像像素通道是通道1，就设置3、4通道颜色顺序为B、G、R
        {
            cv::cvtColor(frame, frame, cv::COLOR_GRAY2BGR);
        }

        /* 颜色修正在预览/拍照的小图上做（640x480 全图做太费 CPU，
           而线性变换跟缩放可交换，结果一样）*/
        pthread_mutex_lock(&s_lock);
        if (s_latest.empty()) 
        {
            s_latest = frame.clone();    //新建一份内存（这份内存代表图像容器） 将第一帧的图像数据深拷贝到图像容器中
        } else 
        {
            frame.copyTo(s_latest);      //不新建一份内存，将图像数据深拷贝到图像容器中
        }
        pthread_mutex_unlock(&s_lock);
        s_frames++;
    }
    s_exited = 1;
    return NULL;
}

/* 让线程尽快退出：先置标志，卡住的话把 cap 关掉把它从 read 里踢出来。
 * （V4L2 读没数据时会一直阻塞，不 release 就可能永远 join 不回来） */
static void _stop_capture_thread(void)
{
    s_stop = 1;
    for (int i = 0; i < 30 && !s_exited; i++) usleep(10 * 1000);   /* 最多等 300ms */
    if (!s_exited) {
        s_cap.release();    //释放摄像头数据
        for (int i = 0; i < 100 && !s_exited; i++) usleep(10 * 1000);
    }
    pthread_join(s_thread, NULL);   //等待目标线程正常退出，回收他的资源
    s_exited = 0;
}

/* 打开一次摄像头并等 2.5 秒看有没有画面
 *
 * ⚠️ 顺序很重要：必须 **先 set 再 open**（和 yolov5 demo 的写法一致）。
 *    本板 sensor 是 SC3336，驱动里只有 2304x1296 一种原生模式，其它尺寸靠 ISP 缩放；
 *    如果在 open 之后再 set，V4L2 协商不会真正生效，opencv 却仍按请求的宽高建 Mat，
 *    于是 Mat 的“一行”里其实装了两行图像 → 预览会显示成**两份并排、且变小**。
 */
static int _cam_try_open(int w, int h)
{
    s_frames = 0;
    s_cap.set(cv::CAP_PROP_FRAME_WIDTH,  w);        //设置摄像头流宽度
    s_cap.set(cv::CAP_PROP_FRAME_HEIGHT, h);        //设置摄像头流高度
    if (!s_cap.open(0))     //打开摄像头节点
    {
        printf("[cam] 打不开 /dev/video0\n");
        return -1;
    }
    printf("[cam] 请求 %dx%d，等待画面...\n", w, h);

    s_stop = 0;
    s_running = 1;
    //创建线程，调用图片采集函数
    if (pthread_create(&s_thread, NULL, _capture_loop, NULL) != 0) 
    {
        printf("[cam] 创建采集线程失败\n");
        s_cap.release();
        s_running = 0;
        return -1;
    }

    //
    for (int i = 0; i < 50; i++) 
    {       /* 最多 2.5 秒 */
        if (s_frames > 3)  //如果收到的图像大于3，就代表有画面
        {
            pthread_mutex_lock(&s_lock);
            s_width  = s_latest.cols;   //记录图像宽度
            s_height = s_latest.rows;   //记录图像高度
            int ch   = s_latest.channels(); //获取图像通道数
            pthread_mutex_unlock(&s_lock);
            printf("[cam] 拿到画面：%dx%d，%d 通道（请求 %dx%d）%s\n",
                   s_width, s_height, ch, w, h,
                   (s_width != w || s_height != h) ? " ← 尺寸和请求不一致！" : "");
            return 0;
        }
        usleep(50 * 1000);
    }

    printf("[cam] %dx%d 没有画面，换个分辨率再试\n", w, h);
    _stop_capture_thread();
    s_cap.release();
    pthread_mutex_lock(&s_lock);
    s_latest.release();
    pthread_mutex_unlock(&s_lock);
    s_running = 0;
    return -1;
}

//
int cam_start(int req_w, int req_h)
{
    if (s_running) return 0;

    /* 颜色修正开关（默认 mode 2）
     * 不用重新烧固件：板子上 echo 0 > /root/bin/cam_color 再重启 app 即可 */
    _cam_load_color_mode(); //设置相机输出颜色模式

    //从环境变量ECHO_CAM_SIZE中读取分辨率
    const char *env = getenv("ECHO_CAM_SIZE");
    if (env && env[0]) 
    {
        int ew = atoi(env);
        const char *sep = strpbrk(env, "xX*");
        int eh = sep ? atoi(sep + 1) : 0;
        if (ew >= 64 && eh >= 64) 
        {
            printf("[cam] ECHO_CAM_SIZE -> %dx%d\n", ew, eh);
            req_w = ew;
            req_h = eh;
        }
    }

    /* 默认 640x480：这是 yolov5 demo 在本板上验证过的唯一可靠尺寸
       （SC3336 只有 2304x1296 原生模式，其它尺寸要 ISP 缩放，容易协商失败） */
    if (req_w <= 0 || req_h <= 0) {
        req_w = 640;
        req_h = 480;
    }

    //调用相机尝试打开函数，确定有画面
    if (_cam_try_open(req_w, req_h) == 0) return 0;

    /* 退回 640x480 再试一次 */
    if ((req_w != 640 || req_h != 480) && _cam_try_open(640, 480) == 0) return 0;

    printf("[cam] 相机启动失败\n");
    return -1;
}

void cam_stop(void)
{
    if (!s_running) return;

    _stop_capture_thread();

    pthread_mutex_lock(&s_lock);
    s_latest.release();
    pthread_mutex_unlock(&s_lock);

    s_cap.release();
    s_running = 0;
    s_width = 0;
    s_height = 0;
    printf("[cam] 已关闭\n");
}

int cam_is_running(void)
{
    return s_running;
}

void cam_get_size(int *w, int *h)
{
    if (w) *w = s_width;
    if (h) *h = s_height;
}

/* 取一份最新帧的拷贝（调用者自己 release） */
static bool _cam_copy_latest(cv::Mat *out)
{
    pthread_mutex_lock(&s_lock);
    bool ok = !s_latest.empty();
    if (ok) out->create(s_latest.size(), s_latest.type());   //分配图形容器的像素缓冲区
    if (ok) s_latest.copyTo(*out);  //将最新一帧图像拷贝到指定OpenCV容器中
    pthread_mutex_unlock(&s_lock);
    return ok;
}

/* 中心裁剪出 1/zoom 的区域，并按 aspect（宽/高）裁成目标比例，保证画面不变形 */
static cv::Rect _zoom_rect(int w, int h, float zoom, float aspect)
{
    if (zoom < 1.0f) zoom = 1.0f;
    if (zoom > 5.0f) zoom = 5.0f;

    int cw = (int)(w / zoom);
    int ch = (int)(h / zoom);
    if (cw < 8)  cw = 8;
    if (ch < 8)  ch = 8;
    if (cw > w)  cw = w;
    if (ch > h)  ch = h;

    /* 采集是 4:3 时这里不生效；万一设了 16:9（ECHO_CAM_SIZE）也不会被拉变形 */
    if (aspect > 0.1f) {
        if ((float)cw / (float)ch > aspect) cw = (int)(ch * aspect);
        else                                ch = (int)(cw / aspect);
        if (cw > w) cw = w;
        if (ch > h) ch = h;
    }

    return cv::Rect((w - cw) / 2, (h - ch) / 2, cw, ch);
}

int cam_get_preview(uint8_t *rgb565, int w, int h, float zoom)
{
    if (rgb565 == NULL || w <= 0 || h <= 0) return -1;

    cv::Mat bgr;    //定义OpenCV图像容器
    if (!_cam_copy_latest(&bgr)) return -1; //去当前图像容器中的最新一帧图像拷贝到新容器中

    float aspect = (float)w / (float)h;      /* 预览是 4:3，裁剪保持同比例 */
    cv::Mat roi = bgr(_zoom_rect(bgr.cols, bgr.rows, zoom, aspect));    //变焦 + 保持预览比例不变，画面不会拉伸。
    cv::Mat small;
    cv::resize(roi, small, cv::Size(w, h), 0, 0, cv::INTER_LINEAR);     //把裁切后的 ROI 缩放到预览分辨率，线性插值，适合预览

    _cam_color_fix(small);                   /* 默认模式 0 = 不处理，和 YOLO 一致 */

    /* ⚠️ 自己打包成 RGB565（R 在高 5 位）：
     *    OpenCV 的 COLOR_BGR2BGR565 输出是 **B 在高位**，直接喂给 LVGL 的
     *    LV_COLOR_FORMAT_RGB565（R 在高位）会红蓝互换 —— 相册照片对、预览发蓝
     *    就是这个原因（相册那边是我们自己按 R,G,B 打包的，所以一直没这问题）。
     *    这里按 (R,G,B) 手动移位，位序完全确定，不再依赖任何库的约定。*/
    uint16_t *dst = (uint16_t *)rgb565;
    for (int y = 0; y < h; y++) 
    {
        const uint8_t *s = small.ptr<uint8_t>(y);
        uint16_t *d = dst + (size_t)y * w;
        for (int x = 0; x < w; x++, s += 3) 
        {
            d[x] = (uint16_t)(((s[2] & 0xF8) << 8) |   /* R5 */
                              ((s[1] & 0xFC) << 3) |   /* G6 */
                               (s[0] >> 3));           /* B5 */
        }
    }
    return 0;
}

int cam_save_jpeg(const char *path, float zoom, int min_w)
{
    if (path == NULL) return -1;

    cv::Mat bgr;
    if (!_cam_copy_latest(&bgr)) {
        printf("[cam] 还没有画面，拍照失败\n");
        return -1;
    }

    cv::Mat roi  = bgr(_zoom_rect(bgr.cols, bgr.rows, zoom, 4.0f / 3.0f));
    cv::Mat shot = roi.clone();

    /* 5 倍变焦时区域很小（1280x720 → 256x144），放大到 640 宽看着正常些 */
    if (min_w > 0 && shot.cols < min_w) {
        int nw = min_w;
        int nh = (int)((double)shot.rows * nw / shot.cols);
        cv::resize(shot, shot, cv::Size(nw, nh), 0, 0, cv::INTER_LINEAR);
    }

    if (!shot.isContinuous()) shot = shot.clone();

    _cam_color_fix(shot);                    /* 存图前修一次颜色 */

    tjhandle tj = tjInitCompress();
    if (tj == NULL) {
        printf("[cam] tjInitCompress 失败\n");
        return -2;
    }

    unsigned char *jpg_buf = NULL;
    unsigned long  jpg_len = 0;
    int rc = tjCompress2(tj, shot.data, shot.cols, 0, shot.rows, TJPF_BGR,
                         &jpg_buf, &jpg_len, TJSAMP_420, 90, TJFLAG_FASTDCT);
    if (rc != 0) {
        printf("[cam] tjCompress2 失败: %s\n", tjGetErrorStr());
        tjDestroy(tj);
        return -3;
    }

    FILE *fp = fopen(path, "wb");
    if (fp == NULL) {
        printf("[cam] 打不开 %s（SD 卡没挂上？）\n", path);
        tjFree(jpg_buf);
        tjDestroy(tj);
        return -4;
    }
    size_t wr = fwrite(jpg_buf, 1, jpg_len, fp);
    fflush(fp);
    fclose(fp);

    tjFree(jpg_buf);
    tjDestroy(tj);

    if (wr != jpg_len) {
        printf("[cam] 写入不完整 %u/%u\n", (unsigned)wr, (unsigned)jpg_len);
        return -5;
    }

    printf("[cam] 已保存 %s（%dx%d, %u 字节）\n", path, shot.cols, shot.rows,
           (unsigned)jpg_len);
    return 0;
}
