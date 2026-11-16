/* CapTrim audio preview: decode + resample + mix to a fixed PCM format.
 *
 * The platform owns the audio output device (OH_AudioRenderer / AudioTrack).
 * This ABI owns everything that must be identical on both platforms: which
 * source instant to read, how to convert it to 48 kHz stereo float, and how to
 * sum overlapping clips. Keeping it in the C shim means Android and HarmonyOS
 * cannot disagree about what the preview sounds like.
 *
 * Output format is fixed: 48000 Hz, 2 channels, interleaved float32, little
 * endian. That is the format HarmonyOS accepts as AUDIOSTREAM_SAMPLE_F32LE
 * (API 17+) and Android accepts as AudioTrack.ENCODING_PCM_FLOAT (API 21+).
 */
#ifndef CAPTRIM_AUDIO_H
#define CAPTRIM_AUDIO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One audible contributor for one audio block. `source_us` is the source
 * instant at the START of the block, already speed-mapped and clamped by the
 * Rust frame planner. It is deliberately not frame-quantised: audio is pulled
 * at the renderer's callback rate, which is usually faster than video. */
typedef struct {
    const char *uri;
    int64_t source_us;
    float volume;
} ct_audio_item_t;

typedef struct ct_audio_t ct_audio_t;

/* Output format constants. */
#define CT_AUDIO_SAMPLE_RATE 48000
#define CT_AUDIO_CHANNELS 2

/* Create a mixer. Source decoders are opened lazily on first render and kept
 * for the mixer's lifetime, keyed by URI. */
int32_t ct_audio_create(ct_audio_t **out_audio, char **out_error);

/* Free the mixer and every cached decoder. Safe to call with NULL. */
void ct_audio_destroy(ct_audio_t *audio);

/* Render `frames` sample frames into `out` (interleaved f32 stereo).
 *
 * `items` may be NULL when `item_count` is 0, in which case the whole block
 * is silence.
 *
 * Returns CT_OK and writes exactly `frames * 2` floats. A source that cannot
 * be opened is skipped (its contribution is silence) rather than failing the
 * whole block: a missing file is a gap, not a broken preview. */
int32_t ct_audio_render(ct_audio_t *audio,
                        float *out,
                        int frames,
                        const ct_audio_item_t *items,
                        int item_count,
                        char **out_error);

#ifdef __cplusplus
}
#endif

#endif /* CAPTRIM_AUDIO_H */
