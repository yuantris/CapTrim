// HarmonyOS audio preview: OH_AudioRenderer + the CapTrim PCM mixer.
//
// The renderer callback thread owns audio. It publishes the position the
// speaker actually reached into an atomic; the render loop reads that once per
// step and feeds it to captrim_preview_sync_audio. That keeps the preview
// session single-threaded (the render loop) while making audio the master
// clock, which is what the Rust clock module was designed for.
//
// Same rule as presenter.h: no std::string, no new/delete, no exceptions.
#ifndef CAPTRIM_AUDIO_OUTPUT_H
#define CAPTRIM_AUDIO_OUTPUT_H

#include <stdint.h>

#include "captrim.h"

namespace captrim {

struct AudioOutput;

AudioOutput *AudioOutputCreate(captrim_engine_t *engine);
void AudioOutputDestroy(AudioOutput *audio);

/// Start the renderer at `timeline_start_us`. Non-zero on failure; on failure
/// the caller should continue with video only rather than stopping playback.
int32_t AudioOutputStart(AudioOutput *audio, int64_t timeline_start_us);
int32_t AudioOutputStop(AudioOutput *audio);

/// Re-anchor after a scrub. Safe to call while running.
void AudioOutputSeek(AudioOutput *audio, int64_t timeline_us);

/// Paused previews stay silent. The renderer keeps running (which keeps its
/// position monotonic); resuming re-anchors through AudioOutputSeek.
void AudioOutputSetPlaying(AudioOutput *audio, int32_t playing);

/// Latest timeline position the audio device actually rendered, in us.
/// Returns 0 until the first timestamp is available.
int64_t AudioOutputPositionUs(AudioOutput *audio);
uint64_t AudioOutputFramesWritten(AudioOutput *audio);

} // namespace captrim

#endif // CAPTRIM_AUDIO_OUTPUT_H
