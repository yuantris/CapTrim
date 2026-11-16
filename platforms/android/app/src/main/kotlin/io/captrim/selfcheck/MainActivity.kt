package io.captrim.selfcheck

import android.app.Activity
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

    fun runAll() {
        check("原生库加载与 JNI 绑定") {
            val version = CapTrim.version
            expect(version.isNotEmpty(), "version() 为空，so 已加载但符号绑定可疑")
            "libcaptrim $version"
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

        check("FFmpeg 复杂剪辑（滤镜+libx264重编码）") {
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
                    "libx264",
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

        // ---- 三段式管线 ------------------------------------------------------
        check("三段式管线（软件基线 / 回退链路）") {
            pipelineRun("pipeline_sw.mp4", "hw:sw:libx264")
        }

        // "hw:" prefers the AMediaCodec stage-1/3 factories (captrim_android_hw.c)
        // while stage 2 stays FFmpeg libavfilter; any stage failure degrades
        // per-stage, a streaming failure retries the whole job in software.
        check("三段式管线（硬解/硬编优先，失败自动回退）") {
            pipelineRun("pipeline_hw.mp4", "hw:libx264")
        }

        check("硬编产物可被 FFmpeg 软解（pipeline_hw.mp4）") {
            CapTrim.create().use { engine ->
                val dst = "$filesDir/verify.mp4"
                delete("verify.mp4")
                val ticks = mutableListOf<Float>()
                engine.exportTranscode(
                    "file://$filesDir/pipeline_hw.mp4", dst,
                    "scale=64:64", "libx264",
                    ProgressCallback { v -> ticks.add(v); true },
                )
                val stat = File(dst)
                expect(stat.length() > 0, "软解再编码产物为空")
                expect(ticks.size >= 2, "可解码帧数异常 (进度 ${ticks.size} 次)")
                "FFmpeg 软解→重编码成功，进度 ${ticks.size} 次"
            }
        }
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
            expect(outVideo.getString("codec") == "h264",
                "编码器应为 h264，实际 ${outVideo.getString("codec")}")
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
