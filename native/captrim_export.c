/* CapTrim export executor: runs the multi-input `filter_complex` that
 * crates/captrim-core/src/export_graph.rs compiles.
 *
 * The single-input ct_ff_transcode cannot run a timeline: `concat`, `overlay`
 * and `amix` need N decoder-fed sources in ONE graph, each on its own
 * buffersrc. This executor is the C-side half of M1:
 *
 *   1. rewrite the compiler's labels  [k:v] / [k:a]  into buffersrc names,
 *      one source per LABEL USE (the compiler may trim the same file at two
 *      different ranges, and an avfilter pad cannot fan out on its own — this
 *      is exactly what the ffmpeg CLI does, one buffer source per use),
 *   2. open every input, find its video/audio stream, decode the first frame
 *      of each referenced kind (a buffersrc description needs width, height
 *      and pixel format, and containers report AV_PIX_FMT_NONE),
 *   3. build and configure the graph with `vout` / optional `aout` sinks,
 *   4. round-robin the inputs: demux -> decode -> every buffersrc of that
 *      stream, draining both sinks after each packet so `vout`/`aout` never
 *      fall behind,
 *   5. encode video with the requested codec and audio with AAC through an
 *      audio FIFO (AAC needs fixed 1024-sample frames), mux to mp4, and stop
 *      at the plan's exact duration so the tail cannot run past the last
 *      planned picture frame.
 *
 * Frames are restamped as they are encoded (video: an increasing frame
 * counter, audio: the sample count) so the output timeline is the plan's, not
 * the container's.
 */
#include "captrim_ffmpeg.h"
#include "captrim_common.h"

#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavformat/avformat.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CT_MAX_INPUTS 32
/* Uses of one [k:v] / [k:a] label. A timeline that reuses one file more often
 * than this is refused with its input index rather than silently dropped. */
#define CT_MAX_USES 16
/* Frames decoded during warm-up beyond the one that describes the source. */
#define CT_WARMUP_EXTRA 8

typedef struct {
    AVFormatContext *fmt;
    int vidx;
    int aidx;
    AVCodecContext *vdec;
    AVCodecContext *adec;
    int need_v;
    int need_a;
    int n_vsrc;
    int n_asrc;
    AVFilterContext *vsrc[CT_MAX_USES];
    AVFilterContext *asrc[CT_MAX_USES];
    /* First decoded frame of each referenced kind: describes the buffersrc. */
    AVFrame *v_first;
    AVFrame *a_first;
    AVFrame *v_extra[CT_WARMUP_EXTRA];
    int n_v_extra;
    AVFrame *a_extra[CT_WARMUP_EXTRA];
    int n_a_extra;
    /* need_a but the file carries no audio stream: a timeline may reference
     * the clip on an audio track only for its silence (a video-only asset
     * dropped on a music bed). We synthesise silence instead of failing. */
    int a_silent;
    int64_t a_silent_end;   /* samples at 48 kHz, 0 = bounded by plan only */
    int64_t a_silent_pts;
    int v_dec_eof;
    int a_dec_eof;
    int read_done;
} CtExportInput;

typedef struct {
    CtExportInput in[CT_MAX_INPUTS];
    int n;
    AVFilterGraph *graph;
    AVFilterContext *vsink;
    AVFilterContext *asink;
    AVCodecContext *venc;
    AVCodecContext *aenc;
    AVFormatContext *out;
    AVStream *vst;
    AVStream *ast;
    AVAudioFifo *fifo;
    AVFrame *vframe;   /* scratch: graph video output */
    AVFrame *aframe;   /* scratch: graph audio output */
    AVFrame *aenc_frame;
    AVPacket *pkt;
    int64_t v_pts;     /* next video pts, in venc->time_base */
    int64_t a_pts;     /* samples handed to the AAC encoder */
    int64_t a_sent;    /* audio frames the encoder accepted */
    int64_t a_cap;     /* sample cap from duration_us, 0 = unlimited */
    int64_t total_us;
    AVRational vfr;
    int video_full;    /* reached the duration cap */
    int audio_full;
} CtExportJob;

static void ct_export_input_free(CtExportInput *e) {
    int i;
    av_frame_free(&e->v_first);
    av_frame_free(&e->a_first);
    for (i = 0; i < e->n_v_extra; ++i) av_frame_free(&e->v_extra[i]);
    for (i = 0; i < e->n_a_extra; ++i) av_frame_free(&e->a_extra[i]);
    e->n_v_extra = 0;
    e->n_a_extra = 0;
    if (e->vdec) avcodec_free_context(&e->vdec);
    if (e->adec) avcodec_free_context(&e->adec);
    if (e->fmt) avformat_close_input(&e->fmt);
}

/* -------------------------------------------------------------------------
 * 1. Label rewrite
 * ------------------------------------------------------------------------- */

/* Copy filter_graph, replacing every [k:v] / [k:a] with a unique buffersrc
 * name (v<k>_<use> / a<k>_<use>) and record which inputs need which stream.
 * `*sink_a` reports whether the graph still produces [aout]. */
static int32_t ct_export_rewrite(const char *src, CtExportInput *in, int n_in,
                                 StrBuf *out, int *sink_a, char **out_error) {
    int vc[CT_MAX_INPUTS] = {0};
    int ac[CT_MAX_INPUTS] = {0};
    size_t i = 0;

    while (src[i]) {
        if (src[i] != '[') {
            sb_printf(out, "%c", src[i]);
            ++i;
            continue;
        }
        size_t j = i + 1;
        while (src[j] && src[j] != ']') ++j;
        if (!src[j]) {
            ct_set_error(out_error, "filter graph has an unbalanced '[' near %.40s", src + i);
            return CT_ERR_INVALID;
        }

        size_t k = i + 1;
        int idx = 0;
        int digits = 0;
        while (k < j && isdigit((unsigned char)src[k])) {
            idx = idx * 10 + (src[k] - '0');
            ++digits;
            ++k;
        }
        if (digits > 0 && k + 2 == j && src[k] == ':' &&
            (src[k + 1] == 'v' || src[k + 1] == 'a')) {
            if (idx < 0 || idx >= n_in) {
                ct_set_error(out_error, "filter graph refers to input %d but only %d were given",
                             idx, n_in);
                return CT_ERR_INVALID;
            }
            if (src[k + 1] == 'v') {
                if (vc[idx] >= CT_MAX_USES) {
                    ct_set_error(out_error, "input %d video is used more than %d times",
                                 idx, CT_MAX_USES);
                    return CT_ERR_INVALID;
                }
                in[idx].need_v = 1;
                sb_printf(out, "[xv%d_%d]", idx, vc[idx]++);
            } else {
                if (ac[idx] >= CT_MAX_USES) {
                    ct_set_error(out_error, "input %d audio is used more than %d times",
                                 idx, CT_MAX_USES);
                    return CT_ERR_INVALID;
                }
                in[idx].need_a = 1;
                sb_printf(out, "[xa%d_%d]", idx, ac[idx]++);
            }
            i = j + 1;
            continue;
        }

        /* A real label (bg0, seg1, vout, aout...): copy verbatim, but note
         * whether the graph still ends in an audio sink. */
        if (j - i - 1 == 4 && strncmp(src + i + 1, "aout", 4) == 0) *sink_a = 1;
        for (size_t t = i; t <= j; ++t) sb_printf(out, "%c", src[t]);
        i = j + 1;
    }
    /* Every buffersrc the graph will need is known now: the counts ARE the
     * per-use source lists built in ct_export_build_graph. */
    for (int k = 0; k < n_in; ++k) {
        in[k].n_vsrc = vc[k];
        in[k].n_asrc = ac[k];
    }
    return CT_OK;
}

/* -------------------------------------------------------------------------
 * 2. Inputs
 * ------------------------------------------------------------------------- */

static int ct_export_first_index(AVFormatContext *fmt, enum AVMediaType type) {
    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        if (fmt->streams[i]->codecpar->codec_type == type) return (int)i;
    }
    return -1;
}

static int32_t ct_export_open_decoder(AVFormatContext *fmt, int index,
                                      AVCodecContext **out, const char *what,
                                      char **out_error) {
    AVStream *st = fmt->streams[index];
    const AVCodec *codec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!codec) {
        ct_set_error(out_error, "no %s decoder for %s", what,
                     avcodec_get_name(st->codecpar->codec_id));
        return CT_ERR_DECODE;
    }
    AVCodecContext *dec = avcodec_alloc_context3(codec);
    if (!dec || avcodec_parameters_to_context(dec, st->codecpar) < 0) {
        if (dec) avcodec_free_context(&dec);
        ct_set_error(out_error, "avcodec_parameters_to_context(%s) failed", what);
        return CT_ERR_DECODE;
    }
    dec->pkt_timebase = st->time_base;
    dec->time_base = st->time_base;
    int ret = avcodec_open2(dec, codec, NULL);
    if (ret < 0) {
        avcodec_free_context(&dec);
        ct_av_error(out_error, "avcodec_open2(decoder)", ret);
        return CT_ERR_DECODE;
    }
    *out = dec;
    return CT_OK;
}

static int32_t ct_export_keep_frame(AVFrame **slot, AVFrame **extras, int *n_extra,
                                    const AVFrame *frame, int index, char **out_error) {
    if (!*slot) {
        *slot = av_frame_clone(frame);
        if (!*slot) {
            ct_set_error(out_error, "av_frame_clone failed");
            return CT_ERR_INTERNAL;
        }
        return CT_OK;
    }
    if (*n_extra >= CT_WARMUP_EXTRA) {
        ct_set_error(out_error,
                     "input %d produced more than %d frames before the graph existed",
                     index, CT_WARMUP_EXTRA + 1);
        return CT_ERR_INTERNAL;
    }
    extras[*n_extra] = av_frame_clone(frame);
    if (!extras[*n_extra]) {
        ct_set_error(out_error, "av_frame_clone failed");
        return CT_ERR_INTERNAL;
    }
    ++*n_extra;
    return CT_OK;
}

/* Send whatever the decoder has and stash frames into first/extra. */
static int32_t ct_export_warmup_drain(CtExportInput *e, int index, int video,
                                      char **out_error) {
    AVFrame *f = av_frame_alloc();
    if (!f) {
        ct_set_error(out_error, "av_frame_alloc failed");
        return CT_ERR_INTERNAL;
    }
    AVCodecContext *dec = video ? e->vdec : e->adec;
    for (;;) {
        int ret = avcodec_receive_frame(dec, f);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
        if (ret < 0) {
            av_frame_free(&f);
            ct_av_error(out_error, "avcodec_receive_frame", ret);
            return CT_ERR_DECODE;
        }
        int32_t st = video
                         ? ct_export_keep_frame(&e->v_first, e->v_extra, &e->n_v_extra, f,
                                                index, out_error)
                         : ct_export_keep_frame(&e->a_first, e->a_extra, &e->n_a_extra, f,
                                                index, out_error);
        av_frame_unref(f);
        if (st != CT_OK) {
            av_frame_free(&f);
            return st;
        }
    }
    av_frame_free(&f);
    return CT_OK;
}

/* How many 48 kHz samples a silent input should hand the graph. The graph
 * trims each use anyway; this only stops us from inventing silence forever
 * when the container duration is known. */
static void ct_export_silence_span(CtExportInput *e) {
    e->a_silent_end = 0;
    if (e->fmt && e->fmt->duration > 0 && e->fmt->duration != AV_NOPTS_VALUE) {
        e->a_silent_end = av_rescale(e->fmt->duration, 48000, AV_TIME_BASE);
    }
}

/* Open one input and decode the first frame of every kind the graph asks for.
 * Returns CT_OK when both descriptions exist (or EOF made it clear they never
 * will, which is an error for a stream the graph needs). */
static int32_t ct_export_prepare_input(CtExportInput *e, const char *uri, int index,
                                       char **out_error) {
    int ret = avformat_open_input(&e->fmt, uri, NULL, NULL);
    if (ret < 0) {
        char why[AV_ERROR_MAX_STRING_SIZE] = {0};
        av_strerror(ret, why, sizeof(why));
        ct_set_error(out_error, "input %d: cannot open %s (%s)", index, uri, why);
        return CT_ERR_IO;
    }
    ret = avformat_find_stream_info(e->fmt, NULL);
    if (ret < 0) {
        ct_av_error(out_error, "avformat_find_stream_info", ret);
        return CT_ERR_DECODE;
    }

    e->vidx = ct_export_first_index(e->fmt, AVMEDIA_TYPE_VIDEO);
    e->aidx = ct_export_first_index(e->fmt, AVMEDIA_TYPE_AUDIO);
    if (e->need_v && e->vidx < 0) {
        ct_set_error(out_error, "input %d (%s) has no video stream", index, uri);
        return CT_ERR_INVALID;
    }
    if (e->need_a && e->aidx < 0) {
        e->a_silent = 1;
        ct_export_silence_span(e);
    }
    /* Decoders exist only for streams the graph actually consumes: a timeline
     * whose picture comes from a video-only file must not pay for an audio
     * decoder it will never use. */
    if (e->need_v && e->vidx >= 0) {
        int32_t st = ct_export_open_decoder(e->fmt, e->vidx, &e->vdec, "video", out_error);
        if (st != CT_OK) return st;
    }
    if (e->need_a && !e->a_silent && e->aidx >= 0) {
        int32_t st = ct_export_open_decoder(e->fmt, e->aidx, &e->adec, "audio", out_error);
        if (st != CT_OK) return st;
    }

    AVPacket *pkt = av_packet_alloc();
    if (!pkt) {
        ct_set_error(out_error, "av_packet_alloc failed");
        return CT_ERR_INTERNAL;
    }
    int32_t status = CT_OK;
    while ((e->need_v && !e->v_first) ||
           (e->need_a && !e->a_silent && !e->a_first)) {
        ret = av_read_frame(e->fmt, pkt);
        if (ret == AVERROR_EOF) {
            if (e->vdec && !e->v_dec_eof) {
                e->v_dec_eof = 1;
                avcodec_send_packet(e->vdec, NULL);
                status = ct_export_warmup_drain(e, index, 1, out_error);
                if (status != CT_OK) break;
            }
            if (e->adec && !e->a_dec_eof) {
                e->a_dec_eof = 1;
                avcodec_send_packet(e->adec, NULL);
                status = ct_export_warmup_drain(e, index, 0, out_error);
                if (status != CT_OK) break;
            }
            break;
        }
        if (ret < 0) {
            ct_av_error(out_error, "av_read_frame", ret);
            status = CT_ERR_DECODE;
            break;
        }
        if (pkt->stream_index == e->vidx && e->vdec) {
            ret = avcodec_send_packet(e->vdec, pkt);
            av_packet_unref(pkt);
            if (ret < 0) {
                ct_av_error(out_error, "avcodec_send_packet(video)", ret);
                status = CT_ERR_DECODE;
                break;
            }
            status = ct_export_warmup_drain(e, index, 1, out_error);
            if (status != CT_OK) break;
        } else if (pkt->stream_index == e->aidx && e->adec) {
            ret = avcodec_send_packet(e->adec, pkt);
            av_packet_unref(pkt);
            if (ret < 0) {
                ct_av_error(out_error, "avcodec_send_packet(audio)", ret);
                status = CT_ERR_DECODE;
                break;
            }
            status = ct_export_warmup_drain(e, index, 0, out_error);
            if (status != CT_OK) break;
        } else {
            av_packet_unref(pkt);
        }
    }
    av_packet_free(&pkt);
    if (status != CT_OK) return status;

    if (e->need_v && !e->v_first) {
        ct_set_error(out_error, "input %d (%s) has no decodable video frame", index, uri);
        return CT_ERR_DECODE;
    }
    if (e->need_a && !e->a_silent && !e->a_first) {
        ct_set_error(out_error, "input %d (%s) has no decodable audio frame", index, uri);
        return CT_ERR_DECODE;
    }
    return CT_OK;
}

/* -------------------------------------------------------------------------
 * 3. Graph
 * ------------------------------------------------------------------------- */

static int32_t ct_export_add_vsrc(CtExportJob *j, CtExportInput *e, int index,
                                  char **out_error) {
    const AVFilter *buffer = avfilter_get_by_name("buffer");
    if (!buffer) {
        ct_set_error(out_error, "the FFmpeg build has no 'buffer' filter");
        return CT_ERR_INTERNAL;
    }
    AVFrame *first = e->v_first;
    if (!av_pix_fmt_desc_get(first->format)) {
        ct_set_error(out_error, "input %d decoded frame has invalid pixel format %d", index,
                     first->format);
        return CT_ERR_INVALID;
    }
    AVRational sar = first->sample_aspect_ratio.num > 0 ? first->sample_aspect_ratio
                                                       : (AVRational){1, 1};
    AVRational tb = e->fmt->streams[e->vidx]->time_base;
    char args[512];
    snprintf(args, sizeof(args),
             "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d", first->width,
             first->height, first->format, tb.num, tb.den, sar.num, sar.den);
    for (int n = 0; n < e->n_vsrc; ++n) {
        char name[32];
        snprintf(name, sizeof(name), "xv%d_%d", index, n);
        int ret = avfilter_graph_create_filter(&e->vsrc[n], buffer, name, args, NULL,
                                               j->graph);
        if (ret < 0) {
            ct_av_error(out_error, "avfilter_graph_create_filter(buffer)", ret);
            return CT_ERR_INTERNAL;
        }
    }
    return CT_OK;
}

static int32_t ct_export_add_asrc(CtExportJob *j, CtExportInput *e, int index,
                                  char **out_error) {
    const AVFilter *abuffer = avfilter_get_by_name("abuffer");
    if (!abuffer) {
        ct_set_error(out_error, "the FFmpeg build has no 'abuffer' filter");
        return CT_ERR_INTERNAL;
    }
    AVFrame *first = e->a_first;
    char args[512];
    if (e->a_silent) {
        /* No audio stream to describe: pin the source to the format every
         * timeline clip in a music bed arrives in. */
        snprintf(args, sizeof(args),
                 "time_base=1/48000:sample_rate=48000:sample_fmt=fltp:"
                 "channel_layout=stereo");
    } else {
        if (first->sample_rate <= 0) {
            ct_set_error(out_error, "input %d audio frame has no sample rate", index);
            return CT_ERR_INVALID;
        }
        char layout[64];
        if (first->ch_layout.nb_channels > 0) {
            av_channel_layout_describe(&first->ch_layout, layout, sizeof(layout));
        } else {
            snprintf(layout, sizeof(layout), "stereo");
        }
        const char *fmt_name = av_get_sample_fmt_name((enum AVSampleFormat)first->format);
        if (!fmt_name) {
            ct_set_error(out_error, "input %d audio frame has an unknown sample format %d",
                         index, first->format);
            return CT_ERR_INVALID;
        }
        AVRational tb = e->fmt->streams[e->aidx]->time_base;
        snprintf(args, sizeof(args),
                 "time_base=%d/%d:sample_rate=%d:sample_fmt=%s:channel_layout=%s", tb.num,
                 tb.den, first->sample_rate, fmt_name, layout);
    }
    for (int n = 0; n < e->n_asrc; ++n) {
        char name[32];
        snprintf(name, sizeof(name), "xa%d_%d", index, n);
        int ret = avfilter_graph_create_filter(&e->asrc[n], abuffer, name, args, NULL,
                                               j->graph);
        if (ret < 0) {
            ct_av_error(out_error, "avfilter_graph_create_filter(abuffer)", ret);
            return CT_ERR_INTERNAL;
        }
    }
    return CT_OK;
}

static int32_t ct_export_build_graph(CtExportJob *j, const char *desc, int want_audio,
                                     char **out_error) {
    j->graph = avfilter_graph_alloc();
    if (!j->graph) {
        ct_set_error(out_error, "avfilter_graph_alloc failed");
        return CT_ERR_INTERNAL;
    }

    for (int i = 0; i < j->n; ++i) {
        CtExportInput *e = &j->in[i];
        if (e->need_v) {
            int32_t st = ct_export_add_vsrc(j, e, i, out_error);
            if (st != CT_OK) return st;
        }
        if (e->need_a) {
            int32_t st = ct_export_add_asrc(j, e, i, out_error);
            if (st != CT_OK) return st;
        }
    }

    const AVFilter *sink = avfilter_get_by_name("buffersink");
    int ret = avfilter_graph_create_filter(&j->vsink, sink, "vout", NULL, NULL, j->graph);
    if (ret < 0) {
        ct_av_error(out_error, "avfilter_graph_create_filter(buffersink/vout)", ret);
        return CT_ERR_INTERNAL;
    }
    static const enum AVPixelFormat pix_fmts[] = {AV_PIX_FMT_YUV420P, AV_PIX_FMT_NONE};
    ret = av_opt_set_int_list(j->vsink, "pix_fmts", pix_fmts, AV_PIX_FMT_NONE,
                              AV_OPT_SEARCH_CHILDREN);
    if (ret < 0) {
        ct_av_error(out_error, "av_opt_set_int_list(pix_fmts)", ret);
        return CT_ERR_INTERNAL;
    }

    if (want_audio) {
        const AVFilter *asink = avfilter_get_by_name("abuffersink");
        ret = avfilter_graph_create_filter(&j->asink, asink, "aout", NULL, NULL, j->graph);
        if (ret < 0) {
            ct_av_error(out_error, "avfilter_graph_create_filter(abuffersink/aout)", ret);
            return CT_ERR_INTERNAL;
        }
        /* The compiler already resamples to 48 kHz stereo; pinning the format
         * here keeps the AAC encoder from negotiating something it cannot
         * open. FLTP is not a preference but a requirement: the FIFO and the
         * frame handed to the encoder are built in the encoder's own format,
         * so letting the sink emit s16 would feed float bits into the
         * encoder (it rejects them as NaN). */
        static const enum AVSampleFormat sample_fmts[] = {AV_SAMPLE_FMT_FLTP,
                                                         AV_SAMPLE_FMT_NONE};
        ret = av_opt_set_int_list(j->asink, "sample_fmts", sample_fmts, AV_SAMPLE_FMT_NONE,
                                  AV_OPT_SEARCH_CHILDREN);
        if (ret < 0) {
            ct_av_error(out_error, "av_opt_set_int_list(sample_fmts)", ret);
            return CT_ERR_INTERNAL;
        }
        static const int sample_rates[] = {48000, 0};
        ret = av_opt_set_int_list(j->asink, "sample_rates", sample_rates, 0,
                                  AV_OPT_SEARCH_CHILDREN);
        if (ret < 0) {
            ct_av_error(out_error, "av_opt_set_int_list(sample_rates)", ret);
            return CT_ERR_INTERNAL;
        }
    }

    AVFilterInOut *inputs = NULL;   /* graph outputs -> our sinks */
    AVFilterInOut *outputs = NULL;  /* our buffersrcs -> graph inputs */
    AVFilterInOut **tail_in = &inputs;
    AVFilterInOut **tail_out = &outputs;

    for (int i = 0; i < j->n; ++i) {
        CtExportInput *e = &j->in[i];
        for (int n = 0; n < e->n_vsrc; ++n) {
            AVFilterInOut *x = avfilter_inout_alloc();
            if (!x) {
                ct_set_error(out_error, "avfilter_inout_alloc failed");
                return CT_ERR_INTERNAL;
            }
            x->name = av_strdup(e->vsrc[n]->name);
            x->filter_ctx = e->vsrc[n];
            x->pad_idx = 0;
            *tail_out = x;
            tail_out = &x->next;
        }
        for (int n = 0; n < e->n_asrc; ++n) {
            AVFilterInOut *x = avfilter_inout_alloc();
            if (!x) {
                ct_set_error(out_error, "avfilter_inout_alloc failed");
                return CT_ERR_INTERNAL;
            }
            x->name = av_strdup(e->asrc[n]->name);
            x->filter_ctx = e->asrc[n];
            x->pad_idx = 0;
            *tail_out = x;
            tail_out = &x->next;
        }
    }
    {
        AVFilterInOut *x = avfilter_inout_alloc();
        if (!x) {
            ct_set_error(out_error, "avfilter_inout_alloc failed");
            return CT_ERR_INTERNAL;
        }
        x->name = av_strdup("vout");
        x->filter_ctx = j->vsink;
        x->pad_idx = 0;
        *tail_in = x;
        tail_in = &x->next;
    }
    if (want_audio) {
        AVFilterInOut *x = avfilter_inout_alloc();
        if (!x) {
            ct_set_error(out_error, "avfilter_inout_alloc failed");
            return CT_ERR_INTERNAL;
        }
        x->name = av_strdup("aout");
        x->filter_ctx = j->asink;
        x->pad_idx = 0;
        *tail_in = x;
        tail_in = &x->next;
    }

    ct_capture_begin();
    av_log_set_callback(ct_log_capture);
    ret = avfilter_graph_parse_ptr(j->graph, desc, &inputs, &outputs, NULL);
    av_log_set_callback(NULL);
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);
    if (ret < 0) {
        ct_capture_end(out_error, "avfilter_graph_parse_ptr", ret);
        return CT_ERR_INVALID;
    }
    ct_capture_begin();
    av_log_set_callback(ct_log_capture);
    ret = avfilter_graph_config(j->graph, NULL);
    av_log_set_callback(NULL);
    if (ret < 0) {
        ct_capture_end(out_error, "avfilter_graph_config", ret);
        return CT_ERR_INVALID;
    }
    return CT_OK;
}

/* -------------------------------------------------------------------------
 * 4. Encoders, muxer
 * ------------------------------------------------------------------------- */

static int32_t ct_export_write_packets(CtExportJob *j, AVCodecContext *enc, AVStream *st,
                                       char **out_error) {
    for (;;) {
        int ret = avcodec_receive_packet(enc, j->pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) return CT_OK;
        if (ret < 0) {
            ct_av_error(out_error, "avcodec_receive_packet", ret);
            return CT_ERR_ENCODE;
        }
        av_packet_rescale_ts(j->pkt, enc->time_base, st->time_base);
        j->pkt->stream_index = st->index;
        ret = av_interleaved_write_frame(j->out, j->pkt);
        av_packet_unref(j->pkt);
        if (ret < 0) {
            ct_av_error(out_error, "av_interleaved_write_frame", ret);
            return CT_ERR_ENCODE;
        }
    }
}

static int32_t ct_export_open_output(CtExportJob *j, const char *dst, const char *codec,
                                     int crf, const char *preset, int audio_kbps,
                                     int want_audio, char **out_error) {
    int ret;
    const char *enc_name = (codec && *codec) ? codec : "libx264";
    const AVCodec *enc_codec = avcodec_find_encoder_by_name(enc_name);
    if (!enc_codec) {
        ct_set_error(out_error, "encoder not available: %s", enc_name);
        return CT_ERR_ENCODE;
    }
    j->venc = avcodec_alloc_context3(enc_codec);
    if (!j->venc) {
        ct_set_error(out_error, "avcodec_alloc_context3(video encoder) failed");
        return CT_ERR_ENCODE;
    }
    j->venc->width = av_buffersink_get_w(j->vsink);
    j->venc->height = av_buffersink_get_h(j->vsink);
    j->venc->pix_fmt = av_buffersink_get_format(j->vsink);
    if (j->venc->pix_fmt == AV_PIX_FMT_NONE) j->venc->pix_fmt = AV_PIX_FMT_YUV420P;
    j->venc->sample_aspect_ratio = av_buffersink_get_sample_aspect_ratio(j->vsink);
    AVRational fr = av_buffersink_get_frame_rate(j->vsink);
    if (fr.num <= 0 || fr.den <= 0) fr = (AVRational){30, 1};
    j->vfr = fr;
    j->venc->framerate = fr;
    j->venc->time_base = av_inv_q(fr);
    j->venc->gop_size = 48;
    j->venc->max_b_frames = 0;
    j->venc->bit_rate = 0;
    if (j->venc->codec_id == AV_CODEC_ID_H264 || j->venc->codec_id == AV_CODEC_ID_HEVC) {
        char crf_text[16];
        snprintf(crf_text, sizeof(crf_text), "%d", crf > 0 ? crf : 20);
        if (preset && *preset) av_opt_set(j->venc->priv_data, "preset", preset, 0);
        av_opt_set(j->venc->priv_data, "crf", crf_text, 0);
    }

    ret = avcodec_open2(j->venc, enc_codec, NULL);
    if (ret < 0) {
        ct_av_error(out_error, "avcodec_open2(video encoder)", ret);
        return CT_ERR_ENCODE;
    }

    if (want_audio) {
        const AVCodec *aac = avcodec_find_encoder(AV_CODEC_ID_AAC);
        if (!aac) {
            ct_set_error(out_error, "the FFmpeg build has no AAC encoder");
            return CT_ERR_ENCODE;
        }
        j->aenc = avcodec_alloc_context3(aac);
        if (!j->aenc) {
            ct_set_error(out_error, "avcodec_alloc_context3(audio encoder) failed");
            return CT_ERR_ENCODE;
        }
        j->aenc->sample_rate = av_buffersink_get_sample_rate(j->asink);
        if (j->aenc->sample_rate <= 0) j->aenc->sample_rate = 48000;
        av_buffersink_get_ch_layout(j->asink, &j->aenc->ch_layout);
        if (j->aenc->ch_layout.nb_channels <= 0) {
            av_channel_layout_default(&j->aenc->ch_layout, 2);
        }
        j->aenc->sample_fmt = (enum AVSampleFormat)av_buffersink_get_format(j->asink);
        if (j->aenc->sample_fmt == AV_SAMPLE_FMT_NONE) j->aenc->sample_fmt = AV_SAMPLE_FMT_FLTP;
        j->aenc->bit_rate = (audio_kbps > 0 ? audio_kbps : 192) * 1000;
        j->aenc->time_base = (AVRational){1, j->aenc->sample_rate};
        ret = avcodec_open2(j->aenc, aac, NULL);
        if (ret < 0) {
            ct_av_error(out_error, "avcodec_open2(audio encoder)", ret);
            return CT_ERR_ENCODE;
        }
        j->fifo = av_audio_fifo_alloc(j->aenc->sample_fmt,
                                      j->aenc->ch_layout.nb_channels,
                                      j->aenc->frame_size > 0 ? j->aenc->frame_size : 1024);
        j->aenc_frame = av_frame_alloc();
        if (!j->fifo || !j->aenc_frame) {
            ct_set_error(out_error, "audio FIFO allocation failed");
            return CT_ERR_INTERNAL;
        }
        if (j->total_us > 0) {
            j->a_cap = av_rescale(j->total_us, j->aenc->sample_rate, 1000000);
        }
    }

    ret = avformat_alloc_output_context2(&j->out, NULL, "mp4", dst);
    if (ret < 0 || !j->out) {
        ct_av_error(out_error, "avformat_alloc_output_context2(mp4)", ret);
        return CT_ERR_ENCODE;
    }

    j->vst = avformat_new_stream(j->out, NULL);
    if (!j->vst) {
        ct_set_error(out_error, "avformat_new_stream(video) failed");
        return CT_ERR_ENCODE;
    }
    if (j->out->oformat->flags & AVFMT_GLOBALHEADER) {
        j->venc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }
    ret = avcodec_parameters_from_context(j->vst->codecpar, j->venc);
    if (ret < 0) {
        ct_av_error(out_error, "avcodec_parameters_from_context(video)", ret);
        return CT_ERR_ENCODE;
    }
    j->vst->time_base = j->venc->time_base;

    if (want_audio) {
        j->ast = avformat_new_stream(j->out, NULL);
        if (!j->ast) {
            ct_set_error(out_error, "avformat_new_stream(audio) failed");
            return CT_ERR_ENCODE;
        }
        if (j->out->oformat->flags & AVFMT_GLOBALHEADER) {
            j->aenc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        }
        ret = avcodec_parameters_from_context(j->ast->codecpar, j->aenc);
        if (ret < 0) {
            ct_av_error(out_error, "avcodec_parameters_from_context(audio)", ret);
            return CT_ERR_ENCODE;
        }
        j->ast->time_base = j->aenc->time_base;
    }

    if (!(j->out->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open(&j->out->pb, dst, AVIO_FLAG_WRITE);
        if (ret < 0) {
            ct_av_error(out_error, "avio_open(dst)", ret);
            return CT_ERR_IO;
        }
    }
    ret = avformat_write_header(j->out, NULL);
    if (ret < 0) {
        ct_av_error(out_error, "avformat_write_header", ret);
        return CT_ERR_ENCODE;
    }
    return CT_OK;
}

/* -------------------------------------------------------------------------
 * 5. Feeding and draining
 * ------------------------------------------------------------------------- */

static int32_t ct_export_push_video(CtExportJob *j, CtExportInput *e, AVFrame *f,
                                    char **out_error) {
    for (int n = 0; n < e->n_vsrc; ++n) {
        if (!e->vsrc[n]) continue;
        int ret = av_buffersrc_add_frame_flags(e->vsrc[n], f, AV_BUFFERSRC_FLAG_KEEP_REF);
        if (ret == AVERROR_EOF) {
            /* The filter this source feeds is done with it (a trim reached its
             * out point). The per-use source has no other consumer, so it
             * retires instead of turning into a hard error. */
            e->vsrc[n] = NULL;
            continue;
        }
        if (ret < 0) {
            ct_av_error(out_error, "av_buffersrc_add_frame_flags(video)", ret);
            return CT_ERR_DECODE;
        }
    }
    return CT_OK;
}

/* Silence for an input that has no audio stream. av_frame_get_buffer zeroes
 * the planes, so the frames really are silent, not uninitialised. */
static int32_t ct_export_push_silence(CtExportJob *j, CtExportInput *e, int samples,
                                      char **out_error) {
    if (e->n_asrc <= 0 || samples <= 0) return CT_OK;
    AVFrame *f = av_frame_alloc();
    if (!f) {
        ct_set_error(out_error, "av_frame_alloc failed");
        return CT_ERR_INTERNAL;
    }
    f->nb_samples = samples;
    f->format = AV_SAMPLE_FMT_FLTP;
    f->sample_rate = 48000;
    av_channel_layout_default(&f->ch_layout, 2);
    int ret = av_frame_get_buffer(f, 0);
    if (ret < 0) {
        av_frame_free(&f);
        ct_av_error(out_error, "av_frame_get_buffer(silence)", ret);
        return CT_ERR_INTERNAL;
    }
    /* av_frame_get_buffer hands back uninitialised planes: without this the
     * "silence" is whatever malloc left behind, and float planes full of
     * garbage are exactly what the AAC encoder rejects as NaN. */
    av_samples_set_silence(f->data, 0, samples, f->ch_layout.nb_channels,
                           AV_SAMPLE_FMT_FLTP);
    f->pts = e->a_silent_pts;
    e->a_silent_pts += samples;
    int open = 0;
    for (int s = 0; s < e->n_asrc; ++s) {
        if (!e->asrc[s]) continue;
        ++open;
        ret = av_buffersrc_add_frame_flags(e->asrc[s], f, AV_BUFFERSRC_FLAG_KEEP_REF);
        if (ret == AVERROR_EOF) {
            e->asrc[s] = NULL;
            continue;
        }
        if (ret < 0) {
            char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
            av_strerror(ret, buf, sizeof(buf));
            ct_set_error(out_error, "silence push to %s at pts %lld: %s",
                         e->asrc[s]->name, (long long)f->pts, buf);
            av_frame_free(&f);
            return CT_ERR_DECODE;
        }
    }
    av_frame_free(&f);
    return open > 0 ? CT_OK : AVERROR_EOF;
}

/* Hand the graph its silence on the same rhythm as the video packets: one
 * block per pass keeps the buffersrc queue short, and `final` flushes the
 * rest once the file has no more packets to read. */
static int32_t ct_export_silence_supply(CtExportJob *j, CtExportInput *e, int final,
                                        char **out_error) {
    if (!e->a_silent) return CT_OK;
    int64_t limit = e->a_silent_end;
    if (j->a_cap > 0 && (limit == 0 || j->a_cap < limit)) limit = j->a_cap;
    for (;;) {
        int64_t left = limit > 0 ? limit - e->a_silent_pts : 1024;
        if (left <= 0) break;
        int n = left < 1024 ? (int)left : 1024;
        int32_t st = ct_export_push_silence(j, e, n, out_error);
        if (st == AVERROR_EOF) break;   /* every use of this input is done */
        if (st != CT_OK) return st;
        if (!final) break;
    }
    return CT_OK;
}

static int32_t ct_export_push_audio(CtExportJob *j, CtExportInput *e, AVFrame *f,
                                    char **out_error) {
    for (int n = 0; n < e->n_asrc; ++n) {
        if (!e->asrc[n]) continue;
        int ret = av_buffersrc_add_frame_flags(e->asrc[n], f, AV_BUFFERSRC_FLAG_KEEP_REF);
        if (ret == AVERROR_EOF) {
            e->asrc[n] = NULL;
            continue;
        }
        if (ret < 0) {
            ct_av_error(out_error, "av_buffersrc_add_frame_flags(audio)", ret);
            return CT_ERR_DECODE;
        }
    }
    return CT_OK;
}

static int32_t ct_export_drain_video(CtExportJob *j, ct_progress_cb progress,
                                     void *user_data, char **out_error) {
    for (;;) {
        int ret = av_buffersink_get_frame(j->vsink, j->vframe);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) return CT_OK;
        if (ret < 0) {
            ct_av_error(out_error, "av_buffersink_get_frame(vout)", ret);
            return CT_ERR_DECODE;
        }
        if (j->total_us > 0 &&
            av_rescale_q(j->v_pts, j->venc->time_base, AV_TIME_BASE_Q) >= j->total_us) {
            /* Past the plan: keep draining (the graph must not back up) but
             * write nothing, so the tail cannot overshoot. */
            j->video_full = 1;
            av_frame_unref(j->vframe);
            continue;
        }
        j->vframe->pict_type = AV_PICTURE_TYPE_NONE;
        j->vframe->pts = j->v_pts++;
        ret = avcodec_send_frame(j->venc, j->vframe);
        av_frame_unref(j->vframe);
        if (ret < 0) {
            ct_av_error(out_error, "avcodec_send_frame", ret);
            return CT_ERR_ENCODE;
        }
        int32_t st = ct_export_write_packets(j, j->venc, j->vst, out_error);
        if (st != CT_OK) return st;
        if (progress && j->total_us > 0) {
            int64_t pos_us = av_rescale_q(j->v_pts, j->venc->time_base, AV_TIME_BASE_Q);
            float value = (float)((double)pos_us / (double)j->total_us);
            if (value < 0.0f) value = 0.0f;
            if (value > 1.0f) value = 1.0f;
            if (progress(value, user_data) != 0) {
                ct_set_error(out_error, "export cancelled by caller");
                return CT_ERR_CANCELLED;
            }
        }
    }
}

static int32_t ct_export_encode_audio_ready(CtExportJob *j, char **out_error) {
    int frame_size = j->aenc->frame_size > 0 ? j->aenc->frame_size : 1024;
    while (av_audio_fifo_size(j->fifo) >= frame_size) {
        if (j->a_cap > 0 && j->a_pts >= j->a_cap) {
            j->audio_full = 1;
            return CT_OK;
        }
        AVFrame *f = j->aenc_frame;
        av_frame_unref(f);
        f->nb_samples = frame_size;
        f->format = j->aenc->sample_fmt;
        f->sample_rate = j->aenc->sample_rate;
        av_channel_layout_copy(&f->ch_layout, &j->aenc->ch_layout);
        int ret = av_frame_get_buffer(f, 0);
        if (ret < 0) {
            ct_av_error(out_error, "av_frame_get_buffer(audio)", ret);
            return CT_ERR_ENCODE;
        }
        if (av_audio_fifo_read(j->fifo, (void **)f->extended_data, frame_size) < frame_size) {
            ct_set_error(out_error, "audio FIFO underrun");
            return CT_ERR_INTERNAL;
        }
        f->pts = j->a_pts;
        j->a_pts += frame_size;
        ct_capture_begin();
        av_log_set_callback(ct_log_capture);
        ret = avcodec_send_frame(j->aenc, f);
        av_log_set_callback(NULL);
        if (ret < 0) {
            av_frame_unref(f);
            ct_capture_end(out_error, "avcodec_send_frame(audio)", ret);
            return CT_ERR_ENCODE;
        }
        ++j->a_sent;
        int32_t st = ct_export_write_packets(j, j->aenc, j->ast, out_error);
        if (st != CT_OK) return st;
    }
    return CT_OK;
}

static int32_t ct_export_drain_audio(CtExportJob *j, char **out_error) {
    if (!j->asink) return CT_OK;
    for (;;) {
        int ret = av_buffersink_get_frame(j->asink, j->aframe);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) return CT_OK;
        if (ret < 0) {
            ct_av_error(out_error, "av_buffersink_get_frame(aout)", ret);
            return CT_ERR_DECODE;
        }
        if (!j->audio_full && j->aframe->nb_samples > 0) {
            if (j->aframe->format != j->aenc->sample_fmt) {
                int got = j->aframe->format;
                av_frame_unref(j->aframe);
                ct_set_error(out_error,
                             "audio graph emitted sample format %d but the encoder wants %d",
                             got, j->aenc->sample_fmt);
                return CT_ERR_INTERNAL;
            }
            int scaled = av_audio_fifo_write(j->fifo, (void **)j->aframe->extended_data,
                                            j->aframe->nb_samples);
            av_frame_unref(j->aframe);
            if (scaled < 0) {
                ct_av_error(out_error, "av_audio_fifo_write", scaled);
                return CT_ERR_INTERNAL;
            }
            int32_t st = ct_export_encode_audio_ready(j, out_error);
            if (st != CT_OK) return st;
        } else {
            av_frame_unref(j->aframe);
        }
    }
}

/* Send one packet to a decoder and forward every frame it yields. */
static int32_t ct_export_forward(CtExportJob *j, CtExportInput *e, int video,
                                 AVPacket *pkt, char **out_error) {
    AVCodecContext *dec = video ? e->vdec : e->adec;
    if (!dec) {
        av_packet_unref(pkt);
        return CT_OK;
    }
    int ret = avcodec_send_packet(dec, pkt);
    if (pkt) av_packet_unref(pkt);
    if (ret < 0) {
        ct_av_error(out_error, video ? "avcodec_send_packet(video)"
                                     : "avcodec_send_packet(audio)", ret);
        return CT_ERR_DECODE;
    }
    AVFrame *f = av_frame_alloc();
    if (!f) {
        ct_set_error(out_error, "av_frame_alloc failed");
        return CT_ERR_INTERNAL;
    }
    for (;;) {
        ret = avcodec_receive_frame(dec, f);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
        if (ret < 0) {
            av_frame_free(&f);
            ct_av_error(out_error, "avcodec_receive_frame", ret);
            return CT_ERR_DECODE;
        }
        int32_t st = video ? ct_export_push_video(j, e, f, out_error)
                           : ct_export_push_audio(j, e, f, out_error);
        av_frame_unref(f);
        if (st != CT_OK) {
            av_frame_free(&f);
            return st;
        }
    }
    av_frame_free(&f);
    return CT_OK;
}

static int32_t ct_export_drain_sinks(CtExportJob *j, ct_progress_cb progress,
                                     void *user_data, char **out_error) {
    int32_t st = ct_export_drain_video(j, progress, user_data, out_error);
    if (st != CT_OK) return st;
    return ct_export_drain_audio(j, out_error);
}

static int32_t ct_export_eof_input(CtExportJob *j, CtExportInput *e, ct_progress_cb progress,
                                   void *user_data, char **out_error) {
    int32_t st = CT_OK;
    if (e->vdec && !e->v_dec_eof) {
        e->v_dec_eof = 1;
        st = ct_export_forward(j, e, 1, NULL, out_error);
        if (st != CT_OK) return st;
    }
    if (e->adec && !e->a_dec_eof) {
        e->a_dec_eof = 1;
        st = ct_export_forward(j, e, 0, NULL, out_error);
        if (st != CT_OK) return st;
    }
    for (int n = 0; n < e->n_vsrc; ++n) {
        if (!e->vsrc[n]) continue;
        int ret = av_buffersrc_add_frame_flags(e->vsrc[n], NULL, 0);
        e->vsrc[n] = NULL;
        if (ret < 0 && ret != AVERROR_EOF) {
            ct_av_error(out_error, "av_buffersrc_add_frame_flags(video eof)", ret);
            return CT_ERR_DECODE;
        }
    }
    if (e->a_silent) {
        st = ct_export_silence_supply(j, e, 1, out_error);
        if (st != CT_OK) return st;
    }
    for (int n = 0; n < e->n_asrc; ++n) {
        if (!e->asrc[n]) continue;
        int ret = av_buffersrc_add_frame_flags(e->asrc[n], NULL, 0);
        e->asrc[n] = NULL;
        if (ret < 0 && ret != AVERROR_EOF) {
            ct_av_error(out_error, "av_buffersrc_add_frame_flags(audio eof)", ret);
            return CT_ERR_DECODE;
        }
    }
    return ct_export_drain_sinks(j, progress, user_data, out_error);
}

/* -------------------------------------------------------------------------
 * 6. Public entry point
 * ------------------------------------------------------------------------- */

int32_t ct_ff_export_graph(const char *const *src_uris, int32_t src_count,
                           const char *filter_complex, const char *dst_uri,
                           const char *video_codec, int32_t crf, const char *preset,
                           int32_t audio_bitrate_kbps, int64_t duration_us,
                           ct_progress_cb progress, void *user_data, char **out_error) {
    if (!src_uris || src_count <= 0 || src_count > CT_MAX_INPUTS || !filter_complex ||
        !*filter_complex || !dst_uri || !*dst_uri || !out_error) {
        return CT_ERR_INVALID;
    }
    *out_error = NULL;

    CtExportJob j;
    memset(&j, 0, sizeof(j));
    j.n = src_count;
    j.total_us = duration_us > 0 ? duration_us : 0;
    StrBuf desc;
    memset(&desc, 0, sizeof(desc));
    int32_t status = CT_OK;
    int have_sink_audio = 0;

    j.vframe = av_frame_alloc();
    j.aframe = av_frame_alloc();
    j.pkt = av_packet_alloc();
    if (!j.vframe || !j.aframe || !j.pkt) {
        ct_set_error(out_error, "av_frame_alloc / av_packet_alloc failed");
        status = CT_ERR_INTERNAL;
        goto cleanup;
    }

    if (sb_init(&desc, 1024) != 0) {
        ct_set_error(out_error, "out of memory rewriting the graph");
        status = CT_ERR_INTERNAL;
        goto cleanup;
    }
    status = ct_export_rewrite(filter_complex, j.in, j.n, &desc, &have_sink_audio,
                              out_error);
    if (status != CT_OK) goto cleanup;

    for (int i = 0; i < j.n; ++i) {
        status = ct_export_prepare_input(&j.in[i], src_uris[i], i, out_error);
        if (status != CT_OK) goto cleanup;
    }

    status = ct_export_build_graph(&j, desc.data, have_sink_audio, out_error);
    if (status != CT_OK) goto cleanup;

    status = ct_export_open_output(&j, dst_uri, video_codec, crf, preset,
                                   audio_bitrate_kbps, have_sink_audio, out_error);
    if (status != CT_OK) goto cleanup;

    /* Warm-up frames describe the sources; now they become the first frames
     * of each stream. Extra frames decoded in the same packet go right after
     * them so not a single sample is lost at the head of the timeline. */
    for (int i = 0; i < j.n; ++i) {
        CtExportInput *e = &j.in[i];
        if (e->need_v) {
            status = ct_export_push_video(&j, e, e->v_first, out_error);
            if (status != CT_OK) goto cleanup;
            for (int n = 0; n < e->n_v_extra && status == CT_OK; ++n)
                status = ct_export_push_video(&j, e, e->v_extra[n], out_error);
            if (status != CT_OK) goto cleanup;
        }
        if (e->need_a && !e->a_silent) {
            status = ct_export_push_audio(&j, e, e->a_first, out_error);
            if (status != CT_OK) goto cleanup;
            for (int n = 0; n < e->n_a_extra && status == CT_OK; ++n)
                status = ct_export_push_audio(&j, e, e->a_extra[n], out_error);
            if (status != CT_OK) goto cleanup;
        }
    }
    status = ct_export_drain_sinks(&j, progress, user_data, out_error);
    if (status != CT_OK) goto cleanup;

    /* Round-robin: one packet per input per pass. Every buffersrc keeps its
     * own queue, so a branch that `concat` has not reached yet simply waits. */
    int eof_inputs = 0;
    while (eof_inputs < j.n) {
        for (int i = 0; i < j.n && eof_inputs < j.n; ++i) {
            CtExportInput *e = &j.in[i];
            if (e->read_done) continue;
            int ret = av_read_frame(e->fmt, j.pkt);
            if (ret == AVERROR_EOF) {
                e->read_done = 1;
                ++eof_inputs;
                status = ct_export_eof_input(&j, e, progress, user_data, out_error);
                if (status != CT_OK) goto cleanup;
                continue;
            }
            if (ret < 0) {
                ct_av_error(out_error, "av_read_frame", ret);
                status = CT_ERR_DECODE;
                goto cleanup;
            }
            if (j.pkt->stream_index == e->vidx) {
                status = ct_export_forward(&j, e, 1, j.pkt, out_error);
            } else if (j.pkt->stream_index == e->aidx) {
                status = ct_export_forward(&j, e, 0, j.pkt, out_error);
            } else {
                av_packet_unref(j.pkt);
            }
            if (status != CT_OK) goto cleanup;
            status = ct_export_silence_supply(&j, e, 0, out_error);
            if (status != CT_OK) goto cleanup;
            status = ct_export_drain_sinks(&j, progress, user_data, out_error);
            if (status != CT_OK) goto cleanup;
        }
    }

    /* Drain whatever the graph still holds, then pad the last audio frame. */
    for (int guard = 0; guard < 8; ++guard) {
        int64_t before_v = j.v_pts;
        int64_t before_a = av_audio_fifo_size(j.fifo);
        status = ct_export_drain_audio(&j, out_error);
        if (status != CT_OK) goto cleanup;
        status = ct_export_drain_video(&j, progress, user_data, out_error);
        if (status != CT_OK) goto cleanup;
        if (j.v_pts == before_v && av_audio_fifo_size(j.fifo) == before_a) break;
    }

    if (j.aenc && j.fifo && !j.audio_full) {
        int frame_size = j.aenc->frame_size > 0 ? j.aenc->frame_size : 1024;
        int have = av_audio_fifo_size(j.fifo);
        if (have > 0) {
            if (j.a_cap > 0 && j.a_pts + have > j.a_cap) have = (int)(j.a_cap - j.a_pts);
            if (have > 0) {
                AVFrame *f = j.aenc_frame;
                av_frame_unref(f);
                f->nb_samples = frame_size;
                f->format = j.aenc->sample_fmt;
                f->sample_rate = j.aenc->sample_rate;
                av_channel_layout_copy(&f->ch_layout, &j.aenc->ch_layout);
                int ret = av_frame_get_buffer(f, 0);
                if (ret < 0) {
                    ct_av_error(out_error, "av_frame_get_buffer(audio tail)", ret);
                    status = CT_ERR_ENCODE;
                    goto cleanup;
                }
                if (av_audio_fifo_read(j.fifo, (void **)f->extended_data, have) < have) {
                    ct_set_error(out_error, "audio FIFO underrun at the tail");
                    status = CT_ERR_INTERNAL;
                    goto cleanup;
                }
                /* Silence for the rest of the AAC frame, so the encoder gets a
                 * whole block without inventing audible content. */
                int bytes = av_get_bytes_per_sample(j.aenc->sample_fmt);
                int ch_n = j.aenc->ch_layout.nb_channels;
                if (av_sample_fmt_is_planar(j.aenc->sample_fmt)) {
                    for (int ch = 0; ch < ch_n; ++ch) {
                        memset(f->extended_data[ch] + (size_t)have * bytes, 0,
                               (size_t)(frame_size - have) * bytes);
                    }
                } else {
                    memset(f->extended_data[0] + (size_t)have * bytes * ch_n, 0,
                           (size_t)(frame_size - have) * bytes * ch_n);
                }
                f->pts = j.a_pts;
                j.a_pts += frame_size;
                ret = avcodec_send_frame(j.aenc, f);
                av_frame_unref(f);
                if (ret < 0) {
                    ct_av_error(out_error, "avcodec_send_frame(audio tail)", ret);
                    status = CT_ERR_ENCODE;
                    goto cleanup;
                }
                status = ct_export_write_packets(&j, j.aenc, j.ast, out_error);
                if (status != CT_OK) goto cleanup;
            }
        }
    }

    if (j.venc) {
        int ret = avcodec_send_frame(j.venc, NULL);
        if (ret < 0 && ret != AVERROR_EOF) {
            ct_av_error(out_error, "avcodec_send_frame(flush video)", ret);
            status = CT_ERR_ENCODE;
            goto cleanup;
        }
        status = ct_export_write_packets(&j, j.venc, j.vst, out_error);
        if (status != CT_OK) goto cleanup;
    }
    if (j.aenc) {
        int ret = avcodec_send_frame(j.aenc, NULL);
        if (ret < 0 && ret != AVERROR_EOF) {
            ct_av_error(out_error, "avcodec_send_frame(flush audio)", ret);
            status = CT_ERR_ENCODE;
            goto cleanup;
        }
        status = ct_export_write_packets(&j, j.aenc, j.ast, out_error);
        if (status != CT_OK) goto cleanup;
    }
    if (j.out) {
        int ret = av_write_trailer(j.out);
        if (ret < 0) {
            ct_av_error(out_error, "av_write_trailer", ret);
            status = CT_ERR_ENCODE;
            goto cleanup;
        }
    }

cleanup:
    if (desc.data) free(desc.data);
    av_frame_free(&j.vframe);
    av_frame_free(&j.aframe);
    av_frame_free(&j.aenc_frame);
    av_packet_free(&j.pkt);
    if (j.fifo) av_audio_fifo_free(j.fifo);
    if (j.venc) avcodec_free_context(&j.venc);
    if (j.aenc) avcodec_free_context(&j.aenc);
    if (j.graph) avfilter_graph_free(&j.graph);
    for (int i = 0; i < j.n; ++i) ct_export_input_free(&j.in[i]);
    if (j.out) {
        if (!(j.out->oformat->flags & AVFMT_NOFILE) && j.out->pb) avio_closep(&j.out->pb);
        avformat_free_context(j.out);
    }
    if (status == CT_ERR_CANCELLED) remove(dst_uri);
    return status;
}
