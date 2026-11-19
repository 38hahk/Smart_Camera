#ifndef _UI_AUDIO_OUT_H
#define _UI_AUDIO_OUT_H

/*
 * 极简音频播放输出（PortAudio）。
 *
 * 结构：视频解码线程 --环形缓冲--> PortAudio 回调线程（PortAudio 自己管的线程）
 *
 * 关键设计（沿用了其他音频回放链路上验证过的经验）：
 *   1. 实时回调里绝不用阻塞锁：try_lock 拿不到锁（说明解码线程正在写入）就输出静音，
 *      下一次回调接着放，数据不会丢，也绝不会阻塞音频线程。
 *   2. 缓冲满了丢**最旧**的数据：视频场景下宁可丢一点声音也不能让延迟越积越大。
 *   3. 打开时先垫 prebuf_ms 静音：视频是「每解一帧才喂一次音频」，一个视频帧间隔
 *      （12fps = 83ms）才突发一批音频，不垫静音会周期性欠载（表现为咔哒声）。
 *      代价是**声音比画面晚 prebuf_ms**（见 ui_VideoPage.c 里的说明）。
 *   4. 主缓冲不设「上限丢帧」策略之外的花样，保持简单可预测。
 *
 * 仿真机上常常没有声卡（VMware 里没配音频设备），所以另留了一个调试钩子：
 *   环境变量 ECHO_VIDEO_AUDWAV=/tmp/out.wav → 把收到的 PCM 落盘成 wav，
 *   用来在「听不到声音」的环境里验证解码链路是不是真的出了声音。
 */

#ifdef __cplusplus
extern "C" {
#endif

/* 编译期是否带上了 portaudio */
int ui_audio_out_available(void);

/*
 * 打开输出设备并立刻开始播放（输出格式：S16 交织）。
 * prebuf_ms：开头先垫多少毫秒静音（<=0 表示不垫）
 * 返回 0 = 成功；-1 = 没有可用设备 / 打不开（此时 write 仍然会写 wav，不会崩）
 */
int ui_audio_out_open(int rate, int channels, int prebuf_ms);

void ui_audio_out_close(void);
int  ui_audio_out_is_open(void);
int  ui_audio_out_rate(void);
int  ui_audio_out_channels(void);

/* 解码线程调用：写一段 PCM（S16 交织）。bytes 为奇数会被截断 */
void ui_audio_out_write(const void *pcm, int bytes);

/* 时间轴跳转（seek / 循环回绕）：清空缓冲并重新垫预缓冲 */
void ui_audio_out_flush(void);

/* 暂停：输出静音并清空缓冲；恢复：重新垫一次 prebuf 静音（保持和开播时一致的延迟） */
void ui_audio_out_set_paused(int paused);

/* 统计（日志 / 自检用） */
int  ui_audio_out_buffered_bytes(void);
int  ui_audio_out_written_bytes(void);   /* 累计喂进来的 */
int  ui_audio_out_dropped_bytes(void);   /* 因为缓冲满被丢掉的 */
int  ui_audio_out_underruns(void);       /* 真欠载次数：缓冲空了（会累积延迟） */
int  ui_audio_out_lockskips(void);       /* trylock 冲突次数：单次回调静音，不丢数据 */

#ifdef __cplusplus
}
#endif

#endif
