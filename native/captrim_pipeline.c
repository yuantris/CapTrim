/* CapTrim three-stage transcode pipeline: orchestration + FFmpeg stages.
 *
 * Platform neutral. Hardware decoder/encoder factories are weak symbols (see
 * captrim_pipeline.h); when they are absent or their open() fails, that single
 * stage falls back to the FFmpeg software implementation below. The middle
 * libavfilter stage is ALWAYS FFmpeg - that is the "complex edit command".
 *
 * Compiled into libcaptrim_ffmpeg.so together with captrim_ffmpeg.c and, on
 * HarmonyOS only, captrim_ohos_hw.c.
 */
#include "captrim_pipeline.h"

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libavutil/time.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libswscale/swscale.h>

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------- */
/* Shared helpers                                                            */
/* ------------------------------------------------------------------------- */

void ct_pipeline_string_free(char *s) { free(s); }

/* Diagnostic switch for the platform hardware module. ct_pipeline_run() sets
 * it from options->force_async before creating any codec; the OHOS backend
 * reads it in oh_choose_mode(). Single-threaded export pipeline, so a plain
 * static is sufficient. */
static int g_force_async = 0;
void ct_pipeline_set_force_async(int value) { g_force_async = value ? 1 : 0; }
int  ct_pipeline_force_async(void) { return g_force_async; }

/* strdup() is POSIX, not ISO C; musl hides it under strict -std=c11. */
static char *ctp_strdup(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char *p = (char *)malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

static void ctp_set_error(char **out_error, const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    if (!out_error) return;
    if (*out_error) { free(*out_error); *out_error = NULL; }
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    *out_error = ctp_strdup(buf);
}

static void ctp_av_error(char **out_error, const char *what, int err) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(err, buf, sizeof(buf));
    ctp_set_error(out_error, "%s: %s (%d)", what, buf, err);
}

static void ctp_log(const char *message) {
    if (ct_pipeline_platform_log) ct_pipeline_platform_log(message);
}

static void ctp_logf(const char *fmt, ...) {
    char buf[768];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    ctp_log(buf);
}

static void ctp_clear_error(char **err) {
    if (err && *err) { free(*err); *err = NULL; }
}

static const char *ctp_backend_name(ct_backend_kind_t k) {
    switch (k) {
        case CT_BACKEND_OHOS_HW:    return "OHOS_HW";
        case CT_BACKEND_ANDROID_HW: return "ANDROID_HW";
        default:                    return "FFMPEG_SW";
    }
}

static enum AVPixelFormat ctp_to_av_fmt(ct_pix_fmt_t f) {
    switch (f) {
        case CT_PIX_FMT_NV12: return AV_PIX_FMT_NV12;
        case CT_PIX_FMT_NV21: return AV_PIX_FMT_NV21;
        case CT_PIX_FMT_RGBA: return AV_PIX_FMT_RGBA;
        default:              return AV_PIX_FMT_YUV420P;
    }
}

static ct_pix_fmt_t ctp_from_av_fmt(enum AVPixelFormat f) {
    switch (f) {
        case AV_PIX_FMT_NV12: return CT_PIX_FMT_NV12;
        case AV_PIX_FMT_NV21: return CT_PIX_FMT_NV21;
        case AV_PIX_FMT_RGBA: return CT_PIX_FMT_RGBA;
        default:              return CT_PIX_FMT_I420;
    }
}

/* Describe an AVFrame as a ct_frame_t. data[] points into the AVFrame's
 * buffers; the producer owns them and release_frame() unrefs them. */
static void ctp_frame_from_av(ct_frame_t *out, AVFrame *f, AVRational tb,
                              void *opaque, int64_t fallback_pts_us) {
    memset(out, 0, sizeof(*out));
    out->format = ctp_from_av_fmt(f->format);
    out->width = f->width;
    out->height = f->height;
    out->plane_count = av_pix_fmt_count_planes(f->format);
    for (int i = 0; i < 4; ++i) {
        out->data[i] = f->data[i];
        out->stride[i] = f->linesize[i];
    }
    out->pts_us = (f->pts != AV_NOPTS_VALUE)
                      ? av_rescale_q(f->pts, tb, AV_TIME_BASE_Q)
                      : fallback_pts_us;
    out->opaque = opaque;
}

/* Copy a possibly strided/NV12 ct_frame_t into a CONTIGUOUS yuv420p AVFrame.
 * Never assumes width == stride: hardware buffers are 16/32/64-byte aligned. */
static int ctp_copy_420(AVFrame *dst, const ct_frame_t *in) {
    if (!dst->data[0]) {
        dst->format = AV_PIX_FMT_YUV420P;
        dst->width = in->width;
        dst->height = in->height;
        if (av_frame_get_buffer(dst, 1) < 0) return -1;  /* align=1 -> contiguous */
    }
    if (dst->width != in->width || dst->height != in->height) return -1;

    if (in->format == CT_PIX_FMT_NV12 || in->format == CT_PIX_FMT_NV21 ||
        in->format == CT_PIX_FMT_RGBA) {
        /* Hand semi-planar / packed sources to libswscale. */
        AVFrame tmp;
        memset(&tmp, 0, sizeof(tmp));
        tmp.format = ctp_to_av_fmt(in->format);
        tmp.width = in->width;
        tmp.height = in->height;
        for (int i = 0; i < 4; ++i) {
            tmp.data[i] = in->data[i];
            tmp.linesize[i] = in->stride[i];
        }
        struct SwsContext *sws = sws_getContext(
            in->width, in->height, tmp.format,
            in->width, in->height, AV_PIX_FMT_YUV420P,
            SWS_FAST_BILINEAR, NULL, NULL, NULL);
        if (!sws) return -1;
        int r = sws_scale(sws, (const uint8_t *const *)tmp.data, tmp.linesize,
                          0, in->height, dst->data, dst->linesize);
        sws_freeContext(sws);
        return r == in->height ? 0 : -1;
    }

    /* Planar I420: copy row by row so stride padding is skipped. */
    const int h = in->height, w = in->width;
    const int planes[3] = {0, 1, 2};
    for (int p = 0; p < 3; ++p) {
        int ph = (p == 0) ? h : h / 2;
        int pw = (p == 0) ? w : w / 2;
        const uint8_t *src = in->data[planes[p]];
        uint8_t *d = dst->data[p];
        int sstride = in->stride[planes[p]] > 0 ? in->stride[planes[p]] : pw;
        if (!src || !d) return -1;
        for (int y = 0; y < ph; ++y) {
            memcpy(d + (size_t)y * dst->linesize[p],
                   src + (size_t)y * sstride, (size_t)pw);
        }
    }
    return 0;
}

/* ------------------------------------------------------------------------- */
/* STAGE 1 (software): FFmpeg demux + libavcodec decode                      */
/* ------------------------------------------------------------------------- */

typedef struct {
    AVFormatContext *in;
    AVCodecContext *dec;
    AVPacket *pkt;
    AVFrame *frame;
    int video_index;
    int draining;
} SwDecCtx;

static int32_t swdec_open(ct_decoder_t *self, const char *src_uri, char **out_error) {
    SwDecCtx *c = (SwDecCtx *)calloc(1, sizeof(SwDecCtx));
    int ret;
    if (!c) { ctp_set_error(out_error, "decoder ctx alloc failed"); return CT_ERR_INTERNAL; }
    self->ctx = c;
    c->video_index = -1;

    ret = avformat_open_input(&c->in, src_uri, NULL, NULL);
    if (ret < 0) { ctp_av_error(out_error, "avformat_open_input", ret); return CT_ERR_IO; }
    ret = avformat_find_stream_info(c->in, NULL);
    if (ret < 0) { ctp_av_error(out_error, "avformat_find_stream_info", ret); return CT_ERR_DECODE; }

    for (unsigned i = 0; i < c->in->nb_streams; ++i) {
        if (c->in->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            c->video_index = (int)i;
            break;
        }
    }
    if (c->video_index < 0) {
        ctp_set_error(out_error, "no video stream in source");
        return CT_ERR_INVALID;
    }
    AVStream *vs = c->in->streams[c->video_index];
    const AVCodec *codec = avcodec_find_decoder(vs->codecpar->codec_id);
    if (!codec) {
        ctp_set_error(out_error, "no FFmpeg decoder for source codec");
        return CT_ERR_DECODE;
    }
    c->dec = avcodec_alloc_context3(codec);
    if (!c->dec || avcodec_parameters_to_context(c->dec, vs->codecpar) < 0) {
        ctp_set_error(out_error, "avcodec_parameters_to_context failed");
        return CT_ERR_DECODE;
    }
    c->dec->time_base = vs->time_base;
    ret = avcodec_open2(c->dec, codec, NULL);
    if (ret < 0) { ctp_av_error(out_error, "avcodec_open2(decoder)", ret); return CT_ERR_DECODE; }

    c->pkt = av_packet_alloc();
    c->frame = av_frame_alloc();
    if (!c->pkt || !c->frame) {
        ctp_set_error(out_error, "av_packet_alloc / av_frame_alloc failed");
        return CT_ERR_INTERNAL;
    }
    self->duration_us = c->in->duration > 0 ? c->in->duration : 0;
    return CT_OK;
}

static int32_t swdec_next(ct_decoder_t *self, ct_frame_t *out, char **out_error) {
    SwDecCtx *c = (SwDecCtx *)self->ctx;
    for (;;) {
        int ret = avcodec_receive_frame(c->dec, c->frame);
        if (ret == 0) {
            AVStream *vs = c->in->streams[c->video_index];
            ctp_frame_from_av(out, c->frame, vs->time_base, c->frame, 0);
            return 1;
        }
        if (ret == AVERROR_EOF) return 0;
        if (ret != AVERROR(EAGAIN)) {
            ctp_av_error(out_error, "avcodec_receive_frame", ret);
            return CT_ERR_DECODE;
        }
        if (c->draining) return 0;

        ret = av_read_frame(c->in, c->pkt);
        if (ret == AVERROR_EOF) {
            avcodec_send_packet(c->dec, NULL);
            c->draining = 1;
            continue;
        }
        if (ret < 0) {
            ctp_av_error(out_error, "av_read_frame", ret);
            return CT_ERR_DECODE;
        }
        if (c->pkt->stream_index != c->video_index) {
            av_packet_unref(c->pkt);
            continue;
        }
        ret = avcodec_send_packet(c->dec, c->pkt);
        av_packet_unref(c->pkt);
        if (ret < 0) {
            ctp_av_error(out_error, "avcodec_send_packet", ret);
            return CT_ERR_DECODE;
        }
    }
}

static void swdec_release(ct_decoder_t *self, ct_frame_t *frame) {
    (void)self;
    if (frame && frame->opaque) av_frame_unref((AVFrame *)frame->opaque);
}

static void swdec_close(ct_decoder_t *self) {
    SwDecCtx *c = (SwDecCtx *)self->ctx;
    if (!c) return;
    av_frame_free(&c->frame);
    av_packet_free(&c->pkt);
    avcodec_free_context(&c->dec);
    if (c->in) avformat_close_input(&c->in);
    free(c);
    self->ctx = NULL;
}

ct_decoder_t *ct_decoder_create_ffmpeg(void) {
    ct_decoder_t *d = (ct_decoder_t *)calloc(1, sizeof(ct_decoder_t));
    if (!d) return NULL;
    d->kind = CT_BACKEND_FFMPEG_SW;
    d->open = swdec_open;
    d->next = swdec_next;
    d->release_frame = swdec_release;
    d->close = swdec_close;
    return d;
}

/* ------------------------------------------------------------------------- */
/* STAGE 2: FFmpeg libavfilter (canonical, all platforms)                    */
/* ------------------------------------------------------------------------- */

typedef struct {
    AVFilterGraph *graph;
    AVFilterContext *bufsrc;
    AVFilterContext *sink;
    AVFrame *norm;   /* contiguous yuv420p input to the graph */
    AVFrame *out;    /* filtered frame handed to pull() */
    int flushed;
} SwProcCtx;

static int32_t swproc_open(ct_processor_t *self, const ct_frame_t *first,
                           const char *filter_desc, char **out_error) {
    SwProcCtx *c = (SwProcCtx *)calloc(1, sizeof(SwProcCtx));
    int ret;
    if (!c) { ctp_set_error(out_error, "processor ctx alloc failed"); return CT_ERR_INTERNAL; }
    self->ctx = c;

    const AVFilter *src = avfilter_get_by_name("buffer");
    const AVFilter *sink = avfilter_get_by_name("buffersink");
    if (!src || !sink) {
        ctp_set_error(out_error, "buffer/buffersink filters not compiled into FFmpeg");
        return CT_ERR_INTERNAL;
    }
    c->graph = avfilter_graph_alloc();
    if (!c->graph) { ctp_set_error(out_error, "avfilter_graph_alloc failed"); return CT_ERR_INTERNAL; }

    /* The buffer source is ALWAYS contiguous yuv420p: ctp_copy_420() has
     * already normalised hardware stride / NV12. Time base is microseconds so
     * ct_frame_t.pts_us can be used verbatim. */
    char args[256];
    snprintf(args, sizeof(args),
             "video_size=%dx%d:pix_fmt=%d:time_base=1/1000000:pixel_aspect=1/1",
             first->width, first->height, (int)AV_PIX_FMT_YUV420P);
    ret = avfilter_graph_create_filter(&c->bufsrc, src, "in", args, NULL, c->graph);
    if (ret < 0) { ctp_av_error(out_error, "create buffer filter", ret); return CT_ERR_INTERNAL; }
    ret = avfilter_graph_create_filter(&c->sink, sink, "out", NULL, NULL, c->graph);
    if (ret < 0) { ctp_av_error(out_error, "create buffersink filter", ret); return CT_ERR_INTERNAL; }

    /* av_opt_set_int_list() stops at the AV_PIX_FMT_NONE sentinel. Using
     * av_opt_set_bin(sizeof(array)) would register -1 as a real pixel format
     * and avfilter_graph_config() dereferences a NULL descriptor -> SIGSEGV. */
    static const enum AVPixelFormat pix_fmts[] = {AV_PIX_FMT_YUV420P, AV_PIX_FMT_NONE};
    ret = av_opt_set_int_list(c->sink, "pix_fmts", pix_fmts,
                              AV_PIX_FMT_NONE, AV_OPT_SEARCH_CHILDREN);
    if (ret < 0) { ctp_av_error(out_error, "set buffersink pix_fmts", ret); return CT_ERR_INTERNAL; }

    AVFilterInOut *inputs = avfilter_inout_alloc();
    AVFilterInOut *outputs = avfilter_inout_alloc();
    if (!inputs || !outputs) {
        avfilter_inout_free(&inputs);
        avfilter_inout_free(&outputs);
        ctp_set_error(out_error, "avfilter_inout_alloc failed");
        return CT_ERR_INTERNAL;
    }
    outputs->name = av_strdup("in");
    outputs->filter_ctx = c->bufsrc;
    outputs->pad_idx = 0;
    inputs->name = av_strdup("out");
    inputs->filter_ctx = c->sink;
    inputs->pad_idx = 0;

    const char *desc = (filter_desc && *filter_desc) ? filter_desc : "null";
    ret = avfilter_graph_parse_ptr(c->graph, desc, &inputs, &outputs, NULL);
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);
    if (ret < 0) { ctp_av_error(out_error, "avfilter_graph_parse_ptr", ret); return CT_ERR_INVALID; }
    ret = avfilter_graph_config(c->graph, NULL);
    if (ret < 0) { ctp_av_error(out_error, "avfilter_graph_config", ret); return CT_ERR_INVALID; }

    c->norm = av_frame_alloc();
    c->out = av_frame_alloc();
    if (!c->norm || !c->out) {
        ctp_set_error(out_error, "av_frame_alloc(processor) failed");
        return CT_ERR_INTERNAL;
    }
    c->norm->format = AV_PIX_FMT_YUV420P;
    c->norm->width = first->width;
    c->norm->height = first->height;
    if (av_frame_get_buffer(c->norm, 1) < 0) {
        ctp_set_error(out_error, "av_frame_get_buffer(norm) failed");
        return CT_ERR_INTERNAL;
    }

    AVRational fr = av_buffersink_get_frame_rate(c->sink);
    if (fr.num > 0 && fr.den > 0) {
        self->out_fps_num = fr.num;
        self->out_fps_den = fr.den;
    } else {
        self->out_fps_num = 25;
        self->out_fps_den = 1;
    }
    return CT_OK;
}

static int32_t swproc_push(ct_processor_t *self, const ct_frame_t *in, char **out_error) {
    SwProcCtx *c = (SwProcCtx *)self->ctx;
    int ret;
    /* AV_BUFFERSRC_FLAG_KEEP_REF makes the graph hold a reference to the SAME
     * AVBuffer. Reusing c->norm's storage for the next frame would overwrite
     * queued frames. Give every pushed frame its own buffer. */
    av_frame_unref(c->norm);
    c->norm->format = AV_PIX_FMT_YUV420P;
    c->norm->width = in->width;
    c->norm->height = in->height;
    if (ctp_copy_420(c->norm, in) < 0) {
        ctp_set_error(out_error, "normalising input frame to yuv420p failed");
        return CT_ERR_DECODE;
    }
    c->norm->pts = in->pts_us;
    c->norm->pict_type = AV_PICTURE_TYPE_NONE;
    ret = av_buffersrc_add_frame_flags(c->bufsrc, c->norm,
                                       AV_BUFFERSRC_FLAG_KEEP_REF);
    if (ret == AVERROR_EOF) {
        /* A terminating filter (trim=duration=...) has already closed the
         * graph. Further decoded frames are irrelevant - signal the
         * orchestrator to stop decoding instead of treating this as an error. */
        self->graph_done = 1;
        /* Make sure the buffersink reaches EOF so any frames still buffered
         * in fps/scale are drained before the orchestrator stops decoding. */
        int eof_ret = av_buffersrc_add_frame_flags(c->bufsrc, NULL, 0);
        (void)eof_ret;
        return CT_OK;
    }
    if (ret < 0) {
        ctp_av_error(out_error, "av_buffersrc_add_frame_flags", ret);
        return CT_ERR_DECODE;
    }
    return CT_OK;
}

static int32_t swproc_pull(ct_processor_t *self, ct_frame_t *out, char **out_error) {
    SwProcCtx *c = (SwProcCtx *)self->ctx;
    int ret = av_buffersink_get_frame(c->sink, c->out);
    if (ret == AVERROR(EAGAIN)) return 0;
    if (ret == AVERROR_EOF) return -1;
    if (ret < 0) {
        ctp_av_error(out_error, "av_buffersink_get_frame", ret);
        return CT_ERR_DECODE;
    }
    ctp_frame_from_av(out, c->out, AV_TIME_BASE_Q, c->out, 0);
    return 1;
}

static int32_t swproc_flush(ct_processor_t *self, char **out_error) {
    SwProcCtx *c = (SwProcCtx *)self->ctx;
    if (c->flushed || self->graph_done) return CT_OK;
    c->flushed = 1;
    int ret = av_buffersrc_add_frame_flags(c->bufsrc, NULL, 0);
    if (ret == AVERROR_EOF) { self->graph_done = 1; return CT_OK; }
    if (ret < 0) {
        ctp_av_error(out_error, "av_buffersrc_add_frame_flags(eof)", ret);
        return CT_ERR_DECODE;
    }
    return CT_OK;
}

static void swproc_release(ct_processor_t *self, ct_frame_t *frame) {
    (void)self;
    if (frame && frame->opaque) av_frame_unref((AVFrame *)frame->opaque);
}

static void swproc_close(ct_processor_t *self) {
    SwProcCtx *c = (SwProcCtx *)self->ctx;
    if (!c) return;
    av_frame_free(&c->out);
    av_frame_free(&c->norm);
    if (c->graph) avfilter_graph_free(&c->graph);
    free(c);
    self->ctx = NULL;
}

ct_processor_t *ct_processor_create_ffmpeg(void) {
    ct_processor_t *p = (ct_processor_t *)calloc(1, sizeof(ct_processor_t));
    if (!p) return NULL;
    p->kind = CT_BACKEND_FFMPEG_SW;
    p->open = swproc_open;
    p->push = swproc_push;
    p->pull = swproc_pull;
    p->flush = swproc_flush;
    p->release_frame = swproc_release;
    p->close = swproc_close;
    return p;
}

/* ------------------------------------------------------------------------- */
/* STAGE 3 (software): libx264/libx265 + FFmpeg mp4 muxer                    */
/* ------------------------------------------------------------------------- */

typedef struct {
    AVCodecContext *enc;
    AVFormatContext *out;
    AVStream *vs;
    AVPacket *pkt;
    AVFrame *frame;
    int64_t out_pts;
    char *dst_uri;
} SwEncCtx;

static int32_t swenc_drain(SwEncCtx *c, char **out_error) {
    int ret;
    for (;;) {
        ret = avcodec_receive_packet(c->enc, c->pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) return CT_OK;
        if (ret < 0) { ctp_av_error(out_error, "avcodec_receive_packet", ret); return CT_ERR_ENCODE; }
        av_packet_rescale_ts(c->pkt, c->enc->time_base, c->vs->time_base);
        c->pkt->stream_index = c->vs->index;
        ret = av_interleaved_write_frame(c->out, c->pkt);
        av_packet_unref(c->pkt);
        if (ret < 0) { ctp_av_error(out_error, "av_interleaved_write_frame", ret); return CT_ERR_ENCODE; }
    }
}

static int32_t swenc_open(ct_encoder_t *self, const ct_frame_t *first,
                          const char *dst_uri, const char *video_codec,
                          char **out_error) {
    SwEncCtx *c = (SwEncCtx *)calloc(1, sizeof(SwEncCtx));
    int ret;
    if (!c) { ctp_set_error(out_error, "encoder ctx alloc failed"); return CT_ERR_INTERNAL; }
    self->ctx = c;
    c->dst_uri = ctp_strdup(dst_uri);

    const char *codec_name = (video_codec && *video_codec) ? video_codec : "libx264";
    const AVCodec *codec = avcodec_find_encoder_by_name(codec_name);
    if (!codec) {
        ctp_set_error(out_error, "encoder not available: %s", codec_name);
        return CT_ERR_ENCODE;
    }
    c->enc = avcodec_alloc_context3(codec);
    if (!c->enc) { ctp_set_error(out_error, "avcodec_alloc_context3 failed"); return CT_ERR_ENCODE; }
    c->enc->width = first->width;
    c->enc->height = first->height;
    c->enc->pix_fmt = AV_PIX_FMT_YUV420P;
    int fn = self->fps_num > 0 ? self->fps_num : 25;
    int fd = self->fps_den > 0 ? self->fps_den : 1;
    c->enc->framerate = (AVRational){fn, fd};
    c->enc->time_base = (AVRational){fd, fn};
    c->enc->gop_size = 48;
    c->enc->max_b_frames = 0;
    if (c->enc->codec_id == AV_CODEC_ID_H264 || c->enc->codec_id == AV_CODEC_ID_HEVC) {
        av_opt_set(c->enc->priv_data, "preset", "ultrafast", 0);
        av_opt_set(c->enc->priv_data, "crf", "26", 0);
    }
    ret = avcodec_open2(c->enc, codec, NULL);
    if (ret < 0) { ctp_av_error(out_error, "avcodec_open2(encoder)", ret); return CT_ERR_ENCODE; }

    ret = avformat_alloc_output_context2(&c->out, NULL, "mp4", dst_uri);
    if (ret < 0 || !c->out) {
        ctp_av_error(out_error, "avformat_alloc_output_context2(mp4)", ret);
        return CT_ERR_ENCODE;
    }
    c->vs = avformat_new_stream(c->out, NULL);
    if (!c->vs) { ctp_set_error(out_error, "avformat_new_stream failed"); return CT_ERR_ENCODE; }
    ret = avcodec_parameters_from_context(c->vs->codecpar, c->enc);
    if (ret < 0) { ctp_av_error(out_error, "avcodec_parameters_from_context", ret); return CT_ERR_ENCODE; }
    c->vs->time_base = c->enc->time_base;
    if (!(c->out->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open(&c->out->pb, dst_uri, AVIO_FLAG_WRITE);
        if (ret < 0) { ctp_av_error(out_error, "avio_open(dst)", ret); return CT_ERR_IO; }
    }
    ret = avformat_write_header(c->out, NULL);
    if (ret < 0) { ctp_av_error(out_error, "avformat_write_header", ret); return CT_ERR_ENCODE; }

    c->pkt = av_packet_alloc();
    c->frame = av_frame_alloc();
    if (!c->pkt || !c->frame) {
        ctp_set_error(out_error, "av_packet_alloc / av_frame_alloc failed");
        return CT_ERR_INTERNAL;
    }
    c->frame->format = AV_PIX_FMT_YUV420P;
    c->frame->width = first->width;
    c->frame->height = first->height;
    if (av_frame_get_buffer(c->frame, 1) < 0) {
        ctp_set_error(out_error, "av_frame_get_buffer(encoder) failed");
        return CT_ERR_INTERNAL;
    }
    return CT_OK;
}

static int32_t swenc_push(ct_encoder_t *self, const ct_frame_t *in, char **out_error) {
    SwEncCtx *c = (SwEncCtx *)self->ctx;
    /* avcodec_send_frame() keeps a reference to refcounted frames. Allocate a
     * fresh buffer per frame so the encoder's queued input is not overwritten. */
    av_frame_unref(c->frame);
    c->frame->format = AV_PIX_FMT_YUV420P;
    c->frame->width = in->width;
    c->frame->height = in->height;
    if (ctp_copy_420(c->frame, in) < 0) {
        ctp_set_error(out_error, "normalising filtered frame failed");
        return CT_ERR_ENCODE;
    }
    c->frame->pts = c->out_pts++;
    c->frame->pict_type = AV_PICTURE_TYPE_NONE;
    int ret = avcodec_send_frame(c->enc, c->frame);
    if (ret < 0) { ctp_av_error(out_error, "avcodec_send_frame", ret); return CT_ERR_ENCODE; }
    return swenc_drain(c, out_error);
}

static int32_t swenc_finish(ct_encoder_t *self, char **out_error) {
    SwEncCtx *c = (SwEncCtx *)self->ctx;
    int ret = avcodec_send_frame(c->enc, NULL);
    if (ret < 0) { ctp_av_error(out_error, "avcodec_send_frame(flush)", ret); return CT_ERR_ENCODE; }
    int32_t st = swenc_drain(c, out_error);
    if (st != CT_OK) return st;
    ret = av_write_trailer(c->out);
    if (ret < 0) { ctp_av_error(out_error, "av_write_trailer", ret); return CT_ERR_ENCODE; }
    return CT_OK;
}

static void swenc_close(ct_encoder_t *self) {
    SwEncCtx *c = (SwEncCtx *)self->ctx;
    if (!c) return;
    av_frame_free(&c->frame);
    av_packet_free(&c->pkt);
    avcodec_free_context(&c->enc);
    if (c->out) {
        if (!(c->out->oformat->flags & AVFMT_NOFILE) && c->out->pb) avio_closep(&c->out->pb);
        avformat_free_context(c->out);
    }
    free(c->dst_uri);
    free(c);
    self->ctx = NULL;
}

ct_encoder_t *ct_encoder_create_ffmpeg(void) {
    ct_encoder_t *e = (ct_encoder_t *)calloc(1, sizeof(ct_encoder_t));
    if (!e) return NULL;
    e->kind = CT_BACKEND_FFMPEG_SW;
    e->open = swenc_open;
    e->push = swenc_push;
    e->finish = swenc_finish;
    e->close = swenc_close;
    return e;
}

/* ------------------------------------------------------------------------- */
/* Orchestration                                                             */
/* ------------------------------------------------------------------------- */

static void ctp_free_decoder(ct_decoder_t *d) {
    if (!d) return;
    if (d->close) d->close(d);
    free(d);
}
static void ctp_free_encoder(ct_encoder_t *e) {
    if (!e) return;
    if (e->close) e->close(e);
    free(e);
}
static void ctp_free_processor(ct_processor_t *p) {
    if (!p) return;
    if (p->close) p->close(p);
    free(p);
}

static void ctp_note_fallback(ct_pipeline_report_t *rep, const char *fmt, ...) {
    char buf[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    size_t used = strlen(rep->fallback_reason);
    if (used && used < sizeof(rep->fallback_reason) - 1) {
        strncat(rep->fallback_reason, "; ", sizeof(rep->fallback_reason) - used - 1);
        used += 2;
    }
    if (used < sizeof(rep->fallback_reason) - 1) {
        strncat(rep->fallback_reason, buf, sizeof(rep->fallback_reason) - used - 1);
    }
}

static ct_decoder_t *ctp_pick_hardware_decoder(void) {
    if (ct_decoder_create_ohos) {
        ct_decoder_t *d = ct_decoder_create_ohos();
        if (d) return d;
    }
    if (ct_decoder_create_android) {
        ct_decoder_t *d = ct_decoder_create_android();
        if (d) return d;
    }
    return NULL;
}

static ct_encoder_t *ctp_pick_hardware_encoder(void) {
    if (ct_encoder_create_ohos) {
        ct_encoder_t *e = ct_encoder_create_ohos();
        if (e) return e;
    }
    if (ct_encoder_create_android) {
        ct_encoder_t *e = ct_encoder_create_android();
        if (e) return e;
    }
    return NULL;
}

/* Try to open the hardware encoder on the first FILTERED frame. On failure the
 * caller disposes it and retries with FFmpeg software (per-stage fallback). */
static ct_encoder_t *ctp_open_encoder(const ct_pipeline_options_t *opt,
                                      ct_pipeline_report_t *rep,
                                      const ct_frame_t *first,
                                      const char *dst_uri, const char *video_codec,
                                      int32_t fps_num, int32_t fps_den,
                                      char **err, int32_t *status) {
    ct_encoder_t *e = NULL;
    if (opt->prefer_hardware && !opt->force_software) {
        e = ctp_pick_hardware_encoder();
        if (e) {
            e->fps_num = fps_num;
            e->fps_den = fps_den;
            if (e->open(e, first, dst_uri, video_codec, err) == CT_OK) {
                rep->encoder = e->kind;
                ctp_logf("CapTrim pipeline: hardware encoder ACTIVE (%s)",
                         ctp_backend_name(e->kind));
                return e;
            }
            ctp_note_fallback(rep, "HW encoder: %s", err && *err ? *err : "open failed");
            ctp_logf("CapTrim pipeline: HW encoder rejected (%s), falling back",
                     err && *err ? *err : "open failed");
            ctp_free_encoder(e);
            e = NULL;
            ctp_clear_error(err);
            if (!opt->allow_sw_fallback) {
                ctp_set_error(err, "hardware encoder unavailable and fallback disabled");
                *status = CT_ERR_ENCODE;
                return NULL;
            }
        } else {
            ctp_note_fallback(rep, "HW encoder factory unavailable");
        }
    }
    e = ct_encoder_create_ffmpeg();
    if (!e) { ctp_set_error(err, "software encoder factory failed"); *status = CT_ERR_INTERNAL; return NULL; }
    e->fps_num = fps_num;
    e->fps_den = fps_den;
    if (e->open(e, first, dst_uri, video_codec, err) != CT_OK) {
        int32_t code = (err && *err && strstr(*err, "avio_open")) ? CT_ERR_IO : CT_ERR_ENCODE;
        ctp_free_encoder(e);
        *status = code;
        return NULL;
    }
    rep->encoder = e->kind;
    ctp_logf("CapTrim pipeline: software encoder ACTIVE (%s)", ctp_backend_name(e->kind));
    return e;
}

int32_t ct_pipeline_run(const char *src_uri, const char *dst_uri,
                        const char *filter_desc, const char *video_codec,
                        const ct_pipeline_options_t *options,
                        ct_progress_cb progress, void *user_data,
                        char **out_error, ct_pipeline_report_t *out_report) {
    if (!src_uri || !dst_uri || !out_error) return CT_ERR_INVALID;
    *out_error = NULL;

    ct_pipeline_options_t defaults = {1, 1, 0, -1, 0};
    const ct_pipeline_options_t *opt = options ? options : &defaults;
    ct_pipeline_set_force_async(opt->force_async);
    ct_pipeline_report_t rep;
    memset(&rep, 0, sizeof(rep));
    rep.decoder = CT_BACKEND_FFMPEG_SW;
    rep.encoder = CT_BACKEND_FFMPEG_SW;

    char *err = NULL;
    int32_t status = CT_OK;
    ct_decoder_t *dec = NULL;
    ct_processor_t *proc = NULL;
    ct_encoder_t *enc = NULL;
    int proc_opened = 0;
    int64_t start_ms = (int64_t)av_gettime() / 1000;

    /* ---- STAGE 1 selection (per-stage fallback) ---- */
    if (opt->prefer_hardware && !opt->force_software) {
        ct_decoder_t *hw = ctp_pick_hardware_decoder();
        if (hw) {
            if (hw->open(hw, src_uri, &err) == CT_OK) {
                dec = hw;
                rep.decoder = hw->kind;
                ctp_logf("CapTrim pipeline: hardware decoder ACTIVE (%s)",
                         ctp_backend_name(hw->kind));
            } else {
                ctp_note_fallback(&rep, "HW decoder: %s", err ? err : "open failed");
                ctp_logf("CapTrim pipeline: HW decoder rejected (%s), falling back",
                         err ? err : "open failed");
                ctp_free_decoder(hw);
                ctp_clear_error(&err);
                if (!opt->allow_sw_fallback) {
                    ctp_set_error(&err, "hardware decoder unavailable and fallback disabled");
                    status = CT_ERR_DECODE;
                    goto cleanup;
                }
            }
        } else {
            ctp_note_fallback(&rep, "HW decoder factory unavailable");
        }
    }
    if (!dec) {
        dec = ct_decoder_create_ffmpeg();
        if (!dec) { ctp_set_error(&err, "software decoder factory failed"); status = CT_ERR_INTERNAL; goto cleanup; }
        if (dec->open(dec, src_uri, &err) != CT_OK) { status = CT_ERR_IO; goto cleanup; }
        ctp_log("CapTrim pipeline: software decoder ACTIVE (FFMPEG_SW)");
    }

    proc = ct_processor_create_ffmpeg();
    if (!proc) { ctp_set_error(&err, "processor factory failed"); status = CT_ERR_INTERNAL; goto cleanup; }

    /* ---- main loop: decode -> normalise/filter -> encode ---- */
    const int64_t total_us = dec->duration_us;
    for (;;) {
        ct_frame_t f;
        int r = dec->next(dec, &f, &err);
        if (r == 0) break;
        /* next(): 1 = frame, 0 = EOF, otherwise CT_ERR_* (software stages
         * return it positive, hardware stages negative). Only 1 is a frame. */
        if (r != 1) {
            if (dec->kind != CT_BACKEND_FFMPEG_SW) {
                ctp_note_fallback(&rep, "HW decoder stream failure: %s",
                                  err ? err : "no frame produced");
                ctp_logf("CapTrim pipeline: HW decoder failed mid-stream (%s)",
                         err ? err : "no frame produced");
            }
            status = r > 0 ? r : -r;
            goto cleanup;
        }
        rep.frames_decoded++;

        if (!proc_opened) {
            status = proc->open(proc, &f, filter_desc, &err);
            if (status != CT_OK) goto cleanup;
            proc_opened = 1;
            ctp_logf("CapTrim pipeline: filter graph ready, fps=%d/%d",
                     proc->out_fps_num, proc->out_fps_den);
        }
        status = proc->push(proc, &f, &err);
        dec->release_frame(dec, &f);
        if (status != CT_OK) goto cleanup;

        for (;;) {
            ct_frame_t o;
            int pr = proc->pull(proc, &o, &err);
            if (pr == 0) break;
            if (pr == -1) { proc->graph_done = 1; break; }
            if (pr != 1) { status = pr > 0 ? pr : CT_ERR_DECODE; goto cleanup; }

            if (!enc) {
                int32_t sub = CT_OK;
                enc = ctp_open_encoder(opt, &rep, &o, dst_uri, video_codec,
                                       proc->out_fps_num, proc->out_fps_den,
                                       &err, &sub);
                if (!enc) { status = sub ? sub : CT_ERR_ENCODE; goto cleanup; }
            }
            status = enc->push(enc, &o, &err);
            rep.frames_encoded++;
            int64_t pts = o.pts_us;
            proc->release_frame(proc, &o);
            if (status != CT_OK) goto cleanup;

            if (progress && total_us > 0 && pts > 0) {
                float v = (float)((double)pts / (double)total_us);
                if (v < 0.0f) v = 0.0f;
                if (v > 0.99f) v = 0.99f;
                if (progress(v, user_data) != 0) {
                    ctp_set_error(&err, "export cancelled by caller");
                    status = CT_ERR_CANCELLED;
                    goto cleanup;
                }
            }
        }
        /* A terminating filter (trim=...) finished early: stop decoding the
         * rest of the source instead of walking all 22 seconds on the UI
         * thread. The flush pass below still drains buffered frames. */
        if (proc->graph_done) break;
    }

    if (!proc_opened) {
        ctp_set_error(&err, "no decoded video frame in source");
        status = CT_ERR_DECODE;
        goto cleanup;
    }

    /* ---- flush filter graph, then encoder ---- */
    status = proc->flush(proc, &err);
    if (status != CT_OK) goto cleanup;
    for (;;) {
        ct_frame_t o;
        int pr = proc->pull(proc, &o, &err);
        if (pr == 0 || pr == -1) break;
        if (pr != 1) { status = pr > 0 ? pr : CT_ERR_DECODE; goto cleanup; }
        if (!enc) {
            int32_t sub = CT_OK;
            enc = ctp_open_encoder(opt, &rep, &o, dst_uri, video_codec,
                                   proc->out_fps_num, proc->out_fps_den,
                                   &err, &sub);
            if (!enc) { status = sub ? sub : CT_ERR_ENCODE; goto cleanup; }
        }
        status = enc->push(enc, &o, &err);
        rep.frames_encoded++;
        proc->release_frame(proc, &o);
        if (status != CT_OK) goto cleanup;
    }
    if (!enc) {
        ctp_set_error(&err, "filter graph produced no frames");
        status = CT_ERR_DECODE;
        goto cleanup;
    }
    status = enc->finish(enc, &err);
    if (status != CT_OK) goto cleanup;
    if (progress) progress(1.0f, user_data);

cleanup:
    rep.duration_ms = (int64_t)av_gettime() / 1000 - start_ms;
    ctp_logf("CapTrim pipeline: decoder=%s encoder=%s decoded=%d encoded=%d %lldms fallback=[%s]",
             ctp_backend_name(rep.decoder), ctp_backend_name(rep.encoder),
             rep.frames_decoded, rep.frames_encoded, (long long)rep.duration_ms,
             rep.fallback_reason[0] ? rep.fallback_reason : "none");
    if (status != CT_OK) {
        ctp_logf("CapTrim pipeline: FAILED status=%d error=%s",
                 status, err ? err : "(none)");
    }

    ctp_free_encoder(enc);
    ctp_free_processor(proc);
    ctp_free_decoder(dec);
    if (status != CT_OK) {
        remove(dst_uri);
        if (!err) ctp_set_error(&err, "pipeline failed with code %d", status);
        *out_error = err;
    } else {
        ctp_clear_error(&err);
    }
    if (out_report) *out_report = rep;
    return status;
}

/* ------------------------------------------------------------------------- */
/* Capability query                                                          */
/* ------------------------------------------------------------------------- */

int32_t ct_pipeline_hardware_caps(char **out_json) {
    if (!out_json) return CT_ERR_INVALID;
    *out_json = NULL;
    char platform[1024] = {0};
    if (ct_pipeline_platform_caps) {
        char *p = NULL;
        if (ct_pipeline_platform_caps(&p) == CT_OK && p) {
            snprintf(platform, sizeof(platform), "%s", p);
            free(p);
        }
    }
    const size_t cap = 2048;
    char *buf = (char *)malloc(cap);
    if (!buf) return CT_ERR_INTERNAL;
    snprintf(buf, cap,
             "{\"ohosDecoder\":%s,\"ohosEncoder\":%s,"
             "\"androidDecoder\":%s,\"androidEncoder\":%s,"
             "\"software\":true,\"middleStage\":\"FFmpeg libavfilter\","
             "\"platform\":{%s}}",
             ct_decoder_create_ohos ? "true" : "false",
             ct_encoder_create_ohos ? "true" : "false",
             ct_decoder_create_android ? "true" : "false",
             ct_encoder_create_android ? "true" : "false",
             platform[0] ? platform : "");
    *out_json = buf;
    return CT_OK;
}
