/* CapTrim FFmpeg C shim.
 *
 * The only component that includes FFmpeg headers. Keeping FFmpeg behind a
 * tiny C ABI means the Rust side never needs bindgen, generated bindings or
 * knowledge of FFmpeg struct layouts.
 *
 * Build as: libcaptrim_ffmpeg.so, linked dynamically against FULL GPL FFmpeg
 * shared libraries (--enable-gpl --enable-libx264 --enable-libx265). The whole
 * CapTrim distribution is therefore GPL v2+; shipping the binaries requires
 * publishing the FFmpeg + x264 + x265 source. The OHOS build additionally needs
 * the NDK C++ runtime libc++_shared.so bundled in the HAP (see
 * build-ffmpeg-ohos.sh).
 */
#ifndef CAPTRIM_FFMPEG_H
#define CAPTRIM_FFMPEG_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CT_OK 0
#define CT_ERR_INVALID 1
#define CT_ERR_IO 2
#define CT_ERR_DECODE 3
#define CT_ERR_ENCODE 4
#define CT_ERR_CANCELLED 5
#define CT_ERR_INTERNAL 99

/* Return 0 to continue, non-zero to cancel. */
typedef int32_t (*ct_progress_cb)(float progress, void *user_data);

/* On success *out_json is a malloc'd JSON MediaInfo. Caller frees it with
 * ct_ff_string_free. On failure *out_error is a malloc'd message. */
int32_t ct_ff_probe(const char *uri, char **out_json, char **out_error);

/* P0 lossless remux (stream copy). No re-encode, so no encoder is linked. */
int32_t ct_ff_remux(const char *src_uri, const char *dst_uri,
                    ct_progress_cb progress, void *user_data,
                    char **out_error);

/* Transcode: decode -> libavfilter graph -> re-encode -> mp4 mux. This is the
 * software backbone and the fallback when a hardware codec is unavailable.
 *   filter_desc : libavfilter graph applied to video (e.g. "scale=640:-2,fps=24,hflip").
 *                 NULL or "" passes frames through (still re-encoded).
 *   video_codec : encoder name, e.g. "libx264" / "libx265" / "mpeg4".
 *                 NULL or "" defaults to "libx264".
 * Non-video streams are stream-copied. Video is forced to yuv420p. */
int32_t ct_ff_transcode(const char *src_uri, const char *dst_uri,
                        const char *filter_desc, const char *video_codec,
                        ct_progress_cb progress, void *user_data,
                        char **out_error);

/* Multi-input export: runs the `filter_complex` compiled by
 * captrim-core's export_graph (labels `[k:v]` / `[k:a]` refer to src_uris[k]).
 *
 * Unlike ct_ff_transcode this takes N inputs in ONE graph, encodes the graph's
 * `vout` with `video_codec` (libx26* also gets crf/preset) and its optional
 * `aout` with AAC at `audio_bitrate_kbps`, and stops at `duration_us`. Inputs
 * are demuxed round-robin, each source label gets its own buffersrc, and every
 * output frame is restamped to the plan's timeline instead of the container's.
 * Returns CT_ERR_CANCELLED and deletes dst when *progress returns non-zero. */
int32_t ct_ff_export_graph(const char *const *src_uris, int32_t src_count,
                           const char *filter_complex, const char *dst_uri,
                           const char *video_codec, int32_t crf, const char *preset,
                           int32_t audio_bitrate_kbps, int64_t duration_us,
                           ct_progress_cb progress, void *user_data, char **out_error);

void ct_ff_string_free(char *s);

/* 1 when this FFmpeg build contains the named encoder (libx264 / libx265 /
 * mpeg4 ...), 0 otherwise. The LGPL profile deliberately lacks the GPL
 * encoders; asking libavcodec keeps the answer tied to the shipped binaries. */
int32_t ct_ff_encoder_available(const char *name);

/* 1 when this FFmpeg build contains the named libavfilter filter, 0 otherwise.
 * `eq` and `boxblur` are GPL filters inside FFmpeg itself, so they are absent
 * from the LGPL profile together with the software encoders; the SDK reports
 * that per effect instead of emitting a graph libavfilter will reject. */
int32_t ct_ff_filter_available(const char *name);

/* The software encoder to use when the caller asks for "any" (empty name) or
 * when the pipeline spec has an empty encoder field: libx264 -> libopenh264 ->
 * mpeg4, decided by what the shipped libavcodec actually contains. Never NULL
 * for a normal build; a build with none returns NULL and the caller fails with
 * a clear error instead of naming an encoder it does not have. */
const char *ct_sw_encoder_default(void);

#ifdef __cplusplus
}
#endif

#endif /* CAPTRIM_FFMPEG_H */
