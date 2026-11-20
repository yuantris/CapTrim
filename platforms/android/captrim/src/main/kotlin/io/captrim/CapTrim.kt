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

    external fun nativeExportTranscode(
        handle: Long,
        srcUri: String,
        dstUri: String,
        filterDesc: String,
        videoCodec: String,
        callback: ProgressCallback?,
    ): Int

    external fun nativeExportBurnSubtitles(
        handle: Long,
        srcUri: String,
        dstUri: String,
        filterPrefix: String,
        filterSuffix: String,
        videoCodec: String,
        workDir: String,
        callback: ProgressCallback?,
    ): Int

    external fun nativeVersion(): String
    external fun nativeCodeMessage(code: Int): String
    external fun nativeLastError(): String

    external fun nativePlanFrameAt(handle: Long, frameIndex: Long): String
    external fun nativePlanFrame(handle: Long, tUs: Long): String
    external fun nativeExportPlan(handle: Long): String
    external fun nativeEffectsToFilterDesc(effectsJson: String): String
    external fun nativeDuration(handle: Long, out: LongArray): Int

    external fun nativePreviewNew(engine: Long): Long
    external fun nativePreviewFree(preview: Long)
    external fun nativePreviewSeek(preview: Long, tUs: Long, nowUs: Long): Int
    external fun nativePreviewSetPlaying(preview: Long, playing: Boolean, nowUs: Long): Int
    external fun nativePreviewStep(preview: Long, nowUs: Long): String
    external fun nativePreviewPosition(preview: Long, nowUs: Long, out: LongArray): Int
    external fun nativePreviewSyncAudio(
        preview: Long,
        audioUs: Long,
        nowUs: Long,
        out: LongArray,
    ): Int
    external fun nativePreviewStats(preview: Long, out: LongArray): Int
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

        /**
         * Translate an ExportVideoItem.effects list into the FFmpeg filter_desc
         * for exportTranscode(). The vocabulary and constants live in the Rust
         * core, so the AAR never duplicates them (and stays dependency-free).
         * Effect names are lowercase identifiers from the fixed v1 vocabulary,
         * so a JSON array needs no general-purpose escaping.
         */
        @JvmStatic
        fun effectsToFilterDesc(vararg effects: String): String {
            val json = effects.joinToString(",", "[", "]") { "\"$it\"" }
            return CapTrimNative.nativeEffectsToFilterDesc(json)
        }
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

    /**
     * Full transcode: decode -> FFmpeg filter graph -> re-encode -> mp4 mux.
     * Software backbone of the editing pipeline and fallback when a hardware
     * codec is unavailable. `filterDesc` is a libavfilter graph for the video
     * track (e.g. "scale=640:-2,fps=24,hflip"); empty keeps frames as-is.
     * `videoCodec` defaults to libx264 when empty. Audio is stream-copied.
     * Runs on the calling thread; use a background dispatcher.
     */
    fun exportTranscode(
        srcUri: String,
        dstUri: String,
        filterDesc: String = "",
        videoCodec: String = "",
        progress: ProgressCallback? = null,
    ) {
        CapTrimNative.nativeExportTranscode(
            requireOpen(), srcUri, dstUri, filterDesc, videoCodec, progress)
    }

    /**
     * Export with this timeline's subtitles burned into the picture.
     *
     * The core rasterises every subtitle clip into [workDir] (one .pam per
     * clip), builds the overlay graph and transcodes. [filterPrefix] runs
     * first and must trim the source to the timeline range, put the frame on
     * the timeline canvas and reset PTS to zero (`setpts=PTS-STARTPTS`);
     * [filterSuffix] runs after the last overlay.
     *
     * [workDir] must be an absolute cache directory whose path contains no
     * ':' , quotes, commas, semicolons, brackets or '%' — FFmpeg filter graphs
     * carry paths verbatim. Runs on the calling thread.
     */
    fun exportBurnSubtitles(
        srcUri: String,
        dstUri: String,
        filterPrefix: String,
        filterSuffix: String,
        workDir: String,
        videoCodec: String = "",
        progress: ProgressCallback? = null,
    ) {
        CapTrimNative.nativeExportBurnSubtitles(
            requireOpen(), srcUri, dstUri, filterPrefix, filterSuffix, videoCodec,
            workDir, progress)
    }

    /** Timeline length in microseconds. */
    fun duration(): Long = readDuration()[0]

    /** Total frames the exporter must write. */
    fun frameCount(): Long = readDuration()[1]

    private fun readDuration(): LongArray {
        val out = LongArray(2)
        CapTrimNative.nativeDuration(requireOpen(), out)
        return out
    }

    /**
     * Plan the frame covering [tUs], for scrubbing.
     *
     * Returns a FramePlan JSON document: `t_us`, `frame_index`, `width`,
     * `height`, `layers[]` (bottom-to-top, each with `clip_id`, `source_id`,
     * `uri`, `source_us`, `z`, `kind`, `transform`, `opacity`, `effects`) and
     * `audio[]` (`clip_id`, `source_id`, `uri`, `source_us`, `volume`).
     *
     * JSON is returned raw on purpose: the AAR stays dependency-free, and a
     * production renderer should call the same C ABI directly from C++ rather
     * than marshal JSON per frame. Parse it with whatever serializer the host
     * app already uses (kotlinx.serialization, Moshi, Gson).
     */
    fun planFrame(tUs: Long): String =
        CapTrimNative.nativePlanFrame(requireOpen(), tUs)

    /**
     * Plan one frame by index. This is the export path — do not walk a timeline
     * with [planFrame] using truncated PTS values, because flooring twice makes
     * NTSC sequences repeat frames.
     */
    fun planFrameAt(frameIndex: Long): String =
        CapTrimNative.nativePlanFrameAt(requireOpen(), frameIndex)

    /** Full export breakdown as an ExportPlan JSON string. */
    fun exportPlan(): String = CapTrimNative.nativeExportPlan(requireOpen())

    /** Open a playback session. Close any previous one first. */
    fun preview(): CapTrimPreview =
        CapTrimPreview(CapTrimNative.nativePreviewNew(requireOpen()))

    override fun close() {
        if (handle != 0L) {
            CapTrimNative.nativeFree(handle)
            handle = 0L
        }
    }
}

/**
 * Playback session: owns the clock and the "frame currently on screen" state.
 *
 * ```kotlin
 * val step = JSONObject(preview.step(monotonicUs()))
 * when (step.getString("kind")) {
 *     "present" -> render(step.getJSONObject("frame"))
 *     "wait_frame" -> sleepUntil(step.getLong("due_wall_us"))
 *     "idle" -> awaitUiEvent()
 *     "end" -> stop()
 * }
 * ```
 *
 * [nowUs] must come from the same monotonic source on every call. Never pass
 * `System.currentTimeMillis()`: a clock that goes backwards stalls playback and
 * one that jumps forwards drops the whole buffer. Use `System.nanoTime() / 1000`.
 *
 * The session notices edits to the draft on the next [step], so the current
 * frame repaints after a command without any extra call.
 */
class CapTrimPreview internal constructor(private var handle: Long) : AutoCloseable {

    fun seek(tUs: Long, nowUs: Long) {
        CapTrimNative.nativePreviewSeek(requireOpen(), tUs, nowUs)
    }

    /** Starting playback while parked on the last frame rewinds first. */
    fun setPlaying(playing: Boolean, nowUs: Long) {
        CapTrimNative.nativePreviewSetPlaying(requireOpen(), playing, nowUs)
    }

    /**
     * Next step as JSON: `{"kind":"present","frame":{...}}`,
     * `{"kind":"wait_frame","due_wall_us":N}`, `{"kind":"idle"}` or
     * `{"kind":"end","at_us":N}`.
     */
    fun step(nowUs: Long): String = CapTrimNative.nativePreviewStep(requireOpen(), nowUs)

    fun position(nowUs: Long): Long {
        val out = LongArray(1)
        CapTrimNative.nativePreviewPosition(requireOpen(), nowUs, out)
        return out[0]
    }

    /**
     * Re-anchor to the audio renderer's position, which is the master clock.
     * Returns the drift that was corrected: log it when a device looks out of
     * sync.
     */
    fun syncAudio(audioPositionUs: Long, nowUs: Long): Long {
        val out = LongArray(1)
        CapTrimNative.nativePreviewSyncAudio(requireOpen(), audioPositionUs, nowUs, out)
        return out[0]
    }

    /** Frames skipped because the render loop fell behind. */
    fun droppedFrames(): Long {
        val out = LongArray(1)
        CapTrimNative.nativePreviewStats(requireOpen(), out)
        return out[0]
    }

    override fun close() {
        if (handle != 0L) {
            CapTrimNative.nativePreviewFree(handle)
            handle = 0L
        }
    }

    private fun requireOpen(): Long = handle.also {
        check(it != 0L) { "CapTrim preview is closed" }
    }
}
