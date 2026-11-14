/* CapTrim FFmpeg C shim - P0: probe + lossless remux.
 *
 * Only libavformat / libavcodec / libavutil are used. No encoder, no filter,
 * no CLI tools. Build FFmpeg with --disable-everything plus the components
 * listed in scripts/ffmpeg-config-common.sh.
 */
#include "captrim_ffmpeg.h"

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char *data;
    size_t len;
    size_t cap;
    int failed;
} StrBuf;

static int sb_init(StrBuf *b, size_t cap) {
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

static void sb_puts(StrBuf *b, const char *s) {
    size_t n = strlen(s);
    if (sb_reserve(b, n) != 0) return;
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
}

static void sb_printf(StrBuf *b, const char *fmt, ...) {
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

static void ct_set_error(char **out_error, const char *fmt, ...) {
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

static void ct_av_error(char **out_error, const char *what, int err) {
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
