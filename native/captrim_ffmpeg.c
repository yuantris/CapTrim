/* CapTrim FFmpeg C shim: probe, lossless remux, and the software transcode
 * backbone (decode -> libavfilter graph -> re-encode -> mp4 mux).
 *
 * No CLI tools. Build FFmpeg with --disable-everything plus the components
 * listed in scripts/ffmpeg-config-common.sh. This build is GPL because it
 * enables libx264 / libx265.
 */
#include "captrim_ffmpeg.h"
#include "captrim_common.h"
#include "captrim_pipeline.h"

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* StrBuf is declared in captrim_common.h so captrim_export.c can share it. */

int sb_init(StrBuf *b, size_t cap) {
    b->data = (char *)malloc(cap);
    b->len = 0;
    b->cap = cap;
    b->failed = 0;
    if (!b->data) {
        b->failed = 1;
        return -1;
    }
    b->data[0] = '\0';
    return 0;
}

static int sb_reserve(StrBuf *b, size_t extra) {
    if (b->failed) return -1;
    if (b->len + extra + 1 <= b->cap) return 0;
    size_t next = b->cap ? b->cap : 256;
    while (next < b->len + extra + 1) next *= 2;
    char *p = (char *)realloc(b->data, next);
    if (!p) {
        b->failed = 1;
        return -1;
    }
    b->data = p;
    b->cap = next;
    return 0;
}

void sb_puts(StrBuf *b, const char *s) {
    size_t n = strlen(s);
    if (sb_reserve(b, n) != 0) return;
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
}

void sb_printf(StrBuf *b, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0 || sb_reserve(b, (size_t)n) != 0) {
        va_end(ap2);
        return;
    }
    vsnprintf(b->data + b->len, b->cap - b->len, fmt, ap2);
    va_end(ap2);
    b->len += (size_t)n;
}

static void sb_put_json_string(StrBuf *b, const char *s) {
    sb_puts(b, "\"");
    if (!s) {
        sb_puts(b, "\"");
        return;
    }
    for (const unsigned char *p = (const unsigned char *)s; *p; ++p) {
        switch (*p) {
        case '"': sb_puts(b, "\\\""); break;
        case '\\': sb_puts(b, "\\\\"); break;
        case '\n': sb_puts(b, "\\n"); break;
        case '\r': sb_puts(b, "\\r"); break;
        case '\t': sb_puts(b, "\\t"); break;
        default:
            if (*p < 0x20) {
                sb_printf(b, "\\u%04x", (unsigned)*p);
            } else {
                char ch[2] = {(char)*p, '\0'};
                sb_puts(b, ch);
            }
        }
    }
    sb_puts(b, "\"");
}

static void ct_free(char *s) { free(s); }

void ct_ff_string_free(char *s) { ct_free(s); }

void ct_set_error(char **out_error, const char *fmt, ...) {
    if (!out_error) return;
    *out_error = NULL;
    StrBuf b;
    if (sb_init(&b, 256) != 0) return;
    va_list ap;
    va_start(ap, fmt);
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n >= 0 && sb_reserve(&b, (size_t)n) == 0) {
        vsnprintf(b.data + b.len, b.cap - b.len, fmt, ap2);
        b.len += (size_t)n;
    }
    va_end(ap2);
    *out_error = b.data;
}

/* FFmpeg explains *why* a filter graph failed only through av_log, and the
 * default callback writes to stderr, which is invisible inside an app process.
 * Capture the first error line for the duration of one parse/config call so a
 * failed burn reports "Option not found" or "Invalid filterchain" instead of a
 * bare EINVAL. */
static __thread char ct_log_tail[1024];

void ct_log_capture(void *avcl, int level, const char *fmt, va_list vl) {
    (void)avcl;
    if (level > AV_LOG_ERROR || ct_log_tail[0] != '\0') return;
    vsnprintf(ct_log_tail, sizeof(ct_log_tail), fmt, vl);
}

void ct_capture_begin(void) { ct_log_tail[0] = '\0'; }

void ct_capture_end(char **out_error, const char *what, int err) {
    char base[AV_ERROR_MAX_STRING_SIZE] = {0};
    if (av_strerror(err, base, sizeof(base)) < 0) {
        snprintf(base, sizeof(base), "code %d", err);
    }
    char joined[1600];
    if (ct_log_tail[0] != '\0') {
        snprintf(joined, sizeof(joined), "%s: %s (ffmpeg: %s)", what, base, ct_log_tail);
    } else {
        snprintf(joined, sizeof(joined), "%s: %s", what, base);
    }
    ct_set_error(out_error, "%s", joined);
}

void ct_av_error(char **out_error, const char *what, int err) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
    if (av_strerror(err, buf, sizeof(buf)) < 0) {
        snprintf(buf, sizeof(buf), "code %d", err);
    }
    ct_set_error(out_error, "%s: %s", what, buf);
}

static const char *kind_name(enum AVMediaType type) {
    switch (type) {
    case AVMEDIA_TYPE_VIDEO: return "video";
    case AVMEDIA_TYPE_AUDIO: return "audio";
    case AVMEDIA_TYPE_SUBTITLE: return "subtitle";
    case AVMEDIA_TYPE_DATA: return "data";
    default: return "unknown";
    }
}

static int64_t stream_duration_us(const AVFormatContext *fmt, const AVStream *s) {
    if (s->duration != AV_NOPTS_VALUE && s->duration > 0) {
        return av_rescale_q(s->duration, s->time_base, AV_TIME_BASE_Q);
    }
    return fmt->duration > 0 ? fmt->duration : 0;
}

int32_t ct_ff_probe(const char *uri, char **out_json, char **out_error) {
    if (!uri || !out_json || !out_error) return CT_ERR_INVALID;
    *out_json = NULL;
    *out_error = NULL;

    AVFormatContext *fmt = NULL;
    int ret = avformat_open_input(&fmt, uri, NULL, NULL);
    if (ret < 0) {
        ct_av_error(out_error, "avformat_open_input", ret);
        return CT_ERR_IO;
    }
    ret = avformat_find_stream_info(fmt, NULL);
    if (ret < 0) {
        ct_av_error(out_error, "avformat_find_stream_info", ret);
        avformat_close_input(&fmt);
        return CT_ERR_DECODE;
    }

    StrBuf b;
    if (sb_init(&b, 2048) != 0) {
        avformat_close_input(&fmt);
        ct_set_error(out_error, "out of memory building probe JSON");
        return CT_ERR_INTERNAL;
    }

    sb_puts(&b, "{\"uri\":");
    sb_put_json_string(&b, uri);
    sb_puts(&b, ",\"format\":");
    sb_put_json_string(&b, fmt->iformat ? fmt->iformat->name : "");
    sb_printf(&b, ",\"duration_us\":%" PRId64, fmt->duration > 0 ? fmt->duration : 0);
    sb_printf(&b, ",\"bitrate_bps\":%" PRId64, fmt->bit_rate > 0 ? fmt->bit_rate : 0);
    sb_puts(&b, ",\"streams\":[");

    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        const AVStream *s = fmt->streams[i];
        const AVCodecParameters *par = s->codecpar;
        const AVDictionaryEntry *rotate = av_dict_get(s->metadata, "rotate", NULL, 0);
        if (i) sb_puts(&b, ",");
        sb_puts(&b, "{\"index\":");
        sb_printf(&b, "%u", i);
        sb_puts(&b, ",\"kind\":");
        sb_put_json_string(&b, kind_name(par->codec_type));
        sb_puts(&b, ",\"codec\":");
        sb_put_json_string(&b, avcodec_get_name(par->codec_id));
        sb_printf(&b, ",\"time_base\":{\"num\":%d,\"den\":%d}",
                  s->time_base.num, s->time_base.den);
        sb_printf(&b, ",\"duration_us\":%" PRId64, stream_duration_us(fmt, s));

        if (par->codec_type == AVMEDIA_TYPE_VIDEO) {
            sb_printf(&b, ",\"width\":%d,\"height\":%d", par->width, par->height);
            if (s->avg_frame_rate.num > 0 && s->avg_frame_rate.den > 0) {
                sb_printf(&b, ",\"frame_rate\":{\"num\":%d,\"den\":%d}",
                          s->avg_frame_rate.num, s->avg_frame_rate.den);
            } else {
                sb_puts(&b, ",\"frame_rate\":null");
            }
            sb_puts(&b, ",\"sample_rate\":null,\"channels\":null");
        } else if (par->codec_type == AVMEDIA_TYPE_AUDIO) {
            sb_puts(&b, ",\"width\":null,\"height\":null,\"frame_rate\":null");
#if LIBAVUTIL_VERSION_MAJOR >= 57
            int channels = par->ch_layout.nb_channels;
#else
            int channels = par->channels;
#endif
            sb_printf(&b, ",\"sample_rate\":%d,\"channels\":%d",
                      par->sample_rate, channels);
        } else {
            sb_puts(&b, ",\"width\":null,\"height\":null,\"frame_rate\":null");
            sb_puts(&b, ",\"sample_rate\":null,\"channels\":null");
        }
        sb_printf(&b, ",\"rotation_deg\":%d",
                  rotate ? (int)strtol(rotate->value, NULL, 10) : 0);
        sb_printf(&b, ",\"bitrate_bps\":%" PRId64 "}",
                  par->bit_rate > 0 ? par->bit_rate : 0);
    }
    sb_puts(&b, "]}");

    avformat_close_input(&fmt);
    if (b.failed) {
        ct_free(b.data);
        ct_set_error(out_error, "out of memory building probe JSON");
        return CT_ERR_INTERNAL;
    }
    *out_json = b.data;
    return CT_OK;
}

int32_t ct_ff_remux(const char *src_uri, const char *dst_uri,
                    ct_progress_cb progress, void *user_data,
                    char **out_error) {
    if (!src_uri || !dst_uri || !out_error) return CT_ERR_INVALID;
    *out_error = NULL;

    AVFormatContext *in = NULL;
    AVFormatContext *out = NULL;
    AVPacket *pkt = NULL;
    int32_t status = CT_OK;

    int ret = avformat_open_input(&in, src_uri, NULL, NULL);
    if (ret < 0) {
        ct_av_error(out_error, "avformat_open_input", ret);
        return CT_ERR_IO;
    }
    ret = avformat_find_stream_info(in, NULL);
    if (ret < 0) {
        ct_av_error(out_error, "avformat_find_stream_info", ret);
        status = CT_ERR_DECODE;
        goto cleanup;
    }

    ret = avformat_alloc_output_context2(&out, NULL, "mp4", dst_uri);
    if (ret < 0 || !out) {
        ct_av_error(out_error, "avformat_alloc_output_context2(mp4)", ret);
        status = CT_ERR_ENCODE;
        goto cleanup;
    }

    for (unsigned i = 0; i < in->nb_streams; ++i) {
        AVStream *in_stream = in->streams[i];
        AVStream *out_stream = avformat_new_stream(out, NULL);
        if (!out_stream) {
            ct_set_error(out_error, "avformat_new_stream failed");
            status = CT_ERR_ENCODE;
            goto cleanup;
        }
        ret = avcodec_parameters_copy(out_stream->codecpar, in_stream->codecpar);
        if (ret < 0) {
            ct_av_error(out_error, "avcodec_parameters_copy", ret);
            status = CT_ERR_ENCODE;
            goto cleanup;
        }
        out_stream->codecpar->codec_tag = 0;
        out_stream->time_base = in_stream->time_base;
        av_dict_copy(&out_stream->metadata, in_stream->metadata, 0);
    }

    if (!(out->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open(&out->pb, dst_uri, AVIO_FLAG_WRITE);
        if (ret < 0) {
            ct_av_error(out_error, "avio_open", ret);
            status = CT_ERR_IO;
            goto cleanup;
        }
    }

    ret = avformat_write_header(out, NULL);
    if (ret < 0) {
        ct_av_error(out_error, "avformat_write_header", ret);
        status = CT_ERR_ENCODE;
        goto cleanup;
    }

    pkt = av_packet_alloc();
    if (!pkt) {
        ct_set_error(out_error, "av_packet_alloc failed");
        status = CT_ERR_INTERNAL;
        goto cleanup;
    }

    const int64_t total_us = in->duration > 0 ? in->duration : 0;
    for (;;) {
        ret = av_read_frame(in, pkt);
        if (ret == AVERROR_EOF) break;
        if (ret < 0) {
            ct_av_error(out_error, "av_read_frame", ret);
            status = CT_ERR_DECODE;
            goto cleanup;
        }
        if (pkt->stream_index < 0 || (unsigned)pkt->stream_index >= out->nb_streams) {
            av_packet_unref(pkt);
            continue;
        }

        AVStream *in_stream = in->streams[pkt->stream_index];
        AVStream *out_stream = out->streams[pkt->stream_index];

        if (progress && total_us > 0 && pkt->pts != AV_NOPTS_VALUE) {
            int64_t pos_us = av_rescale_q(pkt->pts, in_stream->time_base, AV_TIME_BASE_Q);
            float value = (float)((double)pos_us / (double)total_us);
            if (value < 0.0f) value = 0.0f;
            if (value > 1.0f) value = 1.0f;
            if (progress(value, user_data) != 0) {
                av_packet_unref(pkt);
                ct_set_error(out_error, "export cancelled by caller");
                status = CT_ERR_CANCELLED;
                goto cleanup;
            }
        }

        av_packet_rescale_ts(pkt, in_stream->time_base, out_stream->time_base);
        pkt->stream_index = out_stream->index;
        ret = av_interleaved_write_frame(out, pkt);
        if (ret < 0) {
            ct_av_error(out_error, "av_interleaved_write_frame", ret);
            av_packet_unref(pkt);
            status = CT_ERR_ENCODE;
            goto cleanup;
        }
        av_packet_unref(pkt);
    }

    ret = av_write_trailer(out);
    if (ret < 0) {
        ct_av_error(out_error, "av_write_trailer", ret);
        status = CT_ERR_ENCODE;
    }

cleanup:
    av_packet_free(&pkt);
    if (out) {
        if (!(out->oformat->flags & AVFMT_NOFILE) && out->pb) avio_closep(&out->pb);
        avformat_free_context(out);
    }
    if (in) avformat_close_input(&in);
    if (status == CT_ERR_CANCELLED) remove(dst_uri);
    return status;
}

/* ---------------------------------------------------------------------------
 * Transcode: decode -> libavfilter graph -> re-encode -> mux.
 *
 * This is the software backbone AND the fallback for the hardware paths: an
 * OHOS/Android hardware decoder produces raw frames that can be fed into the
 * same filter graph, and a hardware encoder can consume the filtered frames.
 * Keeping one canonical filter/encode loop here means "complex edit commands"
 * behave identically whether the first/last mile was hardware or software.
 *
 * `filter_desc` is a libavfilter graph string applied to video only, e.g.
 *   "scale=640:-2,fps=24,hflip"
 * NULL or "" means no filter (frames pass through, still re-encoded).
 * `video_codec` is an encoder name, e.g. "libx264" / "libx265" / "mpeg4";
 * NULL or "" defaults to "libx264". Audio is stream-copied unchanged.
 * ------------------------------------------------------------------------- */

typedef struct {
    AVFilterContext *buffersrc; /* "in"  filter, frame injection point */
    AVFilterContext *buffersink; /* "out" filter, filtered frame source */
    AVFilterGraph *graph;
} CtFilter;

static void ct_filter_free(CtFilter *f) {
    if (f->graph) {
        avfilter_graph_free(&f->graph);
    }
    f->buffersrc = NULL;
    f->buffersink = NULL;
}

/* Build: [in]buffer -> <filter_desc> -> [out]buffersink(yuv420p).
 * Forces yuv420p so libx264/libx265 (which need yuv420p for 8-bit) always work.
 * If filter_desc is empty, a scale passthrough keeps the graph valid. */
static int32_t ct_filter_build(CtFilter *f, const AVFrame *first,
                               AVRational time_base,
                               const char *filter_desc, char **out_error) {
    const AVFilter *src = avfilter_get_by_name("buffer");
    const AVFilter *sink = avfilter_get_by_name("buffersink");
    AVFilterInOut *inputs = NULL;
    AVFilterInOut *outputs = NULL;
    char args[512];
    int ret;

    f->graph = avfilter_graph_alloc();
    if (!f->graph || !src || !sink) {
        ct_set_error(out_error, "avfilter_graph_alloc / filter lookup failed");
        return CT_ERR_INTERNAL;
    }

    /* Describe the source using the FIRST DECODED FRAME, not the demuxer's
     * codecpar: containers generally report AV_PIX_FMT_NONE for video, and
     * putting -1 into the buffer filter makes avfilter_graph_config()
     * dereference a NULL AVPixFmtDescriptor (SIGSEGV on device). */
    if (!av_pix_fmt_desc_get(first->format)) {
        ct_set_error(out_error, "decoded frame has invalid pixel format %d",
                     first->format);
        return CT_ERR_INVALID;
    }
    AVRational sar = first->sample_aspect_ratio.num > 0
                         ? first->sample_aspect_ratio
                         : (AVRational){1, 1};
    snprintf(args, sizeof(args),
             "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
             first->width, first->height, first->format,
             time_base.num, time_base.den, sar.num, sar.den);

    ret = avfilter_graph_create_filter(&f->buffersrc, src, "in", args, NULL, f->graph);
    if (ret < 0) {
        ct_av_error(out_error, "avfilter_graph_create_filter(buffer)", ret);
        return CT_ERR_INTERNAL;
    }
    ret = avfilter_graph_create_filter(&f->buffersink, sink, "out", NULL, NULL, f->graph);
    if (ret < 0) {
        ct_av_error(out_error, "avfilter_graph_create_filter(buffersink)", ret);
        return CT_ERR_INTERNAL;
    }

    /* Constrain sink to yuv420p so the encoder never gets an unsupported fmt.
     * av_opt_set_int_list() stops at the AV_PIX_FMT_NONE sentinel. Passing the
     * whole array via av_opt_set_bin(sizeof(...)) would include -1 as a real
     * pixel format, and av_pix_fmt_desc_get(-1) is NULL -> SIGSEGV inside
     * avfilter_graph_config()'s format merging. */
    static const enum AVPixelFormat pix_fmts[] = {AV_PIX_FMT_YUV420P, AV_PIX_FMT_NONE};
    ret = av_opt_set_int_list(f->buffersink, "pix_fmts", pix_fmts,
                              AV_PIX_FMT_NONE, AV_OPT_SEARCH_CHILDREN);
    if (ret < 0) {
        ct_av_error(out_error, "av_opt_set_bin(pix_fmts)", ret);
        return CT_ERR_INTERNAL;
    }

    outputs = avfilter_inout_alloc();
    inputs = avfilter_inout_alloc();
    if (!outputs || !inputs) {
        ct_set_error(out_error, "avfilter_inout_alloc failed");
        return CT_ERR_INTERNAL;
    }
    outputs->name = av_strdup("in");
    outputs->filter_ctx = f->buffersrc;
    outputs->pad_idx = 0;
    outputs->next = NULL;
    inputs->name = av_strdup("out");
    inputs->filter_ctx = f->buffersink;
    inputs->pad_idx = 0;
    inputs->next = NULL;

    const char *desc = (filter_desc && *filter_desc) ? filter_desc : "null";
    ct_capture_begin();
    av_log_set_callback(ct_log_capture);
    ret = avfilter_graph_parse_ptr(f->graph, desc, &inputs, &outputs, NULL);
    av_log_set_callback(NULL);
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);
    if (ret < 0) {
        ct_capture_end(out_error, "avfilter_graph_parse_ptr", ret);
        return CT_ERR_INVALID;
    }
    ret = avfilter_graph_config(f->graph, NULL);
    if (ret < 0) {
        ct_av_error(out_error, "avfilter_graph_config", ret);
        return CT_ERR_INVALID;
    }
    return CT_OK;
}

/* Drain one filtered frame from the sink, send it to the encoder, and mux any
 * packets the encoder emits. Returns CT_OK when the sink/encoder needs more
 * input (EAGAIN) or is flushed (EOF); an error code otherwise. */
static int32_t ct_filter_drain(AVFilterContext *sink, AVCodecContext *enc,
                              AVStream *out_stream, AVFormatContext *out,
                              AVPacket *pkt, int64_t *out_pts,
                              ct_progress_cb progress, void *user_data,
                              int64_t total_us, char **out_error) {
    AVFrame *frame = av_frame_alloc();
    int32_t status = CT_OK;
    int ret;
    if (!frame) {
        ct_set_error(out_error, "av_frame_alloc failed");
        return CT_ERR_INTERNAL;
    }

    for (;;) {
        ret = av_buffersink_get_frame(sink, frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
        if (ret < 0) {
            ct_av_error(out_error, "av_buffersink_get_frame", ret);
            status = CT_ERR_DECODE;
            break;
        }

        /* Re-stamp onto the encoder timebase with a strictly increasing PTS.
         * Frame rate comes from the encoder context set up by the caller. */
        frame->pict_type = AV_PICTURE_TYPE_NONE;
        frame->pts = (*out_pts)++;

        ret = avcodec_send_frame(enc, frame);
        av_frame_unref(frame);
        if (ret < 0) {
            ct_av_error(out_error, "avcodec_send_frame", ret);
            status = CT_ERR_ENCODE;
            break;
        }

        for (;;) {
            ret = avcodec_receive_packet(enc, pkt);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
            if (ret < 0) {
                ct_av_error(out_error, "avcodec_receive_packet", ret);
                status = CT_ERR_ENCODE;
                break;
            }
            av_packet_rescale_ts(pkt, enc->time_base, out_stream->time_base);
            pkt->stream_index = out_stream->index;

            if (progress && total_us > 0 && pkt->pts != AV_NOPTS_VALUE) {
                int64_t pos_us = av_rescale_q(pkt->pts, out_stream->time_base, AV_TIME_BASE_Q);
                float value = (float)((double)pos_us / (double)total_us);
                if (value < 0.0f) value = 0.0f;
                if (value > 1.0f) value = 1.0f;
                if (progress(value, user_data) != 0) {
                    av_packet_unref(pkt);
                    ct_set_error(out_error, "export cancelled by caller");
                    status = CT_ERR_CANCELLED;
                    break;
                }
            }

            ret = av_interleaved_write_frame(out, pkt);
            av_packet_unref(pkt);
            if (ret < 0) {
                ct_av_error(out_error, "av_interleaved_write_frame", ret);
                status = CT_ERR_ENCODE;
                break;
            }
        }
        if (status != CT_OK) break;
    }

    av_frame_free(&frame);
    return status;
}

/* All state for one transcode job. The filter graph, encoder and muxer are
 * created lazily from the first decoded frame: only then are width, height and
 * pixel format actually known. */
typedef struct {
    AVFormatContext *in;
    AVFormatContext *out;
    AVCodecContext *dec;
    AVCodecContext *enc;
    CtFilter filter;
    AVStream *out_video;
    int video_index;
    int64_t out_pts;
    int64_t total_us;
    int ready;
    int graph_done;   /* a terminating filter (trim=...) closed the graph */
} CtTranscode;

static int32_t ct_transcode_prepare(CtTranscode *t, const AVFrame *first,
                                    const char *filter_desc,
                                    const char *codec_name,
                                    const char *dst_uri, char **out_error) {
    int ret;
    AVStream *in_video = t->in->streams[t->video_index];

    ret = ct_filter_build(&t->filter, first, in_video->time_base,
                          filter_desc, out_error);
    if (ret != CT_OK) return ret;

    const AVCodec *enc_codec = avcodec_find_encoder_by_name(codec_name);
    if (!enc_codec) {
        ct_set_error(out_error, "encoder not available: %s (build with --enable-%s)",
                     codec_name, codec_name);
        return CT_ERR_ENCODE;
    }
    t->enc = avcodec_alloc_context3(enc_codec);
    if (!t->enc) {
        ct_set_error(out_error, "avcodec_alloc_context3(encoder) failed");
        return CT_ERR_ENCODE;
    }
    t->enc->width = av_buffersink_get_w(t->filter.buffersink);
    t->enc->height = av_buffersink_get_h(t->filter.buffersink);
    t->enc->pix_fmt = av_buffersink_get_format(t->filter.buffersink);
    if (t->enc->pix_fmt == AV_PIX_FMT_NONE) t->enc->pix_fmt = AV_PIX_FMT_YUV420P;
    t->enc->sample_aspect_ratio =
        av_buffersink_get_sample_aspect_ratio(t->filter.buffersink);
    AVRational fr = av_buffersink_get_frame_rate(t->filter.buffersink);
    if (fr.num <= 0 || fr.den <= 0) fr = in_video->avg_frame_rate;
    if (fr.num <= 0 || fr.den <= 0) fr = (AVRational){25, 1};
    t->enc->framerate = fr;
    t->enc->time_base = av_inv_q(fr);
    t->enc->gop_size = 48;
    t->enc->max_b_frames = 0; /* low latency, no reorder surprises on mobile */
    t->enc->bit_rate = 0;     /* CRF mode below controls quality */
    if (t->enc->codec_id == AV_CODEC_ID_H264 || t->enc->codec_id == AV_CODEC_ID_HEVC) {
        av_opt_set(t->enc->priv_data, "preset", "ultrafast", 0);
        av_opt_set(t->enc->priv_data, "crf", "26", 0);
    }
    ret = avcodec_open2(t->enc, enc_codec, NULL);
    if (ret < 0) {
        ct_av_error(out_error, "avcodec_open2(encoder)", ret);
        return CT_ERR_ENCODE;
    }

    ret = avformat_alloc_output_context2(&t->out, NULL, "mp4", dst_uri);
    if (ret < 0 || !t->out) {
        ct_av_error(out_error, "avformat_alloc_output_context2(mp4)", ret);
        return CT_ERR_ENCODE;
    }
    /* Mirror every input stream: video is re-encoded, others are copied. */
    for (unsigned i = 0; i < t->in->nb_streams; ++i) {
        AVStream *is = t->in->streams[i];
        AVStream *os = avformat_new_stream(t->out, NULL);
        if (!os) {
            ct_set_error(out_error, "avformat_new_stream failed");
            return CT_ERR_ENCODE;
        }
        if ((int)i == t->video_index) {
            ret = avcodec_parameters_from_context(os->codecpar, t->enc);
            if (ret < 0) {
                ct_av_error(out_error, "avcodec_parameters_from_context", ret);
                return CT_ERR_ENCODE;
            }
            os->time_base = t->enc->time_base;
        } else {
            ret = avcodec_parameters_copy(os->codecpar, is->codecpar);
            if (ret < 0) {
                ct_av_error(out_error, "avcodec_parameters_copy", ret);
                return CT_ERR_ENCODE;
            }
            os->codecpar->codec_tag = 0;
            os->time_base = is->time_base;
        }
        av_dict_copy(&os->metadata, is->metadata, 0);
    }
    if (!(t->out->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open(&t->out->pb, dst_uri, AVIO_FLAG_WRITE);
        if (ret < 0) {
            ct_av_error(out_error, "avio_open(dst)", ret);
            return CT_ERR_IO;
        }
    }
    ret = avformat_write_header(t->out, NULL);
    if (ret < 0) {
        ct_av_error(out_error, "avformat_write_header", ret);
        return CT_ERR_ENCODE;
    }

    t->out_video = t->out->streams[t->video_index];
    t->total_us = t->in->duration > 0 ? t->in->duration : 0;
    t->ready = 1;
    return CT_OK;
}

int32_t ct_ff_transcode(const char *src_uri, const char *dst_uri,
                        const char *filter_desc, const char *video_codec,
                        ct_progress_cb progress, void *user_data,
                        char **out_error) {
    if (!src_uri || !dst_uri || !out_error) return CT_ERR_INVALID;
    *out_error = NULL;

    /* "hw:<fallback-encoder>" opts into the three-stage hybrid pipeline:
     *   hardware decode -> FFmpeg libavfilter -> hardware encode,
     * with PER-STAGE fallback to FFmpeg software. Plain "libx264"/"libx265"/
     * NULL keeps the fully-software implementation below (verified on device),
     * so existing exports cannot regress. See native/captrim_pipeline.h. */
    if (video_codec && strncmp(video_codec, "hw:", 3) == 0) {
        /* "hw:"            -> hardware first, per-stage fallback
         * "hw:sw:"         -> force software (diagnostics / pipeline baseline)
         * "hw:cb:"         -> force OHOS AVCodec CALLBACK Buffer mode
         * "hw:<encoder>"   -> hardware first, <encoder> for the SW fallback */
        const char *spec = video_codec + 3;
        int force_sw = 0, force_async = 0;
        while (strncmp(spec, "sw:", 3) == 0 || strncmp(spec, "cb:", 3) == 0) {
            if (spec[0] == 's') force_sw = 1; else force_async = 1;
            spec += 3;
        }
        const char *fallback = *spec ? spec : "libx264";

        ct_pipeline_report_t report;
        memset(&report, 0, sizeof(report));
        ct_pipeline_options_t opts = {1, 1, force_sw, -1, force_async};
        int32_t st = ct_pipeline_run(src_uri, dst_uri, filter_desc, fallback,
                                     &opts, progress, user_data, out_error, &report);
        /* ct_pipeline_run() already falls back PER STAGE at open() time. A
         * hardware codec can still fail later in the streaming loop (Buffer
         * mode unsupported at runtime, stride mismatch, muxer rejection...).
         * In that case retry the WHOLE job in software: this is the last-resort
         * fallback that guarantees an export instead of a hard failure. */
        if (st != CT_OK && st != CT_ERR_CANCELLED && !force_sw) {
            if (*out_error) { ct_ff_string_free(*out_error); *out_error = NULL; }
            ct_pipeline_options_t sw = {0, 1, 1, -1, 0};
            memset(&report, 0, sizeof(report));
            st = ct_pipeline_run(src_uri, dst_uri, filter_desc, fallback,
                                 &sw, progress, user_data, out_error, &report);
        }
        return st;
    }

    CtTranscode t = {0};
    AVFrame *frame = NULL;
    AVPacket *pkt = NULL;
    int32_t status = CT_OK;
    int ret;
    const char *codec_name = (video_codec && *video_codec) ? video_codec : "libx264";

    ret = avformat_open_input(&t.in, src_uri, NULL, NULL);
    if (ret < 0) {
        ct_av_error(out_error, "avformat_open_input", ret);
        return CT_ERR_IO;
    }
    ret = avformat_find_stream_info(t.in, NULL);
    if (ret < 0) {
        ct_av_error(out_error, "avformat_find_stream_info", ret);
        status = CT_ERR_DECODE;
        goto cleanup;
    }
    for (unsigned i = 0; i < t.in->nb_streams; ++i) {
        if (t.in->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            t.video_index = (int)i;
            break;
        }
    }
    if (t.video_index < 0) {
        ct_set_error(out_error, "no video stream in source");
        status = CT_ERR_INVALID;
        goto cleanup;
    }

    AVStream *in_video = t.in->streams[t.video_index];
    const AVCodec *dec_codec = avcodec_find_decoder(in_video->codecpar->codec_id);
    if (!dec_codec) {
        ct_set_error(out_error, "no decoder for source video codec");
        status = CT_ERR_DECODE;
        goto cleanup;
    }
    t.dec = avcodec_alloc_context3(dec_codec);
    if (!t.dec || avcodec_parameters_to_context(t.dec, in_video->codecpar) < 0) {
        ct_set_error(out_error, "avcodec_parameters_to_context(dec) failed");
        status = CT_ERR_DECODE;
        goto cleanup;
    }
    t.dec->time_base = in_video->time_base;
    ret = avcodec_open2(t.dec, dec_codec, NULL);
    if (ret < 0) {
        ct_av_error(out_error, "avcodec_open2(decoder)", ret);
        status = CT_ERR_DECODE;
        goto cleanup;
    }

    frame = av_frame_alloc();
    pkt = av_packet_alloc();
    if (!frame || !pkt) {
        ct_set_error(out_error, "av_frame_alloc / av_packet_alloc failed");
        status = CT_ERR_INTERNAL;
        goto cleanup;
    }

    /* ---- main decode -> filter -> encode loop ---- */
    for (;;) {
        ret = av_read_frame(t.in, pkt);
        if (ret == AVERROR_EOF) break;
        if (ret < 0) {
            ct_av_error(out_error, "av_read_frame", ret);
            status = CT_ERR_DECODE;
            goto cleanup;
        }

        if (pkt->stream_index == t.video_index) {
            ret = avcodec_send_packet(t.dec, pkt);
            av_packet_unref(pkt);
            if (ret < 0) {
                ct_av_error(out_error, "avcodec_send_packet", ret);
                status = CT_ERR_DECODE;
                goto cleanup;
            }
            for (;;) {
                ret = avcodec_receive_frame(t.dec, frame);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
                if (ret < 0) {
                    ct_av_error(out_error, "avcodec_receive_frame", ret);
                    status = CT_ERR_DECODE;
                    goto cleanup;
                }
                /* Lazily build graph/encoder/muxer from this first real frame. */
                if (!t.ready) {
                    status = ct_transcode_prepare(&t, frame, filter_desc,
                                                  codec_name, dst_uri, out_error);
                    if (status != CT_OK) goto cleanup;
                }
                ret = av_buffersrc_add_frame_flags(t.filter.buffersrc, frame,
                                                   AV_BUFFERSRC_FLAG_KEEP_REF);
                av_frame_unref(frame);
                if (ret == AVERROR_EOF) {
                    /* A terminating filter (trim=duration=...) closed the
                     * graph early. Stop reading the rest of the source. */
                    t.graph_done = 1;
                    int eof_ret = av_buffersrc_add_frame_flags(t.filter.buffersrc, NULL, 0);
                    (void)eof_ret;
                } else if (ret < 0) {
                    ct_av_error(out_error, "av_buffersrc_add_frame_flags", ret);
                    status = CT_ERR_DECODE;
                    goto cleanup;
                }
                status = ct_filter_drain(t.filter.buffersink, t.enc, t.out_video,
                                         t.out, pkt, &t.out_pts, progress,
                                         user_data, t.total_us, out_error);
                if (status != CT_OK) goto cleanup;
                if (t.graph_done) goto main_done;
            }
        } else {
            /* Non-video streams: lossless passthrough. Packets that arrive
             * before the first decoded frame are dropped because the muxer
             * does not exist yet (at most a few ms of leading audio). */
            if (t.ready && pkt->stream_index >= 0 &&
                (unsigned)pkt->stream_index < t.out->nb_streams) {
                AVStream *os = t.out->streams[pkt->stream_index];
                av_packet_rescale_ts(pkt, t.in->streams[pkt->stream_index]->time_base,
                                     os->time_base);
                pkt->stream_index = os->index;
                ret = av_interleaved_write_frame(t.out, pkt);
                if (ret < 0) {
                    ct_av_error(out_error, "av_interleaved_write_frame(copy)", ret);
                    av_packet_unref(pkt);
                    status = CT_ERR_ENCODE;
                    goto cleanup;
                }
            }
            av_packet_unref(pkt);
        }
    }

main_done:
    if (!t.ready) {
        ct_set_error(out_error, "no decoded video frame in source");
        status = CT_ERR_DECODE;
        goto cleanup;
    }

    /* ---- flush decoder -> filter -> encoder ---- */
    if (!t.graph_done) {
    ret = avcodec_send_packet(t.dec, NULL);
    if (ret < 0) {
        ct_av_error(out_error, "avcodec_send_packet(flush)", ret);
        status = CT_ERR_DECODE;
        goto cleanup;
    }
    for (;;) {
        ret = avcodec_receive_frame(t.dec, frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
        if (ret < 0) {
            ct_av_error(out_error, "avcodec_receive_frame(flush)", ret);
            status = CT_ERR_DECODE;
            goto cleanup;
        }
        ret = av_buffersrc_add_frame_flags(t.filter.buffersrc, frame,
                                           AV_BUFFERSRC_FLAG_KEEP_REF);
        av_frame_unref(frame);
        if (ret < 0) {
            ct_av_error(out_error, "av_buffersrc_add_frame_flags(flush)", ret);
            status = CT_ERR_DECODE;
            goto cleanup;
        }
        status = ct_filter_drain(t.filter.buffersink, t.enc, t.out_video, t.out,
                                 pkt, &t.out_pts, progress, user_data,
                                 t.total_us, out_error);
        if (status != CT_OK) goto cleanup;
    }
    ret = av_buffersrc_add_frame_flags(t.filter.buffersrc, NULL, 0);
    if (ret < 0) {
        ct_av_error(out_error, "av_buffersrc_add_frame_flags(eof)", ret);
        status = CT_ERR_DECODE;
        goto cleanup;
    }
    status = ct_filter_drain(t.filter.buffersink, t.enc, t.out_video, t.out,
                             pkt, &t.out_pts, progress, user_data,
                             t.total_us, out_error);
    if (status != CT_OK) goto cleanup;
    }   /* !t.graph_done: the graph already reached EOF */

    ret = avcodec_send_frame(t.enc, NULL);
    if (ret < 0) {
        ct_av_error(out_error, "avcodec_send_frame(flush)", ret);
        status = CT_ERR_ENCODE;
        goto cleanup;
    }
    for (;;) {
        ret = avcodec_receive_packet(t.enc, pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
        if (ret < 0) {
            ct_av_error(out_error, "avcodec_receive_packet(flush)", ret);
            status = CT_ERR_ENCODE;
            goto cleanup;
        }
        av_packet_rescale_ts(pkt, t.enc->time_base, t.out_video->time_base);
        pkt->stream_index = t.out_video->index;
        ret = av_interleaved_write_frame(t.out, pkt);
        av_packet_unref(pkt);
        if (ret < 0) {
            ct_av_error(out_error, "av_interleaved_write_frame(flush)", ret);
            status = CT_ERR_ENCODE;
            goto cleanup;
        }
    }

    ret = av_write_trailer(t.out);
    if (ret < 0) {
        ct_av_error(out_error, "av_write_trailer", ret);
        status = CT_ERR_ENCODE;
    }

cleanup:
    av_frame_free(&frame);
    av_packet_free(&pkt);
    ct_filter_free(&t.filter);
    if (t.enc) avcodec_free_context(&t.enc);
    if (t.dec) avcodec_free_context(&t.dec);
    if (t.out) {
        if (!(t.out->oformat->flags & AVFMT_NOFILE) && t.out->pb) avio_closep(&t.out->pb);
        avformat_free_context(t.out);
    }
    if (t.in) avformat_close_input(&t.in);
    if (status == CT_ERR_CANCELLED) remove(dst_uri);
    return status;
}
