/*
 * 相机 App 的采集后端（板端专用，C 接口）
 *
 * 和 YOLO 那份 AIcamera_c_interface 的区别：
 *   - 不加载 RKNN 模型、不做推理，只取原始画面 → 预览更流畅、CPU 更省；
 *   - 预览带「数字变焦」（中心裁剪 1/zoom 再缩放到 320x240）；
 *   - 拍照时用**原始分辨率**裁剪同样的区域，再用 libjpeg-turbo 存 JPEG
 *     （这套 opencv 没有 imgcodecs，所以不用 cv::imwrite）。
 *
 * 相机节点：/dev/video0（opencv-mobile 的 VideoCapture，V4L2）。
 */

#ifndef CAM_CAPTURE_H
#define CAM_CAPTURE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* 打开摄像头并启动采集线程；返回 0 成功，<0 失败
 * req_w/req_h 是期望分辨率，硬件不支持时会自动退回 640x480 */
int  cam_start(int req_w, int req_h);

/* 停止采集并释放摄像头 */
void cam_stop(void);

int  cam_is_running(void);

/* 实际拿到的分辨率（没启动时返回 0,0） */
void cam_get_size(int *w, int *h);

/* 取一帧预览：中心裁剪 1/zoom 后缩放到 w x h，输出 RGB565（小端，w*h*2 字节）
 * zoom = 1.0 ~ 5.0；返回 0 成功，-1 表示还没拿到帧 */
int  cam_get_preview(uint8_t *rgb565, int w, int h, float zoom);

/* 拍照：中心裁剪 1/zoom 后存 JPEG。min_w 是最小宽度，裁剪后比它小就插值放大
 * （数字变焦 5x 时区域只有 1/5 大，放大一点看着正常些）。返回 0 成功。 */
int  cam_save_jpeg(const char *path, float zoom, int min_w);

#ifdef __cplusplus
}
#endif

#endif /* CAM_CAPTURE_H */
