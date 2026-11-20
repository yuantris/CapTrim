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

/* Transcode: decode -> libavfilter graph -> re-encode -> mp4 mux.
 * filter_desc / video_codec may be NULL or "" for backend defaults
 * (no filter / libx264). Non-video streams are stream-copied.
 * Returns CAPTRIM_ERR_NO_BACKEND when built without the `ffmpeg` feature. */
int32_t captrim_engine_export_transcode(captrim_engine_t *engine,
                                        const char *src_uri, const char *dst_uri,
                                        const char *filter_desc, const char *video_codec,
                                        captrim_progress_cb progress, void *user_data);

/* ---- Frame planning (P1) -------------------------------------------------
 * Preview and export read the same plan, which is what keeps "what I saw" and
 * "what got written" identical.
 *
 * captrim_engine_plan_frame_at() addresses one frame by index: use it to walk
 * an export. captrim_engine_plan_frame() floors a timestamp onto the frame
 * grid: use it while scrubbing or during playback. Do NOT feed a PTS from
 * plan_frame's output back into plan_frame; flooring twice collapses NTSC
 * frames onto their predecessor.
 *
 * out_json is a FramePlan object, owned by CapTrim. Release it with
 * captrim_string_free().
 */
int32_t captrim_engine_plan_frame_at(captrim_engine_t *engine,
                                     uint64_t frame_index, char **out_json);
int32_t captrim_engine_plan_frame(captrim_engine_t *engine, int64_t t_us,
                                  char **out_json);

/* Audio contributors at an arbitrary instant, returned as a JSON array of
 * AudioMix objects. Unlike plan_frame this does NOT snap to the video frame
 * grid: the audio renderer pulls at its own callback rate (typically 10 ms),
 * which is unrelated to the frame rate. Release with captrim_string_free(). */
int32_t captrim_engine_plan_audio(captrim_engine_t *engine, int64_t t_us,
                                  char **out_json);

/* Export plan: frame-aligned segments covering [0, frame_count). Each segment
 * lists the active video/audio sources and their source-time ranges, so an
 * exporter can keep decoders open instead of planning every frame. Gaps are
 * valid segments with empty source arrays. Release out_json with
 * captrim_string_free(). */
int32_t captrim_engine_export_plan(captrim_engine_t *engine, char **out_json);

/* Translate an `ExportVideoItem.effects` list into the FFmpeg filter_desc for
 * captrim_engine_export_transcode(). `effects_json` is a JSON array of effect
 * names (e.g. ["hflip","invert"]), the exact shape found in the export plan.
 * The core owns the vocabulary and constants, so the platform never duplicates
 * them. `out_desc` is an empty string when there is nothing to apply; release
 * it with captrim_string_free(). Invalid JSON or an unknown name is an error. */
int32_t captrim_effects_to_filter_desc(const char *effects_json,
                                       char **out_desc);

/* Timeline length. Either out pointer may be NULL. */
int32_t captrim_engine_duration(captrim_engine_t *engine,
                                int64_t *out_duration_us,
                                uint64_t *out_frame_count);

/* ---- Preview session (P1) ------------------------------------------------
 * One preview owns one playback clock and one "frame currently on screen".
 *
 * Every now_wall_us argument must come from the SAME monotonic clock
 * (CLOCK_MONOTONIC, or OH_GetTimeSinceBoot on HarmonyOS) in microseconds.
 * Never pass a wall-clock date: a clock that jumps backwards will stall
 * playback, and one that jumps forwards will drop the whole buffer.
 *
 * The platform loop is:
 *     step() -> present  -> decode plan.layers, composite, show
 *            -> wait_frame -> sleep until due_wall_us, then step() again
 *            -> idle      -> paused, block on a UI event
 *            -> end       -> stop
 *
 * The session notices engine edits on the next step() and re-plans the same
 * frame index, so an edit repaints without the platform asking for it.
 */
typedef struct captrim_preview captrim_preview_t;

int32_t captrim_preview_new(captrim_engine_t *engine,
                            captrim_preview_t **out_preview);
void captrim_preview_free(captrim_preview_t *preview);

/* Seek. The target is clamped to the last frame so a paused scrub to the end
 * shows the final picture instead of a black frame. */
int32_t captrim_preview_seek(captrim_preview_t *preview, int64_t t_us,
                             int64_t now_wall_us);

/* playing: 0 pauses, non-zero plays. Starting playback while parked on the last
 * frame rewinds to the beginning first. */
int32_t captrim_preview_set_playing(captrim_preview_t *preview, int32_t playing,
                                    int64_t now_wall_us);

/* Returns the next step as JSON, owned by CapTrim:
 *   {"kind":"present","frame":{...}}
 *   {"kind":"wait_frame","due_wall_us":N}
 *   {"kind":"idle"}
 *   {"kind":"end","at_us":N}
 */
int32_t captrim_preview_step(captrim_preview_t *preview, int64_t now_wall_us,
                             char **out_json);

int32_t captrim_preview_position(captrim_preview_t *preview, int64_t now_wall_us,
                                 int64_t *out_us);

/* Re-anchor the clock to the audio renderer's position, which is the master
 * clock. Small drift is removed by slewing the video rate for ~500 ms; drift
 * beyond the slew limit snaps. out_drift_us reports the correction and is the
 * number to log when a device looks out of sync. */
int32_t captrim_preview_sync_audio(captrim_preview_t *preview,
                                   int64_t audio_position_us,
                                   int64_t now_wall_us, int64_t *out_drift_us);

/* Dropped-frame count, for profiling a device. A number that keeps growing
 * means the decoder or the compositor cannot keep up. */
int32_t captrim_preview_stats(captrim_preview_t *preview,
                              uint64_t *out_dropped_frames);

/* ---------------------------------------------------------------------------
 * Binary frame plan, for the renderer.
 *
 * captrim_preview_step() returns JSON, which is right for script front-ends but
 * wrong for a compositor running at 60 Hz: it allocates and parses per frame,
 * on a path that has a hard deadline. A real renderer calls
 * captrim_preview_step_frame() instead and reads the struct directly -- no
 * allocation, no parsing, no ownership rules to remember.
 *
 * The two functions return the same decision for the same inputs; they share
 * one implementation. Nothing here is valid after the preview is freed.
 * ------------------------------------------------------------------------- */

#define CAPTRIM_MAX_LAYERS 8

/* Effect bits in captrim_layer_desc_t.effects. The bit index is the effect's
 * position in the v1 canonical order (hflip, vflip, brightness, contrast,
 * saturation, grayscale, invert, blur) — the same order the GLSL compositor
 * applies them and effects_to_filter_desc() emits them. The Rust core's
 * EFFECT_* constants and effects_mask() are the single source of truth. */
#define CAPTRIM_EFFECT_HFLIP      (1u << 0)
#define CAPTRIM_EFFECT_VFLIP      (1u << 1)
#define CAPTRIM_EFFECT_BRIGHTNESS (1u << 2)
#define CAPTRIM_EFFECT_CONTRAST   (1u << 3)
#define CAPTRIM_EFFECT_SATURATION (1u << 4)
#define CAPTRIM_EFFECT_GRAYSCALE  (1u << 5)
#define CAPTRIM_EFFECT_INVERT     (1u << 6)
#define CAPTRIM_EFFECT_BLUR       (1u << 7)

/* Same values as the JSON "kind" field of a layer. */
typedef enum {
    CAPTRIM_LAYER_VIDEO = 0,
    CAPTRIM_LAYER_SUBTITLE = 1,
    CAPTRIM_LAYER_OVERLAY = 2
} captrim_layer_kind_t;

typedef struct {
    uint64_t clip_id;
    uint64_t source_id;
    /* Exact source instant to decode, already mapped through clip speed. */
    int64_t source_us;
    /* Draw order, 0 is the bottom. */
    int32_t z;
    int32_t kind; /* captrim_layer_kind_t */
    float scale;
    float dx;
    float dy;
    float rotation_deg;
    float opacity;
    /* Bit mask of CAPTRIM_EFFECT_* applied to this layer by the renderer. */
    uint32_t effects;
} captrim_layer_desc_t;

typedef struct {
    int64_t t_us;
    uint64_t frame_index;
    uint32_t width;
    uint32_t height;
    int32_t layer_count;
    /* Layers beyond CAPTRIM_MAX_LAYERS are dropped and reported here, never
     * silently: a renderer that draws 8 of 9 layers looks like a UI bug. */
    int32_t layer_overflow;
    int32_t audio_count;
    captrim_layer_desc_t layers[CAPTRIM_MAX_LAYERS];
} captrim_frame_desc_t;

/* Same values as the JSON "kind" field of a step. */
typedef enum {
    CAPTRIM_STEP_IDLE = 0,
    CAPTRIM_STEP_PRESENT = 1,
    CAPTRIM_STEP_WAIT_FRAME = 2,
    CAPTRIM_STEP_END = 3
} captrim_step_kind_t;

/* One step, written into caller-owned memory.
 *
 * out_frame is filled only when *out_kind is CAPTRIM_STEP_PRESENT;
 * out_due_wall_us only when it is CAPTRIM_STEP_WAIT_FRAME.
 * Either out pointer may be NULL if the caller does not need it.
 *
 * Returns 0 on success, or a CAPTRIM_ERR_* status; on error the outputs are
 * left untouched. */
int32_t captrim_preview_step_frame(captrim_preview_t *preview,
                                   int64_t now_wall_us,
                                   captrim_frame_desc_t *out_frame,
                                   int32_t *out_kind,
                                   int64_t *out_due_wall_us);

/* URI of a media source, so the renderer can open the file the plan refers to.
 *
 * Writes a NUL-terminated UTF-8 string into buf and sets *out_len to the length
 * without the terminator. If the buffer is too small the call fails with
 * CAPTRIM_ERR_OUT_OF_RANGE and still writes the required length to *out_len, so
 * the caller can retry with a bigger buffer. */
int32_t captrim_engine_source_uri(captrim_engine_t *engine, uint64_t source_id,
                                  char *buf, int32_t buf_len, int32_t *out_len);

/* UTF-8 text of a subtitle clip, addressed by the clip_id from a frame plan.
 * Same buffer protocol as captrim_engine_source_uri: a short buffer fails with
 * CAPTRIM_ERR_OUT_OF_RANGE and still writes the required length to *out_len. */
int32_t captrim_engine_clip_text(captrim_engine_t *engine, uint64_t clip_id,
                                 char *buf, int32_t buf_len, int32_t *out_len);

/* ---------------------------------------------------------------------------
 * Subtitle bitmaps.
 *
 * A renderer that switch-cases on captrim_layer_desc_t.kind needs pixels for a
 * CAPTRIM_LAYER_SUBTITLE layer, not text: font choice, wrapping, line spacing
 * and the outline all live in the core, so the preview and the exporter draw
 * the same block. The result is RGBA8, straight alpha, row-major, top row first.
 *
 * Protocol: the outputs are always written; `buf == NULL` is a size query and
 * succeeds; a buffer smaller than *out_len fails with
 * CAPTRIM_ERR_OUT_OF_RANGE. *out_revision is a hash of the rasterised block --
 * a renderer that caches its texture compares it and skips the upload when the
 * subtitle has not changed.
 * ------------------------------------------------------------------------- */

int32_t captrim_engine_clip_text_bitmap(captrim_engine_t *engine, uint64_t clip_id,
                                        uint32_t canvas_w, uint32_t canvas_h,
                                        uint8_t *buf, int32_t buf_len,
                                        int32_t *out_len, int32_t *out_width,
                                        int32_t *out_height, uint64_t *out_revision);

/* The same bitmap, addressed through a preview: a compositor holds a preview
 * handle, not an engine handle. */
int32_t captrim_preview_clip_text_bitmap(captrim_preview_t *preview, uint64_t clip_id,
                                         uint32_t canvas_w, uint32_t canvas_h,
                                         uint8_t *buf, int32_t buf_len,
                                         int32_t *out_len, int32_t *out_width,
                                         int32_t *out_height, uint64_t *out_revision);

/* Replace the font used for subtitles. NULL or an empty path resets to the
 * platform's system font. collection_index selects a face inside a .ttc (0 for
 * a plain .ttf). The font is parsed on the spot, so a bad path fails here
 * instead of leaving the preview with blank captions. */
int32_t captrim_engine_set_text_font(captrim_engine_t *engine, const char *path,
                                     uint32_t collection_index);

#ifdef __cplusplus
}
#endif

#endif /* CAPTRIM_H */
