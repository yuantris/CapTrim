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

struct PresenterState {
    OHNativeWindow *window;
    captrim_engine_t *engine;

    /// EGL context bound to `window`, owned by the render thread. Null until the
    /// loop starts: EGL contexts are per-thread, so it cannot be created here on
    /// the UI thread and used there.
    GlCompositor *compositor;

    /// Canvas size in physical pixels, as reported by the XComponent.
    int32_t surface_width;
    int32_t surface_height;

    /// Where the decoder's output goes: a texture the compositor samples, not
    /// the window. Its native window is what OH_VideoDecoder_SetSurface() gets.
    /// It also owns the frame-available flag, so there is no second clock to
    /// keep in step with the codec.
    CompSource *comp_source;

    /// Forces a redraw without a new decoded frame: the surface needs one after
    /// a resize, when its contents are gone but the picture is not.
    volatile int32_t compose_dirty;
    /// The last plan drawn, so a redraw after a resize has geometry to reuse.
    captrim_frame_desc_t last_desc;
    volatile int32_t has_last_desc;

    uint64_t bound_source;
    char uri[kUriMax];
    int32_t fd;
    OH_AVSource *source;
    OH_AVDemuxer *demuxer;
    OH_AVCodec *codec;
    uint32_t track;
    int32_t width;
    int32_t height;

    // Shared with the codec's callback threads, hence volatile.
    volatile int64_t target_us;
    volatile int64_t fed_pts_us;
    volatile int32_t reached;
    volatile int32_t eos;
    volatile int32_t failed;
    volatile int32_t abort;
    /// Set while the codec is being restarted. The callbacks must not touch the
    /// codec or the demuxer meanwhile: Stop() reclaims every buffer it handed
    /// out, and feeding one back is an AV_ERR_INVALID_STATE at best.
    volatile int32_t seeking;

    /// Mirror of the transport state, so the output callback knows whether the
    /// playhead is moving. While it is not, only the frame at the playhead is
    /// allowed on screen -- the decoder hands out its pipeline worth of frames
    /// on every seek, and drawing all of them would park a paused preview ten
    /// frames past the scrub position.
    volatile int32_t playing;

    int64_t last_rendered_us;
    /// Frames the decoder produced and handed to the NativeImage. This is the
    /// decode frontier, not what the viewer saw: the loop still has to composite
    /// them.
    uint64_t frames_decoded;
    /// Times a frame was actually drawn onto the window. A paused preview
    /// composites once per new target, so this lags frames_decoded by design.
    uint64_t frames_composited;
    /// Frames decoded but deliberately not shown while paused, to land on the
    /// playhead. Not a performance figure; kept apart from the drop count so an
    /// exact scrub does not look like a device that cannot keep up.
    uint64_t frames_skipped;
    uint32_t input_calls;
    uint32_t output_calls;
    uint32_t wait_timeouts;
    uint32_t present_calls;
    uint32_t acquire_ok;
    uint32_t acquire_empty;

    // Two locks, deliberately:
    //   `lock` guards loop state and the condition. It is never held across a
    //   codec call -- OH_VideoDecoder_Flush() waits for the callback threads to
    //   finish, and those callbacks take `lock` to signal, so holding it across
    //   a flush deadlocks the render loop on the first seek (observed: the loop
    //   went silent forever right after "Demuxer seek").
    //   `demux_lock` guards the demuxer, which the input callback reads on one
    //   thread while a seek repositions it on another.
    pthread_mutex_t lock;
    pthread_mutex_t demux_lock;
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
    PresenterState *p = static_cast<PresenterState *>(user_data);
    if (p == nullptr) {
        return;
    }
    p->failed = 1;
    CAPTRIM_LOGE("decoder error %{public}d", error_code);
    Signal(p);
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
    PresenterState *p = static_cast<PresenterState *>(user_data);
    if (p == nullptr || p->abort) {
        return;
    }

    // Backpressure. The playhead is the only clock that matters, so hold the
    // input buffer until the decoder has caught up with it.
    while (!p->abort && p->fed_pts_us > p->target_us + kFeedAheadUs) {
        usleep(2000);
    }
    if (p->abort || p->demuxer == nullptr || p->seeking != 0) {
        // During a codec restart the codec owns every buffer again; it
        // re-requests the ones it still needs, so dropping this call is
        // harmless.
        return;
    }

    OH_AVCodecBufferAttr attr;
    memset(&attr, 0, sizeof(attr));

    pthread_mutex_lock(&p->demux_lock);
    if (p->demuxer == nullptr) {
        pthread_mutex_unlock(&p->demux_lock);
        return;
    }
    OH_AVErrCode rc = OH_AVDemuxer_ReadSampleBuffer(p->demuxer, p->track, buffer);
    pthread_mutex_unlock(&p->demux_lock);

    if (rc != AV_ERR_OK) {
        CAPTRIM_LOGE("demux rc=%{public}d at fed=%{public}lld target=%{public}lld",
                     rc, (long long)p->fed_pts_us, (long long)p->target_us);
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
    p->fed_pts_us = attr.pts;
    p->input_calls++;
    OH_AVErrCode push = OH_VideoDecoder_PushInputBuffer(codec, index);
    // First few samples only: enough to prove the demuxer is feeding the codec
    // the right thing (keyframe, sane size, accepted push) without logging at
    // frame rate.
    if (p->input_calls <= 3) {
        CAPTRIM_LOGI("push #%{public}u size=%{public}d pts=%{public}lld key=%{public}d rc=%{public}d",
                     p->input_calls, attr.size, (long long)attr.pts,
                     (attr.flags & AVCODEC_BUFFER_FLAGS_SYNC_FRAME) != 0, push);
    }
}

void OnNewOutputBuffer(OH_AVCodec *codec, uint32_t index, OH_AVBuffer *buffer,
                       void *user_data) {
    PresenterState *p = static_cast<PresenterState *>(user_data);
    if (p == nullptr || p->abort) {
        CAPTRIM_LOGE("output cb ignored (abort=%{public}d)", p != nullptr ? p->abort : -1);
        return;
    }

    OH_AVCodecBufferAttr attr;
    memset(&attr, 0, sizeof(attr));
    if (OH_AVBuffer_GetBufferAttr(buffer, &attr) != AV_ERR_OK) {
        CAPTRIM_LOGE("output cb: cannot read buffer attr");
        OH_VideoDecoder_FreeOutputBuffer(codec, index);
        return;
    }

    if (p->seeking != 0) {
        // The flush is recycling this buffer; rendering it would race the seek.
        OH_VideoDecoder_FreeOutputBuffer(codec, index);
        return;
    }

    p->output_calls++;
    if (p->output_calls <= 3) {
        CAPTRIM_LOGI("output #%{public}u pts=%{public}lld flags=%{public}u",
                     p->output_calls, (long long)attr.pts, attr.flags);
    }

    if ((attr.flags & AVCODEC_BUFFER_FLAGS_EOS) != 0) {
        p->eos = 1;
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
    if (p->playing == 0 && (attr.pts + 5000 < p->target_us || attr.pts > p->target_us + 1000)) {
        // Paused or scrubbing: draw exactly the frame at the playhead. The
        // decoder emits its whole pipeline (about ten frames) after a seek, so
        // anything else would leave the freeze frame off the scrub position.
        //
        // Counted separately from `frames_dropped`: these frames are not lost to
        // slowness, they are deliberately skipped to land precisely. Rolling
        // them into the same counter made the status line read "丢帧 217" on a
        // device that never missed a deadline.
        p->frames_skipped++;
        OH_VideoDecoder_FreeOutputBuffer(codec, index);
        return;
    }

    // FreeOutputBuffer is not "discard" in surface mode: it hands the buffer to
    // the surface the decoder was configured with, which is now the compositor's
    // NativeImage rather than the XComponent. Its frame-available callback fires
    // during that call and the render thread picks the frame up from there.
    // RenderOutputBuffer, not FreeOutputBuffer: the latter only returns the
    // buffer to the codec, while the former also hands it to the surface the
    // decoder was configured with. That surface is now the compositor's
    // NativeImage, so this call is what queues the frame for the render thread
    // to latch -- using FreeOutputBuffer here decodes every frame and shows
    // none (observed: three outputs, zero buffers in the image).
    int32_t covers = attr.pts + kFrameToleranceUs >= p->target_us ? 1 : 0;
    OH_AVErrCode render_rc = OH_VideoDecoder_RenderOutputBuffer(codec, index);
    if (render_rc == AV_ERR_OK) {
        p->last_rendered_us = attr.pts;
        p->frames_decoded++;
        // The wait is satisfied only by a frame that actually covers the target;
        // older frames are still drawn -- they are the best picture available at
        // this instant -- so the surface ends on the newest frame at or before
        // the playhead instead of jumping straight to a future one.
        if (covers != 0) {
            p->reached = 1;
            Signal(p);
        }
    } else if (p->output_calls <= 5) {
        CAPTRIM_LOGE("RenderOutputBuffer #%{public}u failed (%{public}d) pts=%{public}lld",
                     p->output_calls, (int32_t)render_rc, (long long)attr.pts);
    }
}

void TeardownSource(PresenterState *p) {
    p->abort = 1;
    if (p->codec != nullptr) {
        // Stop before the image goes away: the codec still holds buffers that
        // belong to the NativeImage's queue, and dropping the queue first is a
        // use-after-free inside the decoder.
        OH_VideoDecoder_Stop(p->codec);
        OH_VideoDecoder_Destroy(p->codec);
        p->codec = nullptr;
    }
    if (p->comp_source != nullptr) {
        CompositorDestroySource(p->comp_source);
        p->comp_source = nullptr;
    }
    if (p->demuxer != nullptr) {
        OH_AVDemuxer_Destroy(p->demuxer);
        p->demuxer = nullptr;
    }
    if (p->source != nullptr) {
        OH_AVSource_Destroy(p->source);
        p->source = nullptr;
    }
    if (p->fd >= 0) {
        close(p->fd);
        p->fd = -1;
    }
    p->bound_source = 0;
    p->eos = 0;
    p->reached = 0;
    p->failed = 0;
    p->fed_pts_us = 0;
    p->abort = 0;
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

/// Open `source_id`, positioned at `target_us`, and leave the decoder started.
///
/// The playhead is applied *before* Start(): Start() immediately issues the
/// first input callbacks, and those feed whatever the demuxer is sitting on. An
/// earlier version started at 0 and let RenderAt() seek afterwards, which meant
/// every source switch decoded and threw away everything from 0 to the playhead
/// -- 0.66 s of dead time and a burst of dropped frames at each cut.
int32_t OpenSource(PresenterState *p, uint64_t source_id, int64_t target_us) {
    TeardownSource(p);
    if (p->window == nullptr) {
        return -1;
    }

    int32_t needed = 0;
    int32_t rc = captrim_engine_source_uri(p->engine, source_id, p->uri, kUriMax, &needed);
    if (rc != 0) {
        CAPTRIM_LOGE("source %llu has no uri (needed %d bytes)",
                     (unsigned long long)source_id, needed);
        return rc;
    }

    const char *path = p->uri;
    if (strncmp(path, "file://", 7) == 0) {
        path += 7;
    }
    p->fd = open(path, O_RDONLY);
    if (p->fd < 0) {
        CAPTRIM_LOGE("cannot open %{public}s (errno %{public}d)", path, errno);
        return -5;
    }
    off_t size = lseek(p->fd, 0, SEEK_END);
    CAPTRIM_LOGI("opening %{public}s size=%{public}lld", path, (long long)size);
    if (size <= 0 || lseek(p->fd, 0, SEEK_SET) != 0) {
        CAPTRIM_LOGE("cannot size %{public}s", path);
        TeardownSource(p);
        return -5;
    }

    p->source = OH_AVSource_CreateWithFD(p->fd, 0, size);
    if (p->source == nullptr) {
        CAPTRIM_LOGE("OH_AVSource_CreateWithFD failed for %{public}s", path);
        TeardownSource(p);
        return -5;
    }
    p->demuxer = OH_AVDemuxer_CreateWithSource(p->source);
    if (p->demuxer == nullptr) {
        CAPTRIM_LOGE("OH_AVDemuxer_CreateWithSource failed");
        TeardownSource(p);
        return -5;
    }

    // Find the first video track. A media file with no video is not an error
    // for the editor (audio-only clips exist), it just has nothing to draw.
    OH_AVFormat *source_format = OH_AVSource_GetSourceFormat(p->source);
    int32_t track_count = 0;
    if (source_format == nullptr ||
        !OH_AVFormat_GetIntValue(source_format, kKeyTrackCount, &track_count)) {
        CAPTRIM_LOGE("cannot read track count");
        TeardownSource(p);
        return -5;
    }

    // Select by MIME ("video/...") rather than by OH_MD_KEY_TRACK_TYPE: the
    // track-type string is not stable across containers, while every decoder
    // path is keyed off the MIME anyway.
    CAPTRIM_LOGI("track_count=%{public}d", track_count);
    OH_AVFormat *track_format = nullptr;
    for (int32_t i = 0; i < track_count; i++) {
        OH_AVFormat *candidate = OH_AVSource_GetTrackFormat(p->source, static_cast<uint32_t>(i));
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
            p->track = static_cast<uint32_t>(i);
            track_format = candidate;
            break;
        }
        OH_AVFormat_Destroy(candidate);
    }
    if (track_format == nullptr) {
        CAPTRIM_LOGE("no video track in %{public}s", path);
        TeardownSource(p);
        return -2;
    }

    // Select before Start: Start kicks off the input callback immediately, and a
    // read on a demuxer with no selected track fails with
    // AV_ERR_OPERATE_NOT_PERMIT (2).
    if (OH_AVDemuxer_SelectTrackByID(p->demuxer, p->track) != AV_ERR_OK) {
        CAPTRIM_LOGE("SelectTrackByID failed");
        TeardownSource(p);
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
        TeardownSource(p);
        return -6;
    }
    p->width = width;
    p->height = height;

    p->codec = OH_VideoDecoder_CreateByMime(mime);
    if (p->codec == nullptr) {
        CAPTRIM_LOGE("no decoder for %{public}s (hardware decoder missing?)", mime);
        OH_AVFormat_Destroy(track_format);
        TeardownSource(p);
        return -6;
    }

    // Configure first: this decoder answers AV_ERR_INVALID_STATE for SetSurface
    // while the instance is still created-but-unconfigured.
    OH_AVFormat *configure = OH_AVFormat_Create();
    if (configure == nullptr) {
        OH_AVFormat_Destroy(track_format);
        TeardownSource(p);
        return -9;
    }
    OH_AVFormat_SetIntValue(configure, kKeyWidth, width);
    OH_AVFormat_SetIntValue(configure, kKeyHeight, height);
    OH_AVFormat_SetIntValue(configure, kKeyPixelFormat, AV_PIXEL_FORMAT_SURFACE_FORMAT);
    OH_AVErrCode code = OH_VideoDecoder_Configure(p->codec, configure);
    OH_AVFormat_Destroy(configure);
    if (code != AV_ERR_OK) {
        CAPTRIM_LOGE("Configure failed (%{public}d)", code);
        OH_AVFormat_Destroy(track_format);
        TeardownSource(p);
        return -6;
    }

    // Surface output mode, but the surface is the compositor's NativeImage
    // rather than the XComponent: the pixels still never pass through user
    // space, and they end up in a texture that can be scaled, moved and stacked.
    if (p->comp_source == nullptr) {
        p->comp_source = CompositorNewSource(p->compositor);
        if (p->comp_source == nullptr) {
            CAPTRIM_LOGE("cannot create an image source");
            OH_AVFormat_Destroy(track_format);
            TeardownSource(p);
            return -6;
        }
    }
    OH_AVErrCode surface_code =
        OH_VideoDecoder_SetSurface(p->codec, CompositorSourceWindow(p->comp_source));
    if (surface_code != AV_ERR_OK) {
        CAPTRIM_LOGE("SetSurface failed (%{public}d)", surface_code);
        OH_AVFormat_Destroy(track_format);
        TeardownSource(p);
        return -6;
    }

    OH_AVCodecCallback callback;
    callback.onError = OnError;
    callback.onStreamChanged = OnStreamChanged;
    callback.onNeedInputBuffer = OnNeedInputBuffer;
    callback.onNewOutputBuffer = OnNewOutputBuffer;
    OH_AVErrCode callback_code = OH_VideoDecoder_RegisterCallback(p->codec, callback, p);
    if (callback_code != AV_ERR_OK) {
        CAPTRIM_LOGE("RegisterCallback failed (%{public}d)", callback_code);
        OH_AVFormat_Destroy(track_format);
        TeardownSource(p);
        return -6;
    }
    OH_AVFormat_Destroy(track_format);

    if (OH_VideoDecoder_Prepare(p->codec) != AV_ERR_OK) {
        CAPTRIM_LOGE("Prepare failed");
        TeardownSource(p);
        return -6;
    }

    if (OH_VideoDecoder_Start(p->codec) != AV_ERR_OK) {
        CAPTRIM_LOGE("Start failed");
        TeardownSource(p);
        return -6;
    }

    p->target_us = target_us;
    p->fed_pts_us = target_us - 1;
    p->last_rendered_us = -1;
    if (target_us > 0 &&
        OH_AVDemuxer_SeekToTime(p->demuxer, target_us / 1000, SEEK_MODE_PREVIOUS_SYNC) !=
            AV_ERR_OK) {
        CAPTRIM_LOGE("initial seek to %{public}lld failed", (long long)target_us);
    }

    p->bound_source = source_id;
    CAPTRIM_LOGI("opened %{public}s track %{public}u %{public}dx%{public}d %{public}s at %{public}lldms",
                 path, p->track, width, height, mime, (long long)(target_us / 1000));
    return 0;
}

void SeekTo(PresenterState *p, int64_t target_us) {
    // Reset state first, then release every lock before touching the codec:
    // Stop() waits for the callback threads, and those callbacks take `lock` to
    // signal, so holding anything here deadlocks the render loop.
    pthread_mutex_lock(&p->lock);
    p->fed_pts_us = target_us - 1;
    p->eos = 0;
    p->reached = 0;
    p->failed = 0;
    p->seeking = 1;
    pthread_mutex_unlock(&p->lock);

    pthread_mutex_lock(&p->demux_lock);
    if (p->demuxer != nullptr) {
        // The demuxer seeks in milliseconds and lands on the previous sync
        // sample; the decoder then walks forward to the exact frame.
        OH_AVDemuxer_SeekToTime(p->demuxer, target_us / 1000, SEEK_MODE_PREVIOUS_SYNC);
    }
    pthread_mutex_unlock(&p->demux_lock);

    // Stop/Start rather than Flush: after a Flush this decoder never asked for
    // input again, so the surface stayed on the old frame while RenderAt() ran
    // out its wait budget (observed: a seek to 4 s left the picture at 8 s and
    // the drawn counter frozen). Stop/Start is the documented lifecycle and
    // Start reliably re-issues the input callbacks.
    if (p->codec != nullptr) {
        OH_VideoDecoder_Stop(p->codec);
        if (OH_VideoDecoder_Start(p->codec) != AV_ERR_OK) {
            CAPTRIM_LOGE("decoder restart after seek failed");
        }
    }
    p->seeking = 0;
    Signal(p);
}

/// Ask the decoder for `target_us` and wait until that frame is on the surface.
int32_t RenderAt(PresenterState *p, int64_t target_us) {
    pthread_mutex_lock(&p->lock);
    bool need_seek = p->eos != 0 || target_us < p->last_rendered_us - kSeekBackUs ||
                     target_us > p->fed_pts_us + kSeekAheadUs;
    p->reached = 0;
    p->failed = 0;
    p->target_us = target_us;
    pthread_mutex_unlock(&p->lock);

    if (need_seek) {
        SeekTo(p, target_us);
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
    while (p->reached == 0 && p->failed == 0 && p->eos == 0 && p->abort == 0 &&
           p->stop_requested == 0) {
        if (pthread_cond_timedwait(&p->cond, &p->lock, &deadline) == ETIMEDOUT) {
            break;
        }
    }
    if (p->reached != 0) {
        result = 0;
    } else if (p->stop_requested == 0 && p->abort == 0) {
        // A missed deadline is normal while seeking or when the decoder is
        // slow; only a run of them means something is wrong, so log sparsely.
        // Skip-free logging: this counter is cumulative, and a run of misses is
        // exactly what a starved decoder looks like from the outside.
        if (++p->wait_timeouts % 30 == 1) {
            CAPTRIM_LOGE("no frame (miss #%{public}u): target=%{public}lld "
                         "fed=%{public}lld last=%{public}lld eos=%{public}d failed=%{public}d",
                         p->wait_timeouts, (long long)target_us, (long long)p->fed_pts_us,
                         (long long)p->last_rendered_us, p->eos, p->failed);
        }
    }
    pthread_mutex_unlock(&p->lock);
    return result;
}

/// Ask for `target_us`, waiting only when the transport is paused.
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
int32_t RequestFrame(PresenterState *p, int64_t target_us) {
    if (p->playing == 0) {
        return RenderAt(p, target_us);
    }
    // Playing: only restart the pipeline at end of stream.
    //
    // An earlier version also seeked when the playhead got more than a second
    // ahead of the decode frontier. That is a trap: the decoder falls behind
    // because the surface queue is full because the loop is slow, and a seek
    // makes the loop much slower (Stop/Start plus refilling the pipeline costs
    // hundreds of milliseconds), so it falls further behind and seeks again.
    // Measured: Present averaged 87 ms with 22 calls in a 2 s window, and the
    // decoded-frame counter moved by zero. Lagging during playback is caught up
    // by dropping frames, which is what realtime means.
    if (p->eos != 0) {
        SeekTo(p, target_us);
        return 0;
    }
    pthread_mutex_lock(&p->lock);
    p->target_us = target_us;
    p->reached = 0;
    pthread_mutex_unlock(&p->lock);
    return 0;
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
    if (p->compositor == nullptr || p->comp_source == nullptr) {
        return 0;
    }

    // Always try to latch. Gating this on the frame-available callback lost most
    // of the video: the callback fires once per queue flush, while the decoder
    // runs ~300 ms ahead and recycles buffers in between, so the loop saw one
    // notification and missed the frames behind it (observed: 25 frames
    // composited for an 8 s timeline that had 323 decoded). UpdateSurfaceImage
    // returns NATIVE_ERROR_NO_BUFFER immediately when the queue is empty, so an
    // unconditional attempt costs a function call and nothing else.
    int32_t acquired = 0;
    p->present_calls++;
    if (CompositorSourceAcquire(p->comp_source) == 0) {
        acquired = 1;
        p->acquire_ok++;
    } else {
        p->acquire_empty++;
    }
    if (acquired == 0 && p->compose_dirty == 0 && force == 0) {
        // Nothing new, nothing to repair: the window already shows this frame.
        return 0;
    }

    // A resize wipes the window but not the plan, so a redraw without a fresh
    // step falls back to the last one drawn.
    const captrim_frame_desc_t *plan = desc;
    if (plan == nullptr || plan->layer_count == 0) {
        if (p->has_last_desc == 0) {
            return 0;
        }
        plan = &p->last_desc;
    }

    // One quad per video layer this presenter can actually feed. Only the bound
    // source has a decoder, so a plan with two video layers (picture-in-picture)
    // draws the one it owns and reports the rest through the layer counters --
    // a second source needs a second codec and a second demuxer, which is a
    // scheduling problem in the core rather than a drawing problem here.
    CompLayer layers[CAPTRIM_MAX_LAYERS];
    int32_t count = 0;
    for (int32_t i = 0; i < plan->layer_count && count < CAPTRIM_MAX_LAYERS; i++) {
        const captrim_layer_desc_t *layer = &plan->layers[i];
        if (layer->kind != CAPTRIM_LAYER_VIDEO || layer->source_id != p->bound_source) {
            continue;
        }
        CompLayer &out = layers[count++];
        memset(&out, 0, sizeof(out));
        out.texture = CompositorSourceTexture(p->comp_source);
        out.tex_matrix = CompositorSourceMatrix(p->comp_source);
        out.src_w = p->width;
        out.src_h = p->height;
        out.scale = layer->scale;
        out.dx = layer->dx;
        out.dy = layer->dy;
        out.rotation_deg = layer->rotation_deg;
        out.opacity = layer->opacity;
    }
    if (count == 0) {
        return 0;
    }

    if (CompositorDraw(p->compositor, layers, count) != 0) {
        return -1;
    }
    p->compose_dirty = 0;
    p->last_desc = *plan;
    p->has_last_desc = 1;
    p->frames_composited++;
    return acquired;
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
    }
    if (playing >= 0) {
        int32_t was_playing = p->playing;
        p->playing = playing;
        int32_t rc = captrim_preview_set_playing(p->loop_preview, playing, now);
        if (rc != 0) {
            CAPTRIM_LOGE("set_playing(%{public}d) failed (rc=%{public}d)", playing, rc);
        }
        if (playing == 0 && was_playing != 0) {
            // Pausing has to land on the playhead. The surface currently holds a
            // frame from up to one feed lead ahead, and the paused state never
            // asks for another one, so without this the freeze frame would sit
            // ~300 ms past where the user stopped.
            int64_t position = 0;
            if (captrim_preview_position(p->loop_preview, now, &position) == 0) {
                SeekTo(p, position);
            }
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
            unsigned long long not_shown =
                p->frames_decoded > p->acquire_ok ? p->frames_decoded - p->acquire_ok : 0;
            CAPTRIM_LOGI("loop: pos=%{public}lldms src=%{public}lldms kind=%{public}d "
                         "drawn=%{public}llu unseen=%{public}llu skipped=%{public}llu "
                         "in=%{public}u out=%{public}u calls=%{public}u ok=%{public}u "
                         "empty=%{public}u busy=%{public}lldus/%{public}lld win=%{public}lldms",
                         (long long)(report.position_us / 1000), (long long)(report.source_us / 1000),
                         report.kind, (unsigned long long)p->frames_composited, not_shown,
                         (unsigned long long)p->frames_skipped, p->input_calls, p->output_calls,
                         p->present_calls, p->acquire_ok, p->acquire_empty,
                         (long long)busy_us, (long long)busy_count,
                         (long long)((now - p->clock_base_us) / 1000));
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

    CAPTRIM_LOGI("render loop stopped (drawn %{public}llu, decoded %{public}llu, "
                 "latched %{public}u, err %{public}d)",
                 (unsigned long long)p->frames_composited,
                 (unsigned long long)p->frames_decoded, p->acquire_ok, p->loop_error);

    // The image source has to go before the context that owns its texture, and
    // the context before the thread that made it current.
    if (p->comp_source != nullptr) {
        CompositorDestroySource(p->comp_source);
        p->comp_source = nullptr;
    }
    if (p->compositor != nullptr) {
        CompositorDestroy(p->compositor);
        p->compositor = nullptr;
    }
    p->bound_source = 0;
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
    p->fd = -1;
    p->last_rendered_us = -1;
    p->cmd_playing = -1;
    // Layer geometry is relative to the canvas, so a zero size here would make
    // every layer degenerate. The page knows the XComponent's size; ask for it
    // rather than guessing from the window, which is still unconfigured.
    p->surface_width = width > 0 ? width : 1;
    p->surface_height = height > 0 ? height : 1;
    pthread_mutex_init(&p->lock, nullptr);
    pthread_mutex_init(&p->demux_lock, nullptr);
    pthread_cond_init(&p->cond, nullptr);

    if (OH_NativeWindow_CreateNativeWindowFromSurfaceId(surface_id, &p->window) != 0 ||
        p->window == nullptr) {
        CAPTRIM_LOGE("cannot get native window for surface %llu",
                     (unsigned long long)surface_id);
        pthread_cond_destroy(&p->cond);
        pthread_mutex_destroy(&p->demux_lock);
        pthread_mutex_destroy(&p->lock);
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
    // gone before any of that is torn down.
    PresenterLoopStop(opaque);
    TeardownSource(presenter);
    if (presenter->window != nullptr) {
        OH_NativeWindow_DestroyNativeWindow(presenter->window);
        presenter->window = nullptr;
    }
    pthread_cond_destroy(&presenter->cond);
    pthread_mutex_destroy(&presenter->demux_lock);
    pthread_mutex_destroy(&presenter->lock);
    free(presenter);
}

namespace {

/// Frames the decoder produced that never reached the screen.
///
/// Deliberately computed rather than accumulated: the two counters are updated
/// on different threads (the output callback and the render loop), so a running
/// tally drifts. The subtraction is exact as long as the loop is the only
/// consumer, which it is.
uint64_t FramesNotShown(const PresenterState *p) {
    return p->frames_decoded > p->acquire_ok ? p->frames_decoded - p->acquire_ok : 0;
}

/// Publish a snapshot and attach the counters the loop owns.
void Publish(PresenterState *p, PresentReport *report) {
    report->running = p->loop_running;
    report->error = p->loop_error;
    report->frames = p->frames_composited;
    report->dropped = FramesNotShown(p);
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

    // What is on screen, as opposed to what the latest step planned: an idle or
    // waiting step plans nothing, but the surface still shows the last frame, and
    // a status line that reports "-" while a picture is visible is just noise.
    PresentReport on_screen;
    pthread_mutex_lock(&p->lock);
    on_screen = p->last;
    pthread_mutex_unlock(&p->lock);

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

    // Bottom-most video layer wins. Multi-layer compositing needs the GPU pass
    // that does not exist yet; drawing an arbitrary layer would hide that gap.
    const captrim_layer_desc_t *chosen = nullptr;
    for (int32_t i = 0; i < desc.layer_count; i++) {
        const captrim_layer_desc_t *layer = &desc.layers[i];
        if (layer->kind != CAPTRIM_LAYER_VIDEO) {
            continue;
        }
        if (chosen == nullptr || layer->z < chosen->z) {
            chosen = layer;
        }
    }
    if (chosen == nullptr) {
        Publish(p, out_report);
        return 0;
    }
    out_report->source_us = chosen->source_us;

    if (chosen->source_id != p->bound_source) {
        // Open straight at the playhead: a clip that starts 4 s into the file
        // should not decode the 4 s before it.
        rc = OpenSource(p, chosen->source_id, chosen->source_us);
        if (rc != 0) {
            Publish(p, out_report);
            return rc;
        }
    }
    if (p->codec == nullptr) {
        Publish(p, out_report);
        return 0;
    }

    // Latch and draw before waiting for the exact frame.
    //
    // The decoder writes into a NativeImage, whose buffer queue holds only a
    // couple of frames. Waiting on RenderAt() first means the queue stays full
    // while the loop waits, the decoder stalls behind it, and the frame it is
    // waiting for is the one that cannot be produced -- a mutual stall that
    // showed up as 12 fps and 37 frames on screen for an 8 s clip. Draining the
    // queue first keeps the decoder moving; the wait below then usually finds
    // its frame already there.
    int32_t composed = ComposePlan(p, &desc, 1);
    if (composed < 0) {
        // The EGL surface is gone (the XComponent detached). Nothing can be
        // drawn until it comes back, so say so instead of spinning quietly.
        Publish(p, out_report);
        return -7;
    }

    RequestFrame(p, chosen->source_us);
    if (composed > 0) {
        out_report->drawn = 1;
    }    Publish(p, out_report);
    // A missed deadline is not fatal -- the loop just tries the next frame.
    // Only structural failures (no track, no decoder, unreadable file) propagate
    // as a non-zero return, and those are reported above.
    return 0;
}

} // namespace captrim
