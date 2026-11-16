/*
 * Android hardware stages for the CapTrim three-stage export pipeline.
 *
 * Mirrors native/captrim_ohos_hw.c: a decoder factory and an encoder factory
 * plugged into native/captrim_pipeline.c through weak symbols. Both run in
 * AMediaCodec's native Buffer mode (dequeue/queue), which is the only mode
 * exposed by the NDK C API. FFmpeg remains STAGE 2 (libavfilter) and the
 * software fallback for every stage.
 *
 * Linked only into libcaptrim_ffmpeg.so by scripts/build-ffmpeg-android.sh.
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <android/log.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaExtractor.h>
#include <media/NdkMediaFormat.h>
#include <media/NdkMediaMuxer.h>

#include "captrim_pipeline.h"

#define ALOG_TAG "CapTrimPipeline"

/* Android's OMX color-format constants are not exported by the NDK headers.
 * Values are from AOSP MediaCodecInfo.CodecCapabilities / OMX_COLOR_Format*. */
#define A_COLOR_YUV420P         19  /* I420, 3 planes                       */
#define A_COLOR_YUV420SP        21  /* NV12, Y plane + interleaved UV       */
#define A_COLOR_YUV420PACKEDSP  39  /* NV12 with packed rows                */
#define A_COLOR_YUV420FLEXIBLE  42  /* negotiated semi-planar (usually NV12)*/
#define A_COLOR_SURFACE          7

/* API 21+ flag values; AMEDIACODEC_BUFFER_FLAG_KEY_FRAME is API 34 in NDK 27
 * headers, so use the stable numeric value to keep minSdk 26 clean. */
#define A_FLAG_KEY_FRAME 0x1u

static void a_set_error(char **out_error, const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    if (!out_error) return;
    free(*out_error);
    *out_error = NULL;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    size_t n = strlen(buf) + 1;
    *out_error = (char *)malloc(n);
    if (*out_error) memcpy(*out_error, buf, n);
}

static char *a_strdup(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char *p = (char *)malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

/* Accept both "file:///path" and a plain filesystem path. */
static const char *a_strip_uri(const char *uri) {
    if (!uri) return NULL;
    if (strncmp(uri, "file://", 7) == 0) return uri + 7;
    return uri;
}

static void a_status_error(char **out_error, const char *what,
                           media_status_t st) {
    a_set_error(out_error, "%s failed (media_status_t=%d)", what, (int)st);
}

static int a_format_str(AMediaFormat *fmt, const char *key,
                        char *dst, size_t cap) {
    const char *s = NULL;
    if (!fmt || !dst || cap == 0) return 0;
    if (!AMediaFormat_getString(fmt, key, &s) || !s) return 0;
    snprintf(dst, cap, "%s", s);
    return dst[0] != '\0';
}

static int a_format_int(AMediaFormat *fmt, const char *key, int fallback) {
    int32_t v = 0;
    if (fmt && AMediaFormat_getInt32(fmt, key, &v) && v > 0) return (int)v;
    return fallback;
}

static int a_align(int v, int a) { return (v + a - 1) & ~(a - 1); }

/* ------------------------------------------------------------------------- */
/* Platform hooks consumed by captrim_pipeline.c                            */
/* ------------------------------------------------------------------------- */

void ct_pipeline_platform_log(const char *message) {
    __android_log_print(ANDROID_LOG_INFO, ALOG_TAG, "%s",
                        message ? message : "");
}

int32_t ct_pipeline_platform_caps(char **out_json) {
    if (!out_json) return CT_ERR_INVALID;
    *out_json = NULL;
    AMediaCodec *dec = AMediaCodec_createDecoderByType("video/avc");
    AMediaCodec *enc = AMediaCodec_createEncoderByType("video/avc");
    char *buf = (char *)malloc(512);
    if (!buf) {
        if (dec) AMediaCodec_delete(dec);
        if (enc) AMediaCodec_delete(enc);
        return CT_ERR_INTERNAL;
    }
    snprintf(buf, 512,
             "\"vendor\":\"Android MediaCodec\","
             "\"mode\":\"Buffer(dequeue)\","
             "\"decoderAvc\":%s,\"encoderAvc\":%s",
             dec ? "true" : "false", enc ? "true" : "false");
    if (dec) AMediaCodec_delete(dec);
    if (enc) AMediaCodec_delete(enc);
    *out_json = buf;
    return CT_OK;
}

/* ========================================================================= */
/* STAGE 1: AMediaExtractor + AMediaCodec software-pixel decoder             */
/* ========================================================================= */

typedef struct {
    int fd;
    AMediaExtractor *ex;
    AMediaCodec *codec;
    size_t track;
    int width, height;
    double fps;
    int64_t duration_us;
    int64_t last_pts_us;

    int input_eos;
    int output_eos;
    int output_format_ready;
    int color_format;       /* 19 = I420, otherwise NV12 */
    int stride;
    int slice_height;
} AndroidDecCtx;

static AndroidDecCtx *adec_ctx(void *self) {
    return (AndroidDecCtx *)((ct_decoder_t *)self)->ctx;
}

/* Feed compressed samples until the decoder's input queue is full or EOS. */
static int32_t adec_pump_input(AndroidDecCtx *c, char **out_error) {
    if (c->input_eos || !c->codec || !c->ex) return 0;
    for (;;) {
        ssize_t idx = AMediaCodec_dequeueInputBuffer(c->codec, 0);
        if (idx == AMEDIACODEC_INFO_TRY_AGAIN_LATER) return 0;
        if (idx < 0) {
            a_set_error(out_error, "AMediaCodec_dequeueInputBuffer=%zd", idx);
            return CT_ERR_DECODE;
        }
        size_t cap = 0;
        uint8_t *buf = AMediaCodec_getInputBuffer(c->codec, (size_t)idx, &cap);
        if (!buf) {
            a_set_error(out_error, "AMediaCodec_getInputBuffer(%zd) failed", idx);
            return CT_ERR_DECODE;
        }
        ssize_t n = AMediaExtractor_readSampleData(c->ex, buf, cap);
        int64_t pts = AMediaExtractor_getSampleTime(c->ex);
        uint32_t flags = 0;
        if (n < 0) {
            flags = AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM;
            n = 0;
            c->input_eos = 1;
        } else if (AMediaExtractor_getSampleFlags(c->ex) &
                   AMEDIAEXTRACTOR_SAMPLE_FLAG_SYNC) {
            flags = A_FLAG_KEY_FRAME;
        }
        media_status_t st = AMediaCodec_queueInputBuffer(
            c->codec, (size_t)idx, 0, (size_t)n, (uint64_t)pts, flags);
        if (st != AMEDIA_OK) {
            a_status_error(out_error, "AMediaCodec_queueInputBuffer", st);
            return CT_ERR_DECODE;
        }
        if (c->input_eos) return 0;
        (void)AMediaExtractor_advance(c->ex);
    }
}

static void adec_read_output_format(AndroidDecCtx *c) {
    AMediaFormat *fmt = AMediaCodec_getOutputFormat(c->codec);
    if (!fmt) return;
    c->color_format = a_format_int(fmt, AMEDIAFORMAT_KEY_COLOR_FORMAT,
                                   A_COLOR_YUV420SP);
    c->stride = a_format_int(fmt, AMEDIAFORMAT_KEY_STRIDE, 0);
    int32_t slice = 0;
    if (!AMediaFormat_getInt32(fmt, "slice-height", &slice) || slice <= 0) {
        slice = c->height;
    }
    c->slice_height = slice;
    int32_t w = 0, h = 0;
    if (AMediaFormat_getInt32(fmt, AMEDIAFORMAT_KEY_WIDTH, &w) && w > 0) {
        c->width = w;
    }
    if (AMediaFormat_getInt32(fmt, AMEDIAFORMAT_KEY_HEIGHT, &h) && h > 0) {
        c->height = h;
    }
    AMediaFormat_delete(fmt);
    c->output_format_ready = 1;
}

static int32_t adec_open(ct_decoder_t *self, const char *src_uri,
                         char **out_error) {
    AndroidDecCtx *c = (AndroidDecCtx *)calloc(1, sizeof(AndroidDecCtx));
    if (!c) {
        a_set_error(out_error, "Android decoder context allocation failed");
        return CT_ERR_INTERNAL;
    }
    self->ctx = c;
    c->fd = -1;
    c->track = (size_t)-1;
    c->fps = 25.0;
    c->color_format = A_COLOR_YUV420SP;

    const char *path = a_strip_uri(src_uri);
    struct stat st;
    c->fd = open(path, O_RDONLY);
    if (c->fd < 0 || fstat(c->fd, &st) != 0) {
        a_set_error(out_error, "cannot open source %s (errno=%d)",
                    path ? path : "(null)", errno);
        return CT_ERR_IO;
    }
    c->ex = AMediaExtractor_new();
    if (!c->ex) {
        a_set_error(out_error, "AMediaExtractor_new failed");
        return CT_ERR_DECODE;
    }
    media_status_t ms = AMediaExtractor_setDataSourceFd(
        c->ex, c->fd, 0, (off64_t)st.st_size);
    if (ms != AMEDIA_OK) {
        a_status_error(out_error, "AMediaExtractor_setDataSourceFd", ms);
        return CT_ERR_DECODE;
    }

    char mime[128] = {0};
    size_t tracks = AMediaExtractor_getTrackCount(c->ex);
    for (size_t i = 0; i < tracks; ++i) {
        AMediaFormat *tf = AMediaExtractor_getTrackFormat(c->ex, i);
        if (!tf) continue;
        char m[128] = {0};
        if (a_format_str(tf, AMEDIAFORMAT_KEY_MIME, m, sizeof(m)) &&
            strncmp(m, "video/", 6) == 0) {
            c->track = i;
            snprintf(mime, sizeof(mime), "%s", m);
            c->width = a_format_int(tf, AMEDIAFORMAT_KEY_WIDTH, 0);
            c->height = a_format_int(tf, AMEDIAFORMAT_KEY_HEIGHT, 0);
            int32_t rate = 0;
            if (AMediaFormat_getInt32(tf, AMEDIAFORMAT_KEY_FRAME_RATE, &rate) &&
                rate > 0) {
                c->fps = (double)rate;
            }
            int64_t dur = 0;
            if (AMediaFormat_getInt64(tf, AMEDIAFORMAT_KEY_DURATION, &dur) &&
                dur > 0) {
                c->duration_us = dur;
            }
        }
        AMediaFormat_delete(tf);
        if (c->track != (size_t)-1) break;
    }
    if (c->track == (size_t)-1 || !mime[0] || c->width <= 0 || c->height <= 0) {
        a_set_error(out_error, "no usable video track in AMediaExtractor");
        return CT_ERR_DECODE;
    }
    ms = AMediaExtractor_selectTrack(c->ex, c->track);
    if (ms != AMEDIA_OK) {
        a_status_error(out_error, "AMediaExtractor_selectTrack", ms);
        return CT_ERR_DECODE;
    }

    c->codec = AMediaCodec_createDecoderByType(mime);
    if (!c->codec) {
        a_set_error(out_error, "AMediaCodec_createDecoderByType(%s) failed", mime);
        return CT_ERR_DECODE;
    }
    AMediaFormat *tf = AMediaExtractor_getTrackFormat(c->ex, c->track);
    /* The extractor's track format carries csd-0/csd-1; passing it directly
     * is the documented way to configure an AVC/HEVC decoder. */
    ms = AMediaCodec_configure(c->codec, tf, NULL, NULL, 0);
    AMediaFormat_delete(tf);
    if (ms != AMEDIA_OK) {
        a_status_error(out_error, "AMediaCodec_configure(decoder)", ms);
        return CT_ERR_DECODE;
    }
    ms = AMediaCodec_start(c->codec);
    if (ms != AMEDIA_OK) {
        a_status_error(out_error, "AMediaCodec_start(decoder)", ms);
        return CT_ERR_DECODE;
    }
    self->duration_us = c->duration_us;
    char diag[256];
    snprintf(diag, sizeof(diag),
             "CapTrim Android: hardware decoder ACTIVE mime=%s %dx%d dur=%lldus",
             mime, c->width, c->height, (long long)c->duration_us);
    ct_pipeline_platform_log(diag);
    return CT_OK;
}

static int32_t adec_next(ct_decoder_t *self, ct_frame_t *out, char **out_error) {
    AndroidDecCtx *c = adec_ctx(self);
    if (!c || !c->codec) return CT_ERR_DECODE;
    for (;;) {
        int32_t st = adec_pump_input(c, out_error);
        if (st != CT_OK) return st;

        AMediaCodecBufferInfo info;
        memset(&info, 0, sizeof(info));
        ssize_t idx = AMediaCodec_dequeueOutputBuffer(c->codec, &info, 2000);
        if (idx == AMEDIACODEC_INFO_TRY_AGAIN_LATER) {
            if (c->input_eos && c->output_eos) return 0;
            continue;
        }
        if (idx == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
            adec_read_output_format(c);
            continue;
        }
        if (idx == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) continue;
        if (idx < 0) {
            a_set_error(out_error, "AMediaCodec_dequeueOutputBuffer=%zd", idx);
            return CT_ERR_DECODE;
        }
        if (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) {
            AMediaCodec_releaseOutputBuffer(c->codec, (size_t)idx, false);
            c->output_eos = 1;
            return 0;
        }
        if (info.size <= 0 || !c->output_format_ready) {
            AMediaCodec_releaseOutputBuffer(c->codec, (size_t)idx, false);
            continue;
        }

        size_t cap = 0;
        uint8_t *base = AMediaCodec_getOutputBuffer(c->codec, (size_t)idx, &cap);
        if (!base) {
            AMediaCodec_releaseOutputBuffer(c->codec, (size_t)idx, false);
            a_set_error(out_error, "AMediaCodec_getOutputBuffer(%zd) failed", idx);
            return CT_ERR_DECODE;
        }
        int stride = c->stride;
        int slice = c->slice_height > 0 ? c->slice_height : c->height;
        if (stride <= 0 || slice <= 0) {
            /* Last-resort inference from the buffer capacity. */
            stride = a_align((int)(cap / (size_t)(c->height * 3 / 2)), 16);
            slice = a_align(c->height, 16);
        }
        memset(out, 0, sizeof(*out));
        if (stride * slice * 3 / 2 > (int)cap) {
            AMediaCodec_releaseOutputBuffer(c->codec, (size_t)idx, false);
            a_set_error(out_error,
                        "hardware frame layout %dx%d stride=%d slice=%d exceeds capacity=%zu",
                        c->width, c->height, stride, slice, cap);
            return CT_ERR_DECODE;
        }
        if (c->color_format == A_COLOR_YUV420P) {
            out->format = CT_PIX_FMT_I420;
            out->plane_count = 3;
            out->data[0] = base + info.offset;
            out->data[1] = out->data[0] + (size_t)stride * slice;
            out->data[2] = out->data[1] + (size_t)(stride / 2) * ((slice + 1) / 2);
            out->stride[0] = stride;
            out->stride[1] = stride / 2;
            out->stride[2] = stride / 2;
        } else {
            out->format = CT_PIX_FMT_NV12;
            out->plane_count = 2;
            out->data[0] = base + info.offset;
            out->data[1] = out->data[0] + (size_t)stride * slice;
            out->stride[0] = stride;
            out->stride[1] = stride;
        }
        out->width = c->width;
        out->height = c->height;
        out->pts_us = info.presentationTimeUs;
        out->rotation = 0;
        out->opaque = (void *)(intptr_t)idx;
        c->last_pts_us = out->pts_us;
        return 1;
    }
}

static void adec_release_frame(ct_decoder_t *self, ct_frame_t *frame) {
    AndroidDecCtx *c = adec_ctx(self);
    if (!c || !c->codec || !frame || !frame->opaque) return;
    size_t idx = (size_t)(intptr_t)frame->opaque;
    AMediaCodec_releaseOutputBuffer(c->codec, idx, false);
    frame->opaque = NULL;
}

static void adec_close(ct_decoder_t *self) {
    AndroidDecCtx *c = adec_ctx(self);
    if (!c) return;
    if (c->codec) {
        AMediaCodec_stop(c->codec);
        AMediaCodec_delete(c->codec);
    }
    if (c->ex) AMediaExtractor_delete(c->ex);
    if (c->fd >= 0) close(c->fd);
    free(c);
    self->ctx = NULL;
}

ct_decoder_t *ct_decoder_create_android(void) {
    ct_decoder_t *d = (ct_decoder_t *)calloc(1, sizeof(ct_decoder_t));
    if (!d) return NULL;
    d->kind = CT_BACKEND_ANDROID_HW;
    d->open = adec_open;
    d->next = adec_next;
    d->release_frame = adec_release_frame;
    d->close = adec_close;
    return d;
}

/* ========================================================================= */
/* STAGE 3: AMediaCodec encoder + AMediaMuxer                                 */
/* ========================================================================= */

typedef struct {
    int fd;
    char *dst_path;
    AMediaCodec *codec;
    AMediaMuxer *muxer;
    size_t track;
    int track_added;
    int muxer_started;
    int width, height;
    int color_format;
    int stride;
    int slice_height;
    int64_t fps_num, fps_den;
    int64_t input_index;
    int64_t output_index;
    int input_eos;
    int output_eos;
} AndroidEncCtx;

static AndroidEncCtx *aenc_ctx(void *self) {
    return (AndroidEncCtx *)((ct_encoder_t *)self)->ctx;
}

static double aenc_fps(const AndroidEncCtx *c) {
    if (c->fps_den <= 0) return 25.0;
    double f = (double)c->fps_num / (double)c->fps_den;
    return f > 0 ? f : 25.0;
}

static int32_t aenc_add_track(AndroidEncCtx *c, char **out_error) {
    if (c->track_added) return CT_OK;
    AMediaFormat *fmt = AMediaCodec_getOutputFormat(c->codec);
    if (!fmt) {
        a_set_error(out_error, "AMediaCodec_getOutputFormat failed");
        return CT_ERR_ENCODE;
    }
    /* Keep the negotiated csd-0/csd-1; force the fps the orchestrator
     * selected, because a trim/fps filter can change the source rate. */
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_FRAME_RATE, (int32_t)aenc_fps(c));
    ssize_t track = AMediaMuxer_addTrack(c->muxer, fmt);
    AMediaFormat_delete(fmt);
    if (track < 0) {
        a_set_error(out_error, "AMediaMuxer_addTrack failed (ssize_t=%zd)", track);
        return CT_ERR_ENCODE;
    }
    c->track = (size_t)track;
    c->track_added = 1;
    media_status_t st = AMediaMuxer_start(c->muxer);
    if (st != AMEDIA_OK) {
        a_status_error(out_error, "AMediaMuxer_start", st);
        return CT_ERR_ENCODE;
    }
    c->muxer_started = 1;
    return CT_OK;
}

/* I420/NV12 -> the encoder's NV12 layout, honouring its real stride. */
static int aenc_fill_nv12(AndroidEncCtx *c, uint8_t *dst, size_t cap,
                          const ct_frame_t *in) {
    int w = in->width, h = in->height;
    /* getInputFormat() is API 28+ and we target minSdk 26, so on API 26/27
     * there is no stride key available. Many encoders allocate a tight,
     * contiguous w*h*3/2 buffer; aligning the slice height to 16 here would
     * make us demand more bytes than the buffer actually has. Start from the
     * unaligned height and only use an exposed stride/slice when present. */
    int stride = c->stride > 0 ? c->stride : w;
    int slice = c->slice_height > 0 ? c->slice_height : h;
    size_t need = (size_t)stride * (size_t)slice * 3 / 2;
    if (need > cap) {
        /* Derive a 16-aligned stride from the capacity, keeping the slice
         * height unaligned (the UV plane then starts at stride*h). */
        int derived = (int)(cap / (size_t)(h * 3 / 2));
        if (derived <= 0) return -1;
        stride = derived & ~15;
        if (stride < w) stride = w;
        slice = h;
        need = (size_t)stride * (size_t)slice * 3 / 2;
        if (need > cap) return -1;
        c->stride = stride;
        c->slice_height = slice;
    }
    memset(dst, 0, need);
    int in_y = in->stride[0] > 0 ? in->stride[0] : w;
    uint8_t *dy = dst;
    for (int y = 0; y < h; ++y) {
        memcpy(dy + (size_t)y * stride,
               in->data[0] + (size_t)y * in_y, (size_t)w);
    }
    uint8_t *duv = dst + (size_t)stride * slice;
    int half_w = w / 2, half_h = h / 2;
    if (in->format == CT_PIX_FMT_I420) {
        int in_u = in->stride[1] > 0 ? in->stride[1] : half_w;
        int in_v = in->stride[2] > 0 ? in->stride[2] : half_w;
        for (int y = 0; y < half_h; ++y) {
            const uint8_t *u = in->data[1] + (size_t)y * in_u;
            const uint8_t *v = in->data[2] + (size_t)y * in_v;
            uint8_t *d = duv + (size_t)y * stride;
            for (int x = 0; x < half_w; ++x) {
                d[2 * x] = u[x];
                d[2 * x + 1] = v[x];
            }
        }
    } else if (in->format == CT_PIX_FMT_NV12) {
        int in_uv = in->stride[1] > 0 ? in->stride[1] : w;
        for (int y = 0; y < half_h; ++y) {
            memcpy(duv + (size_t)y * stride,
                   in->data[1] + (size_t)y * in_uv, (size_t)w);
        }
    } else {
        return -1;
    }
    return (int)need;
}

static int32_t aenc_drain(AndroidEncCtx *c, char **out_error) {
    for (;;) {
        AMediaCodecBufferInfo info;
        memset(&info, 0, sizeof(info));
        ssize_t idx = AMediaCodec_dequeueOutputBuffer(c->codec, &info, 0);
        if (idx == AMEDIACODEC_INFO_TRY_AGAIN_LATER) return CT_OK;
        if (idx == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
            int32_t st = aenc_add_track(c, out_error);
            if (st != CT_OK) return st;
            continue;
        }
        if (idx == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) continue;
        if (idx < 0) {
            a_set_error(out_error, "encoder dequeueOutputBuffer=%zd", idx);
            return CT_ERR_ENCODE;
        }
        size_t cap = 0;
        uint8_t *buf = AMediaCodec_getOutputBuffer(c->codec, (size_t)idx, &cap);
        if (!buf) {
            a_set_error(out_error, "encoder getOutputBuffer(%zd) failed", idx);
            return CT_ERR_ENCODE;
        }
        if (!(info.flags & AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG) &&
            info.size > 0 && c->track_added) {
            /* Re-base timestamps on the filter graph's frame rate. */
            info.presentationTimeUs =
                (int64_t)((double)c->output_index * 1000000.0 / aenc_fps(c));
            c->output_index++;
            media_status_t ms = AMediaMuxer_writeSampleData(
                c->muxer, c->track, buf + info.offset, &info);
            if (ms != AMEDIA_OK) {
                AMediaCodec_releaseOutputBuffer(c->codec, (size_t)idx, false);
                a_status_error(out_error, "AMediaMuxer_writeSampleData", ms);
                return CT_ERR_ENCODE;
            }
        }
        int eos = (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) != 0;
        AMediaCodec_releaseOutputBuffer(c->codec, (size_t)idx, false);
        if (eos) {
            c->output_eos = 1;
            return CT_OK;
        }
    }
}

static int32_t aenc_open(ct_encoder_t *self, const ct_frame_t *first,
                         const char *dst_uri, const char *video_codec,
                         char **out_error) {
    AndroidEncCtx *c = (AndroidEncCtx *)calloc(1, sizeof(AndroidEncCtx));
    if (!c) {
        a_set_error(out_error, "Android encoder context allocation failed");
        return CT_ERR_INTERNAL;
    }
    self->ctx = c;
    c->fd = -1;
    c->track = (size_t)-1;
    c->width = first->width;
    c->height = first->height;
    c->fps_num = self->fps_num > 0 ? self->fps_num : 25;
    c->fps_den = self->fps_den > 0 ? self->fps_den : 1;
    c->color_format = A_COLOR_YUV420SP;
    c->dst_path = a_strdup(a_strip_uri(dst_uri));

    const char *mime = "video/avc";
    if (video_codec && (strstr(video_codec, "265") ||
                        strstr(video_codec, "hevc"))) {
        mime = "video/hevc";
    }
    c->codec = AMediaCodec_createEncoderByType(mime);
    if (!c->codec) {
        a_set_error(out_error, "AMediaCodec_createEncoderByType(%s) failed", mime);
        return CT_ERR_ENCODE;
    }
    AMediaFormat *fmt = AMediaFormat_new();
    int fps = (int)aenc_fps(c);
    int bitrate = c->width * c->height * fps / 2;
    if (bitrate < 800000) bitrate = 800000;
    AMediaFormat_setString(fmt, AMEDIAFORMAT_KEY_MIME, mime);
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_WIDTH, c->width);
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_HEIGHT, c->height);
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_FRAME_RATE, fps);
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_BIT_RATE, bitrate);
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_COLOR_FORMAT, A_COLOR_YUV420SP);
    AMediaFormat_setInt32(fmt, "i-frame-interval", 1);
    media_status_t ms = AMediaCodec_configure(
        c->codec, fmt, NULL, NULL, AMEDIACODEC_CONFIGURE_FLAG_ENCODE);
    AMediaFormat_delete(fmt);
    if (ms != AMEDIA_OK) {
        a_status_error(out_error, "AMediaCodec_configure(encoder)", ms);
        return CT_ERR_ENCODE;
    }
    ms = AMediaCodec_start(c->codec);
    if (ms != AMEDIA_OK) {
        a_status_error(out_error, "AMediaCodec_start(encoder)", ms);
        return CT_ERR_ENCODE;
    }

    c->fd = open(c->dst_path ? c->dst_path : "", O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (c->fd < 0) {
        a_set_error(out_error, "cannot create output %s (errno=%d)",
                    c->dst_path ? c->dst_path : "(null)", errno);
        return CT_ERR_IO;
    }
    c->muxer = AMediaMuxer_new(c->fd, AMEDIAMUXER_OUTPUT_FORMAT_MPEG_4);
    if (!c->muxer) {
        a_set_error(out_error, "AMediaMuxer_new failed");
        return CT_ERR_ENCODE;
    }
    char diag[256];
    snprintf(diag, sizeof(diag),
             "CapTrim Android: hardware encoder ACTIVE mime=%s NV12 %dx%d @%d -> mp4",
             mime, c->width, c->height, fps);
    ct_pipeline_platform_log(diag);
    return CT_OK;
}

static int32_t aenc_push(ct_encoder_t *self, const ct_frame_t *in,
                         char **out_error) {
    AndroidEncCtx *c = aenc_ctx(self);
    int got = 0;
    for (int guard = 0; guard < 100 && !got; ++guard) {
        ssize_t idx = AMediaCodec_dequeueInputBuffer(c->codec, 2000);
        if (idx == AMEDIACODEC_INFO_TRY_AGAIN_LATER) {
            int32_t st = aenc_drain(c, out_error);
            if (st != CT_OK) return st;
            continue;
        }
        if (idx < 0) {
            a_set_error(out_error, "encoder dequeueInputBuffer=%zd", idx);
            return CT_ERR_ENCODE;
        }
        got = 1;
        size_t cap = 0;
        uint8_t *buf = AMediaCodec_getInputBuffer(c->codec, (size_t)idx, &cap);
        if (!buf) {
            a_set_error(out_error, "encoder getInputBuffer(%zd) failed", idx);
            return CT_ERR_ENCODE;
        }
        int size = aenc_fill_nv12(c, buf, cap, in);
        if (size <= 0) {
            a_set_error(out_error,
                        "hardware encoder input layout invalid (cap=%zu stride=%d slice=%d)",
                        cap, c->stride, c->slice_height);
            return CT_ERR_ENCODE;
        }
        int64_t pts = (int64_t)((double)c->input_index * 1000000.0 / aenc_fps(c));
        media_status_t ms = AMediaCodec_queueInputBuffer(
            c->codec, (size_t)idx, 0, (size_t)size, (uint64_t)pts, 0);
        if (ms != AMEDIA_OK) {
            a_status_error(out_error, "encoder queueInputBuffer", ms);
            return CT_ERR_ENCODE;
        }
        c->input_index++;
    }
    if (!got) {
        a_set_error(out_error, "Android hardware encoder input queue never became ready");
        return CT_ERR_ENCODE;
    }
    return aenc_drain(c, out_error);
}

static int32_t aenc_finish(ct_encoder_t *self, char **out_error) {
    AndroidEncCtx *c = aenc_ctx(self);
    if (!c->input_eos) {
        int done = 0;
        for (int guard = 0; guard < 100 && !done; ++guard) {
            ssize_t idx = AMediaCodec_dequeueInputBuffer(c->codec, 2000);
            if (idx == AMEDIACODEC_INFO_TRY_AGAIN_LATER) {
                int32_t st = aenc_drain(c, out_error);
                if (st != CT_OK) return st;
                continue;
            }
            if (idx < 0) {
                a_set_error(out_error, "encoder EOS dequeue=%zd", idx);
                return CT_ERR_ENCODE;
            }
            media_status_t ms = AMediaCodec_queueInputBuffer(
                c->codec, (size_t)idx, 0, 0,
                (uint64_t)((double)c->input_index * 1000000.0 / aenc_fps(c)),
                AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
            if (ms != AMEDIA_OK) {
                a_status_error(out_error, "encoder EOS queueInputBuffer", ms);
                return CT_ERR_ENCODE;
            }
            c->input_eos = 1;
            done = 1;
        }
        if (!done) {
            a_set_error(out_error, "hardware encoder never accepted EOS");
            return CT_ERR_ENCODE;
        }
    }
    for (int guard = 0; guard < 200 && !c->output_eos; ++guard) {
        int32_t st = aenc_drain(c, out_error);
        if (st != CT_OK) return st;
        if (!c->output_eos) usleep(2000);
    }
    if (!c->track_added) {
        int32_t st = aenc_add_track(c, out_error);
        if (st != CT_OK) return st;
    }
    if (c->muxer_started) {
        media_status_t ms = AMediaMuxer_stop(c->muxer);
        if (ms != AMEDIA_OK) {
            a_status_error(out_error, "AMediaMuxer_stop", ms);
            return CT_ERR_ENCODE;
        }
    }
    if (c->dst_path) {
        struct stat st;
        if (stat(c->dst_path, &st) == 0 && st.st_size < 1024) {
            a_set_error(out_error, "Android muxer produced a suspicious file (%lld bytes)",
                        (long long)st.st_size);
            return CT_ERR_ENCODE;
        }
    }
    char diag[192];
    snprintf(diag, sizeof(diag),
             "CapTrim Android: encoder finished in=%lld out=%lld",
             (long long)c->input_index, (long long)c->output_index);
    ct_pipeline_platform_log(diag);
    return CT_OK;
}

static void aenc_close(ct_encoder_t *self) {
    AndroidEncCtx *c = aenc_ctx(self);
    if (!c) return;
    if (c->codec) {
        AMediaCodec_stop(c->codec);
        AMediaCodec_delete(c->codec);
    }
    if (c->muxer) AMediaMuxer_delete(c->muxer);
    if (c->fd >= 0) close(c->fd);
    free(c->dst_path);
    free(c);
    self->ctx = NULL;
}

ct_encoder_t *ct_encoder_create_android(void) {
    ct_encoder_t *e = (ct_encoder_t *)calloc(1, sizeof(ct_encoder_t));
    if (!e) return NULL;
    e->kind = CT_BACKEND_ANDROID_HW;
    e->open = aenc_open;
    e->push = aenc_push;
    e->finish = aenc_finish;
    e->close = aenc_close;
    return e;
}
