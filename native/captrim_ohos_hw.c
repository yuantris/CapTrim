/* CapTrim HarmonyOS hardware first/last mile.
 *
 * Compiled ONLY into the OHOS libcaptrim_ffmpeg.so (see
 * scripts/build-ffmpeg-ohos.sh). Implements the platform factories declared
 * weak in captrim_pipeline.h using AVCodec Kit Buffer mode:
 *
 *   STAGE 1  OH_AVSource + OH_AVDemuxer -> OH_VideoDecoder -> ct_frame_t (NV12)
 *   STAGE 3  ct_frame_t (I420) -> OH_VideoEncoder -> OH_AVMuxer -> mp4
 *
 * The middle stage stays FFmpeg libavfilter, so "hard decode + complex edit +
 * hard encode" works without ever converting codec config between two media
 * stacks: demuxer/muxer and decoder/encoder are all OHOS and therefore share
 * the same bitstream conventions (AVCC + codec-config buffer).
 *
 * TWO BUFFER MODES, ONE SET OF LOOPS
 * ----------------------------------
 * AVCodec Kit has two mutually exclusive Buffer modes:
 *
 *  - SYNCHRONOUS (API 20+): opt in at Configure() with
 *    OH_MD_KEY_ENABLE_SYNC_MODE = 1, then poll QueryInputBuffer /
 *    QueryOutputBuffer / Get*Buffer. No callback may be registered.
 *  - ASYNCHRONOUS / CALLBACK (API 11+): RegisterCallback() with
 *    OnNeedInputBuffer / OnNewOutputBuffer; the codec pushes (index, buffer)
 *    pairs onto its own thread and we consume them from queues.
 *
 * The official transcoding sample uses the callback mode. Our first attempt
 * used the synchronous APIs WITHOUT the opt-in key, so on Mate 60 /
 * OpenHarmony 7.0.0 (API 26) every Query returned AV_ERR_OPERATE_NOT_PERMIT(2)
 * ("This function is called in asynchronous mode") and the whole stage fell
 * back to FFmpeg software. Both modes are now supported: sync is the fast
 * path, callback is the automatic fallback (and can be forced for testing via
 * the "hw:cb:" codec prefix -> ct_pipeline_options_t.force_async).
 *
 * To keep the streaming loops identical, both modes expose the same two
 * primitives: "get me an empty input buffer" and "get me a filled output
 * buffer". In sync mode they call Query*Buffer; in callback mode they pop a
 * mutex/condvar queue fed by the codec callbacks.
 *
 * Every failure returns an error code so ct_pipeline_run() can dispose this
 * stage and fall back to the FFmpeg software stage INDEPENDENTLY.
 *
 * Official references (read 2026-11-15, via developer.huawei.com llms index +
 * devecocli docs):
 *   - 《基于Buffer模式进行视频转码》
 *     https://developer.huawei.com/consumer/cn/doc/doccenter-capabilities/video-transcoding-buffer.md
 *   - 《视频编解码宽高、跨距与裁剪信息说明》 (stride / sliceHeight / picWidth)
 *     https://developer.huawei.com/consumer/cn/doc/doccenter-capabilities/video-dimension-guide.md
 *   - native_avcodec_base.h: OH_AVCodecCallback, OH_MD_KEY_ENABLE_SYNC_MODE
 *   - native_avcodec_videodecoder.h / native_avcodec_videoencoder.h
 * The exact signatures are taken from the LOCAL NDK headers, which are
 * authoritative for the installed SDK version.
 */
#include "captrim_pipeline.h"

#include <multimedia/player_framework/native_avbuffer.h>
#include <multimedia/player_framework/native_avbuffer_info.h>
#include <multimedia/player_framework/native_avcodec_base.h>
#include <multimedia/player_framework/native_avcodec_videodecoder.h>
#include <multimedia/player_framework/native_avcodec_videoencoder.h>
#include <multimedia/player_framework/native_avdemuxer.h>
#include <multimedia/player_framework/native_avformat.h>
#include <multimedia/player_framework/native_avmuxer.h>
#include <multimedia/player_framework/native_avsource.h>

#include <hilog/log.h>

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define HW_LOG_TAG "CapTrimPipeline"
#define HW_LOG_DOMAIN 0x3200u

/* Defined below in "Shared helpers". */
static const char *oh_sync_key(void);

/* ------------------------------------------------------------------------- */
/* Platform hooks used by captrim_pipeline.c                                 */
/* ------------------------------------------------------------------------- */

void ct_pipeline_platform_log(const char *message) {
    OH_LOG_Print(LOG_APP, LOG_INFO, HW_LOG_DOMAIN, HW_LOG_TAG,
                 "%{public}s", message ? message : "");
}

int32_t ct_pipeline_platform_caps(char **out_json) {
    if (!out_json) return CT_ERR_INVALID;
    *out_json = NULL;
    OH_AVCodec *dec = OH_VideoDecoder_CreateByMime(OH_AVCODEC_MIMETYPE_VIDEO_AVC);
    OH_AVCodec *enc = OH_VideoEncoder_CreateByMime(OH_AVCODEC_MIMETYPE_VIDEO_AVC);
    char *buf = (char *)malloc(512);
    if (!buf) {
        if (dec) OH_VideoDecoder_Destroy(dec);
        if (enc) OH_VideoEncoder_Destroy(enc);
        return CT_ERR_INTERNAL;
    }
    snprintf(buf, 512,
             "\"vendor\":\"HarmonyOS AVCodec Kit\","
             "\"mode\":\"Buffer(sync+callback)\","
             "\"syncModeKey\":%s,\"decoderAvc\":%s,\"encoderAvc\":%s",
             oh_sync_key() ? "true" : "false",
             dec ? "true" : "false", enc ? "true" : "false");
    if (dec) OH_VideoDecoder_Destroy(dec);
    if (enc) OH_VideoEncoder_Destroy(enc);
    *out_json = buf;
    return CT_OK;
}

/* ------------------------------------------------------------------------- */
/* Shared helpers                                                            */
/* ------------------------------------------------------------------------- */

static void hw_set_error(char **out_error, const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    if (!out_error) return;
    if (*out_error) { free(*out_error); *out_error = NULL; }
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    size_t n = strlen(buf) + 1;
    *out_error = (char *)malloc(n);
    if (*out_error) memcpy(*out_error, buf, n);
}

static void hw_errcode(char **out_error, const char *what, OH_AVErrCode code) {
    hw_set_error(out_error, "%s failed (OH_AVErrCode=%d)", what, (int)code);
}

static char *hw_strdup(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char *p = (char *)malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

/* Accept both "file:///path" and a plain path, as the ArkTS SDK does. */
static const char *hw_strip_uri(const char *uri) {
    if (!uri) return NULL;
    if (strncmp(uri, "file://", 7) == 0) return uri + 7;
    return uri;
}

static int hw_align(int v, int a) { return (v + a - 1) & ~(a - 1); }

/* OH_MD_KEY_ENABLE_SYNC_MODE is API 20+. Resolve it at RUNTIME instead of
 * taking a hard undefined-symbol dependency: on older devices the shim must
 * still load (and simply use callback mode). The NDK stub exports every
 * OH_MD_KEY_* as an aliased no-op function, so the real device library is the
 * only place where the string value exists. */
static const char *oh_sync_key(void) {
    void *sym = dlsym(RTLD_DEFAULT, "OH_MD_KEY_ENABLE_SYNC_MODE");
    if (!sym) return NULL;
    const char *key = *(const char * const *)sym;
    return (key && *key) ? key : NULL;
}

typedef enum {
    OH_MODE_SYNC = 0,      /* Query*Buffer polling, needs the sync-mode key  */
    OH_MODE_CALLBACK = 1   /* RegisterCallback + mutex/condvar queues       */
} OhMode;

static OhMode oh_choose_mode(void) {
    if (ct_pipeline_force_async()) return OH_MODE_CALLBACK;
    return oh_sync_key() ? OH_MODE_SYNC : OH_MODE_CALLBACK;
}

static void hw_desc_stride(OH_AVFormat *fmt, int *stride, int *slice) {
    if (!fmt) return;
    int32_t v = 0;
    if (OH_AVFormat_GetIntValue(fmt, OH_MD_KEY_VIDEO_STRIDE, &v) && v > 0) *stride = v;
    v = 0;
    if (OH_AVFormat_GetIntValue(fmt, OH_MD_KEY_VIDEO_SLICE_HEIGHT, &v) && v > 0) *slice = v;
}

/* Resolve the REAL hardware stride. Guessing width is never safe: hardware
 * commonly aligns to 16/32/64 bytes and the UV plane starts at stride*slice.
 * Prefer the codec description (official guide), then the buffer parameter. */
static void hw_frame_stride(OH_AVBuffer *buf, OH_AVFormat *desc,
                            int w, int h, int *out_stride, int *out_slice) {
    int stride = 0, slice = 0;
    if (desc) hw_desc_stride(desc, &stride, &slice);
    if (!stride || !slice) {
        OH_AVFormat *param = buf ? OH_AVBuffer_GetParameter(buf) : NULL;
        if (param) {
            int s2 = 0, l2 = 0;
            hw_desc_stride(param, &s2, &l2);
            OH_AVFormat_Destroy(param);
            if (!stride) stride = s2;
            if (!slice) slice = l2;
        }
    }
    if (stride <= 0) stride = hw_align(w, 16);
    if (slice <= 0) slice = hw_align(h, 16);
    *out_stride = stride;
    *out_slice = slice;
}

/* ------------------------------------------------------------------------- */
/* Buffer queue (callback mode)                                               */
/* ------------------------------------------------------------------------- */

typedef struct {
    uint32_t index;
    OH_AVBuffer *buf;
} OhBuf;

typedef struct OhNode {
    OhBuf item;
    struct OhNode *next;
} OhNode;

typedef struct {
    OhNode *head;
    OhNode *tail;
    int count;
    int aborted;
    int inited;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
} OhQueue;

static void ohq_init(OhQueue *q) {
    memset(q, 0, sizeof(*q));
    pthread_mutex_init(&q->mutex, NULL);
    pthread_cond_init(&q->cond, NULL);
    q->inited = 1;
}

static void ohq_destroy(OhQueue *q) {
    OhNode *n;
    if (!q->inited) return;
    pthread_mutex_lock(&q->mutex);
    n = q->head;
    q->head = q->tail = NULL;
    q->count = 0;
    pthread_mutex_unlock(&q->mutex);
    while (n) { OhNode *next = n->next; free(n); n = next; }
    pthread_cond_destroy(&q->cond);
    pthread_mutex_destroy(&q->mutex);
    q->inited = 0;
}

static void ohq_push(OhQueue *q, uint32_t index, OH_AVBuffer *buf) {
    OhNode *n;
    if (!q->inited) return;
    n = (OhNode *)malloc(sizeof(OhNode));
    if (!n) return;
    n->item.index = index;
    n->item.buf = buf;
    n->next = NULL;
    pthread_mutex_lock(&q->mutex);
    if (q->aborted) { pthread_mutex_unlock(&q->mutex); free(n); return; }
    if (q->tail) q->tail->next = n; else q->head = n;
    q->tail = n;
    q->count++;
    pthread_cond_signal(&q->cond);
    pthread_mutex_unlock(&q->mutex);
}

/* Returns 1 = item, 0 = timeout, -1 = aborted. */
static int ohq_pop(OhQueue *q, int timeout_ms, OhBuf *out) {
    struct timespec ts;
    int rc = 0;
    if (!q->inited) return -1;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += timeout_ms / 1000;
    ts.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }

    pthread_mutex_lock(&q->mutex);
    while (!q->head && !q->aborted && rc != ETIMEDOUT) {
        if (timeout_ms < 0) rc = pthread_cond_wait(&q->cond, &q->mutex);
        else rc = pthread_cond_timedwait(&q->cond, &q->mutex, &ts);
    }
    if (q->head) {
        OhNode *n = q->head;
        q->head = n->next;
        if (!q->head) q->tail = NULL;
        q->count--;
        *out = n->item;
        pthread_mutex_unlock(&q->mutex);
        free(n);
        return 1;
    }
    rc = q->aborted ? -1 : 0;
    pthread_mutex_unlock(&q->mutex);
    return rc;
}

static void ohq_abort(OhQueue *q) {
    if (!q->inited) return;
    pthread_mutex_lock(&q->mutex);
    q->aborted = 1;
    pthread_cond_broadcast(&q->cond);
    pthread_mutex_unlock(&q->mutex);
}

/* ------------------------------------------------------------------------- */
/* STAGE 1: OHOS hardware decoder (Buffer mode)                              */
/* ------------------------------------------------------------------------- */

typedef struct {
    int fd;
    OH_AVSource *source;
    OH_AVDemuxer *demuxer;
    OH_AVCodec *dec;
    uint32_t track;
    int w, h;
    double fps;
    int input_eos;
    int output_eos;
    int64_t duration_us;
    int frame_index;
    OhMode mode;
    OhQueue in_q, out_q;
    int cb_error;
    int stride, slice, desc_ready;
} OhDecCtx;

typedef struct {
    OH_AVCodec *dec;
    uint32_t index;
} OhFrameRef;

static OhDecCtx *ohdec_ctx_of(void *ud) { return (OhDecCtx *)ud; }

static void ohdec_on_error(OH_AVCodec *codec, int32_t errorCode, void *ud) {
    (void)codec;
    OhDecCtx *c = ohdec_ctx_of(ud);
    if (!c) return;
    c->cb_error = errorCode;
    ohq_abort(&c->in_q);
    ohq_abort(&c->out_q);
}

static void ohdec_on_stream_changed(OH_AVCodec *codec, OH_AVFormat *format, void *ud) {
    (void)codec;
    OhDecCtx *c = ohdec_ctx_of(ud);
    if (!c || !format) return;
    hw_desc_stride(format, &c->stride, &c->slice);
    if (c->stride > 0 && c->slice > 0) c->desc_ready = 1;
}

static void ohdec_on_need_input(OH_AVCodec *codec, uint32_t index,
                                OH_AVBuffer *buffer, void *ud) {
    (void)codec;
    OhDecCtx *c = ohdec_ctx_of(ud);
    if (c) ohq_push(&c->in_q, index, buffer);
}

static void ohdec_on_new_output(OH_AVCodec *codec, uint32_t index,
                                OH_AVBuffer *buffer, void *ud) {
    (void)codec;
    OhDecCtx *c = ohdec_ctx_of(ud);
    if (c) ohq_push(&c->out_q, index, buffer);
}

/* Create + Configure + (RegisterCallback) + Prepare + Start for one mode. */
static int32_t ohdec_setup(OhDecCtx *c, const char *mime, OhMode mode,
                           char **out_error) {
    OH_AVErrCode rc;
    c->mode = mode;
    c->dec = OH_VideoDecoder_CreateByMime(mime);
    if (!c->dec) {
        hw_set_error(out_error, "OH_VideoDecoder_CreateByMime(%s) unavailable", mime);
        return CT_ERR_DECODE;
    }

    OH_AVFormat *fmt = OH_AVFormat_Create();
    if (!fmt) { hw_set_error(out_error, "OH_AVFormat_Create failed"); return CT_ERR_INTERNAL; }
    OH_AVFormat_SetIntValue(fmt, OH_MD_KEY_WIDTH, c->w);
    OH_AVFormat_SetIntValue(fmt, OH_MD_KEY_HEIGHT, c->h);
    OH_AVFormat_SetDoubleValue(fmt, OH_MD_KEY_FRAME_RATE, c->fps > 0 ? c->fps : 25.0);
    OH_AVFormat_SetIntValue(fmt, OH_MD_KEY_PIXEL_FORMAT, AV_PIXEL_FORMAT_NV12);
    OH_AVFormat_SetIntValue(fmt, OH_MD_KEY_ROTATION, 0);
    if (mode == OH_MODE_SYNC) {
        const char *key = oh_sync_key();
        if (key) OH_AVFormat_SetIntValue(fmt, key, 1);
    }
    rc = OH_VideoDecoder_Configure(c->dec, fmt);
    OH_AVFormat_Destroy(fmt);
    if (rc != AV_ERR_OK) { hw_errcode(out_error, "OH_VideoDecoder_Configure", rc); return CT_ERR_DECODE; }

    if (mode == OH_MODE_CALLBACK) {
        OH_AVCodecCallback cb;
        cb.onError = ohdec_on_error;
        cb.onStreamChanged = ohdec_on_stream_changed;
        cb.onNeedInputBuffer = ohdec_on_need_input;
        cb.onNewOutputBuffer = ohdec_on_new_output;
        rc = OH_VideoDecoder_RegisterCallback(c->dec, cb, c);
        if (rc != AV_ERR_OK) {
            hw_errcode(out_error, "OH_VideoDecoder_RegisterCallback", rc);
            return CT_ERR_DECODE;
        }
    }
    rc = OH_VideoDecoder_Prepare(c->dec);
    if (rc != AV_ERR_OK) { hw_errcode(out_error, "OH_VideoDecoder_Prepare", rc); return CT_ERR_DECODE; }
    rc = OH_VideoDecoder_Start(c->dec);
    if (rc != AV_ERR_OK) { hw_errcode(out_error, "OH_VideoDecoder_Start", rc); return CT_ERR_DECODE; }
    return CT_OK;
}

static void ohdec_destroy_codec(OhDecCtx *c) {
    if (!c->dec) return;
    OH_VideoDecoder_Stop(c->dec);
    OH_VideoDecoder_Destroy(c->dec);
    c->dec = NULL;
}

/* Get an EMPTY input buffer (sync: Query+Get; callback: pop queue). */
static int ohdec_next_input(OhDecCtx *c, OhBuf *out, int timeout_ms) {
    if (c->mode == OH_MODE_CALLBACK) return ohq_pop(&c->in_q, timeout_ms, out);
    uint32_t idx = 0;
    OH_AVErrCode rc = OH_VideoDecoder_QueryInputBuffer(
        c->dec, &idx, (int64_t)timeout_ms * 1000);
    if (rc != AV_ERR_OK) return 0;
    OH_AVBuffer *buf = OH_VideoDecoder_GetInputBuffer(c->dec, idx);
    if (!buf) return 0;
    out->index = idx;
    out->buf = buf;
    return 1;
}

/* Get a FILLED output buffer (sync: Query+Get; callback: pop queue). */
static int ohdec_next_output(OhDecCtx *c, OhBuf *out, int timeout_ms) {
    if (c->mode == OH_MODE_CALLBACK) return ohq_pop(&c->out_q, timeout_ms, out);
    uint32_t idx = 0;
    OH_AVErrCode rc = OH_VideoDecoder_QueryOutputBuffer(
        c->dec, &idx, (int64_t)timeout_ms * 1000);
    if (rc != AV_ERR_OK) return 0;
    OH_AVBuffer *buf = OH_VideoDecoder_GetOutputBuffer(c->dec, idx);
    if (!buf) return 0;
    out->index = idx;
    out->buf = buf;
    return 1;
}

/* Demux compressed samples into every input buffer the codec offered us. */
static int32_t ohdec_pump_input(OhDecCtx *c, char **out_error) {
    if (c->input_eos) return CT_OK;
    for (int i = 0; i < 16; ++i) {
        OhBuf ib;
        if (ohdec_next_input(c, &ib, 0) != 1) break;
        OH_AVErrCode sr = OH_AVDemuxer_ReadSampleBuffer(c->demuxer, c->track, ib.buf);
        if (sr != AV_ERR_OK) {
            /* In BUFFER mode EOS travels as a flag on an input OH_AVBuffer.
             * NotifyEndOfStream() is surface-mode only and must not be used. */
            OH_AVCodecBufferAttr attr;
            memset(&attr, 0, sizeof(attr));
            attr.flags = AVCODEC_BUFFER_FLAGS_EOS;
            OH_AVBuffer_SetBufferAttr(ib.buf, &attr);
            c->input_eos = 1;
        }
        OH_AVErrCode pr = OH_VideoDecoder_PushInputBuffer(c->dec, ib.index);
        if (pr != AV_ERR_OK) {
            hw_errcode(out_error, "OH_VideoDecoder_PushInputBuffer", pr);
            return CT_ERR_DECODE;
        }
        if (c->input_eos) break;
    }
    return CT_OK;
}

static int32_t ohdec_open(ct_decoder_t *self, const char *src_uri, char **out_error) {
    OhDecCtx *c = (OhDecCtx *)calloc(1, sizeof(OhDecCtx));
    if (!c) { hw_set_error(out_error, "OHOS decoder ctx alloc failed"); return CT_ERR_INTERNAL; }
    self->ctx = c;
    c->fd = -1;
    c->track = 0xffffffffu;
    c->fps = 25.0;
    ohq_init(&c->in_q);
    ohq_init(&c->out_q);

    const char *path = hw_strip_uri(src_uri);
    struct stat st;
    c->fd = open(path, O_RDONLY);
    if (c->fd < 0 || fstat(c->fd, &st) != 0) {
        hw_set_error(out_error, "cannot open source: %s", path);
        return CT_ERR_IO;
    }
    c->source = OH_AVSource_CreateWithFD(c->fd, 0, (int64_t)st.st_size);
    if (!c->source) { hw_set_error(out_error, "OH_AVSource_CreateWithFD failed"); return CT_ERR_DECODE; }
    c->demuxer = OH_AVDemuxer_CreateWithSource(c->source);
    if (!c->demuxer) { hw_set_error(out_error, "OH_AVDemuxer_CreateWithSource failed"); return CT_ERR_DECODE; }

    OH_AVFormat *src_fmt = OH_AVSource_GetSourceFormat(c->source);
    int32_t track_count = 0;
    int64_t src_duration = 0;
    if (src_fmt) {
        OH_AVFormat_GetIntValue(src_fmt, OH_MD_KEY_TRACK_COUNT, &track_count);
        OH_AVFormat_GetLongValue(src_fmt, OH_MD_KEY_DURATION, &src_duration);
    }
    if (src_fmt) OH_AVFormat_Destroy(src_fmt);
    if (track_count <= 0) track_count = 1;

    int64_t duration = 0;
    for (int32_t i = 0; i < track_count; ++i) {
        OH_AVFormat *tf = OH_AVSource_GetTrackFormat(c->source, (uint32_t)i);
        if (!tf) continue;
        int32_t type = -1;
        OH_AVFormat_GetIntValue(tf, OH_MD_KEY_TRACK_TYPE, &type);
        if (type != MEDIA_TYPE_VID) { OH_AVFormat_Destroy(tf); continue; }
        c->track = (uint32_t)i;
        OH_AVFormat_GetIntValue(tf, OH_MD_KEY_WIDTH, &c->w);
        OH_AVFormat_GetIntValue(tf, OH_MD_KEY_HEIGHT, &c->h);
        OH_AVFormat_GetDoubleValue(tf, OH_MD_KEY_FRAME_RATE, &c->fps);
        OH_AVFormat_GetLongValue(tf, OH_MD_KEY_DURATION, &duration);
        OH_AVFormat_Destroy(tf);
        break;
    }
    if (c->track == 0xffffffffu || c->w <= 0 || c->h <= 0) {
        hw_set_error(out_error, "no usable video track in OH_AVSource");
        return CT_ERR_DECODE;
    }
    /* Re-read the mime into a durable buffer: GetStringValue returns a pointer
     * owned by the (now destroyed) OH_AVFormat. */
    char mime_buf[128] = {0};
    {
        OH_AVFormat *tf = OH_AVSource_GetTrackFormat(c->source, c->track);
        const char *m = NULL;
        if (tf) {
            OH_AVFormat_GetStringValue(tf, OH_MD_KEY_CODEC_MIME, &m);
            if (m) snprintf(mime_buf, sizeof(mime_buf), "%s", m);
            OH_AVFormat_Destroy(tf);
        }
        if (!mime_buf[0]) snprintf(mime_buf, sizeof(mime_buf), "%s",
                                   OH_AVCODEC_MIMETYPE_VIDEO_AVC);
    }

    OH_AVErrCode rc = OH_AVDemuxer_SelectTrackByID(c->demuxer, c->track);
    if (rc != AV_ERR_OK) { hw_errcode(out_error, "OH_AVDemuxer_SelectTrackByID", rc); return CT_ERR_DECODE; }

    OhMode mode = oh_choose_mode();
    int32_t st_setup = ohdec_setup(c, mime_buf, mode, out_error);
    if (st_setup != CT_OK) return st_setup;

    if (mode == OH_MODE_SYNC) {
        /* Even with the key a device may refuse polling. Probe once with a
         * zero timeout on the OUTPUT queue (does not consume an input buffer)
         * and rebuild the codec in callback mode when it is not permitted. */
        uint32_t idx = 0;
        OH_AVErrCode probe = OH_VideoDecoder_QueryOutputBuffer(c->dec, &idx, 0);
        if (probe == AV_ERR_OK) OH_VideoDecoder_FreeOutputBuffer(c->dec, idx);
        if (probe == AV_ERR_OPERATE_NOT_PERMIT || probe == AV_ERR_UNSUPPORT) {
            ct_pipeline_platform_log("CapTrim OHOS: sync decoder Buffer mode refused; switching to callback mode");
            ohdec_destroy_codec(c);
            st_setup = ohdec_setup(c, mime_buf, OH_MODE_CALLBACK, out_error);
            if (st_setup != CT_OK) return st_setup;
        }
    }

    /* Track-level duration is optional; the container-level value is the
     * reliable one. Without a duration ct_pipeline_run() cannot report
     * progress (it guards on total_us > 0). */
    c->duration_us = duration > 0 ? duration : (src_duration > 0 ? src_duration : 0);
    self->duration_us = c->duration_us;
    {
        char diag[192];
        snprintf(diag, sizeof(diag),
                 "CapTrim OHOS: hardware decoder ACTIVE mode=%s NV12 %dx%d dur=%lldus",
                 c->mode == OH_MODE_SYNC ? "sync" : "callback", c->w, c->h,
                 (long long)c->duration_us);
        ct_pipeline_platform_log(diag);
    }
    return CT_OK;
}

static int32_t ohdec_next(ct_decoder_t *self, ct_frame_t *out, char **out_error) {
    OhDecCtx *c = (OhDecCtx *)self->ctx;
    int empty_polls = 0;
    /* Bounded: a stalled codec must fail fast so ct_ff_transcode() can retry
     * the whole job in software. A long timeout here trips the OHOS
     * ThreadBlock6S watchdog because the self check runs on the UI thread. */
    for (;;) {
        int32_t st = ohdec_pump_input(c, out_error);
        if (st != CT_OK) return -st;

        OhBuf ob;
        int r = ohdec_next_output(c, &ob, 20);
        if (r == -1 || c->cb_error) {
            hw_set_error(out_error, "OHOS decoder callback error %d", c->cb_error);
            return -(CT_ERR_DECODE);
        }
        if (r == 1) {
            OH_AVCodecBufferAttr attr;
            memset(&attr, 0, sizeof(attr));
            OH_AVBuffer_GetBufferAttr(ob.buf, &attr);
            if (attr.flags & AVCODEC_BUFFER_FLAGS_EOS) {
                OH_VideoDecoder_FreeOutputBuffer(c->dec, ob.index);
                c->output_eos = 1;
                return 0;
            }

            OH_AVFormat *desc = NULL;
            if (!c->desc_ready) {
                desc = OH_VideoDecoder_GetOutputDescription(c->dec);
                if (desc) {
                    hw_desc_stride(desc, &c->stride, &c->slice);
                    c->desc_ready = 1;
                }
            }
            int stride = 0, slice = 0;
            hw_frame_stride(ob.buf, desc, c->w, c->h, &stride, &slice);
            if (desc) OH_AVFormat_Destroy(desc);

            uint8_t *addr = OH_AVBuffer_GetAddr(ob.buf);
            if (!addr) { OH_VideoDecoder_FreeOutputBuffer(c->dec, ob.index); continue; }

            OhFrameRef *ref = (OhFrameRef *)malloc(sizeof(OhFrameRef));
            if (!ref) {
                OH_VideoDecoder_FreeOutputBuffer(c->dec, ob.index);
                return -(CT_ERR_INTERNAL);
            }
            ref->dec = c->dec;
            ref->index = ob.index;

            memset(out, 0, sizeof(*out));
            out->format = CT_PIX_FMT_NV12;
            out->width = c->w;
            out->height = c->h;
            out->plane_count = 2;
            out->data[0] = addr;
            out->stride[0] = stride;
            out->data[1] = addr + (size_t)stride * slice;
            out->stride[1] = stride;
            out->pts_us = (attr.pts > 0) ? attr.pts
                                         : (int64_t)c->frame_index * 1000000 /
                                           (int)(c->fps > 0 ? c->fps : 25.0);
            out->opaque = ref;
            c->frame_index++;
            return 1;
        }

        if (c->output_eos) return 0;
        if (++empty_polls > 90) {   /* ~1.8 s of no output -> fail to fallback */
            char diag[256];
            snprintf(diag, sizeof(diag),
                     "CapTrim OHOS: decoder stalled mode=%s fed=%d input_eos=%d "
                     "cb_err=%d dims=%dx%d in_q=%d out_q=%d",
                     c->mode == OH_MODE_SYNC ? "sync" : "callback",
                     c->frame_index, c->input_eos, c->cb_error,
                     c->w, c->h, c->in_q.count, c->out_q.count);
            ct_pipeline_platform_log(diag);
            hw_set_error(out_error, "OHOS hardware decoder produced no frame (stalled Buffer mode)");
            return -(CT_ERR_DECODE);
        }
    }
}

static void ohdec_release(ct_decoder_t *self, ct_frame_t *frame) {
    (void)self;
    if (!frame || !frame->opaque) return;
    OhFrameRef *ref = (OhFrameRef *)frame->opaque;
    OH_VideoDecoder_FreeOutputBuffer(ref->dec, ref->index);
    free(ref);
    frame->opaque = NULL;
}

static void ohdec_close(ct_decoder_t *self) {
    OhDecCtx *c = (OhDecCtx *)self->ctx;
    if (!c) return;
    /* Stop callbacks from touching the queues before tearing them down. */
    ohq_abort(&c->in_q);
    ohq_abort(&c->out_q);
    ohdec_destroy_codec(c);
    ohq_destroy(&c->in_q);
    ohq_destroy(&c->out_q);
    if (c->demuxer) OH_AVDemuxer_Destroy(c->demuxer);
    if (c->source) OH_AVSource_Destroy(c->source);
    if (c->fd >= 0) close(c->fd);
    free(c);
    self->ctx = NULL;
}

ct_decoder_t *ct_decoder_create_ohos(void) {
    ct_decoder_t *d = (ct_decoder_t *)calloc(1, sizeof(ct_decoder_t));
    if (!d) return NULL;
    d->kind = CT_BACKEND_OHOS_HW;
    d->open = ohdec_open;
    d->next = ohdec_next;
    d->release_frame = ohdec_release;
    d->close = ohdec_close;
    return d;
}

/* ------------------------------------------------------------------------- */
/* STAGE 3: OHOS hardware encoder + OH_AVMuxer (Buffer mode)                 */
/* ------------------------------------------------------------------------- */

typedef struct {
    int fd;
    OH_AVCodec *enc;
    OH_AVMuxer *muxer;
    int32_t track;
    int w, h;
    int fps_num, fps_den;
    int64_t input_index;   /* PTS for input YUV frames                     */
    int64_t output_index;  /* PTS for encoded samples (separate counter!)  */
    int started;
    int finished;
    char *dst_path;
    OhMode mode;
    OhQueue in_q, out_q;
    int cb_error;
    int stride, slice, desc_ready;
    int layout_logged;
} OhEncCtx;

static OhEncCtx *ohenc_ctx_of(void *ud) { return (OhEncCtx *)ud; }

static void ohenc_on_error(OH_AVCodec *codec, int32_t errorCode, void *ud) {
    (void)codec;
    OhEncCtx *c = ohenc_ctx_of(ud);
    if (!c) return;
    c->cb_error = errorCode;
    ohq_abort(&c->in_q);
    ohq_abort(&c->out_q);
}

static void ohenc_on_stream_changed(OH_AVCodec *codec, OH_AVFormat *format, void *ud) {
    (void)codec;
    OhEncCtx *c = ohenc_ctx_of(ud);
    if (!c || !format) return;
    hw_desc_stride(format, &c->stride, &c->slice);
    if (c->stride > 0 && c->slice > 0) c->desc_ready = 1;
}

static void ohenc_on_need_input(OH_AVCodec *codec, uint32_t index,
                                OH_AVBuffer *buffer, void *ud) {
    (void)codec;
    OhEncCtx *c = ohenc_ctx_of(ud);
    if (c) ohq_push(&c->in_q, index, buffer);
}

static void ohenc_on_new_output(OH_AVCodec *codec, uint32_t index,
                                OH_AVBuffer *buffer, void *ud) {
    (void)codec;
    OhEncCtx *c = ohenc_ctx_of(ud);
    if (c) ohq_push(&c->out_q, index, buffer);
}

static double ohenc_fps(const OhEncCtx *c) {
    double fps = (c->fps_den > 0) ? (double)c->fps_num / (double)c->fps_den : 25.0;
    return fps > 0 ? fps : 25.0;
}

static int32_t ohenc_setup(OhEncCtx *c, const char *mime, OhMode mode,
                           char **out_error) {
    OH_AVErrCode rc;
    c->mode = mode;
    c->enc = OH_VideoEncoder_CreateByMime(mime);
    if (!c->enc) {
        hw_set_error(out_error, "OH_VideoEncoder_CreateByMime(%s) unavailable", mime);
        return CT_ERR_ENCODE;
    }
    OH_AVFormat *fmt = OH_AVFormat_Create();
    if (!fmt) { hw_set_error(out_error, "OH_AVFormat_Create failed"); return CT_ERR_INTERNAL; }
    OH_AVFormat_SetIntValue(fmt, OH_MD_KEY_WIDTH, c->w);
    OH_AVFormat_SetIntValue(fmt, OH_MD_KEY_HEIGHT, c->h);
    OH_AVFormat_SetIntValue(fmt, OH_MD_KEY_PIXEL_FORMAT, AV_PIXEL_FORMAT_NV12);
    OH_AVFormat_SetDoubleValue(fmt, OH_MD_KEY_FRAME_RATE, ohenc_fps(c));
    OH_AVFormat_SetLongValue(fmt, OH_MD_KEY_BITRATE, (int64_t)c->w * c->h * 2);
    OH_AVFormat_SetIntValue(fmt, OH_MD_KEY_I_FRAME_INTERVAL, 48);
    if (mode == OH_MODE_SYNC) {
        const char *key = oh_sync_key();
        if (key) OH_AVFormat_SetIntValue(fmt, key, 1);
    }
    rc = OH_VideoEncoder_Configure(c->enc, fmt);
    OH_AVFormat_Destroy(fmt);
    if (rc != AV_ERR_OK) { hw_errcode(out_error, "OH_VideoEncoder_Configure", rc); return CT_ERR_ENCODE; }

    if (mode == OH_MODE_CALLBACK) {
        OH_AVCodecCallback cb;
        cb.onError = ohenc_on_error;
        cb.onStreamChanged = ohenc_on_stream_changed;
        cb.onNeedInputBuffer = ohenc_on_need_input;
        cb.onNewOutputBuffer = ohenc_on_new_output;
        rc = OH_VideoEncoder_RegisterCallback(c->enc, cb, c);
        if (rc != AV_ERR_OK) {
            hw_errcode(out_error, "OH_VideoEncoder_RegisterCallback", rc);
            return CT_ERR_ENCODE;
        }
    }
    rc = OH_VideoEncoder_Prepare(c->enc);
    if (rc != AV_ERR_OK) { hw_errcode(out_error, "OH_VideoEncoder_Prepare", rc); return CT_ERR_ENCODE; }
    rc = OH_VideoEncoder_Start(c->enc);
    if (rc != AV_ERR_OK) { hw_errcode(out_error, "OH_VideoEncoder_Start", rc); return CT_ERR_ENCODE; }
    c->started = 1;
    return CT_OK;
}

static void ohenc_destroy_codec(OhEncCtx *c) {
    if (!c->enc) return;
    if (c->started) OH_VideoEncoder_Stop(c->enc);
    OH_VideoEncoder_Destroy(c->enc);
    c->enc = NULL;
    c->started = 0;
}

static int ohenc_next_input(OhEncCtx *c, OhBuf *out, int timeout_ms) {
    if (c->mode == OH_MODE_CALLBACK) return ohq_pop(&c->in_q, timeout_ms, out);
    uint32_t idx = 0;
    OH_AVErrCode rc = OH_VideoEncoder_QueryInputBuffer(
        c->enc, &idx, (int64_t)timeout_ms * 1000);
    if (rc != AV_ERR_OK) return 0;
    OH_AVBuffer *buf = OH_VideoEncoder_GetInputBuffer(c->enc, idx);
    if (!buf) return 0;
    out->index = idx;
    out->buf = buf;
    return 1;
}

static int ohenc_next_output(OhEncCtx *c, OhBuf *out, int timeout_ms) {
    if (c->mode == OH_MODE_CALLBACK) return ohq_pop(&c->out_q, timeout_ms, out);
    uint32_t idx = 0;
    OH_AVErrCode rc = OH_VideoEncoder_QueryOutputBuffer(
        c->enc, &idx, (int64_t)timeout_ms * 1000);
    if (rc != AV_ERR_OK) return 0;
    OH_AVBuffer *buf = OH_VideoEncoder_GetOutputBuffer(c->enc, idx);
    if (!buf) return 0;
    out->index = idx;
    out->buf = buf;
    return 1;
}

/* Convert the processor's contiguous I420 output into the encoder's NV12
 * input buffer. The buffer may be stride-aligned (per the official dimension
 * guide) OR allocated tightly at width*height*3/2 (as the official transcoding
 * sample assumes). Query BOTH the description and the real buffer capacity and
 * pick the layout that fits; guessing either one breaks on some devices. */
static int ohenc_fill_nv12(OhEncCtx *c, OH_AVBuffer *buf, const ct_frame_t *in) {
    int w = in->width, h = in->height;
    OH_AVFormat *desc = NULL;
    if (!c->desc_ready) {
        desc = OH_VideoEncoder_GetInputDescription(c->enc);
        if (desc) {
            hw_desc_stride(desc, &c->stride, &c->slice);
            c->desc_ready = 1;
        }
    }
    int d_stride = 0, d_slice = 0;
    hw_frame_stride(buf, desc, w, h, &d_stride, &d_slice);
    if (desc) OH_AVFormat_Destroy(desc);

    uint8_t *addr = OH_AVBuffer_GetAddr(buf);
    if (!addr) {
        ct_pipeline_platform_log("CapTrim OHOS: encoder input buffer has no address");
        return -1;
    }
    int32_t cap = OH_AVBuffer_GetCapacity(buf);
    size_t need = (size_t)d_stride * d_slice * 3 / 2;
    int padded = 1;
    /* A hardware description can advertise a larger stride than the actual
     * OH_AVBuffer allocation. Fall back to the contiguous layout the official
     * sample uses whenever the padded layout would overflow the buffer. */
    if (d_stride < w || d_slice < h || (cap > 0 && need > (size_t)cap)) {
        d_stride = w;
        d_slice = h;
        need = (size_t)w * h * 3 / 2;
        padded = 0;
    }
    if (cap > 0 && need > (size_t)cap) {
        char diag[256];
        snprintf(diag, sizeof(diag),
                 "CapTrim OHOS: encoder input too small w=%d h=%d cap=%d need=%d",
                 w, h, (int)cap, (int)need);
        ct_pipeline_platform_log(diag);
        return -1;
    }
    if (!c->layout_logged) {
        char diag[256];
        snprintf(diag, sizeof(diag),
                 "CapTrim OHOS: encoder input layout=%s w=%d h=%d stride=%d "
                 "slice=%d cap=%d need=%d",
                 padded ? "padded" : "contiguous", w, h, d_stride, d_slice,
                 (int)cap, (int)need);
        ct_pipeline_platform_log(diag);
        c->layout_logged = 1;
    }
    c->stride = d_stride;
    c->slice = d_slice;

    const int in_y = in->stride[0] > 0 ? in->stride[0] : w;
    const int in_u = in->stride[1] > 0 ? in->stride[1] : w / 2;
    const int in_v = in->stride[2] > 0 ? in->stride[2] : w / 2;
    uint8_t *dst_y = addr;
    uint8_t *dst_uv = addr + (size_t)d_stride * d_slice;

    /* Padding bytes must be zeroed or the encoder shows green edges. */
    memset(addr, 0, need);
    for (int y = 0; y < h; ++y) {
        memcpy(dst_y + (size_t)y * d_stride,
               in->data[0] + (size_t)y * in_y, (size_t)w);
    }
    for (int y = 0; y < h / 2; ++y) {
        const uint8_t *u = in->data[1] + (size_t)y * in_u;
        const uint8_t *v = in->data[2] + (size_t)y * in_v;
        uint8_t *d = dst_uv + (size_t)y * d_stride;
        for (int x = 0; x < w / 2; ++x) {
            d[2 * x] = u[x];
            d[2 * x + 1] = v[x];
        }
    }
    return (int)need;
}

/* Write EVERY encoded sample (including the codec-config / SPS-PPS buffer) to
 * the muxer. Only the EOS buffer is not written. */
static int32_t ohenc_write_output(OhEncCtx *c, const OhBuf *ob, char **out_error) {
    OH_AVCodecBufferAttr attr;
    memset(&attr, 0, sizeof(attr));
    OH_AVBuffer_GetBufferAttr(ob->buf, &attr);
    if (attr.flags & AVCODEC_BUFFER_FLAGS_EOS) {
        OH_VideoEncoder_FreeOutputBuffer(c->enc, ob->index);
        c->finished = 1;
        return CT_OK;
    }
    attr.pts = (int64_t)((double)c->output_index * 1000000.0 / ohenc_fps(c));
    if ((attr.flags & AVCODEC_BUFFER_FLAGS_SYNC_FRAME) ||
        attr.flags == AVCODEC_BUFFER_FLAGS_NONE) {
        c->output_index++;
    }
    OH_AVBuffer_SetBufferAttr(ob->buf, &attr);
    OH_AVErrCode wr = OH_AVMuxer_WriteSampleBuffer(c->muxer, (uint32_t)c->track, ob->buf);
    OH_VideoEncoder_FreeOutputBuffer(c->enc, ob->index);
    if (wr != AV_ERR_OK) { hw_errcode(out_error, "OH_AVMuxer_WriteSampleBuffer", wr); return CT_ERR_ENCODE; }
    return CT_OK;
}

static int32_t ohenc_drain(OhEncCtx *c, char **out_error) {
    for (int i = 0; i < 64; ++i) {
        OhBuf ob;
        int r = ohenc_next_output(c, &ob, 0);
        if (r == -1) { hw_set_error(out_error, "OHOS encoder callback error %d", c->cb_error); return CT_ERR_ENCODE; }
        if (r != 1) break;
        int32_t st = ohenc_write_output(c, &ob, out_error);
        if (st != CT_OK) return st;
    }
    return CT_OK;
}

static int32_t ohenc_open(ct_encoder_t *self, const ct_frame_t *first,
                          const char *dst_uri, const char *video_codec,
                          char **out_error) {
    OhEncCtx *c = (OhEncCtx *)calloc(1, sizeof(OhEncCtx));
    if (!c) { hw_set_error(out_error, "OHOS encoder ctx alloc failed"); return CT_ERR_INTERNAL; }
    self->ctx = c;
    c->fd = -1;
    c->track = -1;
    c->w = first->width;
    c->h = first->height;
    c->fps_num = self->fps_num > 0 ? self->fps_num : 25;
    c->fps_den = self->fps_den > 0 ? self->fps_den : 1;
    c->dst_path = hw_strdup(hw_strip_uri(dst_uri));
    ohq_init(&c->in_q);
    ohq_init(&c->out_q);

    const char *mime = OH_AVCODEC_MIMETYPE_VIDEO_AVC;
    if (video_codec && (strstr(video_codec, "265") || strstr(video_codec, "hevc"))) {
        mime = OH_AVCODEC_MIMETYPE_VIDEO_HEVC;
    }

    c->fd = open(c->dst_path, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (c->fd < 0) { hw_set_error(out_error, "cannot create output: %s", c->dst_path); return CT_ERR_IO; }
    c->muxer = OH_AVMuxer_Create(c->fd, AV_OUTPUT_FORMAT_MPEG_4);
    if (!c->muxer) { hw_set_error(out_error, "OH_AVMuxer_Create failed"); return CT_ERR_ENCODE; }

    OH_AVFormat *track_fmt = OH_AVFormat_CreateVideoFormat(mime, c->w, c->h);
    if (!track_fmt) { hw_set_error(out_error, "OH_AVFormat_CreateVideoFormat failed"); return CT_ERR_ENCODE; }
    OH_AVFormat_SetDoubleValue(track_fmt, OH_MD_KEY_FRAME_RATE, ohenc_fps(c));
    OH_AVFormat_SetStringValue(track_fmt, OH_MD_KEY_CODEC_MIME, mime);
    OH_AVErrCode rc = OH_AVMuxer_AddTrack(c->muxer, &c->track, track_fmt);
    OH_AVFormat_Destroy(track_fmt);
    if (rc != AV_ERR_OK || c->track < 0) {
        hw_errcode(out_error, "OH_AVMuxer_AddTrack", rc);
        return CT_ERR_ENCODE;
    }

    OhMode mode = oh_choose_mode();
    int32_t st_setup = ohenc_setup(c, mime, mode, out_error);
    if (st_setup != CT_OK) return st_setup;

    if (mode == OH_MODE_SYNC) {
        uint32_t idx = 0;
        OH_AVErrCode probe = OH_VideoEncoder_QueryOutputBuffer(c->enc, &idx, 0);
        if (probe == AV_ERR_OK) OH_VideoEncoder_FreeOutputBuffer(c->enc, idx);
        if (probe == AV_ERR_OPERATE_NOT_PERMIT || probe == AV_ERR_UNSUPPORT) {
            ct_pipeline_platform_log("CapTrim OHOS: sync encoder Buffer mode refused; switching to callback mode");
            ohenc_destroy_codec(c);
            st_setup = ohenc_setup(c, mime, OH_MODE_CALLBACK, out_error);
            if (st_setup != CT_OK) return st_setup;
        }
    }
    rc = OH_AVMuxer_Start(c->muxer);
    if (rc != AV_ERR_OK) { hw_errcode(out_error, "OH_AVMuxer_Start", rc); return CT_ERR_ENCODE; }
    {
        char diag[192];
        snprintf(diag, sizeof(diag),
                 "CapTrim OHOS: hardware encoder ACTIVE mode=%s NV12 %dx%d -> mp4",
                 c->mode == OH_MODE_SYNC ? "sync" : "callback", c->w, c->h);
        ct_pipeline_platform_log(diag);
    }
    return CT_OK;
}

static int32_t ohenc_push(ct_encoder_t *self, const ct_frame_t *in, char **out_error) {
    OhEncCtx *c = (OhEncCtx *)self->ctx;
    OhBuf ib;
    int got = 0;
    for (int guard = 0; guard < 90 && !got; ++guard) {
        int r = ohenc_next_input(c, &ib, 20);
        if (r == -1 || c->cb_error) {
            hw_set_error(out_error, "OHOS encoder callback error %d", c->cb_error);
            return CT_ERR_ENCODE;
        }
        if (r == 1) { got = 1; break; }
        int32_t st = ohenc_drain(c, out_error);
        if (st != CT_OK) return st;
    }
    if (!got) {
        hw_set_error(out_error, "OHOS hardware encoder input queue never became ready");
        return CT_ERR_ENCODE;
    }
    int size = ohenc_fill_nv12(c, ib.buf, in);
    if (size <= 0) {
        hw_set_error(out_error, "OHOS encoder input buffer too small / invalid stride");
        return CT_ERR_ENCODE;
    }
    OH_AVCodecBufferAttr attr;
    memset(&attr, 0, sizeof(attr));
    attr.pts = (int64_t)((double)c->input_index * 1000000.0 / ohenc_fps(c));
    attr.size = size;
    attr.offset = 0;
    attr.flags = AVCODEC_BUFFER_FLAGS_NONE;
    OH_AVBuffer_SetBufferAttr(ib.buf, &attr);
    OH_AVErrCode rc = OH_VideoEncoder_PushInputBuffer(c->enc, ib.index);
    if (rc != AV_ERR_OK) { hw_errcode(out_error, "OH_VideoEncoder_PushInputBuffer", rc); return CT_ERR_ENCODE; }
    c->input_index++;
    return ohenc_drain(c, out_error);
}

static int32_t ohenc_finish(ct_encoder_t *self, char **out_error) {
    OhEncCtx *c = (OhEncCtx *)self->ctx;
    OhBuf ib;
    int got = 0;
    /* Buffer mode EOS: push an input OH_AVBuffer carrying the EOS flag.
     * OH_VideoEncoder_NotifyEndOfStream() is SURFACE MODE ONLY (per NDK docs)
     * and silently does nothing here - using it was the original bug. */
    for (int guard = 0; guard < 90 && !got; ++guard) {
        int r = ohenc_next_input(c, &ib, 20);
        if (r == 1) { got = 1; break; }
        int32_t st = ohenc_drain(c, out_error);
        if (st != CT_OK) return st;
    }
    if (got) {
        OH_AVCodecBufferAttr attr;
        memset(&attr, 0, sizeof(attr));
        attr.pts = (int64_t)((double)c->input_index * 1000000.0 / ohenc_fps(c));
        attr.size = 0;
        attr.offset = 0;
        attr.flags = AVCODEC_BUFFER_FLAGS_EOS;
        OH_AVBuffer_SetBufferAttr(ib.buf, &attr);
        OH_VideoEncoder_PushInputBuffer(c->enc, ib.index);
    }
    for (int guard = 0; guard < 200 && !c->finished; ++guard) {
        int32_t st = ohenc_drain(c, out_error);
        if (st != CT_OK) return st;
        if (!c->finished) usleep(2000);
    }
    OH_AVErrCode rc = OH_AVMuxer_Stop(c->muxer);
    if (rc != AV_ERR_OK) { hw_errcode(out_error, "OH_AVMuxer_Stop", rc); return CT_ERR_ENCODE; }
    struct stat stt;
    if (c->dst_path && stat(c->dst_path, &stt) == 0 && stt.st_size < 1024) {
        hw_set_error(out_error, "OHOS muxer produced an empty/suspicious file (%lld bytes)",
                     (long long)stt.st_size);
        return CT_ERR_ENCODE;
    }
    {
        char diag[192];
        snprintf(diag, sizeof(diag),
                 "CapTrim OHOS: encoder finished mode=%s in=%lld out=%lld",
                 c->mode == OH_MODE_SYNC ? "sync" : "callback",
                 (long long)c->input_index, (long long)c->output_index);
        ct_pipeline_platform_log(diag);
    }
    return CT_OK;
}

static void ohenc_close(ct_encoder_t *self) {
    OhEncCtx *c = (OhEncCtx *)self->ctx;
    if (!c) return;
    ohq_abort(&c->in_q);
    ohq_abort(&c->out_q);
    ohenc_destroy_codec(c);
    ohq_destroy(&c->in_q);
    ohq_destroy(&c->out_q);
    if (c->muxer) OH_AVMuxer_Destroy(c->muxer);
    if (c->fd >= 0) close(c->fd);
    free(c->dst_path);
    free(c);
    self->ctx = NULL;
}

ct_encoder_t *ct_encoder_create_ohos(void) {
    ct_encoder_t *e = (ct_encoder_t *)calloc(1, sizeof(ct_encoder_t));
    if (!e) return NULL;
    e->kind = CT_BACKEND_OHOS_HW;
    e->open = ohenc_open;
    e->push = ohenc_push;
    e->finish = ohenc_finish;
    e->close = ohenc_close;
    return e;
}
