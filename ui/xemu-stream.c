/*
 * xemu network broadcast (audio/video streaming)
 *
 * Encodes the composed display frame to H.264 (libx264, falling back to
 * MPEG-2) muxed into MPEG-TS and sent to a configurable ffmpeg URL such as
 * udp://host:port. All encoding happens on a dedicated worker thread; the
 * render thread only performs a quick copy into a bounded queue.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"

#include "xemu-stream.h"
#include "xemu-settings.h"

#include "qemu/audio-capture.h"
#include "qemu/audio.h"

#include <glib.h>

#if defined(CONFIG_FFMPEG) && !defined(CONFIG_UWP)

#include "qemu/sockets.h"

#include <math.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libswscale/swscale.h>

#define STREAM_MAX_OUTPUT_WIDTH 1280
#define STREAM_MAX_OUTPUT_HEIGHT 720
#define STREAM_QUEUE_MAX 3
#define STREAM_FRAME_POOL_MAX (STREAM_QUEUE_MAX + 1)
#define STREAM_CONTROL_BUF 64
#define STREAM_AUDIO_RATE 48000
#define STREAM_AUDIO_CHANNELS 2
#define STREAM_AUDIO_BYTES_PER_FRAME (STREAM_AUDIO_CHANNELS * 2)
#define STREAM_AUDIO_QUEUE_MAX_BYTES \
    (STREAM_AUDIO_RATE * STREAM_AUDIO_BYTES_PER_FRAME * 2)

typedef struct StreamFrame {
    uint8_t *data; /* tightly packed BGRA, pitch == width*4 */
    size_t capacity;
    int width;
    int height;
} StreamFrame;

typedef struct StreamAudio {
    uint8_t *data; /* signed 16-bit little-endian stereo PCM */
    size_t size;
} StreamAudio;

/* Snapshot of the settings a running session was started with. */
typedef struct StreamParams {
    char url[512];
    int fps;
    int bitrate_kbps;
} StreamParams;

/* Encoder state, owned exclusively by the worker thread. */
typedef struct StreamEncoder {
    StreamParams params;
    AVFormatContext *fmt;
    AVCodecContext *cc;
    AVStream *st;
    AVCodecContext *audio_cc;
    AVStream *audio_st;
    AVFrame *frame;
    AVFrame *audio_frame;
    AVPacket *pkt;
    struct SwsContext *sws;
    int sws_src_w;
    int sws_src_h;
    int out_w;
    int out_h;
    int64_t pts;
    int64_t audio_pts;
    GByteArray *audio_pcm;
    bool header_written;
} StreamEncoder;

static GMutex s_lock;
static GCond s_cond;
static GQueue *s_queue; /* of StreamFrame* */
static GQueue *s_frame_pool; /* reusable StreamFrame buffers */
static GQueue *s_audio_queue; /* of StreamAudio* */
static size_t s_audio_queue_bytes;
static CaptureVoiceOut *s_audio_capture;

static GThread *s_worker;
static volatile bool s_worker_run; /* worker should keep spinning */
static volatile bool s_active;     /* encoder alive and accepting frames */
static bool s_ready;               /* encoders and MPEG-TS mux initialized */
static bool s_start_failed;        /* last start attempt failed; wait for change */

static bool s_remote_active; /* a remote/manual START is in effect */
static bool s_remote_override;
static StreamParams s_remote_params;

static GThread *s_control;
static volatile bool s_control_run;
static int s_control_port_running;

static StreamParams s_running_params; /* params of the live session */

static char s_status[256];
static int64_t s_last_capture_us;
static int64_t s_frame_interval_us;
static int s_output_width;
static int s_output_height;

static bool s_initialized;

static void set_status(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    g_mutex_lock(&s_lock);
    vsnprintf(s_status, sizeof(s_status), fmt, ap);
    g_mutex_unlock(&s_lock);
    va_end(ap);
}

static void stream_frame_free(StreamFrame *f)
{
    if (f == NULL) {
        return;
    }
    g_free(f->data);
    g_free(f);
}

static StreamFrame *stream_frame_acquire(size_t size)
{
    StreamFrame *f = NULL;

    g_mutex_lock(&s_lock);
    if (s_frame_pool != NULL) {
        f = (StreamFrame *)g_queue_pop_head(s_frame_pool);
    }
    g_mutex_unlock(&s_lock);

    if (f == NULL) {
        f = g_new0(StreamFrame, 1);
    }
    if (f->capacity != size) {
        f->data = g_realloc(f->data, size);
        f->capacity = size;
    }
    return f;
}

static void stream_frame_recycle_locked(StreamFrame *f)
{
    if (f == NULL) {
        return;
    }
    f->width = 0;
    f->height = 0;
    if (s_frame_pool != NULL &&
        g_queue_get_length(s_frame_pool) < STREAM_FRAME_POOL_MAX) {
        g_queue_push_tail(s_frame_pool, f);
    } else {
        stream_frame_free(f);
    }
}

static void stream_frame_recycle(StreamFrame *f)
{
    g_mutex_lock(&s_lock);
    stream_frame_recycle_locked(f);
    g_mutex_unlock(&s_lock);
}

static void stream_audio_free(StreamAudio *audio)
{
    if (audio == NULL) {
        return;
    }
    g_free(audio->data);
    g_free(audio);
}

static void audio_capture_notify(void *opaque, audcnotification_e cmd)
{
    (void)opaque;
    (void)cmd;
}

static void audio_capture_destroy(void *opaque)
{
    (void)opaque;
}

static void audio_capture_pcm(void *opaque, const void *buf, int size)
{
    StreamAudio *audio;

    (void)opaque;
    if (buf == NULL || size <= 0 || !s_initialized) {
        return;
    }

    audio = g_new0(StreamAudio, 1);
    audio->data = g_memdup2(buf, (size_t)size);
    audio->size = (size_t)size;

    g_mutex_lock(&s_lock);
    if (!s_active || s_audio_queue == NULL) {
        g_mutex_unlock(&s_lock);
        stream_audio_free(audio);
        return;
    }

    /* Prefer current audio to stale audio when the network/encoder falls
     * behind. This keeps controller-to-sound latency bounded. */
    while (!g_queue_is_empty(s_audio_queue) &&
           s_audio_queue_bytes + audio->size >
               STREAM_AUDIO_QUEUE_MAX_BYTES) {
        StreamAudio *old = g_queue_pop_head(s_audio_queue);
        s_audio_queue_bytes -= old->size;
        stream_audio_free(old);
    }
    if (audio->size <= STREAM_AUDIO_QUEUE_MAX_BYTES) {
        g_queue_push_tail(s_audio_queue, audio);
        s_audio_queue_bytes += audio->size;
        g_cond_signal(&s_cond);
        audio = NULL;
    }
    g_mutex_unlock(&s_lock);
    stream_audio_free(audio);
}

static void ensure_audio_capture(void)
{
    struct audsettings settings;
    struct audio_capture_ops ops;
    AudioBackend *backend;

    if (s_audio_capture != NULL) {
        return;
    }
    backend = audio_get_default_audio_be(NULL);
    if (backend == NULL) {
        return;
    }

    settings.freq = STREAM_AUDIO_RATE;
    settings.nchannels = STREAM_AUDIO_CHANNELS;
    settings.fmt = AUDIO_FORMAT_S16;
    settings.endianness = 0;
    ops.notify = audio_capture_notify;
    ops.capture = audio_capture_pcm;
    ops.destroy = audio_capture_destroy;
    s_audio_capture = AUD_add_capture(backend, &settings, &ops, NULL);
}

static void snapshot_config_params(StreamParams *out)
{
    const char *url = g_config.display.stream.url;
    int fps = g_config.display.stream.fps;
    int bitrate = g_config.display.stream.bitrate_kbps;

    if (url == NULL || url[0] == '\0') {
        url = "udp://192.168.1.22:26960";
    }
    g_strlcpy(out->url, url, sizeof(out->url));
    out->fps = (fps >= 1 && fps <= 120) ? fps : 30;
    out->bitrate_kbps = (bitrate >= 100 && bitrate <= 100000) ? bitrate : 6000;
}

static void snapshot_params(StreamParams *out)
{
    snapshot_config_params(out);
    g_mutex_lock(&s_lock);
    if (s_remote_active && s_remote_override) {
        *out = s_remote_params;
    }
    g_mutex_unlock(&s_lock);
}

static bool params_differ(const StreamParams *a, const StreamParams *b)
{
    return strcmp(a->url, b->url) != 0 || a->fps != b->fps ||
           a->bitrate_kbps != b->bitrate_kbps;
}

/* ------------------------------------------------------------------ */
/* Encoder (worker thread)                                            */
/* ------------------------------------------------------------------ */

static void compute_output_size(int src_w, int src_h,
                                int *out_w, int *out_h)
{
    double sx = (double)STREAM_MAX_OUTPUT_WIDTH / (double)src_w;
    double sy = (double)STREAM_MAX_OUTPUT_HEIGHT / (double)src_h;
    double scale = sx < sy ? sx : sy;
    int w;
    int h;

    if (scale > 1.0) {
        scale = 1.0; /* never upscale */
    }

    w = (int)lround((double)src_w * scale) & ~1;
    h = (int)lround((double)src_h * scale) & ~1;
    if (w < 16) {
        w = 16;
    }
    if (h < 16) {
        h = 16;
    }
    *out_w = w;
    *out_h = h;
}

static void encoder_close(StreamEncoder *e)
{
    if (e->fmt != NULL && e->header_written) {
        /* Flush the encoder and finalize the container. */
        if (e->cc != NULL) {
            avcodec_send_frame(e->cc, NULL);
            for (;;) {
                int ret = avcodec_receive_packet(e->cc, e->pkt);
                if (ret < 0) {
                    break;
                }
                av_packet_rescale_ts(e->pkt, e->cc->time_base,
                                     e->st->time_base);
                e->pkt->stream_index = e->st->index;
                av_interleaved_write_frame(e->fmt, e->pkt);
                av_packet_unref(e->pkt);
            }
        }
        if (e->audio_cc != NULL) {
            avcodec_send_frame(e->audio_cc, NULL);
            for (;;) {
                int ret = avcodec_receive_packet(e->audio_cc, e->pkt);
                if (ret < 0) {
                    break;
                }
                av_packet_rescale_ts(e->pkt, e->audio_cc->time_base,
                                     e->audio_st->time_base);
                e->pkt->stream_index = e->audio_st->index;
                av_interleaved_write_frame(e->fmt, e->pkt);
                av_packet_unref(e->pkt);
            }
        }
        av_write_trailer(e->fmt);
    }

    if (e->sws != NULL) {
        sws_freeContext(e->sws);
        e->sws = NULL;
    }
    if (e->frame != NULL) {
        av_frame_free(&e->frame);
    }
    if (e->audio_frame != NULL) {
        av_frame_free(&e->audio_frame);
    }
    if (e->pkt != NULL) {
        av_packet_free(&e->pkt);
    }
    if (e->cc != NULL) {
        avcodec_free_context(&e->cc);
    }
    if (e->audio_cc != NULL) {
        avcodec_free_context(&e->audio_cc);
    }
    if (e->audio_pcm != NULL) {
        g_byte_array_unref(e->audio_pcm);
        e->audio_pcm = NULL;
    }
    if (e->fmt != NULL) {
        if (e->fmt->pb != NULL && !(e->fmt->oformat->flags & AVFMT_NOFILE)) {
            avio_closep(&e->fmt->pb);
        }
        avformat_free_context(e->fmt);
        e->fmt = NULL;
    }
    e->header_written = false;
}

static bool encoder_open(StreamEncoder *e, int src_w, int src_h)
{
    const AVCodec *codec = NULL;
    const AVCodec *audio_codec = NULL;
    char errbuf[256];
    int ret;

    compute_output_size(src_w, src_h, &e->out_w, &e->out_h);

    codec = avcodec_find_encoder_by_name("libx264");
    if (codec == NULL) {
        codec = avcodec_find_encoder(AV_CODEC_ID_MPEG2VIDEO);
    }
    if (codec == NULL) {
        set_status("stream error: no H.264/MPEG-2 encoder available");
        return false;
    }

    ret = avformat_alloc_output_context2(&e->fmt, NULL, "mpegts",
                                         e->params.url);
    if (ret < 0 || e->fmt == NULL) {
        av_strerror(ret, errbuf, sizeof(errbuf));
        set_status("stream error: bad output url '%s': %s", e->params.url,
                   errbuf);
        return false;
    }

    e->st = avformat_new_stream(e->fmt, NULL);
    if (e->st == NULL) {
        set_status("stream error: could not allocate output stream");
        return false;
    }

    e->cc = avcodec_alloc_context3(codec);
    if (e->cc == NULL) {
        set_status("stream error: could not allocate encoder context");
        return false;
    }

    e->cc->width = e->out_w;
    e->cc->height = e->out_h;
    e->cc->pix_fmt = AV_PIX_FMT_YUV420P;
    e->cc->time_base.num = 1;
    e->cc->time_base.den = e->params.fps;
    e->cc->framerate.num = e->params.fps;
    e->cc->framerate.den = 1;
    e->cc->gop_size = e->params.fps; /* ~1s keyframe interval */
    e->cc->max_b_frames = 0;
    e->cc->bit_rate = (int64_t)e->params.bitrate_kbps * 1000;
    e->cc->rc_max_rate = e->cc->bit_rate;
    e->cc->rc_buffer_size = (int)e->cc->bit_rate;

    if (e->fmt->oformat->flags & AVFMT_GLOBALHEADER) {
        e->cc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }

    if (strcmp(codec->name, "libx264") == 0) {
        av_opt_set(e->cc->priv_data, "preset", "ultrafast", 0);
        av_opt_set(e->cc->priv_data, "tune", "zerolatency", 0);
    }

    ret = avcodec_open2(e->cc, codec, NULL);
    if (ret < 0) {
        av_strerror(ret, errbuf, sizeof(errbuf));
        set_status("stream error: encoder open failed: %s", errbuf);
        return false;
    }

    ret = avcodec_parameters_from_context(e->st->codecpar, e->cc);
    if (ret < 0) {
        set_status("stream error: could not copy codec parameters");
        return false;
    }
    e->st->time_base = e->cc->time_base;

    audio_codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
    if (audio_codec == NULL) {
        audio_codec = avcodec_find_encoder(AV_CODEC_ID_MP2);
    }
    if (audio_codec == NULL) {
        set_status("stream error: no AAC/MPEG audio encoder available");
        return false;
    }
    e->audio_st = avformat_new_stream(e->fmt, NULL);
    e->audio_cc = avcodec_alloc_context3(audio_codec);
    if (e->audio_st == NULL || e->audio_cc == NULL) {
        set_status("stream error: could not allocate audio encoder");
        return false;
    }
    e->audio_cc->sample_rate = STREAM_AUDIO_RATE;
    e->audio_cc->sample_fmt = audio_codec->id == AV_CODEC_ID_AAC
                                  ? AV_SAMPLE_FMT_FLTP
                                  : AV_SAMPLE_FMT_S16;
    e->audio_cc->time_base = (AVRational){ 1, STREAM_AUDIO_RATE };
    e->audio_cc->bit_rate = 128000;
    av_channel_layout_default(&e->audio_cc->ch_layout,
                              STREAM_AUDIO_CHANNELS);
    if (e->fmt->oformat->flags & AVFMT_GLOBALHEADER) {
        e->audio_cc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }
    ret = avcodec_open2(e->audio_cc, audio_codec, NULL);
    if (ret < 0) {
        av_strerror(ret, errbuf, sizeof(errbuf));
        set_status("stream error: audio encoder open failed: %s", errbuf);
        return false;
    }
    ret = avcodec_parameters_from_context(e->audio_st->codecpar,
                                          e->audio_cc);
    if (ret < 0) {
        set_status("stream error: could not copy audio codec parameters");
        return false;
    }
    e->audio_st->time_base = e->audio_cc->time_base;

    if (!(e->fmt->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open(&e->fmt->pb, e->params.url, AVIO_FLAG_WRITE);
        if (ret < 0) {
            av_strerror(ret, errbuf, sizeof(errbuf));
            set_status("stream error: cannot open '%s': %s", e->params.url,
                       errbuf);
            return false;
        }
    }

    ret = avformat_write_header(e->fmt, NULL);
    if (ret < 0) {
        av_strerror(ret, errbuf, sizeof(errbuf));
        set_status("stream error: write header failed: %s", errbuf);
        return false;
    }
    e->header_written = true;

    e->frame = av_frame_alloc();
    e->audio_frame = av_frame_alloc();
    e->pkt = av_packet_alloc();
    if (e->frame == NULL || e->audio_frame == NULL || e->pkt == NULL) {
        set_status("stream error: could not allocate frame/packet");
        return false;
    }
    e->frame->format = AV_PIX_FMT_YUV420P;
    e->frame->width = e->out_w;
    e->frame->height = e->out_h;
    if (av_frame_get_buffer(e->frame, 0) < 0) {
        set_status("stream error: could not allocate frame buffer");
        return false;
    }

    e->audio_frame->format = e->audio_cc->sample_fmt;
    e->audio_frame->sample_rate = e->audio_cc->sample_rate;
    e->audio_frame->nb_samples = e->audio_cc->frame_size > 0
                                     ? e->audio_cc->frame_size
                                     : 1024;
    if (av_channel_layout_copy(&e->audio_frame->ch_layout,
                               &e->audio_cc->ch_layout) < 0 ||
        av_frame_get_buffer(e->audio_frame, 0) < 0) {
        set_status("stream error: could not allocate audio frame buffer");
        return false;
    }
    e->audio_pcm = g_byte_array_new();

    set_status("streaming %s (%dx%d @ %d fps, %s + %s audio)",
               e->params.url, e->out_w, e->out_h, e->params.fps,
               codec->name, audio_codec->name);
    return true;
}

static bool encoder_write_frame(StreamEncoder *e, StreamFrame *f)
{
    const uint8_t *src_slices[4] = { f->data, NULL, NULL, NULL };
    int src_stride[4] = { f->width * 4, 0, 0, 0 };
    int ret;

    if (e->sws == NULL || e->sws_src_w != f->width ||
        e->sws_src_h != f->height) {
        if (e->sws != NULL) {
            sws_freeContext(e->sws);
        }
        e->sws = sws_getContext(f->width, f->height, AV_PIX_FMT_BGRA,
                                e->out_w, e->out_h, AV_PIX_FMT_YUV420P,
                                SWS_BILINEAR, NULL, NULL, NULL);
        if (e->sws == NULL) {
            set_status("stream error: could not create scaler");
            return false;
        }
        e->sws_src_w = f->width;
        e->sws_src_h = f->height;
    }

    if (av_frame_make_writable(e->frame) < 0) {
        return false;
    }

    sws_scale(e->sws, src_slices, src_stride, 0, f->height, e->frame->data,
              e->frame->linesize);

    e->frame->pts = e->pts++;

    ret = avcodec_send_frame(e->cc, e->frame);
    if (ret < 0) {
        return false;
    }

    for (;;) {
        ret = avcodec_receive_packet(e->cc, e->pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        }
        if (ret < 0) {
            return false;
        }
        av_packet_rescale_ts(e->pkt, e->cc->time_base, e->st->time_base);
        e->pkt->stream_index = e->st->index;
        ret = av_interleaved_write_frame(e->fmt, e->pkt);
        av_packet_unref(e->pkt);
        if (ret < 0) {
            set_status("stream error: send failed (network?)");
            return false;
        }
    }

    return true;
}

static bool encoder_write_audio_frame(StreamEncoder *e, const int16_t *pcm)
{
    int samples = e->audio_frame->nb_samples;
    int ret;

    if (av_frame_make_writable(e->audio_frame) < 0) {
        return false;
    }

    switch (e->audio_cc->sample_fmt) {
    case AV_SAMPLE_FMT_FLTP: {
        float *left = (float *)e->audio_frame->data[0];
        float *right = (float *)e->audio_frame->data[1];
        for (int i = 0; i < samples; i++) {
            left[i] = pcm[i * 2] / 32768.0f;
            right[i] = pcm[i * 2 + 1] / 32768.0f;
        }
        break;
    }
    case AV_SAMPLE_FMT_S16:
        memcpy(e->audio_frame->data[0], pcm,
               (size_t)samples * STREAM_AUDIO_BYTES_PER_FRAME);
        break;
    case AV_SAMPLE_FMT_S16P: {
        int16_t *left = (int16_t *)e->audio_frame->data[0];
        int16_t *right = (int16_t *)e->audio_frame->data[1];
        for (int i = 0; i < samples; i++) {
            left[i] = pcm[i * 2];
            right[i] = pcm[i * 2 + 1];
        }
        break;
    }
    default:
        set_status("stream error: unsupported audio sample format %s",
                   av_get_sample_fmt_name(e->audio_cc->sample_fmt));
        return false;
    }

    e->audio_frame->pts = e->audio_pts;
    e->audio_pts += samples;
    ret = avcodec_send_frame(e->audio_cc, e->audio_frame);
    if (ret < 0) {
        return false;
    }

    for (;;) {
        ret = avcodec_receive_packet(e->audio_cc, e->pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        }
        if (ret < 0) {
            return false;
        }
        av_packet_rescale_ts(e->pkt, e->audio_cc->time_base,
                             e->audio_st->time_base);
        e->pkt->stream_index = e->audio_st->index;
        ret = av_interleaved_write_frame(e->fmt, e->pkt);
        av_packet_unref(e->pkt);
        if (ret < 0) {
            set_status("stream error: audio send failed (network?)");
            return false;
        }
    }
    return true;
}

static bool encoder_write_audio(StreamEncoder *e, const StreamAudio *audio)
{
    size_t frame_bytes;

    if (audio == NULL || audio->size == 0) {
        return true;
    }
    g_byte_array_append(e->audio_pcm, audio->data, (guint)audio->size);
    frame_bytes = (size_t)e->audio_frame->nb_samples *
                  STREAM_AUDIO_BYTES_PER_FRAME;
    while (e->audio_pcm->len >= frame_bytes) {
        if (!encoder_write_audio_frame(
                e, (const int16_t *)e->audio_pcm->data)) {
            return false;
        }
        g_byte_array_remove_range(e->audio_pcm, 0, (guint)frame_bytes);
    }
    return true;
}

static gpointer worker_main(gpointer data)
{
    StreamEncoder e;

    (void)data;
    memset(&e, 0, sizeof(e));
    g_mutex_lock(&s_lock);
    e.params = s_running_params;
    g_mutex_unlock(&s_lock);

    for (;;) {
        StreamFrame *f = NULL;
        StreamAudio *audio = NULL;

        g_mutex_lock(&s_lock);
        while (s_worker_run && g_queue_is_empty(s_queue) &&
               (e.fmt == NULL || g_queue_is_empty(s_audio_queue))) {
            g_cond_wait(&s_cond, &s_lock);
        }
        if (!s_worker_run && g_queue_is_empty(s_queue) &&
            (e.fmt == NULL || g_queue_is_empty(s_audio_queue))) {
            g_mutex_unlock(&s_lock);
            break;
        }
        f = (StreamFrame *)g_queue_pop_head(s_queue);
        if (e.fmt != NULL && !g_queue_is_empty(s_audio_queue)) {
            audio = (StreamAudio *)g_queue_pop_head(s_audio_queue);
            s_audio_queue_bytes -= audio->size;
        }
        g_mutex_unlock(&s_lock);

        if (f != NULL && e.fmt == NULL) {
            if (!encoder_open(&e, f->width, f->height)) {
                stream_frame_recycle(f);
                stream_audio_free(audio);
                /* fatal: stop accepting frames and exit */
                g_mutex_lock(&s_lock);
                s_active = false;
                s_ready = false;
                s_worker_run = false;
                g_mutex_unlock(&s_lock);
                break;
            }

            g_mutex_lock(&s_lock);
            s_ready = true;
            s_output_width = e.out_w;
            s_output_height = e.out_h;
            g_mutex_unlock(&s_lock);

            /* Audio may have arrived while waiting for the first video frame.
             * Pull one queued block now that the muxer is initialized. */
            if (audio == NULL) {
                g_mutex_lock(&s_lock);
                audio = (StreamAudio *)g_queue_pop_head(s_audio_queue);
                if (audio != NULL) {
                    s_audio_queue_bytes -= audio->size;
                }
                g_mutex_unlock(&s_lock);
            }
        }

        if ((f != NULL && !encoder_write_frame(&e, f)) ||
            (audio != NULL && !encoder_write_audio(&e, audio))) {
            stream_frame_recycle(f);
            stream_audio_free(audio);
            g_mutex_lock(&s_lock);
            s_active = false;
            s_ready = false;
            s_worker_run = false;
            g_mutex_unlock(&s_lock);
            break;
        }

        stream_frame_recycle(f);
        stream_audio_free(audio);
    }

    encoder_close(&e);

    /* Drain any leftover queued frames. */
    g_mutex_lock(&s_lock);
    for (;;) {
        StreamFrame *f = (StreamFrame *)g_queue_pop_head(s_queue);
        if (f == NULL) {
            break;
        }
        stream_frame_recycle_locked(f);
    }
    for (;;) {
        StreamAudio *audio = (StreamAudio *)g_queue_pop_head(s_audio_queue);
        if (audio == NULL) {
            break;
        }
        stream_audio_free(audio);
    }
    s_audio_queue_bytes = 0;
    s_active = false;
    s_ready = false;
    g_mutex_unlock(&s_lock);

    return NULL;
}

/* ------------------------------------------------------------------ */
/* Remote-start control channel (worker thread)                       */
/* ------------------------------------------------------------------ */

static gpointer control_main(gpointer data)
{
    int port = GPOINTER_TO_INT(data);
    int fd;
    struct sockaddr_in addr;

    fd = qemu_socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return NULL;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd); /* close is wrapped to handle socket fds on win32 */
        return NULL;
    }

    /* 500ms receive timeout so the loop observes s_control_run changes. */
#ifdef _WIN32
    {
        DWORD timeout_ms = 500;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout_ms,
                   sizeof(timeout_ms));
    }
#else
    {
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 500 * 1000;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }
#endif

    while (s_control_run) {
        char buf[STREAM_CONTROL_BUF];
        int n = recv(fd, buf, sizeof(buf) - 1, 0);
        if (n <= 0) {
            continue; /* timeout or error */
        }
        buf[n] = '\0';
        g_strstrip(buf);
        if (g_ascii_strncasecmp(buf, "START", 5) == 0) {
            xemu_stream_request_start();
        } else if (g_ascii_strncasecmp(buf, "STOP", 4) == 0) {
            xemu_stream_request_stop();
        }
    }

    close(fd);
    return NULL;
}

static void control_start(int port)
{
    if (s_control != NULL) {
        return;
    }
    s_control_run = true;
    s_control_port_running = port;
    s_control = g_thread_new("xemu-stream-ctl", control_main,
                             GINT_TO_POINTER(port));
}

static void control_stop(void)
{
    if (s_control == NULL) {
        return;
    }
    s_control_run = false;
    g_thread_join(s_control);
    s_control = NULL;
    s_control_port_running = 0;
}

/* ------------------------------------------------------------------ */
/* Worker lifecycle                                                   */
/* ------------------------------------------------------------------ */

static void worker_start(void)
{
    StreamAudio *audio;
    StreamParams params;

    snapshot_params(&params);
    g_mutex_lock(&s_lock);
    while ((audio = (StreamAudio *)g_queue_pop_head(s_audio_queue)) != NULL) {
        stream_audio_free(audio);
    }
    s_audio_queue_bytes = 0;
    s_running_params = params;
    s_frame_interval_us = 1000000 / (s_running_params.fps >= 1 ?
                                     s_running_params.fps : 30);
    s_last_capture_us = 0;
    s_worker_run = true;
    s_active = true;
    s_ready = false;
    s_start_failed = false;
    g_mutex_unlock(&s_lock);

    set_status("starting stream to %s...", s_running_params.url);
    s_worker = g_thread_new("xemu-stream", worker_main, NULL);
}

static void worker_stop(void)
{
    if (s_worker == NULL) {
        return;
    }
    g_mutex_lock(&s_lock);
    s_active = false;
    s_ready = false;
    s_worker_run = false;
    g_cond_signal(&s_cond);
    g_mutex_unlock(&s_lock);
    g_thread_join(s_worker);
    s_worker = NULL;
    if (!s_start_failed) {
        set_status("stream stopped");
    }
}

/* ------------------------------------------------------------------ */
/* Public API                                                         */
/* ------------------------------------------------------------------ */

void xemu_stream_init(void)
{
    if (s_initialized) {
        return;
    }
    g_mutex_init(&s_lock);
    g_cond_init(&s_cond);
    s_queue = g_queue_new();
    s_frame_pool = g_queue_new();
    s_audio_queue = g_queue_new();
    s_ready = false;
    s_remote_active = false;
    s_remote_override = false;
    av_log_set_level(AV_LOG_ERROR);
    g_strlcpy(s_status, "stream idle", sizeof(s_status));
    s_initialized = true;
    ensure_audio_capture();
}

void xemu_stream_shutdown(void)
{
    if (!s_initialized) {
        return;
    }
    control_stop();
    worker_stop();
    if (s_audio_capture != NULL) {
        AUD_del_capture(s_audio_capture, NULL);
        s_audio_capture = NULL;
    }
    if (s_queue != NULL) {
        g_queue_free_full(s_queue, (GDestroyNotify)stream_frame_free);
        s_queue = NULL;
    }
    if (s_frame_pool != NULL) {
        g_queue_free_full(s_frame_pool, (GDestroyNotify)stream_frame_free);
        s_frame_pool = NULL;
    }
    if (s_audio_queue != NULL) {
        g_queue_free_full(s_audio_queue, (GDestroyNotify)stream_audio_free);
        s_audio_queue = NULL;
    }
    s_audio_queue_bytes = 0;
    s_remote_active = false;
    s_remote_override = false;
    s_initialized = false;
}

void xemu_stream_request_start(void)
{
    if (!s_initialized) {
        return;
    }
    g_mutex_lock(&s_lock);
    s_remote_active = true;
    s_remote_override = false;
    s_start_failed = false;
    g_mutex_unlock(&s_lock);
}

void xemu_stream_request_start_with_params(const char *url,
                                           int bitrate_kbps, int fps)
{
    StreamParams params;

    if (!s_initialized || url == NULL || url[0] == '\0') {
        return;
    }
    snapshot_config_params(&params);
    g_strlcpy(params.url, url, sizeof(params.url));
    if (bitrate_kbps >= 100 && bitrate_kbps <= 100000) {
        params.bitrate_kbps = bitrate_kbps;
    }
    if (fps >= 1 && fps <= 120) {
        params.fps = fps;
    }
    g_mutex_lock(&s_lock);
    s_remote_params = params;
    s_remote_override = true;
    s_remote_active = true;
    s_start_failed = false;
    g_mutex_unlock(&s_lock);
}

void xemu_stream_request_stop(void)
{
    if (!s_initialized) {
        return;
    }
    g_mutex_lock(&s_lock);
    s_remote_active = false;
    s_remote_override = false;
    g_mutex_unlock(&s_lock);
}

bool xemu_stream_is_active(void)
{
    bool active;

    g_mutex_lock(&s_lock);
    active = s_active;
    g_mutex_unlock(&s_lock);
    return active;
}

bool xemu_stream_is_ready(void)
{
    bool ready;

    g_mutex_lock(&s_lock);
    ready = s_ready;
    g_mutex_unlock(&s_lock);
    return ready;
}

bool xemu_stream_get_output_size(int *width, int *height)
{
    bool valid;

    if (width == NULL || height == NULL) {
        return false;
    }
    g_mutex_lock(&s_lock);
    *width = s_output_width;
    *height = s_output_height;
    valid = s_ready && *width > 0 && *height > 0;
    g_mutex_unlock(&s_lock);
    return valid;
}

bool xemu_stream_is_supported(void)
{
    return true;
}

bool xemu_stream_wants_frame(void)
{
    bool wants;
    int64_t now;

    if (!s_initialized) {
        return false;
    }
    now = g_get_monotonic_time();

    g_mutex_lock(&s_lock);
    wants = s_active && s_queue != NULL &&
            (s_frame_interval_us <= 0 ||
             now - s_last_capture_us >= s_frame_interval_us) &&
            g_queue_get_length(s_queue) < STREAM_QUEUE_MAX;
    if (wants) {
        /* Claim cadence when the GPU copy is recorded, not several frames
         * later when its fence completes. */
        s_last_capture_us = now;
    }
    g_mutex_unlock(&s_lock);
    return wants;
}

void xemu_stream_submit_bgra(const uint8_t *bgra, int width, int height,
                             int pitch)
{
    StreamFrame *f;
    int y;

    if (bgra == NULL || width <= 0 || height <= 0 || pitch < width * 4) {
        return;
    }

    g_mutex_lock(&s_lock);
    if (!s_active) {
        g_mutex_unlock(&s_lock);
        return;
    }
    if (g_queue_get_length(s_queue) >= STREAM_QUEUE_MAX) {
        g_mutex_unlock(&s_lock);
        return; /* backpressure: drop this frame */
    }
    g_mutex_unlock(&s_lock);

    f = stream_frame_acquire((size_t)width * height * 4);
    f->width = width;
    f->height = height;
    for (y = 0; y < height; y++) {
        memcpy(f->data + (size_t)y * width * 4,
               bgra + (size_t)y * pitch, (size_t)width * 4);
    }

    g_mutex_lock(&s_lock);
    if (!s_active || g_queue_get_length(s_queue) >= STREAM_QUEUE_MAX) {
        g_mutex_unlock(&s_lock);
        stream_frame_recycle(f);
        return;
    }
    g_queue_push_tail(s_queue, f);
    g_cond_signal(&s_cond);
    g_mutex_unlock(&s_lock);
}

void xemu_stream_submit_bgra_borrowed(const uint8_t *bgra, int width,
                                      int height, int pitch,
                                      int64_t capture_us,
                                      XemuStreamFrameRelease release,
                                      void *opaque)
{
    (void)capture_us;
    /* The desktop FFmpeg backend retains its existing owned queue. This API is
     * primarily for UWP readback slots, so copy synchronously here and release
     * the borrowed storage immediately. */
    xemu_stream_submit_bgra(bgra, width, height, pitch);
    if (release != NULL) {
        release(opaque);
    }
}

const char *xemu_stream_get_status(void)
{
    return s_status;
}

void xemu_stream_poll(void)
{
    bool desired;
    bool remote_start_enabled;
    int control_port;
    StreamParams cur;

    if (!s_initialized) {
        return;
    }

    ensure_audio_capture();

    /* Remote control channel lifecycle. */
    remote_start_enabled = g_config.display.stream.remote_start;
    control_port = g_config.display.stream.control_port;
    if (control_port < 1 || control_port > 65535) {
        control_port = 26961;
    }
    if (remote_start_enabled) {
        if (s_control != NULL && s_control_port_running != control_port) {
            control_stop();
        }
        control_start(control_port);
    } else if (s_control != NULL) {
        control_stop();
    }

    /* Reap a worker that terminated on its own (encoder/network failure)
     * before considering a (re)start, so a new thread is never spawned over
     * a dead one. */
    if (s_worker != NULL && !s_worker_run && !s_active) {
        g_thread_join(s_worker);
        s_worker = NULL;
        s_start_failed = true; /* latch until settings change; no retry loop */
        s_remote_active = false;
    }

    /* Desired running state. */
    g_mutex_lock(&s_lock);
    desired = g_config.display.stream.broadcast_enabled || s_remote_active;
    g_mutex_unlock(&s_lock);

    if (desired && !s_active && s_worker == NULL && !s_start_failed) {
        worker_start();
        return;
    }

    if (!desired && (s_active || s_worker != NULL)) {
        worker_stop();
        return;
    }

    /* Restart if relevant settings changed while running. */
    if (s_active && s_worker != NULL) {
        snapshot_params(&cur);
        if (params_differ(&cur, &s_running_params)) {
            worker_stop();
            s_start_failed = false;
        }
    }

    /* Clear the failed latch when the user disables the broadcast or edits the
     * relevant settings, so a later start attempt can proceed. */
    if (s_start_failed) {
        if (!desired) {
            s_start_failed = false;
        } else {
            snapshot_params(&cur);
            if (params_differ(&cur, &s_running_params)) {
                s_start_failed = false;
            }
        }
    }
}

#elif !defined(CONFIG_UWP) /* desktop build without FFmpeg */

void xemu_stream_init(void) {}
void xemu_stream_shutdown(void) {}
void xemu_stream_poll(void) {}
bool xemu_stream_is_active(void) { return false; }
bool xemu_stream_is_ready(void) { return false; }
bool xemu_stream_wants_frame(void) { return false; }
void xemu_stream_request_start(void) {}
void xemu_stream_request_start_with_params(const char *url,
                                           int bitrate_kbps, int fps)
{
    (void)url;
    (void)bitrate_kbps;
    (void)fps;
}
void xemu_stream_request_stop(void) {}
bool xemu_stream_is_supported(void) { return false; }
bool xemu_stream_get_output_size(int *width, int *height)
{
    (void)width;
    (void)height;
    return false;
}
void xemu_stream_submit_bgra(const uint8_t *bgra, int width, int height,
                             int pitch)
{
    (void)bgra;
    (void)width;
    (void)height;
    (void)pitch;
}
void xemu_stream_submit_bgra_borrowed(const uint8_t *bgra, int width,
                                      int height, int pitch,
                                      int64_t capture_us,
                                      XemuStreamFrameRelease release,
                                      void *opaque)
{
    (void)capture_us;
    (void)bgra;
    (void)width;
    (void)height;
    (void)pitch;
    if (release != NULL) {
        release(opaque);
    }
}
const char *xemu_stream_get_status(void)
{
    return "streaming not supported in this build (ffmpeg missing)";
}

#endif /* desktop FFmpeg / desktop stub; UWP is in xemu-stream-uwp.cpp */
