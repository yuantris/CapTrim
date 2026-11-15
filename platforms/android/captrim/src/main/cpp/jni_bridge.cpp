// Thin JNI adapter: every method forwards to the single CapTrim C ABI.
// No business logic lives here. Methods are bound with RegisterNatives so
// there is no fragile JNI name mangling to maintain.
#include <jni.h>

#include <cstdint>

#include "captrim.h"

namespace {

constexpr const char *kNativeClass = "io/captrim/CapTrimNative";
constexpr const char *kExceptionClass = "io/captrim/CapTrimException";

class Utf8 {
public:
    Utf8(JNIEnv *env, jstring value)
        : env_(env), value_(value),
          chars_(value ? env->GetStringUTFChars(value, nullptr) : nullptr) {}
    ~Utf8() {
        if (value_ && chars_) env_->ReleaseStringUTFChars(value_, chars_);
    }
    const char *get() const { return chars_; }

private:
    JNIEnv *env_;
    jstring value_;
    const char *chars_;
};

void throwError(JNIEnv *env, int32_t code) {
    jclass klass = env->FindClass(kExceptionClass);
    if (!klass) return;
    const char *message = captrim_last_error_message();
    jstring text = env->NewStringUTF(message ? message : "unknown error");
    jmethodID ctor = env->GetMethodID(klass, "<init>", "(ILjava/lang/String;)V");
    if (ctor) {
        jobject exception = env->NewObject(klass, ctor, static_cast<jint>(code), text);
        if (exception) env->Throw(static_cast<jthrowable>(exception));
    }
    env->DeleteLocalRef(text);
    env->DeleteLocalRef(klass);
}

jint check(JNIEnv *env, int32_t code) {
    if (code != CAPTRIM_OK) throwError(env, code);
    return static_cast<jint>(code);
}

jstring takeString(JNIEnv *env, char *owned) {
    if (!owned) return nullptr;
    jstring result = env->NewStringUTF(owned);
    captrim_string_free(owned);
    return result;
}

jstring staticString(JNIEnv *env, const char *text) {
    return env->NewStringUTF(text ? text : "");
}

captrim_engine_t *asEngine(jlong handle) {
    return reinterpret_cast<captrim_engine_t *>(static_cast<intptr_t>(handle));
}

captrim_preview_t *asPreview(jlong handle) {
    return reinterpret_cast<captrim_preview_t *>(static_cast<intptr_t>(handle));
}

/// Free `owned` and throw on error, or hand the string to Java.
jstring planResult(JNIEnv *env, int32_t code, char *owned) {
    if (code != CAPTRIM_OK) {
        if (owned) captrim_string_free(owned);
        throwError(env, code);
        return nullptr;
    }
    return takeString(env, owned);
}

void writeLong(JNIEnv *env, jlongArray out, jsize index, jlong value) {
    if (!out || env->GetArrayLength(out) <= index) return;
    env->SetLongArrayRegion(out, index, 1, &value);
}

struct ProgressContext {
    JNIEnv *env;
    jobject callback;
    jmethodID onProgress;
};

int32_t progressTrampoline(float value, void *userData) {
    auto *ctx = static_cast<ProgressContext *>(userData);
    jboolean keepGoing = ctx->env->CallBooleanMethod(
        ctx->callback, ctx->onProgress, static_cast<jfloat>(value));
    if (ctx->env->ExceptionCheck()) return 1;
    return keepGoing == JNI_TRUE ? 0 : 1;
}

jlong nativeCreate(JNIEnv *env, jobject, jstring draftJson) {
    Utf8 draft(env, draftJson);
    captrim_engine_t *engine = nullptr;
    int32_t code = captrim_engine_new(draft.get(), &engine);
    if (code != CAPTRIM_OK) {
        throwError(env, code);
        return 0;
    }
    return static_cast<jlong>(reinterpret_cast<intptr_t>(engine));
}

void nativeFree(JNIEnv *, jobject, jlong handle) {
    captrim_engine_free(asEngine(handle));
}

jint nativeApply(JNIEnv *env, jobject, jlong handle, jstring commandJson,
                 jlongArray createdIdOut) {
    Utf8 command(env, commandJson);
    uint64_t created = 0;
    int32_t code = captrim_engine_apply(
        asEngine(handle), command.get(), createdIdOut ? &created : nullptr);
    if (code == CAPTRIM_OK && createdIdOut && env->GetArrayLength(createdIdOut) > 0) {
        jlong value = static_cast<jlong>(created);
        env->SetLongArrayRegion(createdIdOut, 0, 1, &value);
    }
    return check(env, code);
}

jint nativeUndo(JNIEnv *env, jobject, jlong handle) {
    return check(env, captrim_engine_undo(asEngine(handle)));
}

jint nativeRedo(JNIEnv *env, jobject, jlong handle) {
    return check(env, captrim_engine_redo(asEngine(handle)));
}

jstring nativeDraft(JNIEnv *env, jobject, jlong handle) {
    char *json = nullptr;
    int32_t code = captrim_engine_draft(asEngine(handle), &json);
    if (code != CAPTRIM_OK) {
        throwError(env, code);
        return nullptr;
    }
    return takeString(env, json);
}

jint nativeAddMedia(JNIEnv *env, jobject, jlong handle, jstring uri,
                    jlongArray mediaIdOut) {
    Utf8 path(env, uri);
    uint64_t id = 0;
    int32_t code = captrim_engine_add_media(
        asEngine(handle), path.get(), mediaIdOut ? &id : nullptr);
    if (code == CAPTRIM_OK && mediaIdOut && env->GetArrayLength(mediaIdOut) > 0) {
        jlong value = static_cast<jlong>(id);
        env->SetLongArrayRegion(mediaIdOut, 0, 1, &value);
    }
    return check(env, code);
}

jstring nativeProbe(JNIEnv *env, jobject, jlong handle, jstring uri) {
    Utf8 path(env, uri);
    char *json = nullptr;
    int32_t code = captrim_engine_probe(asEngine(handle), path.get(), &json);
    if (code != CAPTRIM_OK) {
        throwError(env, code);
        return nullptr;
    }
    return takeString(env, json);
}

jint nativeSetMediaInfo(JNIEnv *env, jobject, jlong handle, jlong mediaId,
                        jstring infoJson) {
    Utf8 info(env, infoJson);
    return check(env, captrim_engine_set_media_info(
                          asEngine(handle), static_cast<uint64_t>(mediaId), info.get()));
}

jint nativeExportRemux(JNIEnv *env, jobject, jlong handle, jstring srcUri,
                       jstring dstUri, jobject callback) {
    Utf8 src(env, srcUri);
    Utf8 dst(env, dstUri);
    if (!src.get() || !dst.get()) return CAPTRIM_ERR_INVALID;

    ProgressContext ctx{env, nullptr, nullptr};
    captrim_progress_cb nativeCallback = nullptr;
    if (callback) {
        jclass klass = env->GetObjectClass(callback);
        ctx.onProgress = env->GetMethodID(klass, "onProgress", "(F)Z");
        env->DeleteLocalRef(klass);
        if (!ctx.onProgress) return CAPTRIM_ERR_INVALID;
        ctx.callback = callback;
        nativeCallback = progressTrampoline;
    }

    int32_t code = captrim_engine_export_remux(
        asEngine(handle), src.get(), dst.get(), nativeCallback,
        callback ? &ctx : nullptr);
    return check(env, code);
}

jint nativeExportTranscode(JNIEnv *env, jobject, jlong handle, jstring srcUri,
                           jstring dstUri, jstring filterDesc, jstring videoCodec,
                           jobject callback) {
    Utf8 src(env, srcUri);
    Utf8 dst(env, dstUri);
    Utf8 filter(env, filterDesc);
    Utf8 codec(env, videoCodec);
    if (!src.get() || !dst.get()) return CAPTRIM_ERR_INVALID;

    ProgressContext ctx{env, nullptr, nullptr};
    captrim_progress_cb nativeCallback = nullptr;
    if (callback) {
        jclass klass = env->GetObjectClass(callback);
        ctx.onProgress = env->GetMethodID(klass, "onProgress", "(F)Z");
        env->DeleteLocalRef(klass);
        if (!ctx.onProgress) return CAPTRIM_ERR_INVALID;
        ctx.callback = callback;
        nativeCallback = progressTrampoline;
    }

    int32_t code = captrim_engine_export_transcode(
        asEngine(handle), src.get(), dst.get(),
        filter.get() ? filter.get() : "", codec.get() ? codec.get() : "",
        nativeCallback, callback ? &ctx : nullptr);
    return check(env, code);
}

jstring nativeVersion(JNIEnv *env, jobject) {
    return staticString(env, captrim_version());
}

jstring nativeCodeMessage(JNIEnv *env, jobject, jint code) {
    return staticString(env, captrim_code_message(static_cast<int32_t>(code)));
}

jstring nativeLastError(JNIEnv *env, jobject) {
    return staticString(env, captrim_last_error_message());
}

// ---- Frame planning --------------------------------------------------------

jstring nativePlanFrameAt(JNIEnv *env, jobject, jlong handle, jlong frameIndex) {
    char *json = nullptr;
    int32_t code = captrim_engine_plan_frame_at(
        asEngine(handle), static_cast<uint64_t>(frameIndex), &json);
    return planResult(env, code, json);
}

jstring nativePlanFrame(JNIEnv *env, jobject, jlong handle, jlong tUs) {
    char *json = nullptr;
    int32_t code = captrim_engine_plan_frame(
        asEngine(handle), static_cast<int64_t>(tUs), &json);
    return planResult(env, code, json);
}

jint nativeDuration(JNIEnv *env, jobject, jlong handle, jlongArray out) {
    int64_t duration_us = 0;
    uint64_t frames = 0;
    int32_t code = captrim_engine_duration(asEngine(handle), &duration_us, &frames);
    if (code == CAPTRIM_OK && out) {
        writeLong(env, out, 0, static_cast<jlong>(duration_us));
        writeLong(env, out, 1, static_cast<jlong>(frames));
    }
    return check(env, code);
}

// ---- Preview session -------------------------------------------------------

jlong nativePreviewNew(JNIEnv *env, jobject, jlong engine) {
    captrim_preview_t *preview = nullptr;
    int32_t code = captrim_preview_new(asEngine(engine), &preview);
    if (code != CAPTRIM_OK) {
        throwError(env, code);
        return 0;
    }
    return static_cast<jlong>(reinterpret_cast<intptr_t>(preview));
}

void nativePreviewFree(JNIEnv *, jobject, jlong preview) {
    captrim_preview_free(asPreview(preview));
}

jint nativePreviewSeek(JNIEnv *env, jobject, jlong preview, jlong tUs, jlong nowUs) {
    return check(env, captrim_preview_seek(asPreview(preview), static_cast<int64_t>(tUs),
                                           static_cast<int64_t>(nowUs)));
}

jint nativePreviewSetPlaying(JNIEnv *env, jobject, jlong preview, jboolean playing,
                             jlong nowUs) {
    return check(env, captrim_preview_set_playing(asPreview(preview),
                                                  playing == JNI_TRUE ? 1 : 0,
                                                  static_cast<int64_t>(nowUs)));
}

jstring nativePreviewStep(JNIEnv *env, jobject, jlong preview, jlong nowUs) {
    char *json = nullptr;
    int32_t code = captrim_preview_step(asPreview(preview), static_cast<int64_t>(nowUs), &json);
    return planResult(env, code, json);
}

jint nativePreviewPosition(JNIEnv *env, jobject, jlong preview, jlong nowUs, jlongArray out) {
    int64_t position_us = 0;
    int32_t code = captrim_preview_position(asPreview(preview), static_cast<int64_t>(nowUs),
                                            &position_us);
    if (code == CAPTRIM_OK) writeLong(env, out, 0, static_cast<jlong>(position_us));
    return check(env, code);
}

jint nativePreviewSyncAudio(JNIEnv *env, jobject, jlong preview, jlong audioUs, jlong nowUs,
                            jlongArray out) {
    int64_t drift_us = 0;
    int32_t code = captrim_preview_sync_audio(asPreview(preview), static_cast<int64_t>(audioUs),
                                              static_cast<int64_t>(nowUs), &drift_us);
    if (code == CAPTRIM_OK) writeLong(env, out, 0, static_cast<jlong>(drift_us));
    return check(env, code);
}

jint nativePreviewStats(JNIEnv *env, jobject, jlong preview, jlongArray out) {
    uint64_t dropped = 0;
    int32_t code = captrim_preview_stats(asPreview(preview), &dropped);
    if (code == CAPTRIM_OK) writeLong(env, out, 0, static_cast<jlong>(dropped));
    return check(env, code);
}

JNINativeMethod kMethods[] = {
    {"nativeCreate", "(Ljava/lang/String;)J", reinterpret_cast<void *>(nativeCreate)},
    {"nativeFree", "(J)V", reinterpret_cast<void *>(nativeFree)},
    {"nativeApply", "(JLjava/lang/String;[J)I", reinterpret_cast<void *>(nativeApply)},
    {"nativeUndo", "(J)I", reinterpret_cast<void *>(nativeUndo)},
    {"nativeRedo", "(J)I", reinterpret_cast<void *>(nativeRedo)},
    {"nativeDraft", "(J)Ljava/lang/String;", reinterpret_cast<void *>(nativeDraft)},
    {"nativeAddMedia", "(JLjava/lang/String;[J)I", reinterpret_cast<void *>(nativeAddMedia)},
    {"nativeProbe", "(JLjava/lang/String;)Ljava/lang/String;", reinterpret_cast<void *>(nativeProbe)},
    {"nativeSetMediaInfo", "(JJLjava/lang/String;)I", reinterpret_cast<void *>(nativeSetMediaInfo)},
    {"nativeExportRemux",
     "(JLjava/lang/String;Ljava/lang/String;Lio/captrim/ProgressCallback;)I",
     reinterpret_cast<void *>(nativeExportRemux)},
    {"nativeExportTranscode",
     "(JLjava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;"
     "Lio/captrim/ProgressCallback;)I",
     reinterpret_cast<void *>(nativeExportTranscode)},
    {"nativeVersion", "()Ljava/lang/String;", reinterpret_cast<void *>(nativeVersion)},
    {"nativeCodeMessage", "(I)Ljava/lang/String;", reinterpret_cast<void *>(nativeCodeMessage)},
    {"nativeLastError", "()Ljava/lang/String;", reinterpret_cast<void *>(nativeLastError)},
    {"nativePlanFrameAt", "(JJ)Ljava/lang/String;", reinterpret_cast<void *>(nativePlanFrameAt)},
    {"nativePlanFrame", "(JJ)Ljava/lang/String;", reinterpret_cast<void *>(nativePlanFrame)},
    {"nativeDuration", "(J[J)I", reinterpret_cast<void *>(nativeDuration)},
    {"nativePreviewNew", "(J)J", reinterpret_cast<void *>(nativePreviewNew)},
    {"nativePreviewFree", "(J)V", reinterpret_cast<void *>(nativePreviewFree)},
    {"nativePreviewSeek", "(JJJ)I", reinterpret_cast<void *>(nativePreviewSeek)},
    {"nativePreviewSetPlaying", "(JZJ)I", reinterpret_cast<void *>(nativePreviewSetPlaying)},
    {"nativePreviewStep", "(JJ)Ljava/lang/String;", reinterpret_cast<void *>(nativePreviewStep)},
    {"nativePreviewPosition", "(JJ[J)I", reinterpret_cast<void *>(nativePreviewPosition)},
    {"nativePreviewSyncAudio", "(JJJ[J)I", reinterpret_cast<void *>(nativePreviewSyncAudio)},
    {"nativePreviewStats", "(J[J)I", reinterpret_cast<void *>(nativePreviewStats)},
};

}  // namespace

extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *) {
    JNIEnv *env = nullptr;
    if (vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) != JNI_OK) {
        return JNI_ERR;
    }
    jclass klass = env->FindClass(kNativeClass);
    if (!klass) return JNI_ERR;
    jint result = env->RegisterNatives(
        klass, kMethods, sizeof(kMethods) / sizeof(kMethods[0]));
    env->DeleteLocalRef(klass);
    return result == JNI_OK ? JNI_VERSION_1_6 : JNI_ERR;
}
