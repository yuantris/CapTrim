// Thin Node-API adapter for HarmonyOS. Every method forwards to the same
// stable CapTrim C ABI used by Android JNI. No editing logic lives here.
//
// IMPORTANT: P0 export runs synchronously on the ArkTS thread, so calling the
// JS progress callback directly is safe. When export moves to a worker thread
// (P1), this MUST be replaced with napi_create_threadsafe_function: caching a
// napi_env or calling N-API from a native thread is undefined behaviour.
//
// IMPORTANT: this file deliberately avoids the C++ standard library (no
// std::string, no exceptions, no RTTI). Using std::string would make
// libcaptrim_napi.so depend on libc++_shared.so, which in turn would force the
// host application's OHOS_STL setting to match ours -- a very common source of
// "works in the sample app, crashes in the real app" reports. Strings are
// copied from Node-API straight into a malloc'd buffer instead, so this
// library only depends on libc.
#include <napi/native_api.h>

#include <stdint.h>
#include <stdlib.h>

#include "captrim.h"
#include "presenter.h"

namespace {

/// Owns the UTF-8 copy of a JS string. Frees on destruction, movable, not
/// copyable, so call sites cannot leak and cannot double free.
class JsString {
public:
    JsString() = default;
    ~JsString() { free(data_); }
    JsString(const JsString &) = delete;
    JsString &operator=(const JsString &) = delete;
    JsString(JsString &&other) noexcept : data_(other.data_) { other.data_ = nullptr; }
    JsString &operator=(JsString &&other) noexcept {
        if (this != &other) {
            free(data_);
            data_ = other.data_;
            other.data_ = nullptr;
        }
        return *this;
    }

    /// Empty string on any conversion failure, so callers never see nullptr.
    static JsString From(napi_env env, napi_value value) {
        JsString out;
        size_t length = 0;
        if (napi_get_value_string_utf8(env, value, nullptr, 0, &length) != napi_ok) {
            return out;
        }
        out.data_ = static_cast<char *>(malloc(length + 1));
        if (!out.data_) return out;
        size_t written = 0;
        if (napi_get_value_string_utf8(env, value, out.data_, length + 1, &written) != napi_ok) {
            free(out.data_);
            out.data_ = nullptr;
            return out;
        }
        out.data_[written] = '\0';
        return out;
    }

    const char *c_str() const { return data_ ? data_ : ""; }

private:
    char *data_ = nullptr;
};

int64_t GetInt64(napi_env env, napi_value value) {
    int64_t result = 0;
    napi_get_value_int64(env, value, &result);
    return result;
}

int32_t GetInt32(napi_env env, napi_value value) {
    int32_t result = 0;
    napi_get_value_int32(env, value, &result);
    return result;
}

captrim_engine_t *AsEngine(int64_t handle) {
    return reinterpret_cast<captrim_engine_t *>(static_cast<intptr_t>(handle));
}

captrim_preview_t *AsPreview(int64_t handle) {
    return reinterpret_cast<captrim_preview_t *>(static_cast<intptr_t>(handle));
}

napi_value CodeResult(napi_env env, int32_t code) {
    napi_value object, value;
    napi_create_object(env, &object);
    napi_create_int32(env, code, &value);
    napi_set_named_property(env, object, "code", value);
    return object;
}

/// Handles are opaque registry keys, so both the engine and the preview flow
/// through here as a plain pointer without any type punning at the call site.
napi_value HandleResult(napi_env env, int32_t code, void *handle) {
    napi_value object = CodeResult(env, code);
    napi_value value;
    napi_create_int64(env, static_cast<int64_t>(reinterpret_cast<intptr_t>(handle)), &value);
    napi_set_named_property(env, object, "handle", value);
    return object;
}

napi_value IdResult(napi_env env, int32_t code, uint64_t id) {
    napi_value object = CodeResult(env, code);
    napi_value value;
    napi_create_int64(env, static_cast<int64_t>(id), &value);
    napi_set_named_property(env, object, "id", value);
    return object;
}

/// Microsecond value (a timeline position, or a drift correction).
napi_value I64Result(napi_env env, int32_t code, int64_t value_us) {
    napi_value object = CodeResult(env, code);
    napi_value value;
    // JS numbers are f64; timeline microseconds stay far below 2^53, so this is
    // exact for any realistic project length (a 24 h timeline is ~8.6e10).
    napi_create_int64(env, value_us, &value);
    napi_set_named_property(env, object, "value_us", value);
    return object;
}

napi_value DurationResult(napi_env env, int32_t code, int64_t duration_us, uint64_t frame_count) {
    napi_value object = CodeResult(env, code);
    napi_value duration;
    napi_value frames;
    napi_create_int64(env, duration_us, &duration);
    napi_create_int64(env, static_cast<int64_t>(frame_count), &frames);
    napi_set_named_property(env, object, "duration_us", duration);
    napi_set_named_property(env, object, "frame_count", frames);
    return object;
}

napi_value StringResult(napi_env env, int32_t code, char *owned) {
    napi_value object = CodeResult(env, code);
    napi_value value;
    if (owned) {
        napi_create_string_utf8(env, owned, NAPI_AUTO_LENGTH, &value);
        captrim_string_free(owned);
    } else {
        // Keep the .d.ts contract (value: string) valid even on failure.
        napi_create_string_utf8(env, "", NAPI_AUTO_LENGTH, &value);
    }
    napi_set_named_property(env, object, "value", value);
    return object;
}

napi_value StaticStringResult(napi_env env, const char *text) {
    napi_value object = CodeResult(env, 0);
    napi_value value;
    napi_create_string_utf8(env, text ? text : "", NAPI_AUTO_LENGTH, &value);
    napi_set_named_property(env, object, "value", value);
    return object;
}

struct ProgressContext {
    napi_env env;
    napi_value callback;
};

int32_t ProgressTrampoline(float value, void *userData) {
    auto *ctx = static_cast<ProgressContext *>(userData);
    napi_value argument, undefined, result;
    napi_get_undefined(ctx->env, &undefined);
    napi_create_double(ctx->env, static_cast<double>(value), &argument);
    if (napi_call_function(ctx->env, undefined, ctx->callback, 1, &argument, &result) != napi_ok) {
        return 1;
    }
    bool keepGoing = true;
    napi_get_value_bool(ctx->env, result, &keepGoing);
    return keepGoing ? 0 : 1;
}

napi_value CreateEngine(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    JsString draft = argc > 0 ? JsString::From(env, args[0]) : JsString();
    captrim_engine_t *handle = nullptr;
    int32_t code = captrim_engine_new(draft.c_str(), &handle);
    return HandleResult(env, code, handle);
}

napi_value ReleaseEngine(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 1) return CodeResult(env, CAPTRIM_ERR_INVALID);
    captrim_engine_free(AsEngine(GetInt64(env, args[0])));
    return CodeResult(env, CAPTRIM_OK);
}

napi_value Apply(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2] = {nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 2) return IdResult(env, CAPTRIM_ERR_INVALID, 0);
    JsString command = JsString::From(env, args[1]);
    uint64_t created = 0;
    int32_t code = captrim_engine_apply(
        AsEngine(GetInt64(env, args[0])), command.c_str(), &created);
    return IdResult(env, code, created);
}

napi_value Undo(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 1) return CodeResult(env, CAPTRIM_ERR_INVALID);
    return CodeResult(env, captrim_engine_undo(AsEngine(GetInt64(env, args[0]))));
}

napi_value Redo(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 1) return CodeResult(env, CAPTRIM_ERR_INVALID);
    return CodeResult(env, captrim_engine_redo(AsEngine(GetInt64(env, args[0]))));
}

napi_value Draft(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 1) return StringResult(env, CAPTRIM_ERR_INVALID, nullptr);
    char *json = nullptr;
    int32_t code = captrim_engine_draft(AsEngine(GetInt64(env, args[0])), &json);
    return StringResult(env, code, json);
}

napi_value AddMedia(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2] = {nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 2) return IdResult(env, CAPTRIM_ERR_INVALID, 0);
    JsString uri = JsString::From(env, args[1]);
    uint64_t id = 0;
    int32_t code = captrim_engine_add_media(
        AsEngine(GetInt64(env, args[0])), uri.c_str(), &id);
    return IdResult(env, code, id);
}

napi_value Probe(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2] = {nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 2) return StringResult(env, CAPTRIM_ERR_INVALID, nullptr);
    JsString uri = JsString::From(env, args[1]);
    char *json = nullptr;
    int32_t code = captrim_engine_probe(
        AsEngine(GetInt64(env, args[0])), uri.c_str(), &json);
    return StringResult(env, code, json);
}

napi_value SetMediaInfo(napi_env env, napi_callback_info info) {
    size_t argc = 3;
    napi_value args[3] = {nullptr, nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 3) return CodeResult(env, CAPTRIM_ERR_INVALID);
    JsString infoJson = JsString::From(env, args[2]);
    int32_t code = captrim_engine_set_media_info(
        AsEngine(GetInt64(env, args[0])),
        static_cast<uint64_t>(GetInt64(env, args[1])), infoJson.c_str());
    return CodeResult(env, code);
}

napi_value ExportRemux(napi_env env, napi_callback_info info) {
    size_t argc = 4;
    napi_value args[4] = {nullptr, nullptr, nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 3) return CodeResult(env, CAPTRIM_ERR_INVALID);

    JsString src = JsString::From(env, args[1]);
    JsString dst = JsString::From(env, args[2]);

    ProgressContext ctx{env, nullptr};
    captrim_progress_cb callback = nullptr;
    if (argc >= 4) {
        napi_valuetype type = napi_undefined;
        napi_typeof(env, args[3], &type);
        if (type == napi_function) {
            ctx.callback = args[3];
            callback = ProgressTrampoline;
        }
    }

    int32_t code = captrim_engine_export_remux(
        AsEngine(GetInt64(env, args[0])), src.c_str(), dst.c_str(),
        callback, callback ? &ctx : nullptr);
    return CodeResult(env, code);
}

// captrim_version() returns a static string; it must not be freed.
napi_value VersionImpl(napi_env env, napi_callback_info) {
    return StaticStringResult(env, captrim_version());
}

napi_value CodeMessage(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    int32_t code = 0;
    if (argc > 0) napi_get_value_int32(env, args[0], &code);
    return StaticStringResult(env, captrim_code_message(code));
}

napi_value LastError(napi_env env, napi_callback_info) {
    return StaticStringResult(env, captrim_last_error_message());
}

// ---- Frame planning --------------------------------------------------------

napi_value PlanFrameAt(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2] = {nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 2) return StringResult(env, CAPTRIM_ERR_INVALID, nullptr);
    char *json = nullptr;
    int32_t code = captrim_engine_plan_frame_at(
        AsEngine(GetInt64(env, args[0])),
        static_cast<uint64_t>(GetInt64(env, args[1])), &json);
    return StringResult(env, code, json);
}

napi_value PlanFrame(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2] = {nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 2) return StringResult(env, CAPTRIM_ERR_INVALID, nullptr);
    char *json = nullptr;
    int32_t code = captrim_engine_plan_frame(
        AsEngine(GetInt64(env, args[0])), GetInt64(env, args[1]), &json);
    return StringResult(env, code, json);
}

napi_value Duration(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 1) return DurationResult(env, CAPTRIM_ERR_INVALID, 0, 0);
    int64_t duration_us = 0;
    uint64_t frames = 0;
    int32_t code = captrim_engine_duration(
        AsEngine(GetInt64(env, args[0])), &duration_us, &frames);
    return DurationResult(env, code, duration_us, frames);
}

// ---- Preview session -------------------------------------------------------

napi_value PreviewNew(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 1) {
        return HandleResult(env, CAPTRIM_ERR_INVALID, nullptr);
    }
    captrim_preview_t *preview = nullptr;
    int32_t code = captrim_preview_new(AsEngine(GetInt64(env, args[0])), &preview);
    return HandleResult(env, code, preview);
}

napi_value PreviewFree(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 1) return CodeResult(env, CAPTRIM_ERR_INVALID);
    captrim_preview_free(AsPreview(GetInt64(env, args[0])));
    return CodeResult(env, CAPTRIM_OK);
}

napi_value PreviewSeek(napi_env env, napi_callback_info info) {
    size_t argc = 3;
    napi_value args[3] = {nullptr, nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 3) return CodeResult(env, CAPTRIM_ERR_INVALID);
    return CodeResult(env, captrim_preview_seek(AsPreview(GetInt64(env, args[0])),
                                                GetInt64(env, args[1]),
                                                GetInt64(env, args[2])));
}

napi_value PreviewSetPlaying(napi_env env, napi_callback_info info) {
    size_t argc = 3;
    napi_value args[3] = {nullptr, nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 3) return CodeResult(env, CAPTRIM_ERR_INVALID);
    bool playing = false;
    napi_get_value_bool(env, args[1], &playing);
    return CodeResult(env, captrim_preview_set_playing(AsPreview(GetInt64(env, args[0])),
                                                       playing ? 1 : 0,
                                                       GetInt64(env, args[2])));
}

napi_value PreviewStep(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2] = {nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 2) return StringResult(env, CAPTRIM_ERR_INVALID, nullptr);
    char *json = nullptr;
    int32_t code = captrim_preview_step(AsPreview(GetInt64(env, args[0])),
                                        GetInt64(env, args[1]), &json);
    return StringResult(env, code, json);
}

napi_value PreviewPosition(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2] = {nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 2) return I64Result(env, CAPTRIM_ERR_INVALID, 0);
    int64_t position_us = 0;
    int32_t code = captrim_preview_position(AsPreview(GetInt64(env, args[0])),
                                            GetInt64(env, args[1]), &position_us);
    return I64Result(env, code, position_us);
}

napi_value PreviewSyncAudio(napi_env env, napi_callback_info info) {
    size_t argc = 3;
    napi_value args[3] = {nullptr, nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 3) return I64Result(env, CAPTRIM_ERR_INVALID, 0);
    int64_t drift_us = 0;
    int32_t code = captrim_preview_sync_audio(AsPreview(GetInt64(env, args[0])),
                                              GetInt64(env, args[1]),
                                              GetInt64(env, args[2]), &drift_us);
    return I64Result(env, code, drift_us);
}

napi_value PreviewStats(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 1) return IdResult(env, CAPTRIM_ERR_INVALID, 0);
    uint64_t dropped = 0;
    int32_t code = captrim_preview_stats(AsPreview(GetInt64(env, args[0])), &dropped);
    return IdResult(env, code, dropped);
}


napi_value PresentReportResult(napi_env env, int32_t code, const captrim::PresentReport &report) {
    napi_value object = CodeResult(env, code);
    napi_value value;
    napi_create_int32(env, report.kind, &value);
    napi_set_named_property(env, object, "kind", value);
    napi_create_int64(env, report.due_wall_us, &value);
    napi_set_named_property(env, object, "due_us", value);
    napi_create_int64(env, report.source_us, &value);
    napi_set_named_property(env, object, "source_us", value);
    napi_create_int32(env, report.layer_count, &value);
    napi_set_named_property(env, object, "layers", value);
    napi_create_int32(env, report.layer_overflow, &value);
    napi_set_named_property(env, object, "overflow", value);
    napi_create_int32(env, report.drawn, &value);
    napi_set_named_property(env, object, "drawn", value);
    napi_create_int32(env, report.running, &value);
    napi_set_named_property(env, object, "running", value);
    napi_create_int32(env, report.error, &value);
    napi_set_named_property(env, object, "error", value);
    napi_create_int64(env, static_cast<int64_t>(report.frames), &value);
    napi_set_named_property(env, object, "frames", value);
    napi_create_int64(env, static_cast<int64_t>(report.dropped), &value);
    napi_set_named_property(env, object, "dropped", value);
    napi_create_int64(env, report.position_us, &value);
    napi_set_named_property(env, object, "position_us", value);
    return object;
}

/// Surface id arrives as a decimal string from XComponent.onSurfaceCreated;
/// `width` / `height` are its size in physical pixels (the compositing canvas).
napi_value PresentCreate(napi_env env, napi_callback_info info) {
    size_t argc = 4;
    napi_value args[4] = {nullptr, nullptr, nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 4) return HandleResult(env, CAPTRIM_ERR_INVALID, nullptr);

    JsString surface = JsString::From(env, args[1]);
    unsigned long long surface_id = strtoull(surface.c_str(), nullptr, 10);
    captrim::Presenter *presenter = captrim::PresenterCreate(
        AsEngine(GetInt64(env, args[0])), surface_id, GetInt32(env, args[2]),
        GetInt32(env, args[3]));
    // The surface is not attached yet during the first layout pass; the caller
    // retries once onSurfaceChanged. Not an error worth an exception.
    return HandleResult(env, presenter ? CAPTRIM_OK : CAPTRIM_ERR_INVALID, presenter);
}

/// presentResize(presenter, width, height): canvas size changed, e.g. rotation.
napi_value PresentResize(napi_env env, napi_callback_info info) {
    size_t argc = 3;
    napi_value args[3] = {nullptr, nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 3) return CodeResult(env, CAPTRIM_ERR_INVALID);
    auto *presenter = reinterpret_cast<captrim::Presenter *>(
        static_cast<intptr_t>(GetInt64(env, args[0])));
    if (presenter == nullptr) return CodeResult(env, CAPTRIM_ERR_INVALID);
    return CodeResult(env,
                      captrim::PresenterResize(presenter, GetInt32(env, args[1]),
                                               GetInt32(env, args[2])));
}

napi_value PresentStart(napi_env env, napi_callback_info info) {
    size_t argc = 3;
    napi_value args[3] = {nullptr, nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 3) return CodeResult(env, CAPTRIM_ERR_INVALID);

    auto *presenter = reinterpret_cast<captrim::Presenter *>(
        static_cast<intptr_t>(GetInt64(env, args[0])));
    if (presenter == nullptr) return CodeResult(env, CAPTRIM_ERR_INVALID);
    return CodeResult(env, captrim::PresenterLoopStart(presenter, AsPreview(GetInt64(env, args[1])),
                                                       GetInt64(env, args[2])));
}

napi_value PresentStop(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 1) return CodeResult(env, CAPTRIM_ERR_INVALID);
    auto *presenter = reinterpret_cast<captrim::Presenter *>(
        static_cast<intptr_t>(GetInt64(env, args[0])));
    if (presenter == nullptr) return CodeResult(env, CAPTRIM_ERR_INVALID);
    return CodeResult(env, captrim::PresenterLoopStop(presenter));
}

/// presentControl(presenter, playing, seekUs): playing < 0 keeps the state,
/// seekUs < 0 means "no seek". Both are queued for the render thread.
napi_value PresentControl(napi_env env, napi_callback_info info) {
    size_t argc = 3;
    napi_value args[3] = {nullptr, nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 3) return CodeResult(env, CAPTRIM_ERR_INVALID);
    auto *presenter = reinterpret_cast<captrim::Presenter *>(
        static_cast<intptr_t>(GetInt64(env, args[0])));
    if (presenter == nullptr) return CodeResult(env, CAPTRIM_ERR_INVALID);

    int32_t playing = -1;
    napi_get_value_int32(env, args[1], &playing);
    int64_t seek_us = GetInt64(env, args[2]);
    return CodeResult(env, captrim::PresenterControl(presenter, playing, seek_us >= 0 ? 1 : 0,
                                                     seek_us));
}

napi_value PresentStatus(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 1) return PresentReportResult(env, CAPTRIM_ERR_INVALID, captrim::PresentReport{});
    auto *presenter = reinterpret_cast<captrim::Presenter *>(
        static_cast<intptr_t>(GetInt64(env, args[0])));
    if (presenter == nullptr) return PresentReportResult(env, CAPTRIM_ERR_INVALID, captrim::PresentReport{});

    captrim::PresentReport report;
    int32_t code = captrim::PresenterStatus(presenter, &report);
    return PresentReportResult(env, code, report);
}

napi_value PresentRelease(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 1) return CodeResult(env, CAPTRIM_ERR_INVALID);
    auto *presenter = reinterpret_cast<captrim::Presenter *>(
        static_cast<intptr_t>(GetInt64(env, args[0])));
    captrim::PresenterDestroy(presenter);
    return CodeResult(env, CAPTRIM_OK);
}

napi_value Init(napi_env env, napi_value exports) {
    napi_property_descriptor descriptors[] = {
        {"createEngine", nullptr, CreateEngine, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"releaseEngine", nullptr, ReleaseEngine, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"apply", nullptr, Apply, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"undo", nullptr, Undo, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"redo", nullptr, Redo, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"draft", nullptr, Draft, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"addMedia", nullptr, AddMedia, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"probe", nullptr, Probe, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setMediaInfo", nullptr, SetMediaInfo, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"exportRemux", nullptr, ExportRemux, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"version", nullptr, VersionImpl, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"codeMessage", nullptr, CodeMessage, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"lastErrorMessage", nullptr, LastError, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"planFrameAt", nullptr, PlanFrameAt, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"planFrame", nullptr, PlanFrame, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"duration", nullptr, Duration, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"previewNew", nullptr, PreviewNew, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"previewFree", nullptr, PreviewFree, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"previewSeek", nullptr, PreviewSeek, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"previewSetPlaying", nullptr, PreviewSetPlaying, nullptr, nullptr, nullptr, napi_default,
         nullptr},
        {"previewStep", nullptr, PreviewStep, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"previewPosition", nullptr, PreviewPosition, nullptr, nullptr, nullptr, napi_default,
         nullptr},
        {"previewSyncAudio", nullptr, PreviewSyncAudio, nullptr, nullptr, nullptr, napi_default,
         nullptr},
        {"previewStats", nullptr, PreviewStats, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"presentCreate", nullptr, PresentCreate, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"presentResize", nullptr, PresentResize, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"presentStart", nullptr, PresentStart, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"presentStop", nullptr, PresentStop, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"presentControl", nullptr, PresentControl, nullptr, nullptr, nullptr, napi_default,
         nullptr},
        {"presentStatus", nullptr, PresentStatus, nullptr, nullptr, nullptr, napi_default,
         nullptr},
        {"presentRelease", nullptr, PresentRelease, nullptr, nullptr, nullptr, napi_default,
         nullptr},
    };
    napi_define_properties(env, exports,
                           sizeof(descriptors) / sizeof(descriptors[0]), descriptors);
    return exports;
}

}  // namespace

static napi_module g_captrimModule = {
    .nm_version = 1,
    .nm_flags = 0,
    .nm_filename = nullptr,
    .nm_register_func = Init,
    .nm_modname = "captrim_napi",
    .nm_priv = nullptr,
    .reserved = {nullptr, nullptr, nullptr, nullptr},
};

extern "C" __attribute__((constructor)) void RegisterCaptrimNapiModule(void) {
    napi_module_register(&g_captrimModule);
}
