#include "audio_output.h"

#include "captrim_audio.h"
#include "captrim_ffmpeg.h" // CT_OK / CT_ERR_* codes shared by every shim ABI

#include <hilog/log.h>
#include <ohaudio/native_audiorenderer.h>
#include <ohaudio/native_audiostream_base.h>
#include <ohaudio/native_audiostreambuilder.h>

#include <stdlib.h>
#include <string.h>
#include <time.h>

namespace captrim {
namespace {

constexpr unsigned int kLogDomain = 0x0001;
constexpr const char *kLogTag = "CapTrimAudio";

#define CAPTRIM_LOGI(fmt, ...) \
    OH_LOG_Print(LOG_APP, LOG_INFO, kLogDomain, kLogTag, fmt, ##__VA_ARGS__)
#define CAPTRIM_LOGE(fmt, ...) \
    OH_LOG_Print(LOG_APP, LOG_ERROR, kLogDomain, kLogTag, fmt, ##__VA_ARGS__)

constexpr int kMaxItems = 8;
constexpr int kUriMax = 512;
constexpr int kMaxFrames = 4096;

/// Pull the audio items out of the FFI JSON array. Serde emits struct fields
/// in declaration order, so "uri" precedes "source_us" precedes "volume".
/// URIs are file paths and contain no escapes in practice; a malformed entry
/// is skipped rather than guessing at a source instant.
static int parse_audio_json(const char *json, ct_audio_item_t *items,
                            char (*uris)[kUriMax], int cap) {
    int n = 0;
    const char *p = json;
    while (p && *p && n < cap) {
        const char *u = strstr(p, "\"uri\":\"");
        if (!u) break;
        u += 7;
        const char *end = strchr(u, '"');
        if (!end || end == u || (size_t)(end - u) >= kUriMax) break;
        const char *s = strstr(end, "\"source_us\":");
        if (!s) break;
        int64_t source_us = strtoll(s + 12, NULL, 10);
        const char *v = strstr(s, "\"volume\":");
        if (!v) break;
        float volume = strtof(v + 9, NULL);
        memcpy(uris[n], u, (size_t)(end - u));
        uris[n][end - u] = '\0';
        items[n].uri = uris[n];
        items[n].source_us = source_us;
        items[n].volume = volume;
        ++n;
        p = v + 9;
    }
    return n;
}

} // namespace

struct AudioOutput {
    OH_AudioRenderer *renderer;
    captrim_engine_t *engine;
    ct_audio_t *mixer;
    /// Renderer frame position that corresponds to `base_us`.
    int64_t base_frame;
    int64_t base_us;
    /// Published to the render loop; atomic because the callback owns writes.
    int64_t position_us;
    uint64_t frames_written;
    int32_t playing;
    char uris[kMaxItems][kUriMax];
    ct_audio_item_t items[kMaxItems];
};

namespace {

static int64_t current_position(AudioOutput *o) {
    int64_t frame = 0;
    int64_t stamp_ns = 0;
    if (o->renderer &&
        OH_AudioRenderer_GetTimestamp(o->renderer, CLOCK_MONOTONIC, &frame, &stamp_ns) ==
            AUDIOSTREAM_SUCCESS) {
        int64_t base_frame = __atomic_load_n(&o->base_frame, __ATOMIC_ACQUIRE);
        int64_t base_us = __atomic_load_n(&o->base_us, __ATOMIC_ACQUIRE);
        return base_us + (frame - base_frame) * 1000000 / CT_AUDIO_SAMPLE_RATE;
    }
    // GetTimestamp is documented to freeze briefly after a device switch or a
    // pause/resume. FramesWritten is less precise but always monotonic.
    int64_t written = 0;
    if (o->renderer &&
        OH_AudioRenderer_GetFramesWritten(o->renderer, &written) == AUDIOSTREAM_SUCCESS) {
        int64_t base_frame = __atomic_load_n(&o->base_frame, __ATOMIC_ACQUIRE);
        int64_t base_us = __atomic_load_n(&o->base_us, __ATOMIC_ACQUIRE);
        return base_us + (written - base_frame) * 1000000 / CT_AUDIO_SAMPLE_RATE;
    }
    return __atomic_load_n(&o->base_us, __ATOMIC_ACQUIRE);
}

static OH_AudioData_Callback_Result on_write_data(OH_AudioRenderer *renderer, void *user_data,
                                                   void *audio_data, int32_t audio_data_size) {
    (void)renderer;
    AudioOutput *o = static_cast<AudioOutput *>(user_data);
    if (!o || audio_data_size <= 0) return AUDIO_DATA_CALLBACK_RESULT_VALID;
    memset(audio_data, 0, (size_t)audio_data_size);

    int bytes_per_frame = CT_AUDIO_CHANNELS * (int)sizeof(float);
    int frames = audio_data_size / bytes_per_frame;
    if (frames > kMaxFrames) frames = kMaxFrames;
    if (frames <= 0) return AUDIO_DATA_CALLBACK_RESULT_VALID;

    int64_t position = current_position(o);
    __atomic_store_n(&o->position_us, position, __ATOMIC_RELEASE);
    if (__atomic_load_n(&o->playing, __ATOMIC_ACQUIRE) == 0) {
        __atomic_fetch_add(&o->frames_written, (uint64_t)frames, __ATOMIC_RELAXED);
        return AUDIO_DATA_CALLBACK_RESULT_VALID;
    }

    char *json = NULL;
    int32_t rc = captrim_engine_plan_audio(o->engine, position, &json);
    if (rc != 0 || !json) {
        captrim_string_free(json);
        __atomic_fetch_add(&o->frames_written, (uint64_t)frames, __ATOMIC_RELAXED);
        return AUDIO_DATA_CALLBACK_RESULT_VALID;
    }
    int count = parse_audio_json(json, o->items, o->uris, kMaxItems);
    captrim_string_free(json);

    char *error = NULL;
    ct_audio_render(o->mixer, static_cast<float *>(audio_data), frames,
                    count > 0 ? o->items : NULL, count, &error);
    captrim_string_free(error);
    __atomic_fetch_add(&o->frames_written, (uint64_t)frames, __ATOMIC_RELAXED);
    return AUDIO_DATA_CALLBACK_RESULT_VALID;
}

} // namespace

AudioOutput *AudioOutputCreate(captrim_engine_t *engine) {
    AudioOutput *o = static_cast<AudioOutput *>(calloc(1, sizeof(*o)));
    if (!o) return NULL;
    o->engine = engine;
    if (ct_audio_create(&o->mixer, NULL) != CT_OK || !o->mixer) {
        free(o);
        return NULL;
    }
    return o;
}

void AudioOutputDestroy(AudioOutput *o) {
    if (!o) return;
    AudioOutputStop(o);
    ct_audio_destroy(o->mixer);
    free(o);
}

int32_t AudioOutputStart(AudioOutput *o, int64_t timeline_start_us) {
    if (!o) return -1;
    if (o->renderer) return 0;

    OH_AudioStreamBuilder *builder = NULL;
    if (OH_AudioStreamBuilder_Create(&builder, AUDIOSTREAM_TYPE_RENDERER) != AUDIOSTREAM_SUCCESS) {
        CAPTRIM_LOGE("cannot create audio stream builder");
        return -1;
    }
    OH_AudioStreamBuilder_SetSamplingRate(builder, CT_AUDIO_SAMPLE_RATE);
    OH_AudioStreamBuilder_SetChannelCount(builder, CT_AUDIO_CHANNELS);
    OH_AudioStreamBuilder_SetSampleFormat(builder, AUDIOSTREAM_SAMPLE_F32LE);
    OH_AudioStreamBuilder_SetRendererInfo(builder, AUDIOSTREAM_USAGE_MOVIE);
    OH_AudioStreamBuilder_SetRendererWriteDataCallback(builder, on_write_data, o);

    OH_AudioRenderer *renderer = NULL;
    OH_AudioStream_Result rc = OH_AudioStreamBuilder_GenerateRenderer(builder, &renderer);
    OH_AudioStreamBuilder_Destroy(builder);
    if (rc != AUDIOSTREAM_SUCCESS || !renderer) {
        CAPTRIM_LOGE("cannot generate audio renderer rc=%{public}d", (int)rc);
        return -1;
    }

    o->renderer = renderer;
    AudioOutputSeek(o, timeline_start_us);
    rc = OH_AudioRenderer_Start(renderer);
    if (rc != AUDIOSTREAM_SUCCESS) {
        CAPTRIM_LOGE("audio renderer start failed rc=%{public}d", (int)rc);
        OH_AudioRenderer_Release(renderer);
        o->renderer = NULL;
        return -1;
    }
    CAPTRIM_LOGI("audio renderer started at %{public}lld us", (long long)timeline_start_us);
    return 0;
}

int32_t AudioOutputStop(AudioOutput *o) {
    if (!o || !o->renderer) return 0;
    OH_AudioRenderer *renderer = o->renderer;
    o->renderer = NULL; /* the callback may still run; it no longer reads us */
    OH_AudioRenderer_Stop(renderer);
    OH_AudioRenderer_Release(renderer);
    return 0;
}

void AudioOutputSeek(AudioOutput *o, int64_t timeline_us) {
    if (!o) return;
    int64_t written = 0;
    if (o->renderer &&
        OH_AudioRenderer_GetFramesWritten(o->renderer, &written) == AUDIOSTREAM_SUCCESS) {
        __atomic_store_n(&o->base_frame, written, __ATOMIC_RELEASE);
    } else {
        __atomic_store_n(&o->base_frame, (int64_t)0, __ATOMIC_RELEASE);
    }
    __atomic_store_n(&o->base_us, timeline_us, __ATOMIC_RELEASE);
    __atomic_store_n(&o->position_us, timeline_us, __ATOMIC_RELEASE);
}

void AudioOutputSetPlaying(AudioOutput *o, int32_t playing) {
    if (o) __atomic_store_n(&o->playing, playing != 0 ? 1 : 0, __ATOMIC_RELEASE);
}

int64_t AudioOutputPositionUs(AudioOutput *o) {
    return o ? __atomic_load_n(&o->position_us, __ATOMIC_ACQUIRE) : 0;
}

uint64_t AudioOutputFramesWritten(AudioOutput *o) {
    return o ? __atomic_load_n(&o->frames_written, __ATOMIC_ACQUIRE) : 0;
}

} // namespace captrim
