/* Audio preview: decode + resample + mix.
 *
 * One mixer owns a small cache of source decoders keyed by URI. Preview
 * pulls short blocks (the audio renderer callback is usually 10 ms), so
 * re-opening a file for every block would be useless overhead. A source is
 * only seeked when the requested instant jumps away from where the previous
 * block left it -- normal continuous playback never seeks.
 */
#include "captrim_audio.h"
#include "captrim_ffmpeg.h"

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CT_AUDIO_MAX_SOURCES 16
/* A jump larger than this is a seek; anything smaller is continuous playback
 * (float rounding of 48 kHz sample instants is well under a millisecond). */
#define CT_AUDIO_SEEK_EPSILON_US 2000

typedef struct {
    char *uri;
    AVFormatContext *fmt;
    AVCodecContext *dec;
    SwrContext *swr;
    AVFrame *frame;
    int stream_index;
    int64_t next_us;
    int swr_ready;
    int eof;
} ct_audio_source_t;

struct ct_audio_t {
    ct_audio_source_t sources[CT_AUDIO_MAX_SOURCES];
    int source_count;
    float *scratch;
    int scratch_frames;
};

static void ct_audio_error(char **out_error, const char *fmt, ...) {
    if (!out_error) return;
    *out_error = NULL;
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    *out_error = (char *)malloc(strlen(msg) + 1);
    if (*out_error) strcpy(*out_error, msg);
}

static void source_close(ct_audio_source_t *s) {
    if (!s) return;
    if (s->swr) swr_free(&s->swr);
    if (s->frame) av_frame_free(&s->frame);
    if (s->dec) avcodec_free_context(&s->dec);
    if (s->fmt) avformat_close_input(&s->fmt);
    free(s->uri);
    memset(s, 0, sizeof(*s));
}

int32_t ct_audio_create(ct_audio_t **out_audio, char **out_error) {
    if (!out_audio) return CT_ERR_INVALID;
    *out_audio = NULL;
    ct_audio_t *a = (ct_audio_t *)calloc(1, sizeof(*a));
    if (!a) {
        ct_audio_error(out_error, "out of memory creating audio mixer");
        return CT_ERR_INTERNAL;
    }
    *out_audio = a;
    return CT_OK;
}

void ct_audio_destroy(ct_audio_t *audio) {
    if (!audio) return;
    for (int i = 0; i < audio->source_count; ++i) source_close(&audio->sources[i]);
    free(audio->scratch);
    free(audio);
}

static ct_audio_source_t *source_find(ct_audio_t *a, const char *uri) {
    for (int i = 0; i < a->source_count; ++i) {
        if (strcmp(a->sources[i].uri, uri) == 0) return &a->sources[i];
    }
    return NULL;
}

static int source_open(ct_audio_t *a, const char *uri, char **out_error) {
    if (a->source_count >= CT_AUDIO_MAX_SOURCES) {
        ct_audio_error(out_error, "audio source cache full (%d)", CT_AUDIO_MAX_SOURCES);
        return -1;
    }
    ct_audio_source_t *s = &a->sources[a->source_count];
    memset(s, 0, sizeof(*s));

    int rc = avformat_open_input(&s->fmt, uri, NULL, NULL);
    if (rc < 0) {
        char buf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(rc, buf, sizeof(buf));
        ct_audio_error(out_error, "cannot open audio source '%s': %s", uri, buf);
        source_close(s);
        return -1;
    }
    rc = avformat_find_stream_info(s->fmt, NULL);
    if (rc < 0) {
        ct_audio_error(out_error, "cannot read audio stream info from '%s'", uri);
        source_close(s);
        return -1;
    }
    const AVCodec *codec = NULL;
    int stream = av_find_best_stream(s->fmt, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0);
    if (stream < 0 || !codec) {
        ct_audio_error(out_error, "no decodable audio stream in '%s'", uri);
        source_close(s);
        return -1;
    }
    s->dec = avcodec_alloc_context3(codec);
    if (!s->dec ||
        avcodec_parameters_to_context(s->dec, s->fmt->streams[stream]->codecpar) < 0 ||
        avcodec_open2(s->dec, codec, NULL) < 0) {
        ct_audio_error(out_error, "cannot open audio decoder for '%s'", uri);
        source_close(s);
        return -1;
    }
    s->frame = av_frame_alloc();
    if (!s->frame) {
        ct_audio_error(out_error, "out of memory for audio frame");
        source_close(s);
        return -1;
    }
    s->stream_index = stream;
    s->uri = (char *)malloc(strlen(uri) + 1);
    if (!s->uri) {
        ct_audio_error(out_error, "out of memory for audio uri");
        source_close(s);
        return -1;
    }
    strcpy(s->uri, uri);
    ++a->source_count;
    return 0;
}

static int source_seek(ct_audio_source_t *s, int64_t us) {
    AVStream *stream = s->fmt->streams[s->stream_index];
    int64_t ts = av_rescale_q(us, (AVRational){1, 1000000}, stream->time_base);
    if (stream->start_time != AV_NOPTS_VALUE) ts += stream->start_time;
    if (av_seek_frame(s->fmt, s->stream_index, ts, AVSEEK_FLAG_BACKWARD) < 0) return -1;
    avcodec_flush_buffers(s->dec);
    /* Discard resampler state: it may hold samples from before the seek. */
    if (s->swr) swr_free(&s->swr);
    s->swr_ready = 0;
    s->eof = 0;
    return 0;
}

static int source_ensure_swr(ct_audio_source_t *s) {
    if (s->swr_ready) return 0;
    AVChannelLayout out_layout;
    av_channel_layout_default(&out_layout, CT_AUDIO_CHANNELS);
    int rc = swr_alloc_set_opts2(&s->swr,
                                 &out_layout, AV_SAMPLE_FMT_FLT, CT_AUDIO_SAMPLE_RATE,
                                 &s->frame->ch_layout, (enum AVSampleFormat)s->frame->format,
                                 s->frame->sample_rate, 0, NULL);
    av_channel_layout_uninit(&out_layout);
    if (rc < 0 || swr_init(s->swr) < 0) return -1;
    s->swr_ready = 1;
    return 0;
}

/* Pull up to `want` interleaved f32 stereo frames into `out`. Returns the
 * number actually written; a short read means the source ended. */
static int source_pull(ct_audio_source_t *s, float *out, int want) {
    int written = 0;
    AVPacket *pkt = av_packet_alloc();
    if (!pkt) return 0;

    while (written < want && !s->eof) {
        /* First drain anything the resampler is still holding, without new
         * input. This matters when an audio frame decodes to more samples
         * than the callback asked for. */
        uint8_t *dst = (uint8_t *)(out + written * CT_AUDIO_CHANNELS);
        int n = (s->swr_ready && s->swr)
                    ? swr_convert(s->swr, &dst, want - written, NULL, 0)
                    : 0;
        if (n > 0) {
            written += n;
            continue;
        }

        int rc = av_read_frame(s->fmt, pkt);
        if (rc < 0) {
            s->eof = 1;
            break;
        }
        if (pkt->stream_index != s->stream_index) {
            av_packet_unref(pkt);
            continue;
        }
        if (avcodec_send_packet(s->dec, pkt) < 0) {
            av_packet_unref(pkt);
            s->eof = 1;
            break;
        }
        av_packet_unref(pkt);

        for (;;) {
            rc = avcodec_receive_frame(s->dec, s->frame);
            if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) break;
            if (rc < 0) {
                s->eof = 1;
                break;
            }
            if (source_ensure_swr(s) != 0) {
                av_frame_unref(s->frame);
                s->eof = 1;
                break;
            }
            dst = (uint8_t *)(out + written * CT_AUDIO_CHANNELS);
            int conv = swr_convert(s->swr, &dst, want - written,
                                   (const uint8_t **)s->frame->extended_data,
                                   s->frame->nb_samples);
            if (conv > 0) written += conv;
            av_frame_unref(s->frame);
            if (written >= want) break;
        }
    }
    av_packet_free(&pkt);
    return written;
}

int32_t ct_audio_render(ct_audio_t *audio,
                        float *out,
                        int frames,
                        const ct_audio_item_t *items,
                        int item_count,
                        char **out_error) {
    if (!audio || !out || frames < 0) return CT_ERR_INVALID;
    const int samples = frames * CT_AUDIO_CHANNELS;
    memset(out, 0, (size_t)samples * sizeof(float));
    if (frames == 0) return CT_OK;
    if (item_count > 0 && !items) return CT_ERR_INVALID;

    if (frames > audio->scratch_frames) {
        float *p = (float *)realloc(audio->scratch, (size_t)samples * sizeof(float));
        if (!p) {
            ct_audio_error(out_error, "out of memory for audio scratch buffer");
            return CT_ERR_INTERNAL;
        }
        audio->scratch = p;
        audio->scratch_frames = frames;
    }

    for (int i = 0; i < item_count; ++i) {
        const ct_audio_item_t *it = &items[i];
        if (!it->uri || !*it->uri || it->volume == 0.0f) continue;
        ct_audio_source_t *s = source_find(audio, it->uri);
        if (!s) {
            /* A missing/broken source is a silent gap, not a failed preview.
             * Consume the error so the caller can still use the mixer. */
            char *ignored = NULL;
            if (source_open(audio, it->uri, &ignored) != 0) {
                ct_ff_string_free(ignored);
                continue;
            }
            ct_ff_string_free(ignored);
            s = &audio->sources[audio->source_count - 1];
        }
        if (s->next_us == 0 ||
            llabs(s->next_us - it->source_us) > CT_AUDIO_SEEK_EPSILON_US) {
            if (source_seek(s, it->source_us) != 0) continue;
        }
        memset(audio->scratch, 0, (size_t)samples * sizeof(float));
        int got = source_pull(s, audio->scratch, frames);
        if (got > 0) {
            for (int k = 0; k < got * CT_AUDIO_CHANNELS; ++k) {
                out[k] += audio->scratch[k] * it->volume;
            }
            s->next_us = it->source_us + (int64_t)got * 1000000 / CT_AUDIO_SAMPLE_RATE;
        }
    }
    return CT_OK;
}
