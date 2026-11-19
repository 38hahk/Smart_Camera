/*
 * 音频播放输出（PortAudio）+ 可选 WAV 落盘调试钩子
 *
 * 这个文件的全部内容都在解释「为什么这么写」，改动前请先读一遍头部注释，
 * 尤其是 try_lock、丢最旧、垫静音这三个决定 —— 它们都是踩过的坑。
 */

#include "ui_audio_out.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>

#ifdef HAVE_PORTAUDIO
#include <portaudio.h>
#endif

/* 环形缓冲 2 秒：12fps 下一个视频帧间隔只有 83ms，2 秒足够宽松了 */
#define AU_RING_SECONDS   2
/* PortAudio 每次回调要多少帧：512 帧 ≈ 11.6ms@44.1k / 32ms@16k */
#define AU_FRAMES_PER_CB  512

static uint8_t        *ring;
static int             ring_size;
static int             rd, wr;                  /* 读写指针（% ring_size） */
static pthread_mutex_t ring_lock = PTHREAD_MUTEX_INITIALIZER;

static int             out_rate, out_ch;
static int             is_open;
static int             prebuf_bytes;
static volatile int    out_paused;

static volatile int    n_underruns;      /* 缓冲真的空了（会丢时间，声音会整体延后） */
static volatile int    n_lockskips;      /* trylock 没拿到锁：这一次回调放静音，代价 11ms */
static volatile long   n_written, n_dropped;

#ifdef HAVE_PORTAUDIO
static PaStream       *stream;
#endif

/* ---- WAV 落盘钩子（ECHO_VIDEO_AUDWAV）---- */
static FILE           *wav_fp;
static long            wav_bytes;

static int ring_used(void)
{
    int u = wr - rd;
    if (u < 0) u += ring_size;
    return u;
}

/* ---------------- WAV 落盘 ---------------- */

static void wav_tee(const void *pcm, int bytes)
{
    if (!wav_fp || bytes <= 0) return;
    if (fwrite(pcm, 1, bytes, wav_fp) == (size_t)bytes) wav_bytes += bytes;
    /* ⚠️ 必须马上落盘：这是个调试钩子，而进程往往是被 pkill 干掉的，
     * 不会走正常关闭 → stdio 缓冲里的数据会整块丢掉（踩过两次，白分析半天） */
    fflush(wav_fp);
}

static void wav_open(const char *path)
{
    wav_fp = fopen(path, "wb");
    if (!wav_fp) { printf("[AudioOut] 打不开 wav: %s\n", path); return; }
    /* 先写占位头，close 的时候回填真实长度 */
    uint8_t hdr[44] = {0};
    fwrite(hdr, 1, sizeof(hdr), wav_fp);
    wav_bytes = 0;
    printf("[AudioOut] PCM 落盘: %s (%dHz %dch)\n", path, out_rate, out_ch);
}

static void wav_close(void)
{
    if (!wav_fp) return;

    /* 回填 WAV 头（16bit PCM） */
    uint8_t hdr[44];
    uint32_t byte_rate   = (uint32_t)(out_rate * out_ch * 2);
    uint32_t data_size   = (uint32_t)wav_bytes;
    uint32_t riff_size   = data_size + 36;
    uint16_t block_align = (uint16_t)(out_ch * 2);
    uint16_t bits        = 16;

    memcpy(hdr + 0,  "RIFF", 4);
    memcpy(hdr + 4,  &riff_size, 4);
    memcpy(hdr + 8,  "WAVEfmt ", 8);
    uint32_t fmt_size = 16;              memcpy(hdr + 16, &fmt_size, 4);
    uint16_t pcm = 1;                    memcpy(hdr + 20, &pcm, 2);
    uint16_t ch  = (uint16_t)out_ch;     memcpy(hdr + 22, &ch, 2);
    uint32_t rate = (uint32_t)out_rate;  memcpy(hdr + 24, &rate, 4);
    memcpy(hdr + 28, &byte_rate, 4);
    memcpy(hdr + 32, &block_align, 2);
    memcpy(hdr + 34, &bits, 2);
    memcpy(hdr + 36, "data", 4);
    memcpy(hdr + 40, &data_size, 4);

    fseek(wav_fp, 0, SEEK_SET);
    fwrite(hdr, 1, sizeof(hdr), wav_fp);
    fclose(wav_fp);
    wav_fp = NULL;
    printf("[AudioOut] wav 收尾: %ld 字节 PCM\n", wav_bytes);
}

/* ---------------- PortAudio 回调 ---------------- */

#ifdef HAVE_PORTAUDIO
static int au_callback(const void *in, void *out, unsigned long frames,
                       const PaStreamCallbackTimeInfo *ti,
                       PaStreamCallbackFlags flags, void *user)
{
    (void)in; (void)ti; (void)flags; (void)user;

    uint8_t *o = (uint8_t *)out;
    int want = (int)(frames * (unsigned long)out_ch * 2);

    /* 先整块填静音，再覆盖真正拿到的数据 —— 欠载时天然就是补静音 */
    memset(o, 0, (size_t)want);

    if (out_paused) return paContinue;

    /* 绝不阻塞：拿不到锁说明解码线程正在写，这次就放静音（不丢数据，只差这 11ms） */
    if (pthread_mutex_trylock(&ring_lock) != 0) {
        n_lockskips++;
        return paContinue;
    }

    int used = ring_used();
    int n = (used < want) ? used : want;
    int first = ring_size - rd;
    if (first > n) first = n;
    if (n > 0) {
        memcpy(o, ring + rd, (size_t)first);
        if (n > first) memcpy(o + first, ring, (size_t)(n - first));
        rd += n;
        if (rd >= ring_size) rd -= ring_size;
    }
    if (n < want) n_underruns++;

    pthread_mutex_unlock(&ring_lock);
    return paContinue;
}
#endif /* HAVE_PORTAUDIO */

/* ---------------- 对外接口 ---------------- */

int ui_audio_out_available(void)
{
#ifdef HAVE_PORTAUDIO
    return 1;
#else
    return 0;
#endif
}

/* 垫静音：把缓冲填到 prebuf_bytes（不够就填多少算多少） */
static void pad_silence(void)
{
    if (prebuf_bytes <= 0 || !ring) return;

    static const uint8_t zeros[2048] = {0};
    int left = prebuf_bytes;
    while (left > 0 && ring_size - ring_used() > 0) {
        int n = left > (int)sizeof(zeros) ? (int)sizeof(zeros) : left;
        if (n > ring_size - ring_used()) n = ring_size - ring_used();
        ui_audio_out_write(zeros, n);
        left -= n;
    }
}

int ui_audio_out_open(int rate, int channels, int prebuf_ms)
{
    /* 无条件先关：上一次可能只开了一半（比如设备打不开），
     * 而且 ECHO_VIDEO_AUDWAV 的 wav 也要重新开一份干净的 */
    ui_audio_out_close();
    if (rate <= 0) rate = 44100;
    if (channels <= 0) channels = 1;

    out_rate = rate;
    out_ch   = channels;
    out_paused = 0;
    n_underruns = 0;
    n_lockskips = 0;
    n_written = 0;
    n_dropped = 0;
    prebuf_bytes = (prebuf_ms > 0) ? (prebuf_ms * out_rate * out_ch * 2 / 1000) : 0;

    ring_size = out_rate * out_ch * 2 * AU_RING_SECONDS;
    free(ring);
    ring = (uint8_t *)malloc((size_t)ring_size);
    if (!ring) { ring_size = 0; return -1; }
    rd = wr = 0;

    const char *wav = getenv("ECHO_VIDEO_AUDWAV");
    if (wav && wav[0]) wav_open(wav);

#ifdef HAVE_PORTAUDIO
    PaError err = Pa_Initialize();
    if (err != paNoError) {
        printf("[AudioOut] Pa_Initialize 失败: %s\n", Pa_GetErrorText(err));
        return -1;
    }

    PaStreamParameters op;
    memset(&op, 0, sizeof(op));
    op.device = Pa_GetDefaultOutputDevice();
    if (op.device == paNoDevice) {
        printf("[AudioOut] 没有找到音频输出设备（静音播放）\n");
        Pa_Terminate();
        return -1;
    }
    op.channelCount     = out_ch;
    op.sampleFormat     = paInt16;
    op.suggestedLatency = Pa_GetDeviceInfo(op.device)->defaultLowOutputLatency;
    op.hostApiSpecificStreamInfo = NULL;

    err = Pa_OpenStream(&stream, NULL, &op, out_rate, AU_FRAMES_PER_CB, paClipOff,
                        au_callback, NULL);
    if (err != paNoError) {
        printf("[AudioOut] 打开输出流失败(%dHz %dch): %s\n",
               out_rate, out_ch, Pa_GetErrorText(err));
        stream = NULL;
        Pa_Terminate();
        return -1;
    }
    err = Pa_StartStream(stream);
    if (err != paNoError) {
        printf("[AudioOut] 启动输出流失败: %s\n", Pa_GetErrorText(err));
        Pa_CloseStream(stream);
        stream = NULL;
        Pa_Terminate();
        return -1;
    }
    printf("[AudioOut] 输出已启动: %dHz %dch 垫%dms静音 缓冲%d字节 设备=%s\n",
           out_rate, out_ch, prebuf_ms, ring_size,
           Pa_GetDeviceInfo(op.device)->name);
#else
    printf("[AudioOut] 编译时没带 portaudio（静音播放）\n");
#endif

    is_open = 1;
    pad_silence();
    return 0;
}

void ui_audio_out_close(void)
{
#ifdef HAVE_PORTAUDIO
    if (stream) {
        Pa_StopStream(stream);
        Pa_CloseStream(stream);
        stream = NULL;
        Pa_Terminate();
    }
#endif
    wav_close();
    is_open = 0;
    out_paused = 0;
    free(ring);
    ring = NULL;
    ring_size = 0;
    rd = wr = 0;
}

int ui_audio_out_is_open(void)     { return is_open; }
int ui_audio_out_rate(void)        { return out_rate; }
int ui_audio_out_channels(void)    { return out_ch; }
int ui_audio_out_buffered_bytes(void)
{
    if (!ring || !is_open) return 0;
    pthread_mutex_lock(&ring_lock);
    int u = ring_used();
    pthread_mutex_unlock(&ring_lock);
    return u;
}
int ui_audio_out_written_bytes(void) { return (int)n_written; }
int ui_audio_out_dropped_bytes(void) { return (int)n_dropped; }
int ui_audio_out_underruns(void)     { return n_underruns; }
int ui_audio_out_lockskips(void)     { return n_lockskips; }

void ui_audio_out_write(const void *pcm, int bytes)
{
    if (!pcm || bytes < 2) return;
    bytes &= ~1;                       /* S16：奇数字节丢掉 */

    wav_tee(pcm, bytes);               /* 没声卡时也能验证内容 */
    n_written += bytes;

    if (!is_open || !ring || ring_size <= 0) return;

    const uint8_t *p = (const uint8_t *)pcm;

    /* 一次写入超过整个环形缓冲：只保留最后一段 */
    if (bytes > ring_size) {
        n_dropped += bytes - ring_size;
        p += bytes - ring_size;
        bytes = ring_size;
    }

    pthread_mutex_lock(&ring_lock);

    int freeb = ring_size - ring_used();
    if (bytes > freeb) {               /* 缓冲满：丢最旧的 */
        int drop = bytes - freeb;
        rd += drop;
        if (rd >= ring_size) rd -= ring_size;
        n_dropped += drop;
    }
    int first = ring_size - wr;
    if (first > bytes) first = bytes;
    memcpy(ring + wr, p, (size_t)first);
    if (bytes > first) memcpy(ring, p + first, (size_t)(bytes - first));
    wr += bytes;
    if (wr >= ring_size) wr -= ring_size;

    pthread_mutex_unlock(&ring_lock);
}

/* 清空：读指针追上写指针（这样回调立刻得到静音） */
static void ring_clear(void)
{
    if (!ring) return;
    pthread_mutex_lock(&ring_lock);
    rd = wr;
    pthread_mutex_unlock(&ring_lock);
}

/*
 * 时间轴跳了（seek / 循环回绕）：清空并重新垫预缓冲。
 * 不垫的话，解码器要等一帧（12fps = 83ms）才产出新音频，这段时间声卡在空转，
 * 结果是欠载 + 静音，声音会越听越晚。
 */
void ui_audio_out_flush(void)
{
    ring_clear();
    pad_silence();
}

void ui_audio_out_set_paused(int paused)
{
    if (paused == out_paused) return;
    out_paused = paused;
    if (paused) {
        /* 暂停：只清空，不要垫 —— 垫了的话恢复时会白多出一段静音 */
        ring_clear();
    } else {
        /* 恢复：垫上静音，保持和开播时一样的延迟 */
        pad_silence();
    }
}
