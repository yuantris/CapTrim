/* CapTrim FFmpeg C shim.
 *
 * The only component that includes FFmpeg headers. Keeping FFmpeg behind a
 * tiny C ABI means the Rust side never needs bindgen, generated bindings or
 * knowledge of FFmpeg struct layouts.
 *
 * Build as: libcaptrim_ffmpeg.so, linked dynamically against LGPL FFmpeg
 * shared libraries. Never enable --enable-gpl / --enable-nonfree / libx264.
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

void ct_ff_string_free(char *s);

#ifdef __cplusplus
}
#endif

#endif /* CAPTRIM_FFMPEG_H */
