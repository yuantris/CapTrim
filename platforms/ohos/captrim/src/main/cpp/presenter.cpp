#include "presenter.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <hilog/log.h>
#include <multimedia/player_framework/native_avbuffer.h>
#include <multimedia/player_framework/native_avcodec_base.h>
#include <multimedia/player_framework/native_avcodec_videodecoder.h>
#include <multimedia/player_framework/native_avdemuxer.h>
#include <multimedia/player_framework/native_avformat.h>
#include <multimedia/player_framework/native_avsource.h>
#include <native_image/native_image.h>
#include <native_window/external_window.h>

#include "audio_output.h"
#include "gl_compositor.h"

namespace captrim {
namespace {

constexpr unsigned int kLogDomain = 0x0001;
constexpr const char *kLogTag = "CapTrimNative";

#define CAPTRIM_LOGI(fmt, ...) \
    OH_LOG_Print(LOG_APP, LOG_INFO, kLogDomain, kLogTag, fmt, ##__VA_ARGS__)
#define CAPTRIM_LOGE(fmt, ...) \
    OH_LOG_Print(LOG_APP, LOG_ERROR, kLogDomain, kLogTag, fmt, ##__VA_ARGS__)

/// How far ahead of the playhead the demuxer may run.
///
/// This is the decoder's pipeline depth, measured on the device, not a taste
/// setting. Feeding less starves it: at 150 ms (about five frames) this decoder
/// accepted eleven samples and produced *zero* output buffers, so every present
/// waited out its full budget and the screen never updated. At 300 ms it emits
/// immediately.
///
/// The consequence is that the surface can show a frame up to one lead ahead of
/// the playhead during playback. That is why a paused or scrubbed frame is
/// corrected explicitly (see `playing` in OnNewOutputBuffer and the pause path
/// in ApplyCommands) instead of being left to the feed rate.
constexpr int64_t kFeedAheadUs = 300000;

/// A decoded frame earlier than target - tolerance is stale; free it.
constexpr int64_t kFrameToleranceUs = 20000;

/// Seek backwards when the target jumps back by more than this.
constexpr int64_t kSeekBackUs = 100000;

/// Seek forwards when the target jumps beyond what has been fed plus this.
constexpr int64_t kSeekAheadUs = 1000000;

/// Give up waiting for a frame after this long instead of stalling the loop.
constexpr int64_t kWaitBudgetUs = 120000;

/// Decoder output buffers held back for hold-and-present. Four covers more
/// than one 30 fps frame at the measured 300 ms feed lead without exhausting
/// the decoder's output buffer pool.
constexpr int32_t kMaxHeldFrames = 4;

constexpr int32_t kUriMax = 512;
constexpr int32_t kMimeMax = 32;

// Format keys as they appear in OH_AVFormat_DumpInfo on device.
//
// The OH_MD_KEY_* extern constants are the documented way to address these, but
// on the device under test every lookup through them returned a garbage pointer
// (the track was reported as mime=<junk> while DumpInfo clearly showed
// "codec_mime = video/avc"). The dump prints the real key names, and those are
// stable across releases, so the presenter addresses the format by name.
constexpr const char *kKeyCodecMime = "codec_mime";
constexpr const char *kKeyWidth = "width";
constexpr const char *kKeyHeight = "height";
constexpr const char *kKeyTrackCount = "track_count";
constexpr const char *kKeyPixelFormat = "pixel_format";

/// How many layers can be decoded at once.
///
/// Four is a guess at the ceiling that matters in practice (main + one PiP +
/// one sticker video + one spare) rather than a hardware limit: the real limit
/// is how many instances the SoC's decoder will hand out, which is discovered
/// the hard way -- OH_VideoDecoder_CreateByMime() returning null. Exceeding
/// this evicts the least recently used slot instead.
constexpr int32_t kMaxSources = 4;

struct PresenterState;

/// One decoder's worth of state: a demuxer, a codec, and the NativeImage its
/// frames land in.
///
/// There is one slot per *layer*, not per media. The same file can appear twice
/// at two different instants (a picture-in-picture of the same clip), and a
/// decoder can only be at one position at a time, so two layers of one file
/// legitimately need two slots. That is also the honest cost of a PiP: two
/// hardware decoders running at once.
struct SourceSlot {
    PresenterState *owner;

    /// Clip this slot is decoding; 0 means the slot is free.
    uint64_t clip_id;
    uint64_t source_id;

    char uri[kUriMax];
    int32_t fd;
    OH_AVSource *source;
    OH_AVDemuxer *demuxer;
    OH_AVCodec *codec;
    uint32_t track;
    int32_t width;
    int32_t height;

    /// Where this slot's decoded frames go: a texture the compositor samples,
    /// not the window. Its native window is what OH_VideoDecoder_SetSurface()
    /// is given.
    CompSource *comp_source;
    /// 1 once at least one frame has been latched into `comp_source`. Drawing
    /// before that samples an uninitialised texture, which is not black -- it
    /// is whatever happened to be in that memory.
    int32_t has_frame;

    // Touched by this slot's codec callbacks, hence volatile.
    volatile int64_t target_us;
    volatile int64_t fed_pts_us;
    volatile int32_t reached;
    volatile int32_t eos;
    volatile int32_t failed;
    volatile int32_t abort;
    /// Set while this slot's codec is being restarted. Its callbacks must not
    /// touch the codec or the demuxer meanwhile: Stop() reclaims every buffer
    /// it handed out, and feeding one back is an AV_ERR_INVALID_STATE at best.
    volatile int32_t seeking;
    /// Set when the layer comes back into the plan after being absent. Playback
    /// never seeks to catch up (that self-locks, see RequestSlotFrame), but a
    /// layer that was simply not on screen for a while has a decoder parked far
    /// behind the playhead with no way to know it -- so it gets exactly one
    /// seek on re-entry.
    volatile int32_t resume;

    int64_t last_rendered_us;
    /// Plan instant this slot last appeared in. Doubles as the age used when a
    /// new layer needs a slot and every one of them is busy.
    int64_t last_plan_us;
    /// 1 while this slot's layer is part of the current plan. Compared against
    /// the previous present to notice a layer coming back on screen, which is
    /// the one case that earns a seek during playback.
    int32_t in_plan;

    /// Hold-and-present: during playback the decoder runs ~300 ms ahead, but
    /// only frames whose PTS has been reached by the audio-driven playhead may
    /// be handed to the surface. The rest stay in these codec buffers. A full
    /// queue falls back to showing the earliest held frame instead of stalling
    /// the decoder forever. Owned by the callback thread for writes and the
    /// render thread for drains, hence the lock.
    struct {
        uint32_t index;
        int64_t pts_us;
    } held[kMaxHeldFrames];
    int32_t held_count;
    pthread_mutex_t held_lock;

    uint64_t frames_decoded;
    /// Frames decoded and deliberately not shown, to land on the playhead while
    /// paused. Not a performance figure.
    uint64_t frames_skipped;
    /// Frames withheld from the surface because their PTS was in the future.
    uint64_t frames_held;
    uint32_t input_calls;
    uint32_t output_calls;
    uint32_t acquire_ok;
    uint32_t acquire_empty;

    /// Guards `demuxer`, which this slot's input callback reads on one thread
    /// while a seek repositions it on another.
    pthread_mutex_t demux_lock;
};

struct PresenterState {
    OHNativeWindow *window;
    captrim_engine_t *engine;
    /// Audio renderer, created with the presenter and started/stopped with the
    /// loop. The callback thread publishes its position here for the render
    /// loop to consume; it never touches the preview session.
    AudioOutput *audio;

    /// EGL context bound to `window`, owned by the render thread. Null until the
    /// loop starts: EGL contexts are per-thread, so it cannot be created here on
    /// the UI thread and used there.
    GlCompositor *compositor;

    /// Canvas size in physical pixels, as reported by the XComponent.
    int32_t surface_width;
    int32_t surface_height;

    /// Live decoders. A plan with more layers than this evicts the slot that
    /// has gone unused longest.
    SourceSlot slots[kMaxSources];
    /// Slots that could not be opened this session (no decoder instance left,
    /// missing file). Reported, never fatal while another layer still draws.
    int32_t slot_failures;

    /// Forces a redraw without a new decoded frame: the surface needs one after
    /// a resize, when its contents are gone but the picture is not.
    volatile int32_t compose_dirty;
    /// The last plan drawn, so a redraw after a resize has geometry to reuse.
    captrim_frame_desc_t last_desc;
    volatile int32_t has_last_desc;

    /// Mirror of the transport state, so the output callbacks know whether the
    /// playhead is moving. While it is not, only the frame at the playhead is
    /// allowed on screen -- the decoder hands out its pipeline worth of frames
    /// on every seek, and drawing all of them would park a paused preview ten
    /// frames past the scrub position.
    volatile int32_t playing;

    /// Times a frame was actually drawn onto the window. A paused preview
    /// composites once per new target, so this outruns the latch counter by
    /// design (one draw covers every layer).
    uint64_t frames_composited;
    uint32_t wait_timeouts;
    uint32_t present_calls;

    // One lock plus one per slot, deliberately:
    //   `lock` guards loop state and the condition. It is never held across a
    //   codec call -- OH_VideoDecoder_Flush() waits for the callback threads to
    //   finish, and those callbacks take `lock` to signal, so holding it across
    //   a flush deadlocks the render loop on the first seek (observed: the loop
    //   went silent forever right after "Demuxer seek").
    //   Each slot's `demux_lock` guards its demuxer, which its own input
    //   callback reads on one thread while a seek repositions it on another.
    //   They are per slot because the decoders run concurrently: one lock for
    //   all of them would serialise two independent pipelines.
    pthread_mutex_t lock;
    pthread_cond_t cond;

    // The render loop. It lives on its own thread because present() blocks until
    // the decoder hands the frame to the surface: running that from the UI
    // thread starves ArkUI's event loop and the system kills the app for being
    // unresponsive (observed: THREAD_BLOCK_6S).
    pthread_t loop_thread;
    volatile int32_t loop_running;
    int32_t loop_joinable;   // a thread exists and still owes us a join
    captrim_preview_t *loop_preview;
    int64_t clock_base_us;   // caller's clock at start
    int64_t mono_base_us;    // CLOCK_MONOTONIC at start, for delta
    volatile int32_t stop_requested; // makes a blocked RenderAt() return now
    volatile int32_t loop_error;     // sticky: why the loop stopped

    // Transport commands from the UI thread, applied by the loop thread. This
    // keeps the preview session single-owner: no locking inside Rust, no torn
    // clock anchors when a seek lands mid-frame.
    volatile int32_t cmd_playing; // -1 none, 0 pause, 1 play
    volatile int32_t cmd_seek;
    volatile int64_t cmd_seek_us;
    volatile int32_t cmd_resize;
    int32_t cmd_width;
    int32_t cmd_height;

    // Latest snapshot, guarded by `lock`, so status() never blocks on a decode.
    PresentReport last;
};

void Signal(PresenterState *p) {
    pthread_mutex_lock(&p->lock);
    pthread_cond_broadcast(&p->cond);
    pthread_mutex_unlock(&p->lock);
}

void OnError(OH_AVCodec *codec, int32_t error_code, void *user_data) {
    (void)codec;
    // user_data is the slot, not the presenter: two decoders run at once, and a
    // failure has to be attributed to the layer it belongs to.
    SourceSlot *slot = static_cast<SourceSlot *>(user_data);
    if (slot == nullptr || slot->owner == nullptr) {
        return;
    }
    slot->failed = 1;
    CAPTRIM_LOGE("decoder error %{public}d on clip %llu (source %llu)", error_code,
                 (unsigned long long)slot->clip_id, (unsigned long long)slot->source_id);
    Signal(slot->owner);
}

void OnStreamChanged(OH_AVCodec *codec, OH_AVFormat *format, void *user_data) {
    (void)codec;
    (void)format;
    (void)user_data;
    // Resolution changes mid-stream are not supported yet; the surface keeps the
    // geometry the first track declared.
}

void OnNeedInputBuffer(OH_AVCodec *codec, uint32_t index, OH_AVBuffer *buffer,
                       void *user_data) {
    SourceSlot *slot = static_cast<SourceSlot *>(user_data);
    if (slot == nullptr || slot->owner == nullptr || slot->abort) {
        return;
    }
    PresenterState *p = slot->owner;

    // Backpressure. The playhead is the only clock that matters, so hold the
    // input buffer until this slot has caught up with it. A layer that is not
    // in the current plan stops being advanced (`target_us` is left where it
    // was), so its decoder parks here rather than decoding frames nobody will
    // see. `stop_requested` is part of the guard because this loop spins: a
    // shutdown while a decoder is this far ahead would otherwise wait forever
    // for a join that cannot happen.
    while (!slot->abort && p->stop_requested == 0 &&
           slot->fed_pts_us > slot->target_us + kFeedAheadUs) {
        usleep(2000);
    }
    if (slot->abort || slot->demuxer == nullptr || slot->seeking != 0) {
        // During a codec restart the codec owns every buffer again; it
        // re-requests the ones it still needs, so dropping this call is
        // harmless.
        return;
    }

    OH_AVCodecBufferAttr attr;
    memset(&attr, 0, sizeof(attr));

    pthread_mutex_lock(&slot->demux_lock);
    if (slot->demuxer == nullptr) {
        pthread_mutex_unlock(&slot->demux_lock);
        return;
    }
    OH_AVErrCode rc = OH_AVDemuxer_ReadSampleBuffer(slot->demuxer, slot->track, buffer);
    pthread_mutex_unlock(&slot->demux_lock);

    if (rc != AV_ERR_OK) {
        CAPTRIM_LOGE("demux rc=%{public}d clip=%llu at fed=%{public}lld target=%{public}lld", rc,
                     (unsigned long long)slot->clip_id, (long long)slot->fed_pts_us,
                     (long long)slot->target_us);
        // End of stream: push an empty, EOS-flagged buffer so the codec drains
        // and reports EOS through the output callback.
        attr.size = 0;
        attr.offset = 0;
        attr.pts = 0;
        attr.flags = AVCODEC_BUFFER_FLAGS_EOS;
        OH_AVBuffer_SetBufferAttr(buffer, &attr);
        OH_VideoDecoder_PushInputBuffer(codec, index);
        return;
    }

    OH_AVBuffer_GetBufferAttr(buffer, &attr);
    slot->fed_pts_us = attr.pts;
    slot->input_calls++;
    OH_AVErrCode push = OH_VideoDecoder_PushInputBuffer(codec, index);
    // First few samples of each slot only: enough to prove the demuxer is
    // feeding the codec the right thing (keyframe, sane size, accepted push)
    // without logging at frame rate.
    if (slot->input_calls <= 3) {
        CAPTRIM_LOGI("push clip=%llu #%{public}u size=%{public}d pts=%{public}lld "
                     "key=%{public}d rc=%{public}d",
                     (unsigned long long)slot->clip_id, slot->input_calls, attr.size,
                     (long long)attr.pts, (attr.flags & AVCODEC_BUFFER_FLAGS_SYNC_FRAME) != 0, push);
    }
}

/// Hand held frames whose PTS has been reached by the playhead to the surface.
/// Frames still in the future stay in the codec, which is the "hold" half of
/// hold-and-present. `target_us <= 0` means discard them all (seek/teardown).
/// The signal is deliberately sent after releasing held_lock: the render loop
/// takes `lock` first and then held_lock, so signalling under held_lock would
/// invert that order and deadlock on the first seek.
static void ResolveHeldFrames(SourceSlot *slot, int64_t target_us) {
    if (slot->codec == nullptr) return;
    pthread_mutex_lock(&slot->held_lock);
    int keep = 0;
    for (int i = 0; i < slot->held_count; ++i) {
        if (target_us > 0 && slot->held[i].pts_us <= target_us + kFrameToleranceUs) {
            OH_AVErrCode rc = OH_VideoDecoder_RenderOutputBuffer(slot->codec, slot->held[i].index);
            if (rc == AV_ERR_OK) {
                slot->last_rendered_us = slot->held[i].pts_us;
                slot->frames_decoded++;
                slot->reached = 1;
                continue;
            }
        }
        if (target_us <= 0) {
            OH_VideoDecoder_FreeOutputBuffer(slot->codec, slot->held[i].index);
        } else {
            slot->held[keep++] = slot->held[i];
        }
    }
    slot->held_count = keep;
    pthread_mutex_unlock(&slot->held_lock);
    if (target_us > 0) Signal(slot->owner);
}

void OnNewOutputBuffer(OH_AVCodec *codec, uint32_t index, OH_AVBuffer *buffer,
                       void *user_data) {
    SourceSlot *slot = static_cast<SourceSlot *>(user_data);
    if (slot == nullptr || slot->owner == nullptr || slot->abort) {
        CAPTRIM_LOGE("output cb ignored (abort=%{public}d)", slot != nullptr ? slot->abort : -1);
        return;
    }
    PresenterState *p = slot->owner;

    OH_AVCodecBufferAttr attr;
    memset(&attr, 0, sizeof(attr));
    if (OH_AVBuffer_GetBufferAttr(buffer, &attr) != AV_ERR_OK) {
        CAPTRIM_LOGE("output cb: cannot read buffer attr");
        OH_VideoDecoder_FreeOutputBuffer(codec, index);
        return;
    }

    if (slot->seeking != 0) {
        // The flush is recycling this buffer; rendering it would race the seek.
        OH_VideoDecoder_FreeOutputBuffer(codec, index);
        return;
    }

    slot->output_calls++;
    if (slot->output_calls <= 3) {
        CAPTRIM_LOGI("output clip=%llu #%{public}u pts=%{public}lld flags=%{public}u",
                     (unsigned long long)slot->clip_id, slot->output_calls, (long long)attr.pts,
                     attr.flags);
    }

    if ((attr.flags & AVCODEC_BUFFER_FLAGS_EOS) != 0) {
        slot->eos = 1;
        OH_VideoDecoder_FreeOutputBuffer(codec, index);
        Signal(p);
        return;
    }

    // A decoded frame that is merely late still belongs on screen: a picture one
    // frame behind the clock looks like video, while a dropped frame looks like
    // a freeze. Earlier versions dropped anything more than 20 ms behind the
    // playhead, which turned a short decode backlog into a slideshow (observed:
    // 57 frames drawn and 471 dropped over an 8 s timeline). Only frames that
    // cannot be what the playhead asked for are skipped, and only while paused.
    if (p->playing == 0 && (attr.pts + 5000 < slot->target_us || attr.pts > slot->target_us + 1000)) {
        // Paused or scrubbing: draw exactly the frame at the playhead. The
        // decoder emits its whole pipeline (about ten frames) after a seek, so
        // anything else would leave the freeze frame off the scrub position.
        //
        // Counted separately from `dropped`: these frames are not lost to
        // slowness, they are deliberately skipped to land precisely. Rolling
        // them into the same counter made the status line read "丢帧 217" on a
        // device that never missed a deadline.
        slot->frames_skipped++;
        OH_VideoDecoder_FreeOutputBuffer(codec, index);
        return;
    }

    // Playing: hold frames that are still in the future. The decoder's 300 ms
    // feed lead otherwise paints a picture ahead of the audio-driven playhead;
    // holding the codec buffer (instead of rendering it early) keeps the newest
    // texture at or before the playhead. The render loop drains these once the
    // playhead catches up. When the queue is full, release the older frames
    // first so the decoder never runs out of output buffers.
    if (p->playing != 0 && attr.pts > slot->target_us + kFrameToleranceUs) {
        pthread_mutex_lock(&slot->held_lock);
        if (slot->held_count < kMaxHeldFrames) {
            slot->held[slot->held_count].index = index;
            slot->held[slot->held_count].pts_us = attr.pts;
            slot->held_count++;
            slot->frames_held++;
            pthread_mutex_unlock(&slot->held_lock);
            return;
        }
        pthread_mutex_unlock(&slot->held_lock);
        ResolveHeldFrames(slot, attr.pts);
    }

    // RenderOutputBuffer, not FreeOutputBuffer: the latter only returns the
    // buffer to the codec, while the former also hands it to the surface the
    // decoder was configured with. That surface is this slot's NativeImage, so
    // this call is what queues the frame for the render thread to latch --
    // using FreeOutputBuffer here decodes every frame and shows none (observed:
    // three outputs, zero buffers in the image).
    int32_t covers = attr.pts + kFrameToleranceUs >= slot->target_us ? 1 : 0;
    OH_AVErrCode render_rc = OH_VideoDecoder_RenderOutputBuffer(codec, index);
    if (render_rc == AV_ERR_OK) {
        slot->last_rendered_us = attr.pts;
        slot->frames_decoded++;
        // The wait is satisfied only by a frame that actually covers the target;
        // older frames are still drawn -- they are the best picture available at
        // this instant -- so the surface ends on the newest frame at or before
        // the playhead instead of jumping straight to a future one.
        if (covers != 0) {
            slot->reached = 1;
            Signal(p);
        }
    } else if (slot->output_calls <= 5) {
        CAPTRIM_LOGE("RenderOutputBuffer #%{public}u failed (%{public}d) clip=%llu pts=%{public}lld",
                     slot->output_calls, (int32_t)render_rc, (unsigned long long)slot->clip_id,
                     (long long)attr.pts);
    }
}

/// Release everything a slot holds and mark it free.
///
/// Order matters: the codec has to be stopped before its NativeImage goes away,
/// because it still holds buffers that live in that image's queue -- dropping
/// the queue first is a use-after-free inside the decoder.
void TeardownSlot(SourceSlot *slot) {
    slot->abort = 1;
    ResolveHeldFrames(slot, 0); // return held buffers before Stop reclaims them
    if (slot->codec != nullptr) {
        OH_VideoDecoder_Stop(slot->codec);
        OH_VideoDecoder_Destroy(slot->codec);
        slot->codec = nullptr;
    }
    if (slot->comp_source != nullptr) {
        CompositorDestroySource(slot->comp_source);
        slot->comp_source = nullptr;
    }
    if (slot->demuxer != nullptr) {
        OH_AVDemuxer_Destroy(slot->demuxer);
        slot->demuxer = nullptr;
    }
    if (slot->source != nullptr) {
        OH_AVSource_Destroy(slot->source);
        slot->source = nullptr;
    }
    if (slot->fd >= 0) {
        close(slot->fd);
        slot->fd = -1;
    }
    slot->clip_id = 0;
    slot->source_id = 0;
    slot->uri[0] = '\0';
    slot->track = 0;
    slot->width = 0;
    slot->height = 0;
    slot->has_frame = 0;
    slot->eos = 0;
    slot->reached = 0;
    slot->failed = 0;
    slot->resume = 0;
    slot->seeking = 0;
    slot->fed_pts_us = 0;
    slot->target_us = 0;
    slot->last_rendered_us = -1;
    slot->last_plan_us = -1;
    slot->abort = 0;
}


int64_t MonoUs() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000000 + ts.tv_nsec / 1000;
}


/// Read a format value as a NUL-terminated string.
///
/// `OH_AVFormat_GetStringValue` is the documented way, but on the device under
/// test it reports success with a non-null pointer whose contents are not the
/// value: reading "codec_mime" yields two bytes of junk while
/// `OH_AVFormat_DumpInfo` prints `codec_mime = video/avc` for the same object.
/// So: try the accessor, validate the result, and fall back to the dump, which
/// is a documented, stable, human-readable rendering.
bool FormatString(OH_AVFormat *format, const char *key, char *out, int32_t out_len,
                  bool *out_used_dump) {
    if (out_used_dump != nullptr) {
        *out_used_dump = false;
    }
    const char *value = nullptr;
    if (OH_AVFormat_GetStringValue(format, key, &value) && value != nullptr &&
        strncmp(value, "video/", 6) == 0) {
        strncpy(out, value, static_cast<size_t>(out_len - 1));
        out[out_len - 1] = '\0';
        return true;
    }

    const char *dump = OH_AVFormat_DumpInfo(format);
    if (dump == nullptr) {
        return false;
    }
    // Keys are rendered as "<key> = <value> | <key> = ...".
    size_t key_len = strlen(key);
    const char *cursor = dump;
    while ((cursor = strstr(cursor, key)) != nullptr) {
        const char *after = cursor + key_len;
        if (strncmp(after, " = ", 3) == 0) {
            const char *start = after + 3;
            const char *end = strchr(start, '|');
            size_t length = end == nullptr ? strlen(start) : static_cast<size_t>(end - start);
            while (length > 0 && (start[length - 1] == ' ' || start[length - 1] == '\n')) {
                length--;
            }
            if (length == 0 || length >= static_cast<size_t>(out_len)) {
                return false;
            }
            memcpy(out, start, length);
            out[length] = '\0';
            if (out_used_dump != nullptr) {
                *out_used_dump = true;
            }
            return true;
        }
        cursor = after;
    }
    return false;
}

/// Open `source_id` for one layer in `slot`, positioned at `target_us`, and
/// leave the decoder started.
///
/// The playhead is applied *before* Start(): Start() immediately issues the
/// first input callbacks, and those feed whatever the demuxer is sitting on. An
/// earlier version started at 0 and let RenderAt() seek afterwards, which meant
/// every source switch decoded and threw away everything from 0 to the playhead
/// -- 0.66 s of dead time and a burst of dropped frames at each cut.
int32_t OpenSlot(PresenterState *p, SourceSlot *slot, uint64_t clip_id, uint64_t source_id,
                 int64_t target_us) {
    TeardownSlot(slot);
    if (p->window == nullptr || p->compositor == nullptr) {
        return -1;
    }

    int32_t needed = 0;
    int32_t rc = captrim_engine_source_uri(p->engine, source_id, slot->uri, kUriMax, &needed);
    if (rc != 0) {
        CAPTRIM_LOGE("source %llu has no uri (needed %d bytes)", (unsigned long long)source_id,
                     needed);
        return rc;
    }

    const char *path = slot->uri;
    if (strncmp(path, "file://", 7) == 0) {
        path += 7;
    }
    slot->fd = open(path, O_RDONLY);
    if (slot->fd < 0) {
        CAPTRIM_LOGE("cannot open %{public}s (errno %{public}d)", path, errno);
        return -5;
    }
    off_t size = lseek(slot->fd, 0, SEEK_END);
    if (size <= 0 || lseek(slot->fd, 0, SEEK_SET) != 0) {
        CAPTRIM_LOGE("cannot size %{public}s", path);
        TeardownSlot(slot);
        return -5;
    }
    CAPTRIM_LOGI("opening %{public}s size=%{public}lld for clip %llu", path, (long long)size,
                 (unsigned long long)clip_id);

    slot->source = OH_AVSource_CreateWithFD(slot->fd, 0, size);
    if (slot->source == nullptr) {
        CAPTRIM_LOGE("OH_AVSource_CreateWithFD failed for %{public}s", path);
        TeardownSlot(slot);
        return -5;
    }
    slot->demuxer = OH_AVDemuxer_CreateWithSource(slot->source);
    if (slot->demuxer == nullptr) {
        CAPTRIM_LOGE("OH_AVDemuxer_CreateWithSource failed");
        TeardownSlot(slot);
        return -5;
    }

    // Find the first video track. A media file with no video is not an error
    // for the editor (audio-only clips exist), it just has nothing to draw.
    OH_AVFormat *source_format = OH_AVSource_GetSourceFormat(slot->source);
    int32_t track_count = 0;
    if (source_format == nullptr ||
        !OH_AVFormat_GetIntValue(source_format, kKeyTrackCount, &track_count)) {
        CAPTRIM_LOGE("cannot read track count");
        TeardownSlot(slot);
        return -5;
    }

    // Select by MIME ("video/...") rather than by OH_MD_KEY_TRACK_TYPE: the
    // track-type string is not stable across containers, while every decoder
    // path is keyed off the MIME anyway.
    OH_AVFormat *track_format = nullptr;
    for (int32_t i = 0; i < track_count; i++) {
        OH_AVFormat *candidate = OH_AVSource_GetTrackFormat(slot->source, static_cast<uint32_t>(i));
        if (candidate == nullptr) {
            continue;
        }
        char mime[kMimeMax] = {0};
        bool via_dump = false;
        if (!FormatString(candidate, kKeyCodecMime, mime, kMimeMax, &via_dump)) {
            OH_AVFormat_Destroy(candidate);
            continue;
        }
        CAPTRIM_LOGI("track %{public}d mime=%{public}s via_dump=%{public}d", i, mime,
                     via_dump ? 1 : 0);
        if (strncmp(mime, "video/", 6) == 0) {
            slot->track = static_cast<uint32_t>(i);
            track_format = candidate;
            break;
        }
        OH_AVFormat_Destroy(candidate);
    }
    if (track_format == nullptr) {
        CAPTRIM_LOGE("no video track in %{public}s", path);
        TeardownSlot(slot);
        return -2;
    }

    // Select before Start: Start kicks off the input callback immediately, and a
    // read on a demuxer with no selected track fails with
    // AV_ERR_OPERATE_NOT_PERMIT (2).
    if (OH_AVDemuxer_SelectTrackByID(slot->demuxer, slot->track) != AV_ERR_OK) {
        CAPTRIM_LOGE("SelectTrackByID failed");
        OH_AVFormat_Destroy(track_format);
        TeardownSlot(slot);
        return -6;
    }

    char mime[kMimeMax] = {0};
    int32_t width = 0;
    int32_t height = 0;
    FormatString(track_format, kKeyCodecMime, mime, kMimeMax, nullptr);
    OH_AVFormat_GetIntValue(track_format, kKeyWidth, &width);
    OH_AVFormat_GetIntValue(track_format, kKeyHeight, &height);
    if (mime[0] == '\0' || width <= 0 || height <= 0) {
        CAPTRIM_LOGE("video track lacks mime/size");
        OH_AVFormat_Destroy(track_format);
        TeardownSlot(slot);
        return -6;
    }
    slot->width = width;
    slot->height = height;

    slot->codec = OH_VideoDecoder_CreateByMime(mime);
    if (slot->codec == nullptr) {
        // Two layers means two decoder instances, and an SoC that has already
        // handed out all of them answers exactly here. Worth a distinct message:
        // the failure is "the device is out of decoders", not "the file is bad".
        CAPTRIM_LOGE("no decoder for %{public}s (instance limit reached?)", mime);
        OH_AVFormat_Destroy(track_format);
        TeardownSlot(slot);
        return -6;
    }

    // Configure first: this decoder answers AV_ERR_INVALID_STATE for SetSurface
    // while the instance is still created-but-unconfigured.
    OH_AVFormat *configure = OH_AVFormat_Create();
    if (configure == nullptr) {
        OH_AVFormat_Destroy(track_format);
        TeardownSlot(slot);
        return -9;
    }
    OH_AVFormat_SetIntValue(configure, kKeyWidth, width);
    OH_AVFormat_SetIntValue(configure, kKeyHeight, height);
    OH_AVFormat_SetIntValue(configure, kKeyPixelFormat, AV_PIXEL_FORMAT_SURFACE_FORMAT);
    OH_AVErrCode code = OH_VideoDecoder_Configure(slot->codec, configure);
    OH_AVFormat_Destroy(configure);
    if (code != AV_ERR_OK) {
        CAPTRIM_LOGE("Configure failed (%{public}d)", code);
        OH_AVFormat_Destroy(track_format);
        TeardownSlot(slot);
        return -6;
    }

    // Surface output mode, but the surface is this slot's own NativeImage rather
    // than the XComponent: the pixels still never pass through user space, and
    // they end up in a texture that can be scaled, moved and stacked.
    slot->comp_source = CompositorNewSource(p->compositor);
    if (slot->comp_source == nullptr) {
        CAPTRIM_LOGE("cannot create an image source");
        OH_AVFormat_Destroy(track_format);
        TeardownSlot(slot);
        return -6;
    }
    OH_AVErrCode surface_code =
        OH_VideoDecoder_SetSurface(slot->codec, CompositorSourceWindow(slot->comp_source));
    if (surface_code != AV_ERR_OK) {
        CAPTRIM_LOGE("SetSurface failed (%{public}d)", surface_code);
        OH_AVFormat_Destroy(track_format);
        TeardownSlot(slot);
        return -6;
    }

    OH_AVCodecCallback callback;
    callback.onError = OnError;
    callback.onStreamChanged = OnStreamChanged;
    callback.onNeedInputBuffer = OnNeedInputBuffer;
    callback.onNewOutputBuffer = OnNewOutputBuffer;
    OH_AVErrCode callback_code = OH_VideoDecoder_RegisterCallback(slot->codec, callback, slot);
    if (callback_code != AV_ERR_OK) {
        CAPTRIM_LOGE("RegisterCallback failed (%{public}d)", callback_code);
        OH_AVFormat_Destroy(track_format);
        TeardownSlot(slot);
        return -6;
    }
    OH_AVFormat_Destroy(track_format);

    if (OH_VideoDecoder_Prepare(slot->codec) != AV_ERR_OK) {
        CAPTRIM_LOGE("Prepare failed");
        TeardownSlot(slot);
        return -6;
    }

    if (OH_VideoDecoder_Start(slot->codec) != AV_ERR_OK) {
        CAPTRIM_LOGE("Start failed");
        TeardownSlot(slot);
        return -6;
    }

    // Claim the slot last: everything above may call TeardownSlot() on failure,
    // and that frees the slot with clip_id = 0.
    slot->owner = p;
    slot->clip_id = clip_id;
    slot->source_id = source_id;
    slot->target_us = target_us;
    slot->fed_pts_us = target_us - 1;
    slot->last_rendered_us = -1;
    slot->last_plan_us = -1;
    if (target_us > 0 &&
        OH_AVDemuxer_SeekToTime(slot->demuxer, target_us / 1000, SEEK_MODE_PREVIOUS_SYNC) !=
            AV_ERR_OK) {
        CAPTRIM_LOGE("initial seek to %{public}lld failed", (long long)target_us);
    }

    CAPTRIM_LOGI("clip %llu: %{public}s track %{public}u %{public}dx%{public}d %{public}s "
                 "at %{public}lldms",
                 (unsigned long long)clip_id, path, slot->track, width, height, mime,
                 (long long)(target_us / 1000));
    return 0;
}

void SeekSlot(SourceSlot *slot, int64_t target_us) {
    if (slot->codec == nullptr) {
        return;
    }
    PresenterState *p = slot->owner;
    // Reset state first, then release every lock before touching the codec:
    // Stop() waits for the callback threads, and those callbacks take the
    // presenter lock to signal, so holding anything here deadlocks the render
    // loop. This is why the state lock and the codec call are separated even
    // though the codec belongs to the slot.
    pthread_mutex_lock(&p->lock);
    slot->fed_pts_us = target_us - 1;
    slot->eos = 0;
    slot->reached = 0;
    slot->failed = 0;
    slot->seeking = 1;
    pthread_mutex_unlock(&p->lock);

    ResolveHeldFrames(slot, 0); // Stop/Start below reclaims every codec buffer

    pthread_mutex_lock(&slot->demux_lock);
    if (slot->demuxer != nullptr) {
        // The demuxer seeks in milliseconds and lands on the previous sync
        // sample; the decoder then walks forward to the exact frame.
        OH_AVDemuxer_SeekToTime(slot->demuxer, target_us / 1000, SEEK_MODE_PREVIOUS_SYNC);
    }
    pthread_mutex_unlock(&slot->demux_lock);

    // Stop/Start rather than Flush: after a Flush this decoder never asked for
    // input again, so the surface stayed on the old frame while the wait ran
    // out its budget (observed: a seek to 4 s left the picture at 8 s and the
    // drawn counter frozen). Stop/Start is the documented lifecycle and Start
    // reliably re-issues the input callbacks.
    OH_VideoDecoder_Stop(slot->codec);
    if (OH_VideoDecoder_Start(slot->codec) != AV_ERR_OK) {
        CAPTRIM_LOGE("decoder restart after seek failed (clip %llu)",
                     (unsigned long long)slot->clip_id);
    }
    slot->seeking = 0;
    Signal(p);
}

/// Ask one slot for `target_us` and wait until that frame is on its surface.
int32_t RenderSlotAt(SourceSlot *slot, int64_t target_us) {
    PresenterState *p = slot->owner;
    pthread_mutex_lock(&p->lock);
    bool need_seek = slot->eos != 0 || target_us < slot->last_rendered_us - kSeekBackUs ||
                     target_us > slot->fed_pts_us + kSeekAheadUs;
    slot->reached = 0;
    slot->failed = 0;
    slot->target_us = target_us;
    pthread_mutex_unlock(&p->lock);

    // The audio clock may have advanced past frames the callback withheld.
    // Release those now, before waiting: reached=1 lets the loop skip the wait
    // and go straight to the compositor.
    ResolveHeldFrames(slot, target_us);

    if (need_seek) {
        SeekSlot(slot, target_us);
    }

    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += kWaitBudgetUs / 1000000;
    deadline.tv_nsec += (kWaitBudgetUs % 1000000) * 1000;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec += 1;
        deadline.tv_nsec -= 1000000000L;
    }

    int32_t result = -6;
    pthread_mutex_lock(&p->lock);
    while (slot->reached == 0 && slot->failed == 0 && slot->eos == 0 && slot->abort == 0 &&
           p->stop_requested == 0) {
        if (pthread_cond_timedwait(&p->cond, &p->lock, &deadline) == ETIMEDOUT) {
            break;
        }
    }
    if (slot->reached != 0) {
        result = 0;
    } else if (p->stop_requested == 0 && slot->abort == 0) {
        // A missed deadline is normal while seeking or when the decoder is
        // slow; only a run of them means something is wrong, so log sparsely.
        // Skip-free logging: this counter is cumulative, and a run of misses is
        // exactly what a starved decoder looks like from the outside.
        if (++p->wait_timeouts % 30 == 1) {
            CAPTRIM_LOGE("no frame (miss #%{public}u) clip %llu: target=%{public}lld "
                         "fed=%{public}lld last=%{public}lld eos=%{public}d failed=%{public}d",
                         p->wait_timeouts, (unsigned long long)slot->clip_id,
                         (long long)target_us, (long long)slot->fed_pts_us,
                         (long long)slot->last_rendered_us, slot->eos, slot->failed);
        }
    }
    pthread_mutex_unlock(&p->lock);
    return result;
}

/// Ask one slot for `target_us`, waiting only when the transport is paused.
///
/// Returns 0 when the frame at `target_us` is now in the slot's image, 1 when
/// the request was only recorded (the caller must not assume new pixels), and a
/// negative error otherwise.
///
/// Playback is real-time: the clock is authoritative and the decoder is allowed
/// to lag. Waiting for the exact frame is what broke it -- the clock ran ahead of
/// a decoder that was itself stalled by a full surface queue, so every step spent
/// its 120 ms budget, which made the consumer slower, which kept the queue full.
/// Measured before this change: 95 presents and 51 frames over a 12 s window,
/// against 357 decoded. Without the wait the loop keeps draining the queue and the
/// viewer sees the newest frame the decoder has.
///
/// Paused is the opposite case: nothing is moving, so waiting is the only way to
/// land on the scrub position, and there is no clock to fall behind.
int32_t RequestSlotFrame(SourceSlot *slot, int64_t target_us) {
    PresenterState *p = slot->owner;
    if (p->playing == 0) {
        return RenderSlotAt(slot, target_us);
    }
    // Playing: only restart the pipeline at end of stream, or when the layer has
    // just come back into the plan.
    //
    // An earlier version also seeked when the playhead got more than a second
    // ahead of the decode frontier. That is a trap: the decoder falls behind
    // because the surface queue is full because the loop is slow, and a seek
    // makes the loop much slower (Stop/Start plus refilling the pipeline costs
    // hundreds of milliseconds), so it falls further behind and seeks again.
    // Measured: Present averaged 87 ms with 22 calls in a 2 s window, and the
    // decoded-frame counter moved by zero. Lagging during playback is caught up
    // by dropping frames, which is what realtime means.
    //
    // `resume` is what separates the two cases. A picture-in-picture clip that
    // was off screen for two seconds has a decoder parked two seconds back and
    // no way to know it, so it earns exactly one seek; a slow decoder has the
    // same position gap but must never be seeked, because seeking is what made
    // it slow. Only the render loop knows which one happened, and it says so
    // with a flag.
    if (slot->resume != 0) {
        slot->resume = 0;
        SeekSlot(slot, target_us);
        return 1;
    }
    if (slot->eos != 0) {
        SeekSlot(slot, target_us);
        return 1;
    }
    pthread_mutex_lock(&p->lock);
    slot->target_us = target_us;
    slot->reached = 0;
    pthread_mutex_unlock(&p->lock);
    return 1;
}

/// The slot decoding `clip_id`, or null when this layer has no decoder yet.
SourceSlot *SlotForClip(PresenterState *p, uint64_t clip_id) {
    if (clip_id == 0) {
        return nullptr;
    }
    for (int32_t i = 0; i < kMaxSources; i++) {
        if (p->slots[i].clip_id == clip_id && p->slots[i].codec != nullptr) {
            return &p->slots[i];
        }
    }
    return nullptr;
}

/// A slot to decode one more layer in, evicting the stalest one if needed.
///
/// The eviction rule is "the layer that has gone longest without being in a
/// plan", not plain least-recently-used: on a timeline of many short clips every
/// cut needs a new slot, and evicting one that is still on screen would tear the
/// picture apart mid-frame. A slot in the current plan is only evicted when
/// there is no other choice, i.e. when the plan itself has more layers than
/// there are slots.
SourceSlot *AllocSlot(PresenterState *p) {
    for (int32_t i = 0; i < kMaxSources; i++) {
        if (p->slots[i].codec == nullptr) {
            return &p->slots[i];
        }
    }

    int32_t victim = -1;
    for (int32_t i = 0; i < kMaxSources; i++) {
        bool planned = p->slots[i].in_plan != 0;
        if (planned) {
            continue;
        }
        if (victim < 0 || p->slots[i].last_plan_us < p->slots[victim].last_plan_us) {
            victim = i;
        }
    }
    if (victim < 0) {
        for (int32_t i = 0; i < kMaxSources; i++) {
            if (victim < 0 || p->slots[i].last_plan_us < p->slots[victim].last_plan_us) {
                victim = i;
            }
        }
    }
    CAPTRIM_LOGE("no free decoder: evicting clip %llu for a layer of this frame",
                 (unsigned long long)p->slots[victim].clip_id);
    TeardownSlot(&p->slots[victim]);
    return &p->slots[victim];
}

/// Latch the newest decoded frame and draw the plan onto the window.
///
/// Returns 1 when a new frame reached the screen, 0 when there was nothing new
/// to draw, and -1 when the draw itself failed (lost EGL surface, which means
/// the compositor has to be recreated).
///
/// `force` redraws even without a newly decoded frame. The scheduler only
/// returns Present when the plan changed, and a plan can change without the
/// picture's source instant moving -- a transform, an opacity, a clip trimmed
/// under the playhead. Skipping the draw in that case would leave the pre-edit
/// picture on screen until the playhead moved, which reads as "the edit did
/// nothing".
///
/// Deliberately independent of whether RenderAt() met its deadline: if the wait
/// expired, the newest frame the decoder did produce still belongs on screen.
/// Withholding it is what makes a slow decoder look like a freeze.
int32_t ComposePlan(PresenterState *p, const captrim_frame_desc_t *desc, int32_t force) {
    if (p->compositor == nullptr) {
        return 0;
    }
    p->present_calls++;

    // Latch every live slot, unconditionally, and do it before anything can
    // return early.
    //
    // Never gate this on the frame-available callback: it fires once per queue
    // flush, while a decoder runs ~300 ms ahead and recycles buffers in
    // between, so the loop saw one notification and missed the frames behind it
    // (observed: 25 frames composited for an 8 s timeline that had 323
    // decoded). UpdateSurfaceImage returns NATIVE_ERROR_NO_BUFFER immediately
    // when the queue is empty, so an unconditional attempt costs a function
    // call and nothing else.
    //
    // Nor may it be skipped when there is no plan to draw yet: a freshly opened
    // clip takes a pipeline's worth of time to produce its first frame, and the
    // step that opens it returns Present *before* that frame exists. Draining
    // only when a plan is available left the preview stuck on black forever --
    // the idle path then had nothing to draw, so it returned before latching,
    // so no frame ever arrived.
    //
    // Every active slot is drained, not only the ones in this plan. A slot whose
    // layer is currently off screen still has a decoder that would otherwise
    // fill its image queue and block inside RenderOutputBuffer -- and that
    // block happens in a callback, so a later OH_VideoDecoder_Stop() would wait
    // on it forever.
    int32_t fresh[kMaxSources];
    memset(fresh, 0, sizeof(fresh));
    for (int32_t i = 0; i < kMaxSources; i++) {
        SourceSlot &slot = p->slots[i];
        if (slot.codec == nullptr || slot.comp_source == nullptr) {
            continue;
        }
        if (CompositorSourceAcquire(slot.comp_source) == 0) {
            slot.has_frame = 1;
            slot.acquire_ok++;
            fresh[i] = 1;
        } else {
            slot.acquire_empty++;
        }
    }

    // A resize wipes the window but not the plan, so a redraw without a fresh
    // step falls back to the last one drawn.
    const captrim_frame_desc_t *plan = desc;
    if (plan == nullptr || plan->layer_count == 0) {
        if (p->has_last_desc == 0) {
            return 0;
        }
        plan = &p->last_desc;
    } else {
        // Remember the geometry even if nothing can be drawn this time round:
        // the frames that make it drawable arrive later, and the idle path needs
        // the plan to be here when they do.
        p->last_desc = *desc;
        p->has_last_desc = 1;
    }

    // One quad per video layer that has pixels to show. A layer with no slot
    // yet, or with a slot that has never latched a frame, is skipped rather
    // than drawn: an uninitialised texture is not black, it is whatever was in
    // that memory.
    CompLayer layers[CAPTRIM_MAX_LAYERS];
    int32_t count = 0;
    int32_t any_new = 0;
    for (int32_t i = 0; i < plan->layer_count && count < CAPTRIM_MAX_LAYERS; i++) {
        const captrim_layer_desc_t *layer = &plan->layers[i];
        if (layer->kind != CAPTRIM_LAYER_VIDEO) {
            continue;
        }
        int32_t which = -1;
        for (int32_t s = 0; s < kMaxSources; s++) {
            if (p->slots[s].clip_id != 0 && p->slots[s].clip_id == layer->clip_id) {
                which = s;
                break;
            }
        }
        if (which < 0 || p->slots[which].has_frame == 0) {
            continue;
        }
        SourceSlot &slot = p->slots[which];
        CompLayer &out = layers[count++];
        memset(&out, 0, sizeof(out));
        out.texture = CompositorSourceTexture(slot.comp_source);
        out.tex_matrix = CompositorSourceMatrix(slot.comp_source);
        out.src_w = slot.width;
        out.src_h = slot.height;
        out.scale = layer->scale;
        out.dx = layer->dx;
        out.dy = layer->dy;
        out.rotation_deg = layer->rotation_deg;
        out.opacity = layer->opacity;
        if (fresh[which] != 0) {
            any_new = 1;
        }
    }

    if (any_new == 0 && p->compose_dirty == 0 && force == 0) {
        // Nothing new, nothing to repair: the window already shows this frame.
        return 0;
    }
    if (count == 0) {
        // Everything planned is still waiting for its first frame. Leave the
        // window alone instead of clearing it: a freshly opened clip legitimately
        // needs a pipeline's worth of time, and flashing black at every cut
        // looks like a bug in the edit rather than a loading state.
        return 0;
    }

    if (CompositorDraw(p->compositor, layers, count) != 0) {
        return -1;
    }
    p->compose_dirty = 0;
    p->last_desc = *plan;
    p->has_last_desc = 1;
    p->frames_composited++;
    return any_new;
}

/// Put every decoder back on the frame the current plan wants.
///
/// Pausing cannot rely on the feed rate: the surface holds a frame from up to
/// one feed lead ahead, and the paused state never asks for another one, so
/// without this the freeze frame sits ~300 ms past where the user stopped.
///
/// Seeking each slot to its *layer's* source instant, rather than to the
/// timeline position, also gets clip trims right: where a clip sits on the
/// timeline is not where it sits in the file.
void SeekAllToPlan(PresenterState *p) {
    if (p->has_last_desc == 0) {
        return;
    }
    for (int32_t i = 0; i < p->last_desc.layer_count; i++) {
        const captrim_layer_desc_t *layer = &p->last_desc.layers[i];
        if (layer->kind != CAPTRIM_LAYER_VIDEO) {
            continue;
        }
        for (int32_t s = 0; s < kMaxSources; s++) {
            if (p->slots[s].clip_id != 0 && p->slots[s].clip_id == layer->clip_id) {
                SeekSlot(&p->slots[s], layer->source_us);
                break;
            }
        }
    }
}

/// Wait up to `us` on the presenter condition, so a transport command or a
/// decoder event wakes the loop immediately instead of after a fixed sleep.
void WaitOn(PresenterState *p, int64_t us) {
    if (us <= 0) {
        return;
    }
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += static_cast<time_t>(us / 1000000);
    deadline.tv_nsec += static_cast<long>((us % 1000000) * 1000);
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec += 1;
        deadline.tv_nsec -= 1000000000L;
    }
    pthread_mutex_lock(&p->lock);
    pthread_cond_timedwait(&p->cond, &p->lock, &deadline);
    pthread_mutex_unlock(&p->lock);
}

/// Apply anything the UI thread queued. Returns 1 when a seek happened, which
/// means the frame on the surface is stale and must be replaced even if the
/// playhead has not moved.
int32_t ApplyCommands(PresenterState *p, int64_t now) {
    int32_t playing = -1;
    int32_t seek = 0;
    int64_t seek_us = 0;
    int32_t resize = 0;
    int32_t width = 0;
    int32_t height = 0;

    pthread_mutex_lock(&p->lock);
    if (p->cmd_resize != 0) {
        resize = 1;
        width = p->cmd_width;
        height = p->cmd_height;
        p->cmd_resize = 0;
    }
    if (p->cmd_playing >= 0) {
        playing = p->cmd_playing;
        p->cmd_playing = -1;
    }
    if (p->cmd_seek != 0) {
        seek = 1;
        seek_us = p->cmd_seek_us;
        p->cmd_seek = 0;
    }
    pthread_mutex_unlock(&p->lock);

    if (resize != 0 && width > 0 && height > 0) {
        p->surface_width = width;
        p->surface_height = height;
        if (p->compositor != nullptr && CompositorResize(p->compositor, width, height) != 0) {
            CAPTRIM_LOGE("resize to %{public}dx%{public}d failed", width, height);
        }
        // The window lost its contents with the new geometry, so the next step
        // has to draw even if the playhead has not moved.
        p->compose_dirty = 1;
    }

    if (seek != 0) {
        int32_t rc = captrim_preview_seek(p->loop_preview, seek_us, now);
        if (rc != 0) {
            CAPTRIM_LOGE("seek to %{public}lld failed (rc=%{public}d)", (long long)seek_us, rc);
        }
        int64_t landed = 0;
        if (captrim_preview_position(p->loop_preview, now, &landed) == 0) {
            AudioOutputSeek(p->audio, landed);
        }
    }
    if (playing >= 0) {
        int32_t was_playing = p->playing;
        p->playing = playing;
        int32_t rc = captrim_preview_set_playing(p->loop_preview, playing, now);
        if (rc != 0) {
            CAPTRIM_LOGE("set_playing(%{public}d) failed (rc=%{public}d)", playing, rc);
        }
        AudioOutputSetPlaying(p->audio, playing);
        if (playing != 0) {
            // The renderer kept running while paused, so its position has run
            // ahead of the timeline. Re-anchor to where the playhead resumes.
            int64_t resumed = 0;
            if (captrim_preview_position(p->loop_preview, now, &resumed) == 0) {
                AudioOutputSeek(p->audio, resumed);
            }
        }
        if (playing == 0 && was_playing != 0) {
            // Pausing has to land on the playhead. The surface currently holds a
            // frame from up to one feed lead ahead, and the paused state never
            // asks for another one, so without this the freeze frame would sit
            // ~300 ms past where the user stopped.
            SeekAllToPlan(p);
        }
    }
    return seek;
}

void *LoopMain(void *arg) {
    PresenterState *p = static_cast<PresenterState *>(arg);
    CAPTRIM_LOGI("render loop running");

    // The EGL context is created here, on the thread that draws, and dies with
    // it: a context belongs to the thread that made it current, so it cannot be
    // created up in PresenterCreate and used down here.
    p->compositor = CompositorCreate(p->window, p->surface_width, p->surface_height);
    if (p->compositor == nullptr) {
        CAPTRIM_LOGE("cannot set up the compositor; the window has nothing to draw with");
        p->loop_error = -8;
        p->loop_running = 0;
        return nullptr;
    }

    // Periodic progress line. On a device a frozen surface and a decoded frame
    // look identical in a screenshot, so "is it actually playing?" has to be
    // answered by numbers, not by looking.
    int64_t next_beat_us = MonoUs() + 2000000;
    // Time spent inside Present since the last beat. Without this the loop rate
    // is a single number with no attribution, and "26 fps" could be the decoder,
    // the swap, or the wait.
    int64_t busy_us = 0;
    int64_t busy_count = 0;

    while (p->loop_running != 0) {
        int64_t now = p->clock_base_us + (MonoUs() - p->mono_base_us);
        ApplyCommands(p, now);

        PresentReport report;
        int64_t enter_us = MonoUs();
        int32_t rc = PresenterPresent(reinterpret_cast<Presenter *>(p), p->loop_preview, now,
                                      &report);
        busy_us += MonoUs() - enter_us;
        busy_count++;
        if (rc != 0) {
            // Structural failure (no video track, cannot open the file, no
            // decoder). Record it and stop: retrying at 60 Hz would just flood
            // the log with the same error.
            CAPTRIM_LOGE("render loop stopping on rc=%{public}d", rc);
            p->loop_error = rc;
            break;
        }

        if (MonoUs() >= next_beat_us) {
            next_beat_us = MonoUs() + 2000000;
            uint64_t decoded = 0;
            uint64_t latched = 0;
            uint64_t empty = 0;
            uint64_t skipped = 0;
            uint32_t in_calls = 0;
            uint32_t out_calls = 0;
            int32_t live = 0;
            for (int32_t i = 0; i < kMaxSources; i++) {
                const SourceSlot &slot = p->slots[i];
                if (slot.codec == nullptr) {
                    continue;
                }
                live++;
                decoded += slot.frames_decoded;
                latched += slot.acquire_ok;
                empty += slot.acquire_empty;
                skipped += slot.frames_skipped;
                in_calls += slot.input_calls;
                out_calls += slot.output_calls;
            }
            unsigned long long not_shown = decoded > latched ? decoded - latched : 0;
            CAPTRIM_LOGI("loop: pos=%{public}lldms src=%{public}lldms kind=%{public}d "
                         "slots=%{public}d drawn=%{public}llu unseen=%{public}llu "
                         "skipped=%{public}llu in=%{public}u out=%{public}u calls=%{public}u "
                         "ok=%{public}llu empty=%{public}llu fail=%{public}d "
                         "busy=%{public}lldus/%{public}lld win=%{public}lldms",
                         (long long)(report.position_us / 1000), (long long)(report.source_us / 1000),
                         report.kind, live, (unsigned long long)p->frames_composited, not_shown,
                         (unsigned long long)skipped, in_calls, out_calls, p->present_calls,
                         (unsigned long long)latched, (unsigned long long)empty, p->slot_failures,
                         (long long)busy_us, (long long)busy_count,
                         (long long)((now - p->clock_base_us) / 1000));
            // One line per live decoder. Aggregates say "two layers are running"
            // but not which one is starved, and with a picture-in-picture that
            // is the only question worth asking.
            for (int32_t i = 0; i < kMaxSources; i++) {
                const SourceSlot &slot = p->slots[i];
                if (slot.codec == nullptr) {
                    continue;
                }
                CAPTRIM_LOGI("  slot clip=%llu src=%llu at=%{public}lldms dec=%{public}llu "
                             "acq=%{public}u eos=%{public}d",
                             (unsigned long long)slot.clip_id,
                             (unsigned long long)slot.source_id,
                             (long long)(slot.target_us / 1000),
                             (unsigned long long)slot.frames_decoded, slot.acquire_ok, slot.eos);
            }
            busy_us = 0;
            busy_count = 0;
        }

        if (report.kind == CAPTRIM_STEP_WAIT_FRAME) {
            // Sleep exactly until the next frame is due; a command wakes it.
            int64_t wait = report.due_wall_us - now;
            if (wait > 0 && wait <= 100000) {
                WaitOn(p, wait);
                continue;
            }
        }
        if (report.kind == CAPTRIM_STEP_END || report.kind == CAPTRIM_STEP_IDLE) {
            // End of timeline, or paused with nothing to draw. Block until a
            // command arrives; PresenterControl signals the condition, so the
            // wait below is an upper bound, not a latency floor.
            WaitOn(p, 100000);
            continue;
        }
        WaitOn(p, 2000);
    }

    CAPTRIM_LOGI("render loop stopped (drawn %{public}llu, err %{public}d)",
                 (unsigned long long)p->frames_composited, p->loop_error);

    // Order: each slot's codec and image first, then the context that owns the
    // textures, then the thread that made it current.
    for (int32_t i = 0; i < kMaxSources; i++) {
        if (p->slots[i].codec != nullptr || p->slots[i].comp_source != nullptr ||
            p->slots[i].source != nullptr) {
            TeardownSlot(&p->slots[i]);
        }
    }
    if (p->compositor != nullptr) {
        CompositorDestroy(p->compositor);
        p->compositor = nullptr;
    }
    p->loop_running = 0;
    return nullptr;
}

} // namespace

Presenter *PresenterCreate(captrim_engine_t *engine, uint64_t surface_id, int32_t width,
                           int32_t height) {
    if (engine == nullptr) {
        return nullptr;
    }
    PresenterState *p = static_cast<PresenterState *>(calloc(1, sizeof(PresenterState)));
    if (p == nullptr) {
        return nullptr;
    }
    p->engine = engine;
    p->audio = AudioOutputCreate(engine);
    p->cmd_playing = -1;
    // calloc leaves the slots zeroed, but "free" and "at the beginning of the
    // file" must not be the same value, so every default that differs is set.
    for (int32_t i = 0; i < kMaxSources; i++) {
        p->slots[i].owner = p;
        p->slots[i].fd = -1;
        p->slots[i].last_rendered_us = -1;
        p->slots[i].last_plan_us = -1;
        pthread_mutex_init(&p->slots[i].demux_lock, nullptr);
        pthread_mutex_init(&p->slots[i].held_lock, nullptr);
    }
    // Layer geometry is relative to the canvas, so a zero size here would make
    // every layer degenerate. The page knows the XComponent's size; ask for it
    // rather than guessing from the window, which is still unconfigured.
    p->surface_width = width > 0 ? width : 1;
    p->surface_height = height > 0 ? height : 1;
    pthread_mutex_init(&p->lock, nullptr);
    pthread_cond_init(&p->cond, nullptr);

    if (OH_NativeWindow_CreateNativeWindowFromSurfaceId(surface_id, &p->window) != 0 ||
        p->window == nullptr) {
        CAPTRIM_LOGE("cannot get native window for surface %llu",
                     (unsigned long long)surface_id);
        pthread_cond_destroy(&p->cond);
        pthread_mutex_destroy(&p->lock);
        for (int32_t i = 0; i < kMaxSources; i++) {
            pthread_mutex_destroy(&p->slots[i].demux_lock);
        pthread_mutex_destroy(&p->slots[i].held_lock);
        }
        free(p);
        return nullptr;
    }
    CAPTRIM_LOGI("presenter ready for surface %llu %{public}dx%{public}d",
                 (unsigned long long)surface_id, p->surface_width, p->surface_height);
    return reinterpret_cast<Presenter *>(p);
}

void PresenterDestroy(Presenter *opaque) {
    if (opaque == nullptr) {
        return;
    }
    PresenterState *presenter = reinterpret_cast<PresenterState *>(opaque);
    // The loop owns the decoder callbacks and the preview session; it has to be
    // gone before any of that is torn down. It also tears the slots down itself
    // on the way out, on the thread whose EGL context made their textures --
    // which is why what is left here is normally nothing but the window.
    PresenterLoopStop(opaque);
    AudioOutputDestroy(presenter->audio);
    presenter->audio = nullptr;
    for (int32_t i = 0; i < kMaxSources; i++) {
        if (presenter->slots[i].codec != nullptr || presenter->slots[i].source != nullptr ||
            presenter->slots[i].fd >= 0) {
            TeardownSlot(&presenter->slots[i]);
        }
        pthread_mutex_destroy(&presenter->slots[i].demux_lock);
    }
    if (presenter->window != nullptr) {
        OH_NativeWindow_DestroyNativeWindow(presenter->window);
        presenter->window = nullptr;
    }
    pthread_cond_destroy(&presenter->cond);
    pthread_mutex_destroy(&presenter->lock);
    free(presenter);
}

namespace {

/// Frames the decoders produced that never reached the screen.
///
/// Deliberately computed rather than accumulated: the counters are updated on
/// the codec callback threads while the loop latches, so a running tally drifts.
/// The subtraction is exact as long as the loop is the only consumer, which it
/// is.
uint64_t FramesNotShown(const PresenterState *p) {
    uint64_t decoded = 0;
    uint64_t latched = 0;
    for (int32_t i = 0; i < kMaxSources; i++) {
        decoded += p->slots[i].frames_decoded;
        latched += p->slots[i].acquire_ok;
    }
    return decoded > latched ? decoded - latched : 0;
}

uint64_t FramesHeld(const PresenterState *p) {
    uint64_t held = 0;
    for (int32_t i = 0; i < kMaxSources; i++) held += p->slots[i].frames_held;
    return held;
}

/// Publish a snapshot and attach the counters the loop owns.
void Publish(PresenterState *p, PresentReport *report) {
    report->running = p->loop_running;
    report->error = p->loop_error;
    report->frames = p->frames_composited;
    report->dropped = FramesNotShown(p);
    report->frames_held = FramesHeld(p);
    pthread_mutex_lock(&p->lock);
    p->last = *report;
    pthread_mutex_unlock(&p->lock);
}

} // namespace

int32_t PresenterLoopStart(Presenter *opaque, captrim_preview_t *preview,
                           int64_t now_wall_us) {
    if (opaque == nullptr || preview == nullptr) {
        return -1;
    }
    PresenterState *p = reinterpret_cast<PresenterState *>(opaque);
    if (p->loop_joinable != 0) {
        // A previous thread still exists (running, or exited on its own and not
        // yet reaped). Starting another would leak its handle.
        return -1;
    }

    p->loop_preview = preview;
    p->clock_base_us = now_wall_us;
    p->mono_base_us = MonoUs();
    p->loop_error = 0;
    p->stop_requested = 0;
    p->cmd_playing = -1;
    p->cmd_seek = 0;
    p->loop_running = 1;

    int64_t audio_start = 0;
    if (captrim_preview_position(preview, now_wall_us, &audio_start) != 0) audio_start = 0;
    AudioOutputSetPlaying(p->audio, 0); /* the page starts paused */
    if (AudioOutputStart(p->audio, audio_start) != 0) {
        // Audio is an enhancement: a device without an available renderer must
        // still show video. The status report keeps audio_position_us at 0.
        CAPTRIM_LOGE("audio preview unavailable; continuing with video only");
    }

    if (pthread_create(&p->loop_thread, nullptr, LoopMain, p) != 0) {
        CAPTRIM_LOGE("cannot create render thread");
        p->loop_running = 0;
        p->loop_preview = nullptr;
        return -1;
    }
    p->loop_joinable = 1;
    return 0;
}

int32_t PresenterLoopStop(Presenter *opaque) {
    if (opaque == nullptr) {
        return -1;
    }
    PresenterState *p = reinterpret_cast<PresenterState *>(opaque);
    if (p->loop_joinable == 0) {
        return 0;
    }
    p->loop_running = 0;
    p->stop_requested = 1;
    Signal(p); // wake an idle wait, or a RenderAt() blocked on the codec
    pthread_join(p->loop_thread, nullptr);
    AudioOutputStop(p->audio);
    p->loop_joinable = 0;
    p->loop_preview = nullptr;
    p->stop_requested = 0;
    return 0;
}

int32_t PresenterControl(Presenter *opaque, int32_t playing, int32_t has_seek,
                         int64_t seek_us) {
    if (opaque == nullptr) {
        return -1;
    }
    PresenterState *p = reinterpret_cast<PresenterState *>(opaque);
    pthread_mutex_lock(&p->lock);
    if (playing >= 0) {
        p->cmd_playing = playing != 0 ? 1 : 0;
    }
    if (has_seek != 0) {
        p->cmd_seek_us = seek_us;
        p->cmd_seek = 1;
    }
    pthread_mutex_unlock(&p->lock);
    Signal(p);
    return 0;
}

int32_t PresenterResize(Presenter *opaque, int32_t width, int32_t height) {
    if (opaque == nullptr || width <= 0 || height <= 0) {
        return -1;
    }
    PresenterState *p = reinterpret_cast<PresenterState *>(opaque);
    pthread_mutex_lock(&p->lock);
    p->cmd_width = width;
    p->cmd_height = height;
    p->cmd_resize = 1;
    pthread_mutex_unlock(&p->lock);
    Signal(p);
    return 0;
}

int32_t PresenterStatus(Presenter *opaque, PresentReport *out_report) {
    if (opaque == nullptr || out_report == nullptr) {
        return -1;
    }
    PresenterState *p = reinterpret_cast<PresenterState *>(opaque);
    pthread_mutex_lock(&p->lock);
    *out_report = p->last;
    pthread_mutex_unlock(&p->lock);
    // Counters move on the decoder threads; read them unlocked so a busy decode
    // never blocks the UI thread. They only ever increase, so a stale sample
    // reads as a slightly older frame count, never as a wrong one.
    out_report->running = p->loop_running;
    out_report->error = p->loop_error;
    out_report->frames = p->frames_composited;
    out_report->dropped = FramesNotShown(p);
    out_report->frames_held = FramesHeld(p);
    return 0;
}

int32_t PresenterPresent(Presenter *opaque, captrim_preview_t *preview, int64_t now_wall_us,
                         PresentReport *out_report) {
    if (opaque == nullptr || preview == nullptr || out_report == nullptr) {
        return -1;
    }
    PresenterState *p = reinterpret_cast<PresenterState *>(opaque);
    memset(out_report, 0, sizeof(*out_report));
    out_report->source_us = -1;
    out_report->audio_position_us = AudioOutputPositionUs(p->audio);
    out_report->audio_frames_written = AudioOutputFramesWritten(p->audio);

    // What is on screen, as opposed to what the latest step planned: an idle or
    // waiting step plans nothing, but the surface still shows the last frame, and
    // a status line that reports "-" while a picture is visible is just noise.
    PresentReport on_screen;
    pthread_mutex_lock(&p->lock);
    on_screen = p->last;
    pthread_mutex_unlock(&p->lock);

    // Audio is the master clock. Read the position the speaker actually
    // reached before planning this frame; the Rust clock turns the drift into
    // a small rate adjustment or a hard snap. Never do this while paused: the
    // renderer keeps draining its buffer and would drag a frozen playhead on.
    if (p->playing != 0) {
        int64_t audio_pos = AudioOutputPositionUs(p->audio);
        if (audio_pos > 0) {
            int64_t drift = 0;
            captrim_preview_sync_audio(preview, audio_pos, now_wall_us, &drift);
            if (drift < -20000 || drift > 20000) {
                CAPTRIM_LOGI("audio clock snap %{public}lld us", (long long)drift);
            }
        }
    }

    int64_t position = 0;
    if (captrim_preview_position(preview, now_wall_us, &position) == 0) {
        out_report->position_us = position;
    }

    captrim_frame_desc_t desc;
    memset(&desc, 0, sizeof(desc));
    int32_t kind = CAPTRIM_STEP_IDLE;
    int64_t due = 0;
    int32_t rc = captrim_preview_step_frame(preview, now_wall_us, &desc, &kind, &due);
    if (rc != 0) {
        Publish(p, out_report);
        return rc;
    }
    out_report->kind = kind;
    out_report->due_wall_us = due;
    if (kind == CAPTRIM_STEP_END) {
        // The session pauses itself at the end; mirror that so the output
        // callback stops drawing ahead of a playhead that is no longer moving.
        p->playing = 0;
    }
    if (kind != CAPTRIM_STEP_PRESENT) {
        out_report->source_us = on_screen.source_us;
        out_report->layer_count = on_screen.layer_count;
        out_report->layer_overflow = on_screen.layer_overflow;
        // A frame can arrive even when the clock did not ask for one: paused on
        // a still frame, the decoder still finishes the pipeline it was fed.
        // Those frames have to be published, otherwise the surface keeps the
        // picture from before the last seek and the viewer sees a stale freeze
        // frame. ComposePlan() no-ops when there is nothing queued, so this
        // costs a flag check per idle tick.
        if (ComposePlan(p, nullptr, 0) > 0) {
            out_report->drawn = 1;
        }
        Publish(p, out_report);
        return 0;
    }

    out_report->layer_count = desc.layer_count;
    out_report->layer_overflow = desc.layer_overflow;

    // Every video layer gets -- or keeps -- its own decoder. The plan is already
    // bottom-to-top (the core sorts by z), so collecting it in order is also the
    // compositor's draw order.
    //
    // `was_in_plan` is the previous present's membership: a layer that was off
    // screen and is back needs one seek, and that transition is the only thing
    // that can distinguish it from a slow decoder.
    int32_t was_in_plan[kMaxSources];
    for (int32_t i = 0; i < kMaxSources; i++) {
        was_in_plan[i] = p->slots[i].in_plan;
        p->slots[i].in_plan = 0;
    }

    const captrim_layer_desc_t *bottom = nullptr;
    SourceSlot *used[CAPTRIM_MAX_LAYERS];
    int64_t used_us[CAPTRIM_MAX_LAYERS];
    int32_t used_count = 0;
    int32_t open_errors = 0;

    for (int32_t i = 0; i < desc.layer_count && used_count < CAPTRIM_MAX_LAYERS; i++) {
        const captrim_layer_desc_t *layer = &desc.layers[i];
        if (layer->kind != CAPTRIM_LAYER_VIDEO) {
            // Subtitles and overlays need a text or image engine, not a decoder.
            continue;
        }
        if (bottom == nullptr) {
            bottom = layer;
        }

        SourceSlot *slot = SlotForClip(p, layer->clip_id);
        if (slot != nullptr && slot->source_id != layer->source_id) {
            // An edit replaced the clip's media. The decoder is pointing at a
            // file this layer no longer refers to.
            TeardownSlot(slot);
            slot = nullptr;
        }
        int32_t just_opened = 0;
        if (slot == nullptr) {
            slot = AllocSlot(p);
            // Open straight at the playhead: a clip that starts 4 s into the
            // file should not decode the 4 s before it.
            int32_t open_rc = OpenSlot(p, slot, layer->clip_id, layer->source_id, layer->source_us);
            if (open_rc != 0) {
                // Usually "the SoC has no decoder instance left", which is worth
                // reporting and not worth killing the loop over: the layers that
                // did open still draw. A frame where nothing opened is fatal,
                // and is handled below.
                p->slot_failures++;
                open_errors++;
                CAPTRIM_LOGE("layer %llu (source %llu) has no decoder: rc=%{public}d",
                             (unsigned long long)layer->clip_id,
                             (unsigned long long)layer->source_id, open_rc);
                continue;
            }
            just_opened = 1;
        }

        int32_t index = static_cast<int32_t>(slot - p->slots);
        if (just_opened == 0 && was_in_plan[index] == 0) {
            slot->resume = 1;
        }
        slot->in_plan = 1;
        slot->last_plan_us = desc.t_us;
        used[used_count] = slot;
        used_us[used_count] = layer->source_us;
        used_count++;
    }

    if (bottom != nullptr) {
        out_report->source_us = bottom->source_us;
    }
    if (used_count == 0) {
        if (open_errors > 0) {
            // Something was planned and none of it could be decoded. Structural:
            // record it and let the loop stop instead of retrying at 60 Hz.
            Publish(p, out_report);
            return -6;
        }
        Publish(p, out_report);
        return 0;
    }

    // Latch and draw before waiting for the exact frame.
    //
    // The decoder writes into a NativeImage, whose buffer queue holds only a
    // couple of frames. Waiting first means the queue stays full while the loop
    // waits, the decoder stalls behind it, and the frame it is waiting for is
    // the one that cannot be produced -- a mutual stall that showed up as 12 fps
    // and 37 frames on screen for an 8 s clip. Draining the queues first keeps
    // every decoder moving; the waits below then usually find their frame
    // already there.
    int32_t composed = ComposePlan(p, &desc, 1);
    if (composed < 0) {
        // The EGL surface is gone (the XComponent detached). Nothing can be
        // drawn until it comes back, so say so instead of spinning quietly.
        Publish(p, out_report);
        return -7;
    }
    if (composed > 0) {
        out_report->drawn = 1;
    }

    // Ask each decoder for its layer's instant. Paused, this waits and lands
    // exactly; playing, it only moves each target and the loop drains next time.
    int32_t arrived = 0;
    for (int32_t i = 0; i < used_count; i++) {
        if (RequestSlotFrame(used[i], used_us[i]) == 0) {
            arrived = 1;
        }
    }
    if (arrived != 0) {
        // A frame landed during a wait, and for a paused preview that frame is
        // the one the playhead asked for. Draw it now: leaving it for the next
        // tick would show the previous picture one frame longer than the edit
        // that produced it.
        if (ComposePlan(p, &desc, 0) > 0) {
            out_report->drawn = 1;
        }
    }

    Publish(p, out_report);
    // A missed deadline is not fatal -- the loop just tries the next frame.
    // Only structural failures (no track, no decoder, unreadable file) propagate
    // as a non-zero return, and those are reported above.
    return 0;
}

} // namespace captrim
