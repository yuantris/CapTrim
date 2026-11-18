// GLES compositor for the HarmonyOS presenter.
//
// The core decides *which* layers belong on screen and with what transform; this
// is the part that actually draws them. It owns an EGL context bound to the
// XComponent's native window and a single program that blits one external
// texture per layer.
//
// Why not feed the decoder straight to the XComponent, which is simpler and
// already worked? Because that path can only ever show one full-frame image: no
// transform, no opacity, no second layer. The moment the plan has a picture-in
// -picture clip or an overlay, the surface has to be composed rather than
// written to. The decoder therefore renders into an OH_NativeImage (the
// equivalent of Android's SurfaceTexture), and this class turns that texture
// into a quad wherever the plan says it goes.
//
// Everything here is deliberately C++-without-the-runtime: the HAR is built
// with -nostdlib++ so libc++_shared.so stays out of the HAP (see CMakeLists).

#ifndef CAPTRIM_GL_COMPOSITOR_H
#define CAPTRIM_GL_COMPOSITOR_H

#include <stdint.h>

#include <native_image/native_image.h>
#include <native_window/external_window.h>

namespace captrim {

struct GlCompositor;

/// A decoded-image source: one external texture plus the OH_NativeImage that
/// fills it.
///
/// This is HarmonyOS's SurfaceTexture. The decoder renders into
/// `CompositorSourceWindow()` and never touches user memory; the render thread
/// calls `CompositorSourceAcquire()` to promote the queued buffer to texture
/// contents, then draws that texture like any other layer.
///
/// The presenter used to give the decoder the XComponent window directly. That
/// works, and it is cheaper, but it can only ever put one full-frame image on
/// screen -- the whole reason this indirection exists.
struct CompSource;

/// One layer to draw.
///
/// The fields mirror `captrim_layer_desc_t` in meaning, but this struct is not
/// that struct: keeping the C ABI type out of the renderer lets the export path
/// reuse the compositor without going through the FFI.
///
/// Geometry, and the contract the Rust `Transform` follows:
///   * `scale` multiplies the layer's *fitted* size. A layer is first scaled to
///     fit the canvas (`min(canvas_w/src_w, canvas_h/src_h)`), then multiplied.
///     scale = 1 means "as large as it goes without cropping", not "native
///     pixels"; scale = 2 means twice that.
///   * `dx` / `dy` translate in canvas-normalised units: 1.0 is one full canvas
///     width / height, and +dy moves down (screen convention, not NDC).
///   * `rotation_deg` rotates around the layer's centre, clockwise on screen.
///   * `opacity` multiplies the layer's alpha, 0..1.
///   * `effects` is the CAPTRIM_EFFECT_* bit mask from the frame plan; the
///     compositor applies the set bits in the canonical order shared with the
///     FFmpeg export chain.
struct CompLayer {
    /// GL_TEXTURE_EXTERNAL_OES owned by the caller, from CompositorNewTexture.
    uint32_t texture;
    /// 4x4 column-major matrix from OH_NativeImage_GetTransformMatrix, or null
    /// for an identity transform.
    const float *tex_matrix;
    int32_t src_w;
    int32_t src_h;
    float scale;
    float dx;
    float dy;
    float rotation_deg;
    float opacity;
    uint32_t effects;
};

/// Creates the EGL context and binds it to `window`.
///
/// Must be called on the thread that will draw, and that thread must stay the
/// same for the lifetime of the returned object: an EGL context is current per
/// thread, and OH_NativeImage resolves its texture against whatever context is
/// current when it is created.
///
/// Returns null when EGL cannot be set up; the caller should fall back or fail
/// rather than draw into nothing.
GlCompositor *CompositorCreate(OHNativeWindow *window, int32_t width, int32_t height);

/// Releases the EGL context. Safe on a null pointer.
void CompositorDestroy(GlCompositor *c);

/// Binds the context to the calling thread. Needed before any GL call,
/// including creating a texture for a new OH_NativeImage.
int32_t CompositorMakeCurrent(GlCompositor *c);

/// The surface is sized in physical pixels; ArkUI reports them via
/// onSurfaceChanged. Returns non-zero on failure (the old size stays in use).
int32_t CompositorResize(GlCompositor *c, int32_t width, int32_t height);

int32_t CompositorWidth(GlCompositor *c);
int32_t CompositorHeight(GlCompositor *c);

/// Allocates an external texture and leaves it bound to GL_TEXTURE_EXTERNAL_OES
/// on the current unit. Pair with CompositorDeleteTexture.
uint32_t CompositorNewTexture(GlCompositor *c);
void CompositorDeleteTexture(GlCompositor *c, uint32_t texture);

/// Creates a texture and the NativeImage that fills it.
///
/// Must run on the render thread: OH_NativeImage resolves its texture against
/// the EGL context that is current when it is created, and this SDK version
/// offers no way to re-attach to a different one afterwards. Returns null when
/// either step fails, in which case the caller cannot decode into an image.
CompSource *CompositorNewSource(GlCompositor *c);

/// The surface to hand to OH_VideoDecoder_SetSurface.
OHNativeWindow *CompositorSourceWindow(CompSource *s);

/// Promotes the newest queued buffer to texture contents and refreshes the
/// transform matrix.
///
/// Unlike Android's updateTexImage, this one does not wait: it returns
/// NATIVE_ERROR_NO_BUFFER (40601000) straight away when the queue is empty, so
/// callers can poll it every frame instead of trying to track frame-available
/// notifications. That matters because the notification fires once per queue
/// flush while the decoder recycles several buffers in between.
///
/// On failure the texture keeps its previous contents, so a missed acquire is a
/// stale frame rather than a black one.
int32_t CompositorSourceAcquire(CompSource *s);

/// Texture name to put in a CompLayer, and the matrix for its `tex_matrix`.
/// Both are stable for the life of the source.
uint32_t CompositorSourceTexture(CompSource *s);
const float *CompositorSourceMatrix(CompSource *s);

void CompositorDestroySource(CompSource *s);

/// Clears to black, draws `layers` bottom-to-top, and presents.
///
/// Layers are drawn in array order, so the caller sorts by `z` first. A layer
/// with a zero texture or a non-positive size is skipped rather than treated as
/// an error: a plan may name a source the device cannot decode, and one missing
/// overlay must not blank the frame.
int32_t CompositorDraw(GlCompositor *c, const CompLayer *layers, int32_t count);

} // namespace captrim

#endif // CAPTRIM_GL_COMPOSITOR_H
