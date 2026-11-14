// HarmonyOS presenter: turns a CapTrim frame plan into pixels on an
// XComponent surface.
//
// Division of labour (the same one Android follows):
//   Rust owns *when* and *what*       -> captrim_preview_step_frame
//   this file owns *how* it is drawn  -> AVCodec hardware decode into an
//                                        OH_NativeImage, composited by GL
//
// Deliberately free of the C++ standard library: no std::string, no new/delete,
// no exceptions. The HAR must not drag libc++_shared.so into the host app, so
// everything here is malloc + POD structs + C APIs. See napi_bridge.cpp for the
// same rule on the JS side.
#ifndef CAPTRIM_PRESENTER_H
#define CAPTRIM_PRESENTER_H

#include <stdint.h>

#include "captrim.h"

namespace captrim {

/// Snapshot of what the render loop is doing. Filled by PresenterStatus() and
/// by a single PresenterPresent() step, so the page can show real numbers
/// instead of "it looks like it works".
struct PresentReport {
    /// captrim_step_kind_t of the latest step.
    int32_t kind;
    /// wait_frame only: wall time the loop sleeps until next.
    int64_t due_wall_us;
    /// Source instant that reached the surface, -1 when nothing was drawn.
    int64_t source_us;
    /// How many layers the plan had, and how many were dropped by
    /// CAPTRIM_MAX_LAYERS (never silently).
    int32_t layer_count;
    int32_t layer_overflow;
    /// 1 when this step put something on the window -- either a newly decoded
    /// frame or a redraw of the current one after an edit. 0 when the display
    /// already showed the right picture.
    int32_t drawn;

    /// 1 while the render loop thread is alive.
    int32_t running;
    /// Sticky loop exit code: 0 while healthy, otherwise the error that stopped
    /// it. The loop never dies silently.
    int32_t error;
    /// Frames composited onto the window since the presenter was created.
    uint64_t frames;
    /// Frames the Rust scheduler asked to drop (device cannot keep up).
    uint64_t dropped;
    /// Playhead in timeline time.
    int64_t position_us;
};

struct Presenter;

/// `surface_id` comes from ArkTS XComponent's onSurfaceCreated. Returns nullptr
/// when the window cannot be created from it (usually: the surface is not ready
/// yet, or the id belongs to a destroyed surface).
///
/// `width` / `height` are the surface's size in physical pixels, as ArkUI reports
/// it. They are the canvas that every layer's transform is relative to, so they
/// cannot be left to a guess; a zero is treated as 1 and every layer then
/// degenerates, which is visible rather than silent.
Presenter *PresenterCreate(captrim_engine_t *engine, uint64_t surface_id, int32_t width,
                           int32_t height);
void PresenterDestroy(Presenter *presenter);

/// Resize the compositing canvas, e.g. on rotation or a split-screen relayout.
///
/// Thread-safe: the size is handed to the render loop, because the EGL surface
/// belongs to that thread. Layer geometry follows immediately; the window is
/// cleared, so the next composed frame is what the viewer sees.
int32_t PresenterResize(Presenter *presenter, int32_t width, int32_t height);

/// Start the render loop on its own thread.
///
/// The loop drives the preview session from then on: `preview` becomes owned by
/// the loop thread and the caller must not touch it until
/// PresenterLoopStop() has returned. Transport changes go through
/// PresenterControl(), which is the only thread-safe entry point.
///
/// `now_wall_us` is the caller's clock at the moment of the call; the loop
/// advances it from CLOCK_MONOTONIC so the timeline keeps moving even when
/// ArkUI's frame callback is late.
///
/// Returns non-zero when the loop is already running or the thread cannot be
/// created.
int32_t PresenterLoopStart(Presenter *presenter, captrim_preview_t *preview,
                           int64_t now_wall_us);

/// Ask the loop to exit and wait for it. Safe to call when not running.
int32_t PresenterLoopStop(Presenter *presenter);

/// Queue a transport command for the loop thread.
///
/// `playing` < 0 leaves the play state alone, 0 pauses, > 0 plays.
/// `has_seek` != 0 applies `seek_us` before the play state.
///
/// Non-blocking and safe from the UI thread. The command is applied by the loop
/// so the session keeps a single owner and needs no lock of its own.
int32_t PresenterControl(Presenter *presenter, int32_t playing, int32_t has_seek,
                         int64_t seek_us);

/// Copy the latest snapshot. Non-blocking, safe from the UI thread.
int32_t PresenterStatus(Presenter *presenter, PresentReport *out_report);

/// Advance the preview once, synchronously, and draw the frame if one is due.
///
/// Blocking: returns once the frame at the planned source instant has been
/// handed to the codec, or after a short timeout when the decoder cannot keep
/// up. Debug/tooling only -- it blocks the calling thread for up to ~120 ms, so
/// never call it from the UI thread. The page uses PresenterLoopStart().
///
/// For a plan with several video layers, each one gets its own decoder and its
/// own NativeImage, and the compositor draws them in plan order (bottom first).
/// That is what a picture-in-picture needs, and it costs two hardware decoders.
/// Layers whose decoder cannot be opened -- the usual cause is the SoC having no
/// instance left -- are skipped and counted, so the gap is visible rather than
/// silently drawn as a missing picture.
int32_t PresenterPresent(Presenter *presenter, captrim_preview_t *preview,
                         int64_t now_wall_us, PresentReport *out_report);

} // namespace captrim

#endif // CAPTRIM_PRESENTER_H
