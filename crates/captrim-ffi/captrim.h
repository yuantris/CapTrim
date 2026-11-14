/* CapTrim stable C ABI v0.1.0
 *
 * One boundary for both platforms: Android JNI and HarmonyOS Node-API call
 * exactly these functions. No C++ types, no exceptions, no owned objects
 * crossing the boundary.
 *
 * Threading: an engine may be used from any thread, but captrim_engine_free()
 * must not race with other calls. Callback user_data is owned by the caller.
 * Strings returned via char** are owned by CapTrim; release with
 * captrim_string_free(). captrim_last_error_message() is thread-local and
 * remains valid only until the next CapTrim call on the same thread.
 */
#ifndef CAPTRIM_H
#define CAPTRIM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CAPTRIM_OK                 0
#define CAPTRIM_ERR_INVALID       (-1)
#define CAPTRIM_ERR_NOT_FOUND     (-2)
#define CAPTRIM_ERR_OUT_OF_RANGE  (-3)
#define CAPTRIM_ERR_NO_BACKEND    (-4)
#define CAPTRIM_ERR_IO            (-5)
#define CAPTRIM_ERR_DECODE        (-6)
#define CAPTRIM_ERR_ENCODE        (-7)
#define CAPTRIM_ERR_CANCELLED     (-8)
#define CAPTRIM_ERR_INTERNAL      (-9)
#define CAPTRIM_ERR_PANIC         (-10)

typedef struct captrim_engine captrim_engine_t;

/* Return 0 to continue, non-zero to cancel an export. */
typedef int32_t (*captrim_progress_cb)(float progress, void *user_data);

const char *captrim_version(void);
const char *captrim_code_message(int32_t code);
const char *captrim_last_error_message(void);
void captrim_string_free(char *s);

/* draft_json may be NULL or "" for a fresh project. */
int32_t captrim_engine_new(const char *draft_json, captrim_engine_t **out_engine);
void captrim_engine_free(captrim_engine_t *engine);

/* command_json is a tagged JSON object, e.g.
 * {"type":"split_clip","clip_id":3,"at_us":4000000}
 * created_id_out may be NULL. */
int32_t captrim_engine_apply(captrim_engine_t *engine, const char *command_json,
                             uint64_t *created_id_out);
int32_t captrim_engine_undo(captrim_engine_t *engine);
int32_t captrim_engine_redo(captrim_engine_t *engine);
int32_t captrim_engine_draft(captrim_engine_t *engine, char **out_json);

/* Import a source URI, returning its media id. */
int32_t captrim_engine_add_media(captrim_engine_t *engine, const char *uri,
                                 uint64_t *out_media_id);

/* Probe via the FFmpeg backend. Returns CAPTRIM_ERR_NO_BACKEND when built
 * without the `ffmpeg` feature. On success out_json is a MediaInfo object and
 * the engine's draft is updated with it. */
int32_t captrim_engine_probe(captrim_engine_t *engine, const char *uri,
                             char **out_json);

/* Inject probe metadata (useful for tests and for platform-side decoders). */
int32_t captrim_engine_set_media_info(captrim_engine_t *engine,
                                      uint64_t media_id, const char *info_json);

/* P0 export: lossless stream copy / remux. No transcode, no re-encode. */
int32_t captrim_engine_export_remux(captrim_engine_t *engine,
                                    const char *src_uri, const char *dst_uri,
                                    captrim_progress_cb progress, void *user_data);

#ifdef __cplusplus
}
#endif

#endif /* CAPTRIM_H */
