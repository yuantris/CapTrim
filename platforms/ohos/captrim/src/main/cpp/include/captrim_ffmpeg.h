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

void ct_ff_string_free(char *s);

#ifdef __cplusplus
}
#endif

#endif /* CAPTRIM_FFMPEG_H */
