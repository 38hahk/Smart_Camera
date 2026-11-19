#ifndef _UI_VIDEO_DECODE_H
#define _UI_VIDEO_DECODE_H

/*
 * 极简视频解码抽象层。
 *
 * 目标：把「一个带 H.264 的 mp4 文件」解成一张张 RGB565 的位图，交给 LVGL 的 canvas 显示。
 * 输出格式固定为 RGB565（AV_PIX_FMT_RGB565LE / LV_COLOR_FORMAT_NATIVE 在 LE 机器上一致），
 * 尺寸固定为目标尺寸（用 sws_scale 做缩放，带 NEON 加速）。
 *
 * 当前只有一个后端：ffmpeg（libavformat + libavcodec + libswscale）。
 * 如果编译时没找到 ffmpeg，会退化成 stub（ui_video_is_supported() 返回 0，页面上提示不可用）。
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ui_video_decoder ui_video_decoder_t;

/* 后端是否可用（编译期是否有 ffmpeg） */
int ui_video_is_supported(void);

/* 打开文件并准备解码；dst_w/dst_h 是目标尺寸（<=0 表示按原尺寸） */
ui_video_decoder_t *ui_video_open(const char *path, int dst_w, int dst_h);

/* 视频信息（open 之后有效） */
int   ui_video_width(ui_video_decoder_t *d);
int   ui_video_height(ui_video_decoder_t *d);
float ui_video_fps(ui_video_decoder_t *d);
double ui_video_duration(ui_video_decoder_t *d);   /* 秒，未知为 0 */
int   ui_video_frame_count(ui_video_decoder_t *d); /* 未知为 0 */

/*
 * 解出下一帧，写进 dst（大小 = 宽*高*2 字节，RGB565）。
 * 返回 1 = 成功，0 = 文件结束，-1 = 出错。
 */
int ui_video_read_frame(ui_video_decoder_t *d, void *dst);

/* 回到开头（用于循环播放） */
void ui_video_rewind(ui_video_decoder_t *d);

/* 跳到指定秒数（0 到 duration），会清掉解码器内部缓冲 */
int ui_video_seek(ui_video_decoder_t *d, double seconds);

/* 当前已解出这一帧的时间戳（秒），未知返回 0 */
double ui_video_position(ui_video_decoder_t *d);

void ui_video_close(ui_video_decoder_t *d);

/* ---------------- 音频（可选） ----------------
 *
 * 音频和视频共用**同一个 demuxer**：av_read_frame 出来的包按文件顺序交织，
 * 视频包给视频解码器，音频包给音频解码器，所以「音画对齐」是天然成立的
 * （不需要额外的时间戳对齐逻辑，代价是音频只能喂到当前视频帧的位置）。
 *
 * 音频输出固定为 **S16 交织**，采样率/声道数默认跟文件走（可用
 * ui_video_set_audio_format() 改成设备能接受的值，内部用 swresample 转换）。
 */

/* PCM 输出回调：pcm 是 S16 交织数据，bytes 是它的字节数 */
typedef void (*ui_video_pcm_cb_t)(void *user, const void *pcm, int bytes);

/* 文件里有没有音频流 */
int ui_video_has_audio(ui_video_decoder_t *d);
int ui_video_audio_rate(ui_video_decoder_t *d);      /* 输出采样率 */
int ui_video_audio_channels(ui_video_decoder_t *d);  /* 输出声道数 */

/*
 * 注册 PCM 回调：**必须**在第一次 ui_video_read_frame() 之前调用。
 * 传 NULL 表示不要音频（解码器会丢掉音频包）。
 */
void ui_video_set_pcm_callback(ui_video_decoder_t *d, ui_video_pcm_cb_t cb, void *user);

/*
 * 改变音频输出格式（设备不支持文件自带的采样率/声道数时用，做降级）。
 * 必须在读出第一帧音频之前调用。返回 0 = 成功。
 */
int ui_video_set_audio_format(ui_video_decoder_t *d, int rate, int channels);

/* ---------------- 纯音频（音乐播放用） ----------------
 *
 * 和上面共用一个解码器结构和 ffmpeg 后端，区别只是「不需要有视频流」，
 * 所以 mp3/flac/wav/m4a/ogg 这些纯音频文件也能开。
 * PCM 还是通过 ui_video_set_pcm_callback() 注册的回调吐出来（S16 交织）。
 */

/* 打开音频文件；rate/channels <= 0 表示跟文件走。打不开返回 NULL */
ui_video_decoder_t *ui_audio_open(const char *path, int rate, int channels);

/*
 * 解一段音频出来（会通过回调吐 PCM）。
 * 返回 1 = 解出了数据，0 = 文件结束，-1 = 出错。
 * 注意：这个函数**不控速**，解码远快于实时 → 调用方必须自己按播放进度限速
 *（看 ui_audio_out_buffered_bytes()），否则声音会被丢掉。
 */
int ui_audio_read(ui_video_decoder_t *d);

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif
