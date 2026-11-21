package io.captrim.selfcheck

import android.app.Activity
import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.PorterDuff
import android.graphics.PorterDuffXfermode
import android.graphics.Typeface
import android.os.Bundle
import android.util.Log
import android.view.WindowManager
import android.widget.ScrollView
import android.widget.TextView
import io.captrim.CapTrim
import io.captrim.CapTrimException
import io.captrim.ProgressCallback
import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.io.FileOutputStream
import java.util.Locale

// Kotlin/JNI layer self-check, the Android twin of platforms/ohos
// SelfCheck.ets. Everything here goes through the public CapTrim.kt API —
// the exact path a host app uses — so a pass proves the AAR wiring
// (System.loadLibrary -> RegisterNatives -> Rust core -> FFmpeg C shim),
// not just the native libraries.
//
// Results: logcat tag CapTrimSelfCheck, on-screen scroll view, and
// files/selfcheck.txt (readable via `adb exec-out run-as ... cat`).

private const val TAG = "CapTrimSelfCheck"

private const val TOTAL_US = 9_000_000L
private const val CLIP_LEN = 4_000_000L

private data class CheckResult(val name: String, val pass: Boolean, val detail: String)

private fun expect(condition: Boolean, message: String) {
    if (!condition) throw AssertionError(message)
}

private fun mb(bytes: Long): String =
    String.format(Locale.US, "%.2f", bytes / 1_000_000.0)

private fun clipOf(source: Long, inUs: Long, outUs: Long, startUs: Long): JSONObject =
    JSONObject()
        .put("id", 0)
        .put("source", source)
        .put("in_us", inUs)
        .put("out_us", outUs)
        .put("start_us", startUs)
        .put("speed", JSONObject().put("num", 1).put("den", 1))
        .put("volume", 1)
        .put(
            "transform",
            JSONObject().put("scale", 1).put("dx", 0).put("dy", 0).put("rotation_deg", 0),
        )
        .put("effects", JSONArray())

private fun mediaInfo(uri: String, durationUs: Long): String {
    val video = JSONObject()
        .put("index", 0)
        .put("kind", "video")
        .put("codec", "h264")
        .put("time_base", JSONObject().put("num", 1).put("den", 1000))
        .put("duration_us", durationUs)
        .put("width", 1920)
        .put("height", 1080)
        .put("frame_rate", JSONObject().put("num", 30).put("den", 1))
        .put("sample_rate", JSONObject.NULL)
        .put("channels", JSONObject.NULL)
        .put("rotation_deg", 0)
        .put("bitrate_bps", 8_000_000)
    val audio = JSONObject()
        .put("index", 1)
        .put("kind", "audio")
        .put("codec", "aac")
        .put("time_base", JSONObject().put("num", 1).put("den", 1000))
        .put("duration_us", durationUs)
        .put("width", JSONObject.NULL)
        .put("height", JSONObject.NULL)
        .put("frame_rate", JSONObject.NULL)
        .put("sample_rate", 44100)
        .put("channels", 2)
        .put("rotation_deg", 0)
        .put("bitrate_bps", 128_000)
    return JSONObject()
        .put("uri", uri)
        .put("format", "mp4")
        .put("duration_us", durationUs)
        .put("bitrate_bps", 8_128_000)
        .put("streams", JSONArray().put(video).put(audio))
        .toString()
}

/** A three-clip, two-track draft summing to exactly nine seconds. */
private class Fixture : AutoCloseable {
    val editor: CapTrim = CapTrim.create()
    val firstClip: Long

    init {
        val a = editor.addMedia("file:///data/captrim/fixture-a.mp4")
        val b = editor.addMedia("file:///data/captrim/fixture-b.mp4")
        editor.setMediaInfo(a, mediaInfo("file:///data/captrim/fixture-a.mp4", 10_000_000))
        editor.setMediaInfo(b, mediaInfo("file:///data/captrim/fixture-b.mp4", 8_000_000))
        val mainTrack = editor.apply("""{"type":"add_track","kind":"main_video"}""")!!
        val overlayTrack = editor.apply("""{"type":"add_track","kind":"overlay"}""")!!
        firstClip = editor.apply(
            JSONObject()
                .put("type", "insert_clip")
                .put("track_id", mainTrack)
                .put("clip", clipOf(a, 0, CLIP_LEN, 0))
                .toString(),
        )!!
        editor.apply(
            JSONObject()
                .put("type", "insert_clip")
                .put("track_id", mainTrack)
                .put("clip", clipOf(b, 0, 2_000_000, CLIP_LEN))
                .toString(),
        )
        editor.apply(
            JSONObject()
                .put("type", "insert_clip")
                .put("track_id", overlayTrack)
                .put("clip", clipOf(a, 0, 3_000_000, 6_000_000))
                .toString(),
        )
    }

    fun clipCount(): Int {
        val tracks = JSONObject(editor.draft()).getJSONArray("tracks")
        var total = 0
        for (i in 0 until tracks.length()) {
            total += tracks.getJSONObject(i).getJSONArray("clips").length()
        }
        return total
    }

    fun setCanvas(width: Int, height: Int, num: Int, den: Int) {
        editor.apply(
            JSONObject()
                .put("type", "set_canvas")
                .put("width", width)
                .put("height", height)
                .put("frame_rate", JSONObject().put("num", num).put("den", den))
                .toString(),
        )
    }

    override fun close() = editor.close()
}

private class Runner(private val filesDir: String) {
    val results = mutableListOf<CheckResult>()

    fun check(name: String, body: () -> String) {
        val result = try {
            CheckResult(name, true, body())
        } catch (t: Throwable) {
            CheckResult(name, false, t.message ?: t.toString())
        }
        results.add(result)
        Log.i(TAG, "${if (result.pass) "PASS" else "FAIL"} ${result.name} | ${result.detail}")
    }

    private fun src(): String = "file://$filesDir/a.mp4"

    private fun delete(name: String) {
        File(filesDir, name).delete()
    }

    /**
     * A ring on a transparent background, drawn with the platform's own canvas
     * and encoded as PNG.
     *
     * The core decodes it in Rust (the same bytes the device would hand a
     * user-picked sticker), and the hole in the middle is what makes a
     * flattened alpha obvious: it would come out as a solid orange square.
     */
    private fun writeSticker(): File {
        val file = File(filesDir, "sticker.png")
        if (file.length() > 0) return file
        val bitmap = Bitmap.createBitmap(256, 256, Bitmap.Config.ARGB_8888)
        val canvas = Canvas(bitmap)
        val paint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
            color = Color.rgb(255, 122, 69)
        }
        canvas.drawCircle(128f, 128f, 118f, paint)
        paint.color = Color.TRANSPARENT
        paint.xfermode = PorterDuffXfermode(PorterDuff.Mode.CLEAR)
        canvas.drawCircle(128f, 128f, 64f, paint)
        FileOutputStream(file).use { bitmap.compress(Bitmap.CompressFormat.PNG, 100, it) }
        bitmap.recycle()
        expect(file.length() > 0, "贴纸 PNG 没有写出来")
        return file
    }

    /** Ink statistics of a PAM the core wrote: alpha coverage and white fill. */
    private data class Ink(val total: Int, val bright: Int, val partial: Int)

    /**
     * Count ink in a PAM (P7, RGB_ALPHA). Reading it back in the app is the
     * half FFmpeg cannot report: the pixels the overlay will consume.
     */
    private fun pamInk(path: File): Ink {
        val bytes = path.readBytes()
        val tag = "ENDHDR\n".toByteArray(Charsets.US_ASCII)
        var headerEnd = -1
        for (i in 0..bytes.size - tag.size) {
            var match = true
            for (j in tag.indices) {
                if (bytes[i + j] != tag[j]) {
                    match = false
                    break
                }
            }
            if (match) {
                headerEnd = i + tag.size
                break
            }
        }
        expect(headerEnd > 0, "${path.name} 没有 PAM 头")
        var total = 0
        var bright = 0
        var partial = 0
        var p = headerEnd
        while (p + 3 < bytes.size) {
            val alpha = bytes[p + 3].toInt() and 0xFF
            if (alpha != 0) {
                total++
                if (alpha < 255) partial++
                if ((bytes[p].toInt() and 0xFF) > 200 &&
                    (bytes[p + 1].toInt() and 0xFF) > 200 &&
                    (bytes[p + 2].toInt() and 0xFF) > 200
                ) {
                    bright++
                }
            }
            p += 4
        }
        return Ink(total, bright, partial)
    }

    /** Subtitles and a sticker on a 540x960 canvas: the burn fixture. */
    private fun burnFixture(engine: CapTrim): Pair<Long, Long> {
        engine.apply(
            """{"type":"set_canvas","width":540,"height":960,"frame_rate":{"num":30,"den":1}}""",
        )
        val subtitleTrack = engine.apply("""{"type":"add_track","kind":"subtitle"}""")!!
        val caption = engine.apply(
            JSONObject()
                .put("type", "insert_text_clip")
                .put("track_id", subtitleTrack)
                .put("text", "字幕烧录 · Overlay")
                .put("start_us", 400_000)
                .put("duration_us", 1_200_000)
                .toString(),
        )!!
        val imageTrack = engine.apply("""{"type":"add_track","kind":"image"}""")!!
        val sticker = writeSticker()
        val still = engine.apply(
            JSONObject()
                .put("type", "insert_image_clip")
                .put("track_id", imageTrack)
                .put("uri", "file://${sticker.absolutePath}")
                .put("start_us", 400_000)
                .put("duration_us", 1_200_000)
                .toString(),
        )!!
        // Top-right, a quarter size: clear of the caption, and the same
        // placement the OHOS self-check uses.
        engine.apply(
            """{"type":"set_transform","clip_id":$still,""" +
                """"transform":{"scale":0.25,"dx":0.3,"dy":-0.35,"rotation_deg":0}}""",
        )
        return Pair(caption, still)
    }

    fun runAll() {
        check("原生库加载与 JNI 绑定") {
            val version = CapTrim.version
            expect(version.isNotEmpty(), "version() 为空，so 已加载但符号绑定可疑")
            "libcaptrim $version"
        }

        // Which licence profile this build is: the GPL one carries libx264 /
        // libx265, the LGPL one (the only one a closed-source SDK may ship)
        // carries neither and must say so rather than fail at encode time.
        check("软件编码器清单（许可 profile 自报）") {
            val encoders = CapTrim.encoders
            expect(encoders.isNotEmpty(), "encoders() 为空：FFmpeg 构建没有任何软件编码器")
            val x264 = CapTrim.hasEncoder("libx264")
            val x265 = CapTrim.hasEncoder("libx265")
            val profile = if (x264 || x265) "GPL" else "LGPL"
            "$profile profile，可用: $encoders"
        }

        check("时间线构建（2 轨 3 片段）") {
            Fixture().use { fx ->
                expect(fx.clipCount() == 3, "片段数应为 3，实际 ${fx.clipCount()}")
                expect(
                    fx.editor.duration() == TOTAL_US,
                    "时长应为 ${TOTAL_US}us，实际 ${fx.editor.duration()}us",
                )
                expect(
                    fx.editor.frameCount() == 270L,
                    "30fps 下帧数应为 270，实际 ${fx.editor.frameCount()}",
                )
                "9.000s / 270 帧"
            }
        }

        check("分割 / 撤销 / 重做") {
            Fixture().use { fx ->
                fx.editor.apply("""{"type":"split_clip","clip_id":${fx.firstClip},"at_us":2000000}""")
                expect(fx.clipCount() == 4, "分割后应 4 段，实际 ${fx.clipCount()}")
                expect(fx.editor.duration() == TOTAL_US, "分割不应改变总时长")
                fx.editor.undo()
                expect(fx.clipCount() == 3, "撤销后应 3 段，实际 ${fx.clipCount()}")
                fx.editor.redo()
                expect(fx.clipCount() == 4, "重做后应 4 段，实际 ${fx.clipCount()}")
                "4 → 3 → 4"
            }
        }

        check("29.97fps 帧栅格不塌陷") {
            Fixture().use { fx ->
                fx.setCanvas(1920, 1080, 30000, 1001)
                val count = fx.editor.frameCount()
                expect(count == 270L, "9s@29.97 应 270 帧，实际 $count")
                var previous = -1L
                for (i in 0 until count) {
                    val pts = JSONObject(fx.editor.planFrameAt(i)).getLong("t_us")
                    expect(pts > previous, "第 $i 帧 PTS $pts 未递增（上一帧 $previous）")
                    // 1us of slack: the C ABI crosses JSON, not i128 maths.
                    val expected = i * 1001 * 1_000_000L / 30000
                    expect(
                        kotlin.math.abs(pts - expected) <= 1,
                        "第 $i 帧 PTS $pts 与整数公式 $expected 不符",
                    )
                    previous = pts
                }
                "270 帧全部严格递增，最大误差 ≤1us"
            }
        }

        check("逐帧源时间唯一（不重复帧）") {
            Fixture().use { fx ->
                fx.setCanvas(1920, 1080, 30000, 1001)
                var previous = -1L
                for (i in 0 until 60L) {
                    val plan = JSONObject(fx.editor.planFrameAt(i))
                    val layers = plan.getJSONArray("layers")
                    expect(layers.length() > 0, "第 $i 帧没有图层")
                    val source = layers.getJSONObject(0).getLong("source_us")
                    expect(
                        source > previous,
                        "第 $i 帧源时间 $source 未递增（上一帧 $previous），导出会重复帧",
                    )
                    previous = source
                }
                "前 60 帧源时间严格递增"
            }
        }

        check("预览时钟与丢帧统计") {
            Fixture().use { fx ->
                fx.editor.preview().use { preview ->
                    var now = 1_000_000L
                    preview.seek(0, now)
                    preview.setPlaying(true, now)
                    var presents = 0
                    var waits = 0
                    repeat(90) {
                        now += 16_666
                        when (JSONObject(preview.step(now)).getString("kind")) {
                            "present" -> presents++
                            "wait_frame" -> waits++
                        }
                    }
                    val position = preview.position(now)
                    expect(presents > 25, "1.5s 内只呈现 $presents 帧，渲染循环没有推进")
                    expect(
                        position in 1_400_001L..1_599_999L,
                        "位置应约 1.5s，实际 ${position}us",
                    )
                    "呈现 $presents 次 / 等待 $waits 次 / 位置 ${position / 1000}ms / " +
                        "丢帧 ${preview.droppedFrames()}"
                }
            }
        }

        check("编辑后自动重绘") {
            Fixture().use { fx ->
                fx.editor.preview().use { preview ->
                    var now = 500_000L
                    preview.seek(1_000_000, now)
                    expect(
                        JSONObject(preview.step(now)).getString("kind") == "present",
                        "首帧应为 present",
                    )
                    expect(
                        JSONObject(preview.step(now)).getString("kind") != "present",
                        "同一时刻不应重复呈现",
                    )
                    fx.editor.apply(
                        JSONObject()
                            .put("type", "set_volume")
                            .put("clip_id", fx.firstClip)
                            .put("volume", 0.5)
                            .toString(),
                    )
                    now += 1000
                    expect(
                        JSONObject(preview.step(now)).getString("kind") == "present",
                        "改动后应重绘",
                    )
                    "同帧不重复，改动后立即重绘"
                }
            }
        }

        check("音频主导时钟校正") {
            Fixture().use { fx ->
                fx.editor.preview().use { preview ->
                    val now = 2_000_000L
                    preview.seek(0, now)
                    preview.setPlaying(true, now)
                    // 300ms of playback predicts ~300ms; the audio renderer
                    // reports 350ms, so the clock must absorb the 50ms gap.
                    val drift = preview.syncAudio(350_000, now + 300_000)
                    expect(drift == 50_000L, "漂移应为 50000us，实际 ${drift}us")
                    "漂移 ${drift}us 已被校正"
                }
            }
        }

        check("错误码可读") {
            val ok = CapTrim.codeMessage(0)
            expect(ok.isNotEmpty(), "codeMessage(0) 为空")
            var failed = false
            val engine = CapTrim.create()
            try {
                engine.apply("""{"type":"split_clip","clip_id":9999,"at_us":0}""")
            } catch (e: CapTrimException) {
                failed = true
            } finally {
                engine.close()
            }
            expect(failed, "对不存在的片段分割本应失败，却静默成功了")
            """0 → "$ok"，未知片段正确报错"""
        }

        check("滤镜 v1（effect 位 + filter_desc 翻译）") {
            // boxblur / eq are GPL filters *inside* FFmpeg, so an LGPL build
            // must answer -11 naming the effect rather than hand back a chain
            // libavfilter cannot parse. Same ABI, same call, profile-dependent
            // answer — which is exactly what the capability table is for.
            val base = CapTrim.effectsToFilterDesc("invert", "hflip")
            expect(base == "hflip,negate", "canonical order 翻译错误：$base")
            val empty = CapTrim.effectsToFilterDesc()
            expect(empty.isEmpty(), "空 effects 应翻译为空串，实际 \"$empty\"")

            var chain = "unsupported"
            try {
                chain = CapTrim.effectsToFilterDesc("blur", "invert", "hflip")
                expect(chain == "hflip,negate,boxblur=2:1",
                    "canonical order 翻译错误：$chain")
            } catch (e: CapTrimException) {
                expect(e.code == -11, "缺少 boxblur 时应报 -11，实际 ${e.code}: ${e.message}")
                expect(e.message!!.contains("blur") && e.message!!.contains("available effects"),
                    "错误信息应点名 effect 与可用项，实际 \"${e.message}\"")
            }
            "hflip,negate 恒定；blur → $chain"
        }

        check("字幕与贴图数据层（图层种类 + 草稿往返）") {
            CapTrim.create().use { engine ->
                val (captionId, stillId) = burnFixture(engine)
                // t = 1.0s sits inside both windows (0.4s..1.6s).
                val plan = JSONObject(engine.planFrameAt(30))
                val layers = plan.getJSONArray("layers")
                // planFrameAt carries the kind as its name ("subtitle", "image"),
                // not the C enum number.
                val kinds = mutableSetOf<String>()
                for (i in 0 until layers.length()) {
                    kinds.add(layers.getJSONObject(i).getString("kind"))
                }
                expect("subtitle" in kinds, "帧计划里没有字幕层：$kinds")
                expect("image" in kinds, "帧计划里没有图片层：$kinds")

                // The draft has to carry both payloads, or a reloaded session
                // would lose the sticker's file and its intrinsic size.
                val tracks = JSONObject(engine.draft()).getJSONArray("tracks")
                var captionText: String? = null
                var imageSize: String? = null
                for (t in 0 until tracks.length()) {
                    val clips = tracks.getJSONObject(t).getJSONArray("clips")
                    for (c in 0 until clips.length()) {
                        val clip = clips.getJSONObject(c)
                        if (clip.getLong("id") == captionId) captionText = clip.getString("text")
                        if (clip.getLong("id") == stillId) {
                            val image = clip.getJSONObject("image")
                            imageSize = "${image.getInt("width")}x${image.getInt("height")}"
                        }
                    }
                }
                expect(captionText == "字幕烧录 · Overlay", "草稿里的字幕文本丢了：$captionText")
                expect(imageSize == "256x256", "草稿里的贴图尺寸不对：$imageSize")
                "图层 1/3 都在，草稿保留文本与 256x256 贴图尺寸"
            }
        }

        check("字幕与贴图烧录（PAM + overlay）") {
            CapTrim.create().use { engine ->
                val dst = "$filesDir/burn.mp4"
                val base = "$filesDir/burn-base.mp4"
                val workDir = File(filesDir, "burn")
                delete("burn.mp4")
                delete("burn-base.mp4")
                // A stale directory would make the PAM counts describe an
                // earlier run with different clip ids.
                workDir.deleteRecursively()
                workDir.mkdirs()
                burnFixture(engine)

                val prefix = "trim=duration=2,setpts=PTS-STARTPTS," +
                    "scale=540:960:force_original_aspect_ratio=decrease," +
                    "pad=540:960:(ow-iw)/2:(oh-ih)/2"
                val ticks = mutableListOf<Float>()
                engine.exportBurnSubtitles(
                    src(), dst, prefix, "fps=15", workDir.absolutePath, "",
                    ProgressCallback { v -> ticks.add(v); true },
                )
                val burned = File(dst).length()
                expect(burned > 0, "烧录产物为空")

                // Same picture pipeline without the layers: if the overlays did
                // anything the encoded result cannot match.
                engine.exportTranscode(src(), base, "$prefix,fps=15", "",
                    ProgressCallback { _ -> true })
                val plain = File(base).length()
                expect(plain > 0, "基线产物为空")
                expect(burned != plain, "烧录前后文件大小相同 ($burned bytes)，overlay 没生效")

                val outVideo = firstVideo(JSONObject(engine.probe("file://$dst")))
                expect(outVideo.getInt("width") == 540 && outVideo.getInt("height") == 960,
                    "分辨率应为 540x960，实际 ${outVideo.getInt("width")}x${outVideo.getInt("height")}")

                val pams = workDir.listFiles()?.filter { it.name.endsWith(".pam") } ?: emptyList()
                val captions = pams.filter { it.name.startsWith("sub_") }
                val stills = pams.filter { it.name.startsWith("img_") }
                expect(captions.size == 1, "应渲染 1 张字幕 PAM，实际 ${captions.size}")
                expect(stills.size == 1, "应渲染 1 张贴图 PAM，实际 ${stills.size}")

                val captionInk = pamInk(captions[0])
                expect(captionInk.total > 500, "字幕 PAM 只有 ${captionInk.total} 个墨迹像素")
                expect(captionInk.bright > 300, "字幕 PAM 只有 ${captionInk.bright} 个白色像素")

                // 256x256 fitted into 540x960 (2.1x) then drawn at 0.25 => 135px;
                // the ring covers ~47% of that. Partial alpha at its edge is what
                // proves the PNG's alpha survived decode and resample.
                val stillInk = pamInk(stills[0])
                expect(stillInk.total > 3000, "贴图 PAM 只有 ${stillInk.total} 个墨迹像素")
                expect(stillInk.partial > 200,
                    "贴图 PAM 只有 ${stillInk.partial} 个半透明边缘像素，alpha 可能被压平")

                "540x960 burn ${mb(burned)}MB vs base ${mb(plain)}MB，PAM ${pams.size} 张 " +
                    "(字幕 ${captionInk.bright} 白 / ${captionInk.total} 墨迹，贴图 " +
                    "${stillInk.total} 墨迹 / ${stillInk.partial} 半透明)，进度 ${ticks.size} 次"
            }
        }

        check("转场期音频交叉渐变（导出计划增益互补）") {
            CapTrim.create().use { engine ->
                val media = engine.addMedia(src())
                engine.setMediaInfo(media, mediaInfo(src(), 22_000_000))
                val track = engine.apply("""{"type":"add_track","kind":"main_video"}""")!!
                val outgoing = engine.apply(
                    JSONObject()
                        .put("type", "insert_clip")
                        .put("track_id", track)
                        .put("clip", clipOf(media, 0, 2_000_000, 0))
                        .toString(),
                )!!
                val incoming = engine.apply(
                    JSONObject()
                        .put("type", "insert_clip")
                        .put("track_id", track)
                        .put("clip", clipOf(media, 2_000_000, 4_000_000, 2_000_000))
                        .toString(),
                )!!
                // Half a second of dissolve: the window is [2.0s, 2.5s).
                engine.apply(
                    """{"type":"set_transition","clip_id":$incoming,""" +
                        """"transition":{"kind":"fade","duration_us":500000}}""",
                )

                val segments = JSONObject(engine.exportPlan()).getJSONArray("segments")
                var ramp: JSONObject? = null
                for (i in 0 until segments.length()) {
                    val segment = segments.getJSONObject(i)
                    if (segment.getLong("start_frame") == 60L) {
                        ramp = segment
                        break
                    }
                }
                expect(ramp != null, "导出计划里没有从第 60 帧开始的转场段")
                val audio = ramp!!.getJSONArray("audio")
                expect(audio.length() == 2, "转场段应有 2 个音频贡献，实际 ${audio.length()}")

                val old = audio.getJSONObject(0)
                val new = audio.getJSONObject(1)
                expect(old.getLong("clip_id") == outgoing, "第 1 个贡献应是被替换的片段")
                expect(new.getLong("clip_id") == incoming, "第 2 个贡献应是新片段")
                // Starts complementary, and both ramp the other way.
                val oldStart = old.getDouble("volume")
                val newStart = new.getDouble("volume")
                expect(kotlin.math.abs(oldStart + newStart - 1.0) < 1e-3,
                    "窗口起点增益应为 1：$oldStart + $newStart")
                expect(old.getDouble("volume_end") < oldStart, "旧片段增益应递减")
                expect(new.getDouble("volume_end") > newStart, "新片段增益应递增")

                // Mid-window both are audible and still sum to one: a crossfade,
                // not a gap and not a doubled mix.
                val midOld = oldStart + (old.getDouble("volume_end") - oldStart) * 0.5
                val midNew = newStart + (new.getDouble("volume_end") - newStart) * 0.5
                expect(midOld > 0.3 && midNew > 0.3,
                    "窗口中点两边都应可闻：$midOld / $midNew")
                expect(kotlin.math.abs(midOld + midNew - 1.0) < 1e-3,
                    "窗口中点增益和应为 1：$midOld + $midNew")

                // The tail reads past its out point instead of freezing on one
                // sample; the incoming one starts at its own in point.
                expect(old.getLong("source_end_us") > 2_000_000,
                    "旧片段尾部应继续读源：${old.getLong("source_end_us")}")
                expect(new.getLong("source_start_us") == 2_000_000L,
                    "新片段应从 in 点开始读取：${new.getLong("source_start_us")}")

                String.format(
                    Locale.US,
                    "60..%d 帧双贡献，旧 %.2f→%.2f / 新 %.2f→%.2f",
                    ramp!!.getLong("end_frame"),
                    oldStart, old.getDouble("volume_end"),
                    newStart, new.getDouble("volume_end"),
                )
            }
        }

        // ---- FFmpeg 软件后端 ------------------------------------------------
        check("FFmpeg 探测（软后端 probe）") {
            CapTrim.create().use { engine ->
                val info = JSONObject(engine.probe(src()))
                val streams = info.getJSONArray("streams")
                expect(streams.length() > 0, "probe 未返回任何流")
                val kinds = (0 until streams.length())
                    .joinToString(",") { streams.getJSONObject(it).getString("kind") }
                val dur = info.getLong("duration_us") / 1_000_000.0
                String.format(Locale.US, "format=%s dur=%.2fs streams=%d[%s]",
                    info.getString("format"), dur, streams.length(), kinds)
            }
        }

        check("FFmpeg 重封装（软后端 remux）") {
            CapTrim.create().use { engine ->
                val dst = "$filesDir/remuxed.mp4"
                delete("remuxed.mp4")
                engine.exportRemux(src(), dst)
                val stat = File(dst)
                expect(stat.length() > 0, "remux 输出为空 (${stat.length()} bytes)")
                val info = JSONObject(engine.probe("file://$dst"))
                val streams = info.getJSONArray("streams")
                expect(streams.length() > 0, "remux 产物无法被 probe")
                "写回 ${mb(stat.length())}MB，复探 ${streams.length()} 路流"
            }
        }

        check("FFmpeg 复杂剪辑（滤镜+H.264 重编码）") {
            // "" means "whatever software H.264 encoder this build has" (libx264
            // in the GPL profile). An LGPL profile has none, so say so up front.
            if (!CapTrim.hasEncoder("libx264") && !CapTrim.hasEncoder("libopenh264")) {
                return@check "跳过：本构建无软件 H.264 编码器（LGPL profile）"
            }
            CapTrim.create().use { engine ->
                val dst = "$filesDir/edited.mp4"
                delete("edited.mp4")
                val before = JSONObject(engine.probe(src()))
                val srcVideo = firstVideo(before)
                val ticks = mutableListOf<Float>()
                // 2s only: keeps the synchronous run comfortably short.
                engine.exportTranscode(
                    src(), dst,
                    "trim=duration=2,setpts=PTS-STARTPTS,scale=320:180,hflip,fps=15",
                    "",
                    ProgressCallback { v -> ticks.add(v); true },
                )
                val stat = File(dst)
                expect(stat.length() > 0, "转码输出为空 (${stat.length()} bytes)")
                expect(ticks.size >= 20, "进度回调次数过少 (${ticks.size})")
                expect(ticks.last() > ticks.first(), "进度没有推进")
                val outVideo = firstVideo(JSONObject(engine.probe("file://$dst")))
                expect(outVideo.getString("codec") == "h264",
                    "编码器应为 h264，实际 ${outVideo.getString("codec")}")
                expect(outVideo.getInt("width") == 320 && outVideo.getInt("height") == 180,
                    "分辨率应为 320x180，实际 ${outVideo.getInt("width")}x${outVideo.getInt("height")}")
                expect(srcVideo.getInt("width") != 320, "测试无效：源片本来就是 320 宽")
                "${srcVideo.getInt("width")}x${srcVideo.getInt("height")}→320x180 " +
                    "codec=${outVideo.getString("codec")} fps≈15 ${mb(stat.length())}MB " +
                    "进度 ${ticks.size} 次"
            }
        }

        // Real x265: the loader-stub era could fake a green native self-check,
        // never a re-probed hevc bitstream.
        check("FFmpeg 复杂剪辑（libx265 HEVC）") {
            // libx265 only exists in the GPL profile. The LGPL profile has to
            // say so instead of failing, otherwise a licensed build looks broken.
            if (!CapTrim.hasEncoder("libx265")) return@check "跳过：本构建无 libx265（LGPL profile）"
            CapTrim.create().use { engine ->
                val dst = "$filesDir/x265.mp4"
                delete("x265.mp4")
                val ticks = mutableListOf<Float>()
                engine.exportTranscode(
                    src(), dst,
                    "trim=duration=2,setpts=PTS-STARTPTS,scale=320:180,hflip,fps=15",
                    "libx265",
                    ProgressCallback { v -> ticks.add(v); true },
                )
                val stat = File(dst)
                expect(stat.length() > 0, "x265 转码输出为空 (${stat.length()} bytes)")
                expect(ticks.isNotEmpty(), "x265 进度回调未触发")
                val outVideo = firstVideo(JSONObject(engine.probe("file://$dst")))
                expect(outVideo.getString("codec") == "hevc",
                    "编码器应为 hevc，实际 ${outVideo.getString("codec")}")
                expect(outVideo.getInt("width") == 320 && outVideo.getInt("height") == 180,
                    "分辨率应为 320x180，实际 ${outVideo.getInt("width")}x${outVideo.getInt("height")}")
                "x265 真实编码 ${mb(stat.length())}MB codec=hevc 进度 ${ticks.size} 次"
            }
        }

        // ---- 时间线导出（M1）------------------------------------------------
        // The end-to-end M1 check: one call turns a real multi-clip timeline
        // into one file. Everything the compiler emits has to work on the
        // DEVICE FFmpeg build (concat / color / amix / atrim / atempo / fade /
        // tpad / movie / loop / pam ...), which a host ffmpeg.exe cannot prove.
        check("时间线导出（三段拼接 + 画中画 + 字幕贴图 + 双音轨）") {
            CapTrim.create().use { engine ->
                val dst = "$filesDir/timeline.mp4"
                delete("timeline.mp4")
                // Second source for the concat + picture-in-picture: derived
                // from the pushed sample so the check stays self-contained.
                if (File(filesDir, "b.mp4").length() == 0L) {
                    engine.exportRemux(src(), "file://$filesDir/b.mp4", null)
                }
                expect(File(filesDir, "b.mp4").length() > 0, "b.mp4 素材没有生成")
                val a = engine.addMedia(src())
                val b = engine.addMedia("file://$filesDir/b.mp4")
                engine.setMediaInfo(a, mediaInfo(src(), 10_000_000))
                engine.setMediaInfo(b, mediaInfo("file://$filesDir/b.mp4", 8_000_000))
                engine.apply(
                    """{"type":"set_canvas","width":540,"height":960,""" +
                        """"frame_rate":{"num":30,"den":1}}""",
                )
                val main = engine.apply("""{"type":"add_track","kind":"main_video"}""")!!
                val overlay = engine.apply("""{"type":"add_track","kind":"overlay"}""")!!
                engine.apply(
                    JSONObject().put("type", "insert_clip").put("track_id", main)
                        .put("clip", clipOf(a, 0, 2_000_000, 0)).toString(),
                )
                engine.apply(
                    JSONObject().put("type", "insert_clip").put("track_id", main)
                        .put("clip", clipOf(b, 0, 2_000_000, 2_000_000)).toString(),
                )
                engine.apply(
                    JSONObject().put("type", "insert_clip").put("track_id", main)
                        .put("clip", clipOf(a, 2_000_000, 4_000_000, 4_000_000)).toString(),
                )
                val pip = engine.apply(
                    JSONObject().put("type", "insert_clip").put("track_id", overlay)
                        .put("clip", clipOf(b, 0, 6_000_000, 0)).toString(),
                )!!
                engine.apply(
                    """{"type":"set_transform","clip_id":$pip,""" +
                        """"transform":{"scale":0.25,"dx":0.3,"dy":-0.35,"rotation_deg":0}}""",
                )
                // Stills ride in through the same graph: the core rasterises
                // the caption and the sticker into filesDir and overlays them
                // in their windows, so this check also covers pam/movie/loop
                // on the device build.
                val subtitleTrack = engine.apply("""{"type":"add_track","kind":"subtitle"}""")!!
                engine.apply(
                    JSONObject().put("type", "insert_text_clip").put("track_id", subtitleTrack)
                        .put("text", "字幕随片导出")
                        .put("start_us", 500_000).put("duration_us", 3_000_000).toString(),
                )
                val imageTrack = engine.apply("""{"type":"add_track","kind":"image"}""")!!
                val sticker = writeSticker()
                val badge = engine.apply(
                    JSONObject().put("type", "insert_image_clip").put("track_id", imageTrack)
                        .put("uri", "file://${sticker.absolutePath}")
                        .put("start_us", 1_000_000).put("duration_us", 3_000_000).toString(),
                )!!
                engine.apply(
                    """{"type":"set_transform","clip_id":$badge,""" +
                        """"transform":{"scale":0.15,"dx":0.35,"dy":-0.35,"rotation_deg":0}}""",
                )
                expect(engine.duration() == 6_000_000L,
                    "时间线应为 6.000s，实际 ${engine.duration()}us")
                val ticks = mutableListOf<Float>()
                // Stills need a writable directory; the app's files dir has no
                // character the filter graph cannot carry.
                engine.exportTimeline(dst, filesDir, "", 22, "veryfast", 160,
                    ProgressCallback { v -> ticks.add(v); true })
                val stat = File(dst)
                expect(stat.length() > 0, "导出产物为空 (${stat.length()} bytes)")
                expect(ticks.size >= 30, "进度回调次数过少 (${ticks.size})")
                expect(ticks.last() > 0.9f, "进度未走到尾 (${ticks.last()})")
                val info = JSONObject(engine.probe("file://$dst"))
                val video = firstVideo(info)
                expect(video.getString("codec") == "h264",
                    "编码器应为 h264，实际 ${video.getString("codec")}")
                expect(video.getInt("width") == 540 && video.getInt("height") == 960,
                    "画布应为 540x960，实际 ${video.getInt("width")}x${video.getInt("height")}")
                val streams = info.getJSONArray("streams")
                var audioRate = 0
                var audioChannels = 0
                for (i in 0 until streams.length()) {
                    val s = streams.getJSONObject(i)
                    if (s.getString("kind") == "audio") {
                        audioRate = s.getInt("sample_rate")
                        audioChannels = s.getInt("channels")
                    }
                }
                expect(audioRate == 48000 && audioChannels == 2,
                    "音轨应为 48k 立体声，实际 ${audioRate}Hz/${audioChannels}ch")
                val duration = info.getLong("duration_us")
                expect(kotlin.math.abs(duration - 6_000_000L) <= 34_000L,
                    "时长应为 6.000s（±AAC 尾帧），实际 ${duration}us")
                "540x960 h264 / aac ${audioChannels}ch ${audioRate}Hz，" +
                    "${ticks.size} 次进度 至 ${ticks.last()}"
            }
        }

        // ---- 三段式管线 ------------------------------------------------------
        // The software baseline asks for whatever software encoder this build
        // carries: an LGPL build has no H.264 one, and mpeg4 still drives the
        // same decode→filter→encode stages, so the profile difference stays
        // visible instead of silently skipping the whole stage.
        check("三段式管线（软件基线 / 回退链路）") {
            pipelineRun("pipeline_sw.mp4", "hw:sw:${softwareEncoder()}")
        }

        // "hw:" prefers the AMediaCodec stage-1/3 factories (captrim_android_hw.c)
        // while stage 2 stays FFmpeg libavfilter; any stage failure degrades
        // per-stage, a streaming failure retries the whole job in software.
        check("三段式管线（硬解/硬编优先，失败自动回退）") {
            pipelineRun("pipeline_hw.mp4", "hw:libx264")
        }

        check("硬编产物可被 FFmpeg 软解（pipeline_hw.mp4）") {
            val codec = softwareEncoder()
            CapTrim.create().use { engine ->
                val dst = "$filesDir/verify.mp4"
                delete("verify.mp4")
                val ticks = mutableListOf<Float>()
                engine.exportTranscode(
                    "file://$filesDir/pipeline_hw.mp4", dst,
                    "scale=64:64", codec,
                    ProgressCallback { v -> ticks.add(v); true },
                )
                val stat = File(dst)
                expect(stat.length() > 0, "软解再编码产物为空")
                expect(ticks.size >= 2, "可解码帧数异常 (进度 ${ticks.size} 次)")
                "FFmpeg 软解→重编码($codec)成功，进度 ${ticks.size} 次"
            }
        }
    }

    /**
     * The software encoder this build really carries, for cases that must run on
     * both licence profiles. GPL: libx264. LGPL: mpeg4, the only software
     * encoder left once --enable-gpl is gone.
     */
    private fun softwareEncoder(): String = when {
        CapTrim.hasEncoder("libx264") -> "libx264"
        CapTrim.hasEncoder("libopenh264") -> "libopenh264"
        else -> "mpeg4"
    }

    private fun firstVideo(info: JSONObject): JSONObject {
        val streams = info.getJSONArray("streams")
        for (i in 0 until streams.length()) {
            val s = streams.getJSONObject(i)
            if (s.getString("kind") == "video") return s
        }
        throw AssertionError("no video stream")
    }

    private fun pipelineRun(dstName: String, codec: String): String =
        CapTrim.create().use { engine ->
            val dst = "$filesDir/$dstName"
            delete(dstName)
            val ticks = mutableListOf<Float>()
            engine.exportTranscode(
                src(), dst,
                "trim=duration=2,setpts=PTS-STARTPTS,scale=320:180,hflip,fps=15",
                codec,
                ProgressCallback { v -> ticks.add(v); true },
            )
            val stat = File(dst)
            expect(stat.length() > 0, "管线输出为空 (${stat.length()} bytes)")
            expect(ticks.isNotEmpty(), "管线进度回调未触发")
            val outVideo = firstVideo(JSONObject(engine.probe("file://$dst")))
            val expectH264 = codec.endsWith("libx264") || codec.endsWith("libopenh264")
            expect(outVideo.getString("codec") == (if (expectH264) "h264" else "mpeg4"),
                "编码器应为 ${if (expectH264) "h264" else "mpeg4"}，实际 ${outVideo.getString("codec")}")
            expect(outVideo.getInt("width") == 320 && outVideo.getInt("height") == 180,
                "分辨率应为 320x180，实际 ${outVideo.getInt("width")}x${outVideo.getInt("height")}")
            "decode→filter→encode ${mb(stat.length())}MB " +
                "codec=${outVideo.getString("codec")} 进度 ${ticks.size} 次"
        }
}

/** Close helper: Fixture and CapTrimPreview are AutoCloseable. */
private inline fun <T : AutoCloseable, R> T.use(block: (T) -> R): R {
    try {
        return block(this)
    } finally {
        close()
    }
}

class MainActivity : Activity() {

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)

        val view = TextView(this).apply {
            typeface = Typeface.MONOSPACE
            textSize = 12f
            setPadding(24, 24, 24, 24)
        }
        setContentView(ScrollView(this).apply { addView(view) })

        val dir = filesDir.absolutePath
        Thread {
            val runner = Runner(dir)
            runner.runAll()
            val passed = runner.results.count { it.pass }
            val summary = "== selfcheck ${passed}/${runner.results.size} passed =="
            val lines = runner.results.joinToString("\n") {
                "${if (it.pass) "PASS" else "FAIL"} ${it.name} | ${it.detail}"
            }
            Log.i(TAG, summary)
            try {
                File(dir, "selfcheck.txt").writeText("$lines\n$summary\n")
            } catch (e: Exception) {
                Log.w(TAG, "cannot write selfcheck.txt: $e")
            }
            runOnUiThread {
                view.text = runner.results.joinToString("\n\n") {
                    "${if (it.pass) "PASS" else "FAIL"} ${it.name}\n${it.detail}"
                } + "\n\n$summary"
            }
        }.start()
    }
}
