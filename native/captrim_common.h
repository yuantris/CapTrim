/* Helpers shared by captrim_ffmpeg.c and captrim_export.c.
 *
 * Both files are compiled into the same libcaptrim_ffmpeg.so, so the error
 * formatting and the av_log capture live in exactly one place: a graph failure
 * must read the same whether it comes from a one-input transcode or from the
 * multi-input export executor.
 */
#ifndef CAPTRIM_COMMON_H
#define CAPTRIM_COMMON_H

#include <libavutil/log.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char *data;
    size_t len;
    size_t cap;
    int failed;
} StrBuf;

int sb_init(StrBuf *b, size_t cap);
void sb_puts(StrBuf *b, const char *s);
void sb_printf(StrBuf *b, const char *fmt, ...);

/* malloc'd message, caller frees with ct_ff_string_free / free. */
void ct_set_error(char **out_error, const char *fmt, ...);
void ct_av_error(char **out_error, const char *what, int err);

/* av_log only explains *why* a graph failed through its callback; capture the
 * first error line for the duration of one parse/config call. */
void ct_capture_begin(void);
void ct_capture_end(char **out_error, const char *what, int err);
void ct_log_capture(void *avcl, int level, const char *fmt, va_list vl);

#ifdef __cplusplus
}
#endif

#endif /* CAPTRIM_COMMON_H */
