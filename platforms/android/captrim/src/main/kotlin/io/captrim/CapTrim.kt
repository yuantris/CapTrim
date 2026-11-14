package io.captrim

/**
 * Raised for any non-zero status returned by the native CapTrim engine.
 * [code] matches the CAPTRIM_ERR_* constants in captrim.h.
 */
class CapTrimException(val code: Int, message: String) :
    RuntimeException("$message (code=$code)")

/** Return false from [onProgress] to cancel an export. */
fun interface ProgressCallback {
    fun onProgress(value: Float): Boolean
}

/**
 * JNI entry points. Methods are registered from C++ via RegisterNatives in
 * JNI_OnLoad, so the names here are a stable contract with jni_bridge.cpp.
 */
object CapTrimNative {
    init {
        // FFmpeg shared libs are DT_NEEDED dependencies of the C shim; the C
        // shim in turn is a DT_NEEDED dependency of libcaptrim.so.
        System.loadLibrary("captrim_ffmpeg")
        System.loadLibrary("captrim")
        System.loadLibrary("captrim_jni")
    }

    external fun nativeCreate(draftJson: String?): Long
    external fun nativeFree(handle: Long)
    external fun nativeApply(handle: Long, commandJson: String, createdIdOut: LongArray?): Int
    external fun nativeUndo(handle: Long): Int
    external fun nativeRedo(handle: Long): Int
    external fun nativeDraft(handle: Long): String
    external fun nativeAddMedia(handle: Long, uri: String, mediaIdOut: LongArray?): Int
    external fun nativeProbe(handle: Long, uri: String): String
    external fun nativeSetMediaInfo(handle: Long, mediaId: Long, infoJson: String): Int
    external fun nativeExportRemux(
        handle: Long,
        srcUri: String,
        dstUri: String,
        callback: ProgressCallback?,
    ): Int

    external fun nativeVersion(): String
    external fun nativeCodeMessage(code: Int): String
    external fun nativeLastError(): String
}

/**
 * A CapTrim editing session. One instance owns one native engine; call [close]
 * (or use `use {}`) when the editor screen is destroyed.
 *
 * All timeline editing is expressed as tagged JSON commands, e.g.
 * `{"type":"split_clip","clip_id":3,"at_us":4000000}`. Keeping commands as
 * data means Android and HarmonyOS share exactly the same API surface.
 */
class CapTrim private constructor(private var handle: Long) : AutoCloseable {

    companion object {
        @JvmStatic
        fun create(draftJson: String? = null): CapTrim =
            CapTrim(CapTrimNative.nativeCreate(draftJson))

        @JvmStatic
        val version: String get() = CapTrimNative.nativeVersion()

        @JvmStatic
        fun codeMessage(code: Int): String = CapTrimNative.nativeCodeMessage(code)

        @JvmStatic
        fun lastError(): String = CapTrimNative.nativeLastError()
    }

    private fun requireOpen(): Long = handle.also {
        check(it != 0L) { "CapTrim session is closed" }
    }

    /** Apply one command. Returns the id of a newly created object, if any. */
    fun apply(commandJson: String): Long? {
        val out = LongArray(1)
        CapTrimNative.nativeApply(requireOpen(), commandJson, out)
        return out[0].takeIf { it != 0L }
    }

    fun undo() {
        CapTrimNative.nativeUndo(requireOpen())
    }

    fun redo() {
        CapTrimNative.nativeRedo(requireOpen())
    }

    /** Serialize the current timeline for draft persistence. */
    fun draft(): String = CapTrimNative.nativeDraft(requireOpen())

    fun addMedia(uri: String): Long {
        val out = LongArray(1)
        CapTrimNative.nativeAddMedia(requireOpen(), uri, out)
        return out[0]
    }

    /** Probe a source file via the FFmpeg backend and update the draft. */
    fun probe(uri: String): String = CapTrimNative.nativeProbe(requireOpen(), uri)

    /** Inject metadata obtained from a platform decoder (MediaCodec etc.). */
    fun setMediaInfo(mediaId: Long, infoJson: String) {
        CapTrimNative.nativeSetMediaInfo(requireOpen(), mediaId, infoJson)
    }

    /**
     * P0 export: lossless remux. Runs on the calling thread; invoke from a
     * background dispatcher. P1 will move export to a dedicated worker and
     * report progress through a thread-safe callback.
     */
    fun exportRemux(srcUri: String, dstUri: String, progress: ProgressCallback? = null) {
        CapTrimNative.nativeExportRemux(requireOpen(), srcUri, dstUri, progress)
    }

    override fun close() {
        if (handle != 0L) {
            CapTrimNative.nativeFree(handle)
            handle = 0L
        }
    }
}
