#include "gl_compositor.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <hilog/log.h>
#include <native_window/external_window.h>

// GL_TEXTURE_EXTERNAL_OES is defined in <GLES2/gl2ext.h>, which cannot be
// included next to the GLES3 headers (it re-declares GLES2 typedefs). The value
// is fixed by the OES_EGL_image_external extension, so state it here.
#ifndef GL_TEXTURE_EXTERNAL_OES
#define GL_TEXTURE_EXTERNAL_OES 0x8D65
#endif

namespace captrim {
namespace {

constexpr unsigned int kLogDomain = 0x0001;
constexpr const char *kLogTag = "CapTrimNative";

#define CAPTRIM_LOGI(fmt, ...) \
    OH_LOG_Print(LOG_APP, LOG_INFO, kLogDomain, kLogTag, fmt, ##__VA_ARGS__)
#define CAPTRIM_LOGE(fmt, ...) \
    OH_LOG_Print(LOG_APP, LOG_ERROR, kLogDomain, kLogTag, fmt, ##__VA_ARGS__)

// The layer geometry is a rect with a rotation, so the vertex shader only needs
// a half-size, an offset and a rotation: no matrix assembly on the CPU side.
//
// aPos is the unit quad in [-0.5, 0.5]; scaling it by 2 * halfSize gives the
// layer's half extent in NDC. Rotation is applied before translation so a
// rotated layer spins around its own centre instead of the canvas centre.
constexpr const char *kVertexShader = R"(#version 300 es
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aUV;
uniform vec2 uHalfSize;
uniform vec2 uOffset;
uniform vec2 uRot;
uniform mat4 uTexMatrix;
out vec2 vUV;
void main() {
    vec2 p = aPos * 2.0 * uHalfSize;
    vec2 r = vec2(p.x * uRot.x - p.y * uRot.y,
                  p.x * uRot.y + p.y * uRot.x);
    gl_Position = vec4(r + uOffset, 0.0, 1.0);
    vUV = (uTexMatrix * vec4(aUV, 0.0, 1.0)).xy;
}
)";

// samplerExternalOES because the decoder writes into a buffer the GPU samples
// directly; there is no copy into a 2D texture anywhere in this path.
//
// The eight v1 effects mirror the FFmpeg chain in effects.rs. Bit values match
// CAPTRIM_EFFECT_* / the core's EFFECT_* constants; application order matches
// CANONICAL_ORDER: flips (UV) first, colour ops in the middle, blur last.
// FFmpeg works in YUV and this shader in RGB, so the colour maths is an
// *approximation* of the CPU side, not bit-identical — the contract is which
// effects run and in what order, not the exact pixel values.
constexpr const char *kFragmentShader = R"(#version 300 es
#extension GL_OES_EGL_image_external_essl3 : require
precision mediump float;
precision highp int;
uniform samplerExternalOES uTex;
uniform float uOpacity;
uniform uint uEffects;
uniform vec2 uTexel; // 1 / source pixel size, for the blur taps
in vec2 vUV;
out vec4 fragColor;

const uint EFFECT_HFLIP      = 1u << 0;
const uint EFFECT_VFLIP      = 1u << 1;
const uint EFFECT_BRIGHTNESS = 1u << 2;
const uint EFFECT_CONTRAST   = 1u << 3;
const uint EFFECT_SATURATION = 1u << 4;
const uint EFFECT_GRAYSCALE  = 1u << 5;
const uint EFFECT_INVERT     = 1u << 6;
const uint EFFECT_BLUR       = 1u << 7;

vec4 sampleTex(vec2 uv) {
    return texture(uTex, clamp(uv, 0.0, 1.0));
}

void main() {
    vec2 uv = vUV;
    if ((uEffects & EFFECT_HFLIP) != 0u) uv.x = 1.0 - uv.x;
    if ((uEffects & EFFECT_VFLIP) != 0u) uv.y = 1.0 - uv.y;

    vec4 c;
    if ((uEffects & EFFECT_BLUR) != 0u) {
        // 5x5 box, separable into two 1-D passes of 5 taps (boxblur=2:1).
        // tex_matrix may rotate, so blur along the sampled UV axes.
        vec4 sum = vec4(0.0);
        for (int j = -2; j <= 2; j++) {
            for (int i = -2; i <= 2; i++) {
                sum += sampleTex(uv + vec2(float(i), float(j)) * uTexel);
            }
        }
        c = sum / 25.0;
    } else {
        c = sampleTex(uv);
    }

    if ((uEffects & EFFECT_BRIGHTNESS) != 0u) c.rgb += 0.15;
    if ((uEffects & EFFECT_CONTRAST) != 0u)   c.rgb = (c.rgb - 0.5) * 1.30 + 0.5;
    if ((uEffects & EFFECT_SATURATION) != 0u) {
        float luma = dot(c.rgb, vec3(0.299, 0.587, 0.114));
        c.rgb = mix(vec3(luma), c.rgb, 1.30);
    }
    if ((uEffects & EFFECT_GRAYSCALE) != 0u) {
        c.rgb = vec3(dot(c.rgb, vec3(0.299, 0.587, 0.114)));
    }
    if ((uEffects & EFFECT_INVERT) != 0u) c.rgb = 1.0 - c.rgb;

    fragColor = vec4(c.rgb, c.a * uOpacity);
}
)";

/// Fragment shader for layers that are plain 2D textures: subtitle bitmaps
/// produced by the core's text rasteriser. Effects are not applied here -- text
/// has no v1 effects vocabulary, and the exporter overlays the identical bitmap
/// without touching it, so anything done here would break that promise.
constexpr const char *kFragmentShaderBitmap = R"(#version 300 es
precision mediump float;
uniform sampler2D uTex;
uniform float uOpacity;
in vec2 vUV;
out vec4 fragColor;
void main() {
    vec4 c = texture(uTex, vUV);
    fragColor = vec4(c.rgb, c.a * uOpacity);
}
)";


/// Unit quad, two triangles, interleaved position + texture coordinate.
constexpr float kQuad[] = {
    -0.5f, -0.5f, 0.0f, 0.0f,
    0.5f,  -0.5f, 1.0f, 0.0f,
    0.5f,  0.5f,  1.0f, 1.0f,
    -0.5f, -0.5f, 0.0f, 0.0f,
    0.5f,  0.5f,  1.0f, 1.0f,
    -0.5f, 0.5f,  0.0f, 1.0f,
};

/// Identity with the Y axis negated and shifted by one: v' = 1 - v, which
/// turns GL's "first uploaded row is v = 0" into "top row is v = 0", matching
/// every CPU rasteriser's row order (the text module writes top-first).
constexpr float kFlipV[16] = {
    1.0f, 0.0f, 0.0f, 0.0f,
    0.0f, -1.0f, 0.0f, 0.0f,
    0.0f, 0.0f, 1.0f, 0.0f,
    0.0f, 1.0f, 0.0f, 1.0f,
};

constexpr float kIdentity[16] = {
    1.0f, 0.0f, 0.0f, 0.0f,
    0.0f, 1.0f, 0.0f, 0.0f,
    0.0f, 0.0f, 1.0f, 0.0f,
    0.0f, 0.0f, 0.0f, 1.0f,
};

GLuint CompileShader(GLenum type, const char *source) {
    GLuint shader = glCreateShader(type);
    if (shader == 0) {
        return 0;
    }
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (ok == GL_FALSE) {
        char log[512];
        log[0] = '\0';
        glGetShaderInfoLog(shader, sizeof(log) - 1, nullptr, log);
        CAPTRIM_LOGE("shader compile failed (%{public}d): %{public}s", (int32_t)type, log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

GLuint LinkProgram(const char *fragment_source) {
    GLuint vs = CompileShader(GL_VERTEX_SHADER, kVertexShader);
    if (vs == 0) {
        return 0;
    }
    GLuint fs = CompileShader(GL_FRAGMENT_SHADER, fragment_source);
    if (fs == 0) {
        glDeleteShader(vs);
        return 0;
    }
    GLuint program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glLinkProgram(program);
    // The shaders are only referenced by the program from here on.
    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint ok = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (ok == GL_FALSE) {
        char log[512];
        log[0] = '\0';
        glGetProgramInfoLog(program, sizeof(log) - 1, nullptr, log);
        CAPTRIM_LOGE("program link failed: %{public}s", log);
        glDeleteProgram(program);
        return 0;
    }
    return program;
}

} // namespace

struct GlCompositor {
    EGLDisplay display;
    EGLContext context;
    EGLSurface surface;
    OHNativeWindow *window;

    /// Canvas size in physical pixels. Layer geometry is expressed relative to
    /// this, so a resize changes every layer's on-screen size with no other
    /// bookkeeping.
    int32_t width;
    int32_t height;

    GLuint program;
    /// Second program for plain 2D textures (subtitle bitmaps). It shares the
    /// vertex shader and differs only in the sampler type.
    GLuint program2d;
    GLuint vbo;
    GLint uHalfSize;
    GLint uOffset;
    GLint uRot;
    GLint uTexMatrix;
    GLint uOpacity;
    GLint uTex;
    GLint uEffects;
    GLint uTexel;

    // The 2D program references the same uniform names minus the effect ones.
    GLint p2dHalfSize;
    GLint p2dOffset;
    GLint p2dRot;
    GLint p2dTexMatrix;
    GLint p2dOpacity;
    GLint p2dTex;
};

int32_t CompositorMakeCurrent(GlCompositor *c) {
    if (c == nullptr || c->display == EGL_NO_DISPLAY) {
        return -1;
    }
    if (eglMakeCurrent(c->display, c->surface, c->surface, c->context) != EGL_TRUE) {
        CAPTRIM_LOGE("eglMakeCurrent failed (0x%{public}x)", eglGetError());
        return -1;
    }
    return 0;
}

GlCompositor *CompositorCreate(OHNativeWindow *window, int32_t width, int32_t height) {
    if (window == nullptr || width <= 0 || height <= 0) {
        return nullptr;
    }

    GlCompositor *c = static_cast<GlCompositor *>(calloc(1, sizeof(GlCompositor)));
    if (c == nullptr) {
        return nullptr;
    }
    c->window = window;
    c->width = width;
    c->height = height;
    c->display = EGL_NO_DISPLAY;
    c->context = EGL_NO_CONTEXT;
    c->surface = EGL_NO_SURFACE;

    c->display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (c->display == EGL_NO_DISPLAY) {
        CAPTRIM_LOGE("no EGL display");
        CompositorDestroy(c);
        return nullptr;
    }
    EGLint major = 0;
    EGLint minor = 0;
    if (eglInitialize(c->display, &major, &minor) != EGL_TRUE) {
        CAPTRIM_LOGE("eglInitialize failed (0x%{public}x)", eglGetError());
        CompositorDestroy(c);
        return nullptr;
    }

    const EGLint config_attribs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_NONE,
    };
    EGLConfig config = nullptr;
    EGLint config_count = 0;
    if (eglChooseConfig(c->display, config_attribs, &config, 1, &config_count) != EGL_TRUE ||
        config_count == 0) {
        CAPTRIM_LOGE("no ES3 window config (0x%{public}x)", eglGetError());
        CompositorDestroy(c);
        return nullptr;
    }

    c->surface = eglCreateWindowSurface(c->display, config,
                                        reinterpret_cast<EGLNativeWindowType>(window), nullptr);
    if (c->surface == EGL_NO_SURFACE) {
        CAPTRIM_LOGE("eglCreateWindowSurface failed (0x%{public}x)", eglGetError());
        CompositorDestroy(c);
        return nullptr;
    }

    const EGLint context_attribs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    c->context = eglCreateContext(c->display, config, EGL_NO_CONTEXT, context_attribs);
    if (c->context == EGL_NO_CONTEXT) {
        CAPTRIM_LOGE("eglCreateContext failed (0x%{public}x)", eglGetError());
        CompositorDestroy(c);
        return nullptr;
    }
    if (CompositorMakeCurrent(c) != 0) {
        CompositorDestroy(c);
        return nullptr;
    }

    // No vsync wait on swap. The render loop already paces itself against the
    // playback clock (it sleeps until the next frame is due), so waiting here as
    // well just stacks a second, coarser timer on top: with swapInterval 1 the
    // measured rate was 24 fps for a 30 fps clip. Tearing is not a concern for
    // preview -- the frame is replaced at most once per timeline frame.
    eglSwapInterval(c->display, 0);

    c->program = LinkProgram(kFragmentShader);
    if (c->program == 0) {
        CompositorDestroy(c);
        return nullptr;
    }
    c->program2d = LinkProgram(kFragmentShaderBitmap);
    if (c->program2d == 0) {
        CompositorDestroy(c);
        return nullptr;
    }
    c->uHalfSize = glGetUniformLocation(c->program, "uHalfSize");
    c->uOffset = glGetUniformLocation(c->program, "uOffset");
    c->uRot = glGetUniformLocation(c->program, "uRot");
    c->uTexMatrix = glGetUniformLocation(c->program, "uTexMatrix");
    c->uOpacity = glGetUniformLocation(c->program, "uOpacity");
    c->uTex = glGetUniformLocation(c->program, "uTex");
    c->uEffects = glGetUniformLocation(c->program, "uEffects");
    c->uTexel = glGetUniformLocation(c->program, "uTexel");

    c->p2dHalfSize = glGetUniformLocation(c->program2d, "uHalfSize");
    c->p2dOffset = glGetUniformLocation(c->program2d, "uOffset");
    c->p2dRot = glGetUniformLocation(c->program2d, "uRot");
    c->p2dTexMatrix = glGetUniformLocation(c->program2d, "uTexMatrix");
    c->p2dOpacity = glGetUniformLocation(c->program2d, "uOpacity");
    c->p2dTex = glGetUniformLocation(c->program2d, "uTex");

    glGenBuffers(1, &c->vbo);
    glBindBuffer(GL_ARRAY_BUFFER, c->vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(kQuad), kQuad, GL_STATIC_DRAW);

    glViewport(0, 0, width, height);
    glDisable(GL_DEPTH_TEST);
    // Black behind the layers: a clip that does not fill the canvas gets bars,
    // and bars are less distracting than whatever was on screen before.
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);

    CAPTRIM_LOGI("compositor ready: EGL %{public}d.%{public}d %{public}dx%{public}d",
                 (int32_t)major, (int32_t)minor, width, height);
    return c;
}

void CompositorDestroy(GlCompositor *c) {
    if (c == nullptr) {
        return;
    }
    if (c->display != EGL_NO_DISPLAY) {
        // The context has to be current to free GL objects, and it has to be
        // unbound before it is destroyed.
        eglMakeCurrent(c->display, c->surface, c->surface, c->context);
        if (c->vbo != 0) {
            glDeleteBuffers(1, &c->vbo);
        }
        if (c->program != 0) {
            glDeleteProgram(c->program);
        }
        if (c->program2d != 0) {
            glDeleteProgram(c->program2d);
        }
        eglMakeCurrent(c->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (c->context != EGL_NO_CONTEXT) {
            eglDestroyContext(c->display, c->context);
        }
        if (c->surface != EGL_NO_SURFACE) {
            eglDestroySurface(c->display, c->surface);
        }
        eglTerminate(c->display);
    }
    free(c);
}

int32_t CompositorResize(GlCompositor *c, int32_t width, int32_t height) {
    if (c == nullptr || width <= 0 || height <= 0) {
        return -1;
    }
    c->width = width;
    c->height = height;
    // The producer has to be told the new size as well: EGL keeps rendering into
    // whatever the window was configured with, and a mismatch shows up as a
    // stretched or clipped picture rather than an error.
    OH_NativeWindow_NativeWindowHandleOpt(c->window, SET_BUFFER_GEOMETRY, width, height);
    if (CompositorMakeCurrent(c) != 0) {
        return -1;
    }
    glViewport(0, 0, width, height);
    CAPTRIM_LOGI("compositor resized to %{public}dx%{public}d", width, height);
    return 0;
}

int32_t CompositorWidth(GlCompositor *c) {
    return c == nullptr ? 0 : c->width;
}

int32_t CompositorHeight(GlCompositor *c) {
    return c == nullptr ? 0 : c->height;
}

uint32_t CompositorNewTexture(GlCompositor *c) {
    if (c == nullptr || CompositorMakeCurrent(c) != 0) {
        return 0;
    }
    GLuint texture = 0;
    glGenTextures(1, &texture);
    if (texture == 0) {
        return 0;
    }
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, texture);
    // Clamp + linear: the decoder's buffers are already the right size, so the
    // only filtering that ever happens is from a scaled layer.
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return texture;
}

void CompositorDeleteTexture(GlCompositor *c, uint32_t texture) {
    if (c == nullptr || texture == 0 || CompositorMakeCurrent(c) != 0) {
        return;
    }
    GLuint name = texture;
    glDeleteTextures(1, &name);
}

uint32_t CompositorNewBitmapTexture(GlCompositor *c) {
    if (c == nullptr || CompositorMakeCurrent(c) != 0) {
        return 0;
    }
    GLuint texture = 0;
    glGenTextures(1, &texture);
    if (texture == 0) {
        return 0;
    }
    glBindTexture(GL_TEXTURE_2D, texture);
    // Clamp + linear: a caption is drawn at its own size most of the time and
    // scaled by the layer transform occasionally; linear keeps a scaled caption
    // readable instead of blocky.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return texture;
}

int32_t CompositorUploadBitmap(GlCompositor *c, uint32_t texture, int32_t width,
                               int32_t height, const uint8_t *rgba) {
    if (c == nullptr || texture == 0 || rgba == nullptr || width <= 0 || height <= 0) {
        return -1;
    }
    if (CompositorMakeCurrent(c) != 0) {
        return -1;
    }
    // RGBA8 rows are tightly packed; the default unpack alignment of 4 happens
    // to match, but state it so a future caller with odd widths is not surprised.
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, rgba);
    return 0;
}

struct CompSource {
    GlCompositor *owner;
    uint32_t texture;
    OH_NativeImage *image;
    OHNativeWindow *window;
    float matrix[16];
    /// Set by the image's frame-available callback. Not used for pacing any
    /// more -- the render loop simply polls Acquire() -- but the listener stays
    /// registered because it is the documented way to observe the image and
    /// costs nothing.
    volatile int32_t frame_available;
    OH_OnFrameAvailableListener listener;
};

void OnFrameAvailable(void *context) {
    CompSource *s = static_cast<CompSource *>(context);
    if (s != nullptr) {
        s->frame_available = 1;
    }
}

CompSource *CompositorNewSource(GlCompositor *c) {
    if (c == nullptr) {
        return nullptr;
    }
    CompSource *s = static_cast<CompSource *>(calloc(1, sizeof(CompSource)));
    if (s == nullptr) {
        return nullptr;
    }
    s->owner = c;
    memcpy(s->matrix, kIdentity, sizeof(s->matrix));

    s->texture = CompositorNewTexture(c);
    if (s->texture == 0) {
        CAPTRIM_LOGE("cannot allocate an external texture");
        CompositorDestroySource(s);
        return nullptr;
    }
    s->image = OH_NativeImage_Create(s->texture, GL_TEXTURE_EXTERNAL_OES);
    if (s->image == nullptr) {
        CAPTRIM_LOGE("OH_NativeImage_Create failed for texture %{public}u", s->texture);
        CompositorDestroySource(s);
        return nullptr;
    }
    s->window = OH_NativeImage_AcquireNativeWindow(s->image);
    if (s->window == nullptr) {
        CAPTRIM_LOGE("OH_NativeImage_AcquireNativeWindow failed");
        CompositorDestroySource(s);
        return nullptr;
    }

    s->listener.context = s;
    s->listener.onFrameAvailable = OnFrameAvailable;
    if (OH_NativeImage_SetOnFrameAvailableListener(s->image, s->listener) != 0) {
        CAPTRIM_LOGE("cannot listen for frames on the image");
        CompositorDestroySource(s);
        return nullptr;
    }

    uint64_t image_surface_id = 0;
    if (OH_NativeImage_GetSurfaceId(s->image, &image_surface_id) == 0) {
        CAPTRIM_LOGI("image source ready: texture=%{public}u surface=%{public}llu",
                     s->texture, (unsigned long long)image_surface_id);
    } else {
        CAPTRIM_LOGE("image source has no surface id");
    }
    return s;
}

OHNativeWindow *CompositorSourceWindow(CompSource *s) {
    return s == nullptr ? nullptr : s->window;
}

int32_t CompositorSourceAcquire(CompSource *s) {
    if (s == nullptr || s->image == nullptr) {
        return -1;
    }
    if (CompositorMakeCurrent(s->owner) != 0) {
        return -1;
    }
    // This is the hand-off point: it latches the queued buffer and makes its
    // contents the texture's. Until it returns, the texture holds the previous
    // frame.
    int32_t rc = OH_NativeImage_UpdateSurfaceImage(s->image);
    if (rc != 0) {
        return rc;
    }
    s->frame_available = 0;
    OH_NativeImage_GetTransformMatrix(s->image, s->matrix);
    return 0;
}

uint32_t CompositorSourceTexture(CompSource *s) {
    return s == nullptr ? 0 : s->texture;
}

const float *CompositorSourceMatrix(CompSource *s) {
    return s == nullptr ? kIdentity : s->matrix;
}

void CompositorDestroySource(CompSource *s) {
    if (s == nullptr) {
        return;
    }
    if (s->image != nullptr) {
        // Unset first: the callback closes over `s`, and it must not fire while
        // the struct is being torn down.
        OH_NativeImage_UnsetOnFrameAvailableListener(s->image);
        // Destroying the image releases the buffer queue, so the window handle
        // it handed out must not be used afterwards.
        OH_NativeImage_Destroy(&s->image);
        s->image = nullptr;
        s->window = nullptr;
    }
    if (s->texture != 0 && s->owner != nullptr) {
        CompositorDeleteTexture(s->owner, s->texture);
        s->texture = 0;
    }
    free(s);
}

int32_t CompositorDraw(GlCompositor *c, const CompLayer *layers, int32_t count) {
    if (c == nullptr) {
        return -1;
    }
    if (CompositorMakeCurrent(c) != 0) {
        return -1;
    }

    glViewport(0, 0, c->width, c->height);
    glClear(GL_COLOR_BUFFER_BIT);

    if (layers != nullptr && count > 0) {
        glBindBuffer(GL_ARRAY_BUFFER, c->vbo);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                              reinterpret_cast<const void *>(0));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                              reinterpret_cast<const void *>(2 * sizeof(float)));
        glActiveTexture(GL_TEXTURE0);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

        for (int32_t i = 0; i < count; i++) {
            const CompLayer &layer = layers[i];
            if (layer.texture == 0 || layer.src_w <= 0 || layer.src_h <= 0 ||
                layer.opacity <= 0.0f) {
                continue;
            }

            // Two programs, one vertex shader: the video path samples a
            // GL_TEXTURE_EXTERNAL_OES (decoder output) and applies the v1
            // effects; the bitmap path samples a plain GL_TEXTURE_2D (a
            // subtitle rasterised by the core) and applies nothing.
            bool bitmap = layer.external == 0;
            glUseProgram(bitmap ? c->program2d : c->program);
            glUniform1i(bitmap ? c->p2dTex : c->uTex, 0);

            // Size on screen.
            //
            // Video: fit inside the canvas without cropping, then apply the
            // layer's own scale. A portrait clip on a landscape canvas ends up
            // pillarboxed rather than stretched, which is what "scale = 1" has
            // to mean for the timeline's framing to be predictable.
            //
            // Bitmap: the pixels were rasterised for a timeline canvas, so the
            // mapping is view/timeline, not fit; a 1080p caption has to occupy
            // the same fraction of a 1260 px view as of a 1080 px one.
            float scale = layer.scale > 0.0f ? layer.scale : 1.0f;
            float fit;
            if (bitmap) {
                fit = (layer.design_w > 0 && layer.design_h > 0)
                          ? static_cast<float>(c->width) / static_cast<float>(layer.design_w)
                          : 1.0f;
            } else {
                float fit_w = static_cast<float>(c->width) / static_cast<float>(layer.src_w);
                float fit_h = static_cast<float>(c->height) / static_cast<float>(layer.src_h);
                fit = fit_w < fit_h ? fit_w : fit_h;
            }
            float w_px = static_cast<float>(layer.src_w) * fit * scale;
            float h_px = static_cast<float>(layer.src_h) * fit * scale;

            // dx/dy are canvas-normalised and +dy is down; NDC is 2 units wide
            // and its y points up, hence the scale and the sign.
            const GLint half = bitmap ? c->p2dHalfSize : c->uHalfSize;
            const GLint offset = bitmap ? c->p2dOffset : c->uOffset;
            const GLint rot = bitmap ? c->p2dRot : c->uRot;
            const GLint matrix = bitmap ? c->p2dTexMatrix : c->uTexMatrix;
            const GLint opacity = bitmap ? c->p2dOpacity : c->uOpacity;

            glUniform2f(half, w_px / static_cast<float>(c->width),
                        h_px / static_cast<float>(c->height));
            glUniform2f(offset, layer.dx * 2.0f, -layer.dy * 2.0f);

            // A clockwise turn on screen is a counter-clockwise one in NDC,
            // because NDC flips y.
            float radians = layer.rotation_deg * 3.14159265358979f / 180.0f;
            glUniform2f(rot, cosf(radians), -sinf(radians));

            // Decoder output carries its own matrix; a bitmap needs the v flip
            // that turns top-first rows into GL's bottom-first texture space.
            glUniformMatrix4fv(matrix, 1, GL_FALSE,
                               bitmap ? kFlipV
                                      : (layer.tex_matrix != nullptr ? layer.tex_matrix
                                                                     : kIdentity));
            glUniform1f(opacity, layer.opacity);

            if (!bitmap) {
                // v1 effects: the bit mask comes straight from the core's frame
                // plan (CAPTRIM_EFFECT_*), applied in the shader's canonical
                // order. uTexel is in sampled-UV units; a 1/src tap is a stable
                // approximation for the box blur.
                glUniform1ui(c->uEffects, layer.effects);
                glUniform2f(c->uTexel,
                            layer.src_w > 0 ? 1.0f / static_cast<float>(layer.src_w) : 0.0f,
                            layer.src_h > 0 ? 1.0f / static_cast<float>(layer.src_h) : 0.0f);
                glBindTexture(GL_TEXTURE_EXTERNAL_OES, layer.texture);
            } else {
                glBindTexture(GL_TEXTURE_2D, layer.texture);
            }
            glDrawArrays(GL_TRIANGLES, 0, 6);
        }

        glDisableVertexAttribArray(0);
        glDisableVertexAttribArray(1);
        glDisable(GL_BLEND);
    }

    if (eglSwapBuffers(c->display, c->surface) != EGL_TRUE) {
        CAPTRIM_LOGE("eglSwapBuffers failed (0x%{public}x)", eglGetError());
        return -1;
    }
    return 0;
}

} // namespace captrim
