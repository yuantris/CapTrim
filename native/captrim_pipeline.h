/* CapTrim cross-platform three-stage transcode pipeline ABI.
 *
 * This is the INTERNAL native contract (not the public captrim.h C ABI). It
 * exists so the first and last mile can be swapped between platform hardware
 * codecs and the FFmpeg software fallback without changing the middle stage.
 *
 *   source file
 *     -> [ STAGE 1 decoder  ]  ct_frame_t (raw YUV)
 *     -> [ STAGE 2 processor]  FFmpeg libavfilter graph ("complex edit cmd")
 *     -> [ STAGE 3 encoder  ]  H.264/H.265 bitstream
 *     -> [ muxer ]              mp4
 *
 * Backend mapping:
 *   STAGE 1  OHOS    OH_VideoDecoder (Buffer mode, NOT Surface/NativeWindow)
 *            Android AMediaCodec (ByteBuffer mode, NOT Surface)
 *            fallback FFmpeg libavcodec soft decode
 *   STAGE 2  all     FFmpeg libavfilter + libswscale  (canonical, shared)
 *   STAGE 3  OHOS    OH_VideoEncoder (Buffer mode) + OH_AVMuxer
 *            Android AMediaCodec + AMediaMuxer
 *            fallback FFmpeg libx264/libx265 + libavformat mp4 muxer
 *
 * The software-only path (all three stages in FFmpeg) is already implemented
 * by ct_ff_transcode() in captrim_ffmpeg.c and verified on a HarmonyOS device.
 * A hardware stage is selected per-stage: if decoder creation/configure fails,
 * only STAGE 1 falls back to FFmpeg; the same applies independently to STAGE 3.
 *
 * CRITICAL - YUV stride: hardware decoders return buffers whose row stride is
 * aligned (16/32/64 bytes) and therefore LARGER than width. ct_frame_t always
 * carries per-plane stride; never copy a plane as width*height contiguous
 * bytes. STAGE 2 normalises every input frame to CONTIGUOUS yuv420p via
 * libswscale before feeding the filter graph, and STAGE 3 re-expands to the
 * encoder's own stride when writing an OH_AVBuffer.
 *
 * Reference (HarmonyOS docs, read 2026-11-15):
 *   - "基于Buffer模式进行视频转码"
 *     https://developer.huawei.com/consumer/cn/doc/doccenter-capabilities/video-transcoding-buffer.md
 *   - "VideoEncoder" C API module
 *     https://developer.huawei.com/consumer/cn/doc/doccenter-references/api/capi-videoencoder.md
 *   - "VideoDecoder" C API module
 *     https://developer.huawei.com/consumer/cn/doc/doccenter-references/api/capi-videodecoder.md
 *   - "AVMuxer" C API module
 *     https://developer.huawei.com/consumer/cn/doc/doccenter-references/api/capi-avmuxer.md
 *   - "视频编码" / "视频解码" guides
 *     https://developer.huawei.com/consumer/cn/doc/doccenter-capabilities/video-encoding.md
 *     https://developer.huawei.com/consumer/cn/doc/doccenter-capabilities/video-decoding.md
 *   - API Reference index
 *     https://developer.huawei.com/consumer/cn/llms/docs-references.txt
 * The exact signatures used by this project are taken from the LOCAL NDK
 * headers (multimedia/player_framework/native_avcodec_videoencoder.h,
 * native_avmuxer.h, native_avbuffer_info.h, native_avformat.h), which are
 * authoritative for the installed SDK version.
 */
#ifndef CAPTRIM_PIPELINE_H
#define CAPTRIM_PIPELINE_H

#include <stdint.h>

/* Reuse the shim's ct_progress_cb typedef and CT_ERR_* codes so both headers
 * can be included in the same translation unit without duplicate typedefs. */
#include "captrim_ffmpeg.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------- */
/* Common types                                                              */
/* ------------------------------------------------------------------------- */

typedef enum {
    CT_BACKEND_FFMPEG_SW = 0,  /* libavcodec soft decode / libx264 soft encode */
    CT_BACKEND_OHOS_HW   = 1,  /* OH_VideoDecoder / OH_VideoEncoder           */
    CT_BACKEND_ANDROID_HW= 2   /* AMediaCodec                                 */
} ct_backend_kind_t;

/* Pixel formats that hardware codecs actually emit/accept. I420 maps 1:1 to
 * FFmpeg AV_PIX_FMT_YUV420P; NV12/NV21 need libswscale before the filter graph. */
typedef enum {
    CT_PIX_FMT_NONE = 0,
    CT_PIX_FMT_I420 = 1,   /* 3 planes: Y, U, V          (YUV420P) */
    CT_PIX_FMT_NV12 = 2,   /* 2 planes: Y, interleaved UV (YUV420SP) */
    CT_PIX_FMT_NV21 = 3,   /* 2 planes: Y, interleaved VU */
    CT_PIX_FMT_RGBA = 4    /* 1 plane, 4 bytes/pixel */
} ct_pix_fmt_t;

/* Plain-old-data frame descriptor. No ownership is implied by data[]: the
 * producing stage owns the memory and the consumer MUST call release_frame()
 * when done. This keeps zero-copy possible for OH_AVBuffer / AVFrame refs. */
typedef struct {
    ct_pix_fmt_t format;
    int32_t  width;
    int32_t  height;
    int32_t  stride[4];      /* bytes per row, INCLUDING alignment padding */
    int32_t  plane_count;
    uint8_t *data[4];
    int64_t  pts_us;         /* presentation time, microseconds, monotonic */
    int32_t  rotation;       /* 0 / 90 / 180 / 270 from container metadata */
    void    *opaque;         /* backend-owned handle (OH_AVBuffer*, AVFrame*) */
} ct_frame_t;

/* ------------------------------------------------------------------------- */
/* Stage vtables                                                             */
/* ------------------------------------------------------------------------- */

typedef struct ct_decoder ct_decoder_t;
typedef struct ct_processor ct_processor_t;
typedef struct ct_encoder ct_encoder_t;

/* STAGE 1: compressed samples -> raw frames.
 * next() returns 1 when *out is filled, 0 on EOF, negative on error.
 * Every frame returned by next() must be returned via release_frame(). */
struct ct_decoder {
    ct_backend_kind_t kind;
    int32_t (*open)(ct_decoder_t *self, const char *src_uri, char **out_error);
    int32_t (*next)(ct_decoder_t *self, ct_frame_t *out, char **out_error);
    void    (*release_frame)(ct_decoder_t *self, ct_frame_t *frame);
    void    (*close)(ct_decoder_t *self);
    void *ctx;
    /* Filled by open(): total source duration in microseconds, used by the
     * orchestrator to normalise progress. 0 when unknown. */
    int64_t duration_us;
};

/* STAGE 2: raw frame -> filtered raw frame. Always FFmpeg.
 * pull() returns 1 when *out is filled, 0 when it needs more input (EAGAIN),
 * -1 on final EOF, or a positive CT_ERR_* on failure. The graph is built
 * lazily from the FIRST frame (codecpar->format is usually AV_PIX_FMT_NONE),
 * and the sink is constrained to contiguous yuv420p. */
struct ct_processor {
    ct_backend_kind_t kind;
    int32_t (*open)(ct_processor_t *self, const ct_frame_t *first,
                    const char *filter_desc, char **out_error);
    int32_t (*push)(ct_processor_t *self, const ct_frame_t *in, char **out_error);
    int32_t (*pull)(ct_processor_t *self, ct_frame_t *out, char **out_error);
    int32_t (*flush)(ct_processor_t *self, char **out_error);
    void    (*release_frame)(ct_processor_t *self, ct_frame_t *frame);
    void    (*close)(ct_processor_t *self);
    void *ctx;
    /* Filled by open() from the configured buffersink, so the encoder can use
     * the REAL output frame rate (a "fps=15" filter changes it). */
    int32_t out_fps_num;
    int32_t out_fps_den;
    /* Set when a terminating filter (trim=..., select=...) closes the graph.
     * push() then returns CT_OK for any leftover input and the orchestrator
     * stops decoding early instead of walking the whole source file. */
    int32_t graph_done;
};

/* STAGE 3: filtered raw frames -> muxed target file.
 * push() consumes one frame; finish() flushes the encoder and writes the
 * trailer. For hardware backends the muxer lives inside this stage. */
struct ct_encoder {
    ct_backend_kind_t kind;
    int32_t (*open)(ct_encoder_t *self, const ct_frame_t *first,
                    const char *dst_uri, const char *video_codec,
                    char **out_error);
    int32_t (*push)(ct_encoder_t *self, const ct_frame_t *in, char **out_error);
    int32_t (*finish)(ct_encoder_t *self, char **out_error);
    void    (*close)(ct_encoder_t *self);
    void *ctx;
    /* Filled by the orchestrator BEFORE open() with the processor's real
     * output frame rate, so hardware and software encoders agree on timing. */
    int32_t fps_num;
    int32_t fps_den;
};

/* ------------------------------------------------------------------------- */
/* Per-platform factories                                                    */
/* ------------------------------------------------------------------------- */

/* Returns NULL when the backend is unavailable. The caller then tries the next
 * backend. This is the ONLY fallback mechanism - no exceptions, no longjmp.
 * Platform factories are WEAK: a build that does not link the platform file
 * (e.g. Android before AMediaCodec is wired, or a host unit test) simply gets
 * NULL and the pipeline runs fully in software. */

ct_decoder_t   *ct_decoder_create_ffmpeg(void);
ct_encoder_t   *ct_encoder_create_ffmpeg(void);
ct_processor_t *ct_processor_create_ffmpeg(void);

#if defined(__GNUC__) || defined(__clang__)
#define CT_PIPELINE_WEAK __attribute__((weak))
#else
#define CT_PIPELINE_WEAK
#endif

ct_decoder_t   *ct_decoder_create_ohos(void)    CT_PIPELINE_WEAK;
ct_encoder_t   *ct_encoder_create_ohos(void)    CT_PIPELINE_WEAK;
ct_decoder_t   *ct_decoder_create_android(void) CT_PIPELINE_WEAK;
ct_encoder_t   *ct_encoder_create_android(void) CT_PIPELINE_WEAK;

/* Optional platform hook: emit a one-line diagnostic to the device log.
 * Implemented by native/captrim_ohos_hw.c via hilog. Weak so host/Android
 * builds stay log-free. */
void ct_pipeline_platform_log(const char *message) CT_PIPELINE_WEAK;

/* Optional: a platform module can report real codec-instance availability.
 * Returns CT_OK and sets *out_json to a malloc'd object (without braces). */
int32_t ct_pipeline_platform_caps(char **out_json) CT_PIPELINE_WEAK;

/* ------------------------------------------------------------------------- */
/* Orchestration                                                             */
/* ------------------------------------------------------------------------- */

typedef struct {
    int  prefer_hardware;     /* 1: try HW decoder/encoder first (default) */
    int  allow_sw_fallback;   /* 1: fall back to FFmpeg per stage (default) */
    int  force_software;      /* 1: bypass HW entirely (diagnostics / CI)  */
    int  rotation;            /* override container rotation, -1 = use meta */
    int  force_async;         /* 1: force OHOS AVCodec CALLBACK Buffer mode */
} ct_pipeline_options_t;

typedef struct {
    ct_backend_kind_t decoder;
    ct_backend_kind_t encoder;
    int32_t frames_decoded;
    int32_t frames_encoded;
    int64_t duration_ms;
    /* Human-readable reason when a hardware stage was rejected. */
    char fallback_reason[256];
} ct_pipeline_report_t;

/* Run the whole three-stage pipeline. ct_ff_transcode() routes to this when
 * video_codec starts with "hw:" (see captrim_ffmpeg.c). */
int32_t ct_pipeline_run(const char *src_uri, const char *dst_uri,
                        const char *filter_desc, const char *video_codec,
                        const ct_pipeline_options_t *options,
                        ct_progress_cb progress, void *user_data,
                        char **out_error, ct_pipeline_report_t *out_report);

/* Internal switch consumed by the OHOS hardware module: when set, it registers
 * AVCodec callbacks instead of using the API-20 synchronous Query*Buffer path.
 * Kept here (rather than in the public captrim.h) because it is a diagnostic
 * knob for the platform backend, not part of the SDK contract. */
void ct_pipeline_set_force_async(int value);
int  ct_pipeline_force_async(void);

/* Query hardware availability without opening any media. */
int32_t ct_pipeline_hardware_caps(char **out_json);

/* Free a string returned by this API. */
void ct_pipeline_string_free(char *s);

#ifdef __cplusplus
}
#endif

#endif /* CAPTRIM_PIPELINE_H */
