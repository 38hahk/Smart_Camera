/*
 * ffmpeg 后端：读 mp4(H.264) → 解码 → 缩放 → RGB565
 *
 * 没装 ffmpeg 时整个文件退化成 stub，保证工程在任何机器上都能编译。
 */

#include "ui_video_decode.h"

#include <stdio.h>
#include <string.h>

#ifdef HAVE_FFMPEG

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/version.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>

/*
 * ffmpeg 版本兼容：
 *   AVChannelLayout / av_channel_layout_* / swr_alloc_set_opts2 是 ffmpeg 5.1/5.0 才有的，
 *   而 **buildroot 2023.02.6 里的 ffmpeg 是 4.4.4**（仿真机上是 6.1）。
 *   这里用版本宏分两个分支，两边都能编。
 */
#if LIBAVUTIL_VERSION_MAJOR >= 57      /* ffmpeg >= 5.1 */
#define UI_FF_NEW_CHLAYOUT 1
#else
#define UI_FF_NEW_CHLAYOUT 0
#endif

/* 4.x 用 channels，5.1+ 用 ch_layout.nb_channels */
static int actx_channels(AVCodecContext *c)
{
#if UI_FF_NEW_CHLAYOUT
    return c->ch_layout.nb_channels;
#else
    return c->channels;
#endif
}

struct ui_video_decoder {
    AVFormatContext *fmt;
    AVCodecContext  *ctx;
    AVFrame         *frm;
    AVPacket        *pkt;
    struct SwsContext *sws;

    int   stream_index;
    int   src_w, src_h;
    int   dst_w, dst_h;
    float fps;
    double duration;
    int   frame_count;

    int   eof;
    int   got_first_frame;
    double pos;              /* 当前帧时间戳（秒） */
    double skip_until;       /* seek 之后：把时间戳小于它的帧全部丢掉（0 = 不丢） */

    /* ---- 音频 ---- */
    int                   audio_index;   /* <0 = 没有音频流 */
    AVCodecContext       *actx;
    AVFrame              *afrm;
    struct SwrContext    *swr;
    int                   out_rate, out_ch;
    int                   audio_flushed; /* 音频解码器是否已经喂过 flush 包 */
    ui_video_pcm_cb_t     pcm_cb;
    void                 *pcm_user;
    uint8_t              *pcm_buf;
    int                   pcm_cap;
    long                  pcm_bytes;     /* 累计输出的 PCM 字节数 */
    int                   audio_only;    /* 1 = 纯音频文件（音乐），stream_index < 0 */
};

/* 临时调试：ECHO_VIDEO_AUDEBUG=1 时打印 seek/rewind 之后的前几个音频包和帧 */
static int au_dbg;
static int au_dbg_pkts;

int ui_video_is_supported(void) { return 1; }

/* ------------------------- 音频：内部工具 ------------------------- */

/*
 * 指定音频输出格式（S16 交织）。可以重复调用（比如设备不支持文件自带的
 * 采样率时降级到 16k 单声道），每次都会重建 swresample 上下文。
 */
int ui_video_set_audio_format(ui_video_decoder_t *d, int rate, int channels)
{
    if (!d || d->audio_index < 0 || !d->actx) return -1;
    if (rate <= 0) rate = d->actx->sample_rate > 0 ? d->actx->sample_rate : 44100;
    if (channels <= 0) channels = actx_channels(d->actx) > 0 ? actx_channels(d->actx) : 1;

    if (d->swr) swr_free(&d->swr);

#if UI_FF_NEW_CHLAYOUT
    AVChannelLayout in_layout;
    if (d->actx->ch_layout.nb_channels > 0 &&
        d->actx->ch_layout.order != AV_CHANNEL_ORDER_UNSPEC)
        av_channel_layout_copy(&in_layout, &d->actx->ch_layout);
    else
        av_channel_layout_default(&in_layout, channels);

    AVChannelLayout out_layout;
    av_channel_layout_default(&out_layout, channels);

    int r = swr_alloc_set_opts2(&d->swr,
                                &out_layout, AV_SAMPLE_FMT_S16, rate,
                                &in_layout, d->actx->sample_fmt, d->actx->sample_rate,
                                0, NULL);
    av_channel_layout_uninit(&in_layout);
    av_channel_layout_uninit(&out_layout);
    if (r < 0 || !d->swr) {
        printf("[VideoPage] swr 创建失败(%d)\n", r);
        d->swr = NULL;
        return -1;
    }
#else
    /* ffmpeg 4.x：声道用 uint64_t 的 channel_layout 位图 */
    int64_t in_ch = (d->actx->channel_layout != 0)
                    ? (int64_t)d->actx->channel_layout
                    : av_get_default_channel_layout(channels);
    int64_t out_ch = av_get_default_channel_layout(channels);

    d->swr = swr_alloc_set_opts(NULL, out_ch, AV_SAMPLE_FMT_S16, rate,
                                in_ch, d->actx->sample_fmt, d->actx->sample_rate,
                                0, NULL);
    if (!d->swr) {
        printf("[VideoPage] swr 创建失败\n");
        return -1;
    }
#endif

    if (swr_init(d->swr) < 0) {
        printf("[VideoPage] swr 初始化失败\n");
        swr_free(&d->swr);
        return -1;
    }

    d->out_rate = rate;
    d->out_ch   = channels;
    return 0;
}

/* 音频帧的时间戳（秒），未知返回 -1 */
static double audio_frame_pos(ui_video_decoder_t *d)
{
    if (d->afrm->best_effort_timestamp == AV_NOPTS_VALUE) return -1;
    AVRational tb = d->fmt->streams[d->audio_index]->time_base;
    return d->afrm->best_effort_timestamp * av_q2d(tb);
}

/*
 * 把音频解码器里能取出来的帧全部转成 S16 交织，交给回调。
 * pkt == NULL 表示喂 flush 包（文件结尾时把缓冲里的尾巴放出来）。
 */
static void pump_audio(ui_video_decoder_t *d, AVPacket *pkt)
{
    if (!d || d->audio_index < 0 || !d->actx || !d->afrm) return;
    if (pkt && d->audio_flushed) return;      /* 已经到尾巴了，不再收新包 */

    if (au_dbg && au_dbg_pkts > 0 && pkt) {
        au_dbg_pkts--;
        printf("[AudioDbg] pkt idx=%d pts=%lld size=%d\n", pkt->stream_index,
               (long long)pkt->pts, pkt->size);
    }

    if (avcodec_send_packet(d->actx, pkt) < 0) return;
    if (!pkt) d->audio_flushed = 1;

    while (1) {
        int r = avcodec_receive_frame(d->actx, d->afrm);
        if (r < 0) break;                     /* EAGAIN / EOF 都从这里出去 */

        if (!d->pcm_cb || !d->swr) { av_frame_unref(d->afrm); continue; }

        /* seek 之后：整帧都还在目标之前就直接丢掉，保证声音和画面在同一个时间点上 */
        if (d->skip_until > 0) {
            double ap = audio_frame_pos(d);
            if (ap >= 0 && d->actx->sample_rate > 0) {
                double end = ap + (double)d->afrm->nb_samples / d->actx->sample_rate;
                if (end <= d->skip_until) { av_frame_unref(d->afrm); continue; }
            }
        }

        /* 上界：swr_get_out_samples 会把重采样器里攒着的样本算进去 */
        int max_out = swr_get_out_samples(d->swr, d->afrm->nb_samples);
        if (max_out < 0) max_out = d->afrm->nb_samples + 256;
        int need = max_out * d->out_ch * 2;

        if (need > d->pcm_cap) {
            uint8_t *nb = (uint8_t *)av_realloc(d->pcm_buf, (size_t)need);
            if (!nb) { av_frame_unref(d->afrm); return; }
            d->pcm_buf = nb;
            d->pcm_cap = need;
        }

        uint8_t *outp[1] = { d->pcm_buf };
        int got = swr_convert(d->swr, outp, max_out,
                              (const uint8_t **)d->afrm->extended_data,
                              d->afrm->nb_samples);
        if (got > 0) {
            int bytes = got * d->out_ch * 2;
            d->pcm_bytes += bytes;
            /* 纯音频模式：位置只能从音频帧的时间戳取（没有视频帧可参考） */
            if (d->audio_only) {
                double ap = audio_frame_pos(d);
                if (ap >= 0) d->pos = ap;
            }
            if (au_dbg && au_dbg_pkts > 0) {
                const int16_t *s = (const int16_t *)d->pcm_buf;
                printf("[AudioDbg] frame fmt_in=%d nb=%d got=%d ch=%d 前4样本=%d %d %d %d\n",
                       d->afrm->format, d->afrm->nb_samples, got, d->out_ch,
                       s[0], s[1], s[2], s[3]);
            }
            d->pcm_cb(d->pcm_user, d->pcm_buf, bytes);
        }
        av_frame_unref(d->afrm);
    }
}

/* 打开音频流（没有音频流不算错误，只是没声音） */
static void audio_open(ui_video_decoder_t *d)
{
    d->audio_index = -1;

    const AVCodec *acodec = NULL;
    int asidx = av_find_best_stream(d->fmt, AVMEDIA_TYPE_AUDIO, -1, -1, &acodec, 0);
    if (asidx < 0 || !acodec) return;

    d->actx = avcodec_alloc_context3(acodec);
    if (!d->actx) return;
    if (avcodec_parameters_to_context(d->actx, d->fmt->streams[asidx]->codecpar) < 0) {
        avcodec_free_context(&d->actx);
        return;
    }
    if (avcodec_open2(d->actx, acodec, NULL) < 0) {
        printf("[VideoPage] 音频解码器打不开 (%s)\n", acodec->name);
        avcodec_free_context(&d->actx);
        return;
    }
    d->afrm = av_frame_alloc();
    if (!d->afrm) {
        avcodec_free_context(&d->actx);
        return;
    }
    d->audio_index = asidx;

    /* 默认：跟文件走（不做重采样，只把打包格式统一成 S16 交织） */
    ui_video_set_audio_format(d, 0, 0);

    printf("[VideoPage] 音频流 #%d: %s %dHz %dch 采样格式%d → 输出 %dHz %dch\n",
           asidx, acodec->name, d->actx->sample_rate, actx_channels(d->actx),
           d->actx->sample_fmt, d->out_rate, d->out_ch);
}

ui_video_decoder_t *ui_video_open(const char *path, int dst_w, int dst_h)
{
    if (!path || !path[0]) return NULL;

    ui_video_decoder_t *d = (ui_video_decoder_t *)av_mallocz(sizeof(*d));
    if (!d) return NULL;

    if (avformat_open_input(&d->fmt, path, NULL, NULL) < 0) {
        printf("[VideoPage] 打不开文件: %s\n", path);
        av_free(d);
        return NULL;
    }
    if (avformat_find_stream_info(d->fmt, NULL) < 0) {
        printf("[VideoPage] 读不到流信息\n");
        goto fail;
    }

    const AVCodec *codec = NULL;
    int sidx = av_find_best_stream(d->fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
    if (sidx < 0 || !codec) {
        printf("[VideoPage] 文件里没有视频流\n");
        goto fail;
    }
    d->stream_index = sidx;

    d->ctx = avcodec_alloc_context3(codec);
    if (!d->ctx) goto fail;
    if (avcodec_parameters_to_context(d->ctx, d->fmt->streams[sidx]->codecpar) < 0) goto fail;
    if (avcodec_open2(d->ctx, codec, NULL) < 0) {
        printf("[VideoPage] 解码器打不开 (%s)\n", codec->name);
        goto fail;
    }

    d->frm = av_frame_alloc();
    d->pkt = av_packet_alloc();
    if (!d->frm || !d->pkt) goto fail;

    d->src_w = d->ctx->width;
    d->src_h = d->ctx->height;
    d->dst_w = (dst_w > 0) ? dst_w : d->src_w;
    d->dst_h = (dst_h > 0) ? dst_h : d->src_h;

    AVRational fr = d->fmt->streams[sidx]->avg_frame_rate;
    if (fr.num > 0 && fr.den > 0)
        d->fps = (float)fr.num / (float)fr.den;
    else
        d->fps = 25.0f;

    if (d->fmt->duration > 0)
        d->duration = (double)d->fmt->duration / AV_TIME_BASE;
    if (d->duration > 0 && d->fps > 0)
        d->frame_count = (int)(d->duration * d->fps + 0.5);

    audio_open(d);      /* 有音频流就把音频解码器也准备好（没有也不算错） */
    au_dbg = getenv("ECHO_VIDEO_AUDEBUG") ? 1 : 0;

    printf("[VideoPage] 打开成功: %dx%d %.2ffps 时长%.1fs -> 输出 %dx%d\n",
           d->src_w, d->src_h, d->fps, d->duration, d->dst_w, d->dst_h);
    return d;

fail:
    ui_video_close(d);
    return NULL;
}

int   ui_video_width(ui_video_decoder_t *d)  { return d ? d->dst_w : 0; }
int   ui_video_height(ui_video_decoder_t *d) { return d ? d->dst_h : 0; }
float ui_video_fps(ui_video_decoder_t *d)    { return d ? d->fps : 0.0f; }
double ui_video_duration(ui_video_decoder_t *d) { return d ? d->duration : 0.0; }
int   ui_video_frame_count(ui_video_decoder_t *d) { return d ? d->frame_count : 0; }

/* 把一帧解码结果转成 RGB565 写进 dst */
static int convert_frame(ui_video_decoder_t *d, void *dst)
{
    if (!d->sws) {
        d->sws = sws_getContext(d->src_w, d->src_h, d->ctx->pix_fmt,
                                d->dst_w, d->dst_h, AV_PIX_FMT_RGB565LE,
                                SWS_FAST_BILINEAR, NULL, NULL, NULL);
        if (!d->sws) {
            printf("[VideoPage] sws 初始化失败\n");
            return -1;
        }
    }
    uint8_t *dst_data[4] = { (uint8_t *)dst, NULL, NULL, NULL };
    int      dst_lines[4] = { d->dst_w * 2, 0, 0, 0 };
    sws_scale(d->sws, (const uint8_t * const *)d->frm->data, d->frm->linesize,
              0, d->src_h, dst_data, dst_lines);

    /* 记录这一帧的时间戳（进度条要用） */
    if (d->frm->best_effort_timestamp != AV_NOPTS_VALUE) {
        AVRational tb = d->fmt->streams[d->stream_index]->time_base;
        double t = d->frm->best_effort_timestamp * av_q2d(tb);
        if (t >= 0) d->pos = t;
    }
    return 1;
}

int ui_video_read_frame(ui_video_decoder_t *d, void *dst)
{
    if (!d || !dst) return -1;

    if (d->eof) return 0;

    while (1) {
        int r = avcodec_receive_frame(d->ctx, d->frm);
        if (r == 0) {
            d->got_first_frame = 1;
            if (convert_frame(d, dst) < 0) return -1;
            /* seek 落点补偿：关键帧间隔大时（比如整段只有一个 I 帧）
             * av_seek_frame 只能跳到关键帧，这里把多余的前面几帧吃掉，
             * 否则用户拖到 60% 会从头开始放。 */
            if (d->skip_until > 0 && d->pos < d->skip_until) continue;
            d->skip_until = 0;
            return 1;
        }
        if (r == AVERROR(EAGAIN)) {
            /* 需要喂新包 */
            int pr = av_read_frame(d->fmt, d->pkt);
            if (pr < 0) {
                d->eof = 1;
                avcodec_send_packet(d->ctx, NULL);   /* 视频 flush */
                pump_audio(d, NULL);                 /* 音频 flush（把尾巴放出来） */
                continue;                            /* 把缓冲里剩下的帧取完 */
            }
            if (d->pkt->stream_index == d->stream_index) {
                if (avcodec_send_packet(d->ctx, d->pkt) < 0)
                    printf("[VideoPage] send_packet 失败\n");
            } else if (d->audio_index >= 0 && d->pkt->stream_index == d->audio_index) {
                /* 音频包：和视频共用同一个 demuxer，所以音画天然对齐 */
                pump_audio(d, d->pkt);
            }
            av_packet_unref(d->pkt);
            continue;
        }
        /* 其它（含 AVERROR_EOF / 解码错误） */
        d->eof = 1;
        return 0;
    }
}

/* 音频解码器 + 重采样器复位（seek/rewind 时用，避免放出旧位置的声音） */
static void audio_reset(ui_video_decoder_t *d)
{
    if (d->actx) avcodec_flush_buffers(d->actx);
    if (d->swr) swr_convert(d->swr, NULL, 0, NULL, 0);   /* 丢掉重采样器里攒的样本 */
    d->audio_flushed = 0;
    au_dbg_pkts = 6;
}

void ui_video_rewind(ui_video_decoder_t *d)
{
    if (!d) return;
    /* stream_index < 0（纯音频模式）时用 AV_TIME_BASE 单位、流参数传 -1 */
    int sidx = d->audio_only ? -1 : d->stream_index;
    if (av_seek_frame(d->fmt, sidx, 0, AVSEEK_FLAG_BACKWARD) < 0)
        printf("[VideoPage] seek 失败\n");
    if (d->ctx) avcodec_flush_buffers(d->ctx);   /* 纯音频模式没有视频解码器 */
    audio_reset(d);
    d->eof = 0;
    d->skip_until = 0;
    d->pos = 0;
}

/* 纯音频模式：把 AV_TIME_BASE（微秒）单位的时间戳算出来 */
int ui_video_seek(ui_video_decoder_t *d, double seconds)
{
    if (!d) return -1;
    if (seconds < 0) seconds = 0;
    if (d->duration > 0 && seconds > d->duration) seconds = d->duration;

    int sidx;
    int64_t ts;
    if (d->audio_only) {
        sidx = -1;                              /* 让 ffmpeg 按默认时钟找 */
        ts = (int64_t)(seconds * AV_TIME_BASE); /* AV_TIME_BASE = 微秒 */
    } else {
        AVRational tb = d->fmt->streams[d->stream_index]->time_base;
        sidx = d->stream_index;
        ts = (int64_t)(seconds / av_q2d(tb));
    }
    if (av_seek_frame(d->fmt, sidx, ts, AVSEEK_FLAG_BACKWARD) < 0) {
        printf("[VideoPage] seek 到 %.2fs 失败\n", seconds);
        return -1;
    }

    if (d->audio_only) {
        /* 音乐：定位误差听不出来，不用做丢帧补偿 */
        audio_reset(d);
        d->skip_until = 0;
    } else {
        /* 视频：关键帧间隔可能很大（整段只有一个 I 帧），必须把目标之前的帧吃掉，
         * 否则用户拖到 60% 会从头开始放（这是踩过的坑） */
        avcodec_flush_buffers(d->ctx);
        audio_reset(d);
        d->skip_until = seconds;
    }    d->eof = 0;
    d->pos = seconds;
    return 0;
}

/* ---------------- 纯音频（音乐播放） ---------------- */

ui_video_decoder_t *ui_audio_open(const char *path, int rate, int channels)
{
    if (!path || !path[0]) return NULL;

    ui_video_decoder_t *d = (ui_video_decoder_t *)av_mallocz(sizeof(*d));
    if (!d) return NULL;
    d->audio_only = 1;
    d->stream_index = -1;

    if (avformat_open_input(&d->fmt, path, NULL, NULL) < 0) {
        printf("[MusicPage] 打不开文件: %s\n", path);
        av_free(d);
        return NULL;
    }
    if (avformat_find_stream_info(d->fmt, NULL) < 0) {
        printf("[MusicPage] 读不到流信息\n");
        ui_video_close(d);
        return NULL;
    }

    /* ⚠️ 一定要分配 AVPacket：av_read_frame() 的第二个参数不能是 NULL。
     * 视频那边是在 ui_video_open() 里分配的，纯音频这边很容易漏（漏了就是段错误） */
    d->pkt = av_packet_alloc();
    if (!d->pkt) {
        printf("[MusicPage] 分配 packet 失败\n");
        ui_video_close(d);
        return NULL;
    }

    if (d->fmt->duration > 0)
        d->duration = (double)d->fmt->duration / AV_TIME_BASE;

    audio_open(d);                 /* 找音频流（没有就失败） */
    if (d->audio_index < 0) {
        printf("[MusicPage] 文件里没有能解码的音频流: %s\n", path);
        ui_video_close(d);
        return NULL;
    }
    /* 指定输出格式（rate/channels <= 0 表示跟文件走） */
    ui_video_set_audio_format(d, rate, channels);

    printf("[MusicPage] 打开成功: %s  %dHz %dch 时长%.1fs\n",
           path, d->out_rate, d->out_ch, d->duration);
    return d;
}

int ui_audio_read(ui_video_decoder_t *d)
{
    if (!d || d->audio_index < 0) return -1;
    if (d->eof) return 0;

    long before = d->pcm_bytes;
    while (d->pcm_bytes == before) {
        int pr = av_read_frame(d->fmt, d->pkt);
        if (pr < 0) {
            d->eof = 1;
            pump_audio(d, NULL);            /* 把解码器里剩的尾巴放出来 */
            break;
        }
        if (d->pkt->stream_index == d->audio_index)
            pump_audio(d, d->pkt);
        av_packet_unref(d->pkt);
    }
    if (d->pcm_bytes == before) return 0;   /* 到文件尾了 */
    return 1;
}

double ui_video_position(ui_video_decoder_t *d) { return d ? d->pos : 0.0; }

int ui_video_has_audio(ui_video_decoder_t *d)       { return (d && d->audio_index >= 0) ? 1 : 0; }
int ui_video_audio_rate(ui_video_decoder_t *d)      { return d ? d->out_rate : 0; }
int ui_video_audio_channels(ui_video_decoder_t *d)  { return d ? d->out_ch : 0; }

void ui_video_set_pcm_callback(ui_video_decoder_t *d, ui_video_pcm_cb_t cb, void *user)
{
    if (!d) return;
    d->pcm_cb = cb;
    d->pcm_user = user;
}

void ui_video_close(ui_video_decoder_t *d)
{
    if (!d) return;
    if (d->sws) sws_freeContext(d->sws);
    if (d->pkt) av_packet_free(&d->pkt);
    if (d->frm) av_frame_free(&d->frm);
    if (d->ctx) avcodec_free_context(&d->ctx);
    if (d->swr) swr_free(&d->swr);
    if (d->afrm) av_frame_free(&d->afrm);
    if (d->actx) avcodec_free_context(&d->actx);
    if (d->pcm_buf) av_free(d->pcm_buf);
    if (d->fmt) avformat_close_input(&d->fmt);
    av_free(d);
}

#else /* ---------------- 没有 ffmpeg 时的 stub ---------------- */

#include <stdlib.h>

struct ui_video_decoder { int dummy; };

int ui_video_is_supported(void) { return 0; }

ui_video_decoder_t *ui_video_open(const char *path, int dst_w, int dst_h)
{
    (void)path; (void)dst_w; (void)dst_h;
    printf("[VideoPage] 本版本没编进 ffmpeg，无法播放视频\n");
    return NULL;
}
int   ui_video_width(ui_video_decoder_t *d)  { (void)d; return 0; }
int   ui_video_height(ui_video_decoder_t *d) { (void)d; return 0; }
float ui_video_fps(ui_video_decoder_t *d)    { (void)d; return 0; }
double ui_video_duration(ui_video_decoder_t *d) { (void)d; return 0; }
int   ui_video_frame_count(ui_video_decoder_t *d) { (void)d; return 0; }
int   ui_video_read_frame(ui_video_decoder_t *d, void *dst) { (void)d; (void)dst; return -1; }
void  ui_video_rewind(ui_video_decoder_t *d) { (void)d; }
int   ui_video_seek(ui_video_decoder_t *d, double s) { (void)d; (void)s; return -1; }
double ui_video_position(ui_video_decoder_t *d) { (void)d; return 0.0; }
void  ui_video_close(ui_video_decoder_t *d)  { (void)d; }

/* 音频接口的桩：一律「没有音频」 */
ui_video_decoder_t *ui_audio_open(const char *p, int r, int c)
{
    (void)p; (void)r; (void)c;
    return NULL;
}
int  ui_audio_read(ui_video_decoder_t *d) { (void)d; return -1; }
int  ui_video_has_audio(ui_video_decoder_t *d) { (void)d; return 0; }
int  ui_video_audio_rate(ui_video_decoder_t *d) { (void)d; return 0; }
int  ui_video_audio_channels(ui_video_decoder_t *d) { (void)d; return 0; }
void ui_video_set_pcm_callback(ui_video_decoder_t *d, ui_video_pcm_cb_t cb, void *user)
{
    (void)d; (void)cb; (void)user;
}
int  ui_video_set_audio_format(ui_video_decoder_t *d, int r, int ch)
{
    (void)d; (void)r; (void)ch;
    return -1;
}

#endif
