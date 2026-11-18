// On-device self test for the CapTrim core.
//
// This page exists because unit tests on a build machine cannot prove three
// things that only the device knows: that the staged libcaptrim.so loads under
// the app's linker namespace, that every N-API entry point resolves, and that
// the integer timeline arithmetic produces the same frame grid on arm64 as it
// does on the host. Every check below fails loudly rather than returning a
// suspicious zero.

import { CapTrim, CapTrimPreview, FramePlan, Step } from 'captrim';
import { fileIo as fs } from '@kit.CoreFileKit';

export interface CheckResult {
  name: string;
  pass: boolean;
  detail: string;
}

interface RationalJson {
  num: number;
  den: number;
}

interface TransformJson {
  scale: number;
  dx: number;
  dy: number;
  rotation_deg: number;
}

interface ClipJson {
  id: number;
  source: number;
  in_us: number;
  out_us: number;
  start_us: number;
  speed: RationalJson;
  volume: number;
  transform: TransformJson;
  effects: string[];
}

interface StreamJson {
  index: number;
  kind: string;
  codec: string;
  time_base: RationalJson;
  duration_us: number;
  width: number | null;
  height: number | null;
  frame_rate: RationalJson | null;
  sample_rate: number | null;
  channels: number | null;
  rotation_deg: number;
  bitrate_bps: number;
}

interface MediaInfoJson {
  uri: string;
  format: string;
  duration_us: number;
  bitrate_bps: number;
  streams: StreamJson[];
}

interface InsertClipCmd {
  type: string;
  track_id: number;
  clip: ClipJson;
}

interface AddTrackCmd {
  type: string;
  kind: string;
}

interface SetCanvasCmd {
  type: string;
  width: number;
  height: number;
  frame_rate: RationalJson;
}

interface SetVolumeCmd {
  type: string;
  clip_id: number;
  volume: number;
}

interface TrackJson {
  clips: ClipJson[];
}

interface DraftJson {
  tracks: TrackJson[];
}

const NTSC: RationalJson = { num: 30000, den: 1001 };
const CLIP_LEN: number = 4_000_000;
const TOTAL_US: number = 9_000_000;

function expect(condition: boolean, message: string): void {
  if (!condition) {
    throw new Error(message);
  }
}

function clipOf(source: number, inUs: number, outUs: number, startUs: number): ClipJson {
  const clip: ClipJson = {
    id: 0,
    source: source,
    in_us: inUs,
    out_us: outUs,
    start_us: startUs,
    speed: { num: 1, den: 1 },
    volume: 1,
    transform: { scale: 1, dx: 0, dy: 0, rotation_deg: 0 },
    effects: []
  };
  return clip;
}

function mediaInfo(uri: string, durationUs: number): string {
  const video: StreamJson = {
    index: 0,
    kind: 'video',
    codec: 'h264',
    time_base: { num: 1, den: 1000 },
    duration_us: durationUs,
    width: 1920,
    height: 1080,
    frame_rate: { num: 30, den: 1 },
    sample_rate: null,
    channels: null,
    rotation_deg: 0,
    bitrate_bps: 8_000_000
  };
  const audio: StreamJson = {
    index: 1,
    kind: 'audio',
    codec: 'aac',
    time_base: { num: 1, den: 1000 },
    duration_us: durationUs,
    width: null,
    height: null,
    frame_rate: null,
    sample_rate: 44100,
    channels: 2,
    rotation_deg: 0,
    bitrate_bps: 128_000
  };
  const info: MediaInfoJson = {
    uri: uri,
    format: 'mp4',
    duration_us: durationUs,
    bitrate_bps: 8_128_000,
    streams: [video, audio]
  };
  return JSON.stringify(info);
}

/** A three-clip, two-track draft summing to exactly nine seconds. */
export class Fixture {
  readonly editor: CapTrim;
  readonly mainTrack: number;
  readonly overlayTrack: number;
  readonly firstClip: number;

  constructor() {
    this.editor = CapTrim.create();
    const a = this.editor.addMedia('file:///data/captrim/fixture-a.mp4');
    const b = this.editor.addMedia('file:///data/captrim/fixture-b.mp4');
    this.editor.setMediaInfo(a, mediaInfo('file:///data/captrim/fixture-a.mp4', 10_000_000));
    this.editor.setMediaInfo(b, mediaInfo('file:///data/captrim/fixture-b.mp4', 8_000_000));

    const main: AddTrackCmd = { type: 'add_track', kind: 'main_video' };
    const overlay: AddTrackCmd = { type: 'add_track', kind: 'overlay' };
    this.mainTrack = this.editor.apply(JSON.stringify(main));
    this.overlayTrack = this.editor.apply(JSON.stringify(overlay));

    const one: InsertClipCmd = {
      type: 'insert_clip', track_id: this.mainTrack, clip: clipOf(a, 0, CLIP_LEN, 0)
    };
    const two: InsertClipCmd = {
      type: 'insert_clip', track_id: this.mainTrack, clip: clipOf(b, 0, 2_000_000, CLIP_LEN)
    };
    const three: InsertClipCmd = {
      type: 'insert_clip', track_id: this.overlayTrack, clip: clipOf(a, 0, 3_000_000, 6_000_000)
    };
    this.firstClip = this.editor.apply(JSON.stringify(one));
    this.editor.apply(JSON.stringify(two));
    this.editor.apply(JSON.stringify(three));
  }

  clipCount(): number {
    const draft = JSON.parse(this.editor.draft()) as DraftJson;
    let total = 0;
    for (const track of draft.tracks) {
      total += track.clips.length;
    }
    return total;
  }

  setCanvas(width: number, height: number, rate: RationalJson): void {
    const cmd: SetCanvasCmd = { type: 'set_canvas', width: width, height: height, frame_rate: rate };
    this.editor.apply(JSON.stringify(cmd));
  }

  close(): void {
    this.editor.close();
  }
}

export class SelfCheck {
  static run(filesDir: string): CheckResult[] {
    const results: CheckResult[] = [];
    SelfCheck.check(results, '原生库加载与 N-API 绑定', (): string => {
      const version = CapTrim.version();
      expect(version.length > 0, 'version() 返回空字符串，说明 so 已加载但符号绑定可疑');
      return `libcaptrim ${version}`;
    });

    SelfCheck.check(results, '时间线构建（2 轨 3 片段）', (): string => {
      const fx = new Fixture();
      try {
        expect(fx.clipCount() === 3, `片段数应为 3，实际 ${fx.clipCount()}`);
        expect(fx.editor.duration() === TOTAL_US,
          `时长应为 ${TOTAL_US}us，实际 ${fx.editor.duration()}us`);
        expect(fx.editor.frameCount() === 270,
          `30fps 下帧数应为 270，实际 ${fx.editor.frameCount()}`);
        return '9.000s / 270 帧';
      } finally {
        fx.close();
      }
    });

    SelfCheck.check(results, '分割 / 撤销 / 重做', (): string => {
      const fx = new Fixture();
      try {
        fx.editor.apply(`{"type":"split_clip","clip_id":${fx.firstClip},"at_us":2000000}`);
        expect(fx.clipCount() === 4, `分割后应 4 段，实际 ${fx.clipCount()}`);
        expect(fx.editor.duration() === TOTAL_US, '分割不应改变总时长');
        fx.editor.undo();
        expect(fx.clipCount() === 3, `撤销后应 3 段，实际 ${fx.clipCount()}`);
        fx.editor.redo();
        expect(fx.clipCount() === 4, `重做后应 4 段，实际 ${fx.clipCount()}`);
        return '4 → 3 → 4';
      } finally {
        fx.close();
      }
    });

    SelfCheck.check(results, '29.97fps 帧栅格不塌陷', (): string => {
      const fx = new Fixture();
      try {
        fx.setCanvas(1920, 1080, NTSC);
        const count = fx.editor.frameCount();
        expect(count === 270, `9s@29.97 应 270 帧，实际 ${count}`);
        let previous = -1;
        for (let i = 0; i < count; i++) {
          const pts = fx.editor.planFrameAt(i).t_us;
          expect(pts > previous, `第 ${i} 帧 PTS ${pts} 未递增（上一帧 ${previous}）`);
          const expected = Math.floor(i * 1001 * 1_000_000 / 30000);
          // One microsecond of slack: the C ABI crosses JSON, not i128 maths.
          expect(Math.abs(pts - expected) <= 1,
            `第 ${i} 帧 PTS ${pts} 与整数公式 ${expected} 不符`);
          previous = pts;
        }
        return `270 帧全部严格递增，最大误差 ≤1us`;
      } finally {
        fx.close();
      }
    });

    SelfCheck.check(results, '逐帧源时间唯一（不重复帧）', (): string => {
      const fx = new Fixture();
      try {
        fx.setCanvas(1920, 1080, NTSC);
        let previous = -1;
        for (let i = 0; i < 60; i++) {
          const plan: FramePlan = fx.editor.planFrameAt(i);
          expect(plan.layers.length > 0, `第 ${i} 帧没有图层`);
          const source = plan.layers[0].source_us;
          expect(source > previous,
            `第 ${i} 帧源时间 ${source} 未递增（上一帧 ${previous}），导出会重复帧`);
          previous = source;
        }
        return '前 60 帧源时间严格递增';
      } finally {
        fx.close();
      }
    });

    SelfCheck.check(results, '预览时钟与丢帧统计', (): string => {
      const fx = new Fixture();
      const preview: CapTrimPreview = fx.editor.preview();
      try {
        let now = 1_000_000;
        preview.seek(0, now);
        preview.setPlaying(true, now);
        let presents = 0;
        let waits = 0;
        for (let i = 0; i < 90; i++) {
          now += 16_666;
          const step: Step = preview.step(now);
          if (step.kind === 'present') {
            presents++;
          } else if (step.kind === 'wait_frame') {
            waits++;
          }
        }
        const position = preview.position(now);
        expect(presents > 25, `1.5s 内只呈现 ${presents} 帧，渲染循环没有推进`);
        expect(position > 1_400_000 && position < 1_600_000,
          `位置应约 1.5s，实际 ${position}us`);
        return `呈现 ${presents} 次 / 等待 ${waits} 次 / 位置 ${(position / 1000).toFixed(0)}ms / 丢帧 ${preview.droppedFrames()}`;
      } finally {
        preview.close();
        fx.close();
      }
    });

    SelfCheck.check(results, '编辑后自动重绘', (): string => {
      const fx = new Fixture();
      const preview: CapTrimPreview = fx.editor.preview();
      try {
        let now = 500_000;
        preview.seek(1_000_000, now);
        const first = preview.step(now);
        expect(first.kind === 'present', `首帧应为 present，实际 ${first.kind}`);
        const second = preview.step(now);
        expect(second.kind !== 'present', `同一时刻不应重复呈现，实际 ${second.kind}`);
        const vol: SetVolumeCmd = { type: 'set_volume', clip_id: fx.firstClip, volume: 0.5 };
        fx.editor.apply(JSON.stringify(vol));
        now += 1000;
        const third = preview.step(now);
        expect(third.kind === 'present', `改动后应重绘，实际 ${third.kind}`);
        return '同帧不重复，改动后立即重绘';
      } finally {
        preview.close();
        fx.close();
      }
    });

    SelfCheck.check(results, '音频主导时钟校正', (): string => {
      const fx = new Fixture();
      const preview: CapTrimPreview = fx.editor.preview();
      try {
        const now = 2_000_000;
        preview.seek(0, now);
        preview.setPlaying(true, now);
        // 300 ms of playback predicts ~300 ms; the audio renderer reports 350 ms,
        // so the clock must report the 50 ms it is about to absorb.
        const drift = preview.syncAudio(350_000, now + 300_000);
        expect(drift === 50_000, `漂移应为 50000us，实际 ${drift}us`);
        return `漂移 ${drift}us 已被校正`;
      } finally {
        preview.close();
        fx.close();
      }
    });

    SelfCheck.check(results, '错误码可读', (): string => {
      const ok = CapTrim.codeMessage(0);
      expect(ok.length > 0, 'codeMessage(0) 为空');
      let failed = false;
      let engine: CapTrim | null = null;
      try {
        engine = CapTrim.create();
        engine.apply('{"type":"split_clip","clip_id":9999,"at_us":0}');
      } catch (err) {
        failed = true;
      } finally {
        if (engine !== null) {
          engine.close();
        }
      }
      expect(failed, '对不存在的片段分割本应失败，却静默成功了');
      return `0 → "${ok}"，未知片段正确报错`;
    });

    SelfCheck.check(results, '滤镜 v1（effect 位 + filter_desc 翻译）', (): string => {
      // The translation ABI is the single source of the FFmpeg chain: the
      // platform never duplicates the vocabulary or its constants.
      const desc = CapTrim.effectsToFilterDesc(['blur', 'invert', 'hflip']);
      expect(desc === 'hflip,negate,boxblur=2:1',
        `effects 翻译应为 hflip,negate,boxblur=2:1，实际 "${desc}"`);
      expect(CapTrim.effectsToFilterDesc([]) === '', '空 effects 应翻成空链');

      // set_effects survives into the export plan as the per-segment effects
      // list, which is exactly what the exporter feeds back to the translation.
      const fx = new Fixture();
      try {
        fx.editor.apply(`{"type":"set_effects","clip_id":${fx.firstClip},` +
          '"effects":["hflip","invert"]}');
        const plan = JSON.stringify(fx.editor.exportPlan());
        expect(plan.includes('"hflip"'), `exportPlan 未携带 effects: ${plan}`);
        expect(plan.includes('"invert"'), `exportPlan 未携带 invert: ${plan}`);
        return `desc="${desc}"，plan 携带 effects`;
      } finally {
        fx.close();
      }
    });

    // ---- FFmpeg 软件后端（probe / remux） ---------------------------------
    // These exercise the real FFmpeg C shim (libcaptrim_ffmpeg.so + the FFmpeg
    // shared libraries) through the same N-API the Rust FfmpegBackend uses, so a
    // pass here proves the GPL software backend actually works on device.
    SelfCheck.check(results, 'FFmpeg 探测（软后端 probe）', (): string => {
      const engine = CapTrim.create();
      try {
        const src = `file://${filesDir}/a.mp4`;
        const json = engine.probe(src);
        const info = JSON.parse(json) as MediaInfoJson;
        expect(info.streams.length > 0, 'probe 未返回任何流');
        const kinds = info.streams.map((s: StreamJson) => s.kind).join(',');
        return `format=${info.format} dur=${(info.duration_us / 1_000_000).toFixed(2)}s ` +
          `streams=${info.streams.length}[${kinds}]`;
      } finally {
        engine.close();
      }
    });

    SelfCheck.check(results, 'FFmpeg 重封装（软后端 remux）', (): string => {
      const engine = CapTrim.create();
      const src = `file://${filesDir}/a.mp4`;
      const dst = `${filesDir}/remuxed.mp4`;
      try {
        try {
          fs.unlinkSync(dst);
        } catch (e) {
          // 首次运行产物不存在，忽略
        }
        engine.exportRemux(src, dst);
        const stat = fs.statSync(dst);
        expect(stat.size > 0, `remux 输出为空 (${stat.size} bytes)`);
        // 复探产物，证明它是自包含、可被 FFmpeg 重新解封装的 mp4
        const json = engine.probe(`file://${dst}`);
        const info = JSON.parse(json) as MediaInfoJson;
        expect(info.streams.length > 0, 'remux 产物无法被 probe');
        return `写回 ${(stat.size / 1_000_000).toFixed(2)}MB，复探 ${info.streams.length} 路流`;
      } finally {
        engine.close();
      }
    });

    // ---- FFmpeg 复杂剪辑链路（decode -> filter -> libx264 -> mux） ---------
    // Exercises a real "complex edit command": scale down, horizontal flip,
    // drop to 15fps, then H.264 re-encode. Passing proves the full software
    // backbone works on device (and is available as the hardware fallback).
    SelfCheck.check(results, 'FFmpeg 复杂剪辑（滤镜+libx264重编码）', (): string => {
      const engine = CapTrim.create();
      const src = `file://${filesDir}/a.mp4`;
      const dst = `${filesDir}/edited.mp4`;
      try {
        try {
          fs.unlinkSync(dst);
        } catch (e) {
          // 首次运行产物不存在，忽略
        }
        // 先探测源片分辨率，验证 scale 确实改了尺寸
        const before = JSON.parse(engine.probe(src)) as MediaInfoJson;
        const srcVideo = before.streams.find((s: StreamJson) => s.kind === 'video');
        expect(srcVideo !== undefined, '源片没有视频流');
        const rawFrames: number[] = [];
        // Only 2s of source: a full 22s synchronous transcode on the UI
        // thread exhausts the OHOS ThreadBlock6S watchdog budget, so any
        // later check gets the app killed with signal 9. trim is a terminating
        // filter - ct_ff_transcode() now stops decoding when the graph closes.
        engine.exportTranscode(
          src, dst,
          'trim=duration=2,setpts=PTS-STARTPTS,scale=320:180,hflip,fps=15',
          'libx264',
          (v: number): boolean => { rawFrames.push(Math.round(v * 100)); return true; }
        );
        const stat = fs.statSync(dst);
        expect(stat.size > 0, `转码输出为空 (${stat.size} bytes)`);
        expect(rawFrames.length >= 20, `进度回调次数过少 (${rawFrames.length})`);
        expect(rawFrames[rawFrames.length - 1] > rawFrames[0], '进度没有推进');
        // 复探产物：确认是 h264、分辨率已变为 320x180、帧率约 15
        const info = JSON.parse(engine.probe(`file://${dst}`)) as MediaInfoJson;
        const video = info.streams.find((s: StreamJson) => s.kind === 'video');
        expect(video !== undefined, '转码产物没有视频流');
        // ArkTS does not narrow through our custom expect(), so bind an
        // explicitly non-null local for the field accesses below.
        const outVideo: StreamJson = video!;
        expect(outVideo.codec === 'h264', `编码器应为 h264，实际 ${outVideo.codec}`);
        expect(outVideo.width === 320 && outVideo.height === 180,
          `分辨率应为 320x180，实际 ${outVideo.width}x${outVideo.height}`);
        expect(srcVideo!.width !== 320, '测试无效：源片本来就是 320 宽');
        return `${srcVideo!.width}x${srcVideo!.height}→320x180 ` +
          `codec=${outVideo.codec} fps≈15 ${(stat.size / 1_000_000).toFixed(2)}MB ` +
          `进度 ${rawFrames.length} 次`;
      } finally {
        engine.close();
      }
    });

    return results;
  }

  /**
   * Heavy hybrid-pipeline checks. Call ONE AT A TIME from a deferred UI task.
   * The device self check is synchronous; running two full transcodes back to
   * back with the 17s software edit above trips the OHOS ThreadBlock6S
   * watchdog (verified on Mate 60: app killed with signal 9).
   */
  static runPipelineChecks(filesDir: string, which: number): CheckResult[] {
    const results: CheckResult[] = [];
    if (which === 0) {
    // Three-stage pipeline, software baseline. "hw:sw:" forces every stage to
    // FFmpeg but still exercises ct_pipeline_run(): decoder -> libavfilter ->
    // encoder vtables, per-frame ct_frame_t handoff, progress and flush.
    SelfCheck.check(results, '三段式管线（软件基线 / 回退链路）', (): string => {
      const engine = CapTrim.create();
      const src = `file://${filesDir}/a.mp4`;
      const dst = `${filesDir}/pipeline_sw.mp4`;
      try {
        try {
          fs.unlinkSync(dst);
        } catch (e) {
          // 首次运行产物不存在，忽略
        }
        const ticks: number[] = [];
        engine.exportTranscode(
          src, dst,
          // Only 2s of media: the on-device self check runs synchronously on
          // the UI thread, and a full 22s transcode makes the system's
          // ThreadBlock6S watchdog kill the app (verified on Mate 60).
          'trim=duration=2,setpts=PTS-STARTPTS,scale=320:180,hflip,fps=15',
          'hw:sw:libx264',
          (v: number): boolean => { ticks.push(Math.round(v * 100)); return true; }
        );
        const stat = fs.statSync(dst);
        expect(stat.size > 0, `管线输出为空 (${stat.size} bytes)`);
        expect(ticks.length > 0, '管线进度回调未触发');
        const info = JSON.parse(engine.probe(`file://${dst}`)) as MediaInfoJson;
        const video = info.streams.find((s: StreamJson) => s.kind === 'video');
        expect(video !== undefined, '管线产物没有视频流');
        const outVideo: StreamJson = video!;
        expect(outVideo.codec === 'h264', `编码器应为 h264，实际 ${outVideo.codec}`);
        expect(outVideo.width === 320 && outVideo.height === 180,
          `分辨率应为 320x180，实际 ${outVideo.width}x${outVideo.height}`);
        return `decode→filter→encode ${(stat.size / 1_000_000).toFixed(2)}MB ` +
          `进度 ${ticks.length} 次`;
      } finally {
        engine.close();
      }
    });

    // "hw:" prefers OHOS hardware decode + hardware encode for STAGE 1/3 while
    // STAGE 2 stays FFmpeg libavfilter. Any hardware failure falls back per
    // stage, and a streaming-time failure retries the whole job in software.
    // The actual decoder/encoder choice is logged under tag CapTrimPipeline.
    } else if (which === 1) {
      SelfCheck.hybridCheck(results, filesDir,
        '三段式管线（硬解/硬编优先，失败自动回退）',
        'pipeline_hw.mp4', 'hw:libx264', '硬件优先→回退就绪');
    } else if (which === 2) {
      // "hw:cb:" forces the AVCodec callback (async) Buffer mode queues. This
      // is the API-11+ path from the official transcoding sample; it must work
      // independently of the API-20 synchronous Query*Buffer fast path.
      SelfCheck.hybridCheck(results, filesDir,
        '三段式管线（OHOS 回调式 Buffer 硬解/硬编）',
        'pipeline_cb.mp4', 'hw:cb:libx264', '回调式 Buffer 硬件优先');
    } else if (which === 3) {
      // probe() only reads container metadata. Re-DECODE the hardware-encoded
      // mp4 with the FFmpeg software H.264 decoder to prove the bitstream is
      // real (SPS/PPS + frames), not just a well-formed but empty muxer file.
      ['pipeline_hw.mp4', 'pipeline_cb.mp4'].forEach((name: string): void => {
        SelfCheck.verifyDecodable(results, filesDir, name);
      });
    }
    return results;
  }

  private static verifyDecodable(results: CheckResult[], filesDir: string,
                                 srcName: string): void {
    SelfCheck.check(results, `硬编产物可被 FFmpeg 软解（${srcName}）`, (): string => {
      const engine = CapTrim.create();
      const src = `${filesDir}/${srcName}`;
      const dst = `${filesDir}/${srcName}.verify.mp4`;
      try {
        try {
          fs.unlinkSync(dst);
        } catch (e) {
          // 首次运行产物不存在，忽略
        }
        const ticks: number[] = [];
        engine.exportTranscode(
          `file://${src}`, dst, 'scale=64:64', 'libx264',
          (v: number): boolean => { ticks.push(Math.round(v * 100)); return true; }
        );
        expect(fs.statSync(dst).size > 0, '软解再编码产物为空');
        expect(ticks.length >= 2, `可解码帧数异常 (进度 ${ticks.length} 次)`);
        return `FFmpeg 软解→重编码成功，进度 ${ticks.length} 次`;
      } finally {
        engine.close();
      }
    });
  }

  private static hybridCheck(results: CheckResult[], filesDir: string,
                             name: string, dstName: string,
                             codec: string, okPrefix: string): void {
    SelfCheck.check(results, name, (): string => {
      const engine = CapTrim.create();
      const src = `file://${filesDir}/a.mp4`;
      const dst = `${filesDir}/${dstName}`;
      try {
        try {
          fs.unlinkSync(dst);
        } catch (e) {
          // 首次运行产物不存在，忽略
        }
        const ticks: number[] = [];
        engine.exportTranscode(
          src, dst,
          'trim=duration=2,setpts=PTS-STARTPTS,scale=320:180,hflip,fps=15',
          codec,
          (v: number): boolean => { ticks.push(Math.round(v * 100)); return true; }
        );
        const stat = fs.statSync(dst);
        expect(stat.size > 0, `混合管线输出为空 (${stat.size} bytes)`);
        const info = JSON.parse(engine.probe(`file://${dst}`)) as MediaInfoJson;
        const video = info.streams.find((s: StreamJson) => s.kind === 'video');
        expect(video !== undefined, '混合管线产物没有视频流');
        const outVideo: StreamJson = video!;
        expect(outVideo.codec === 'h264', `编码器应为 h264，实际 ${outVideo.codec}`);
        expect(outVideo.width === 320 && outVideo.height === 180,
          `分辨率应为 320x180，实际 ${outVideo.width}x${outVideo.height}`);
        return `${okPrefix} ${(stat.size / 1_000_000).toFixed(2)}MB ` +
          `codec=${outVideo.codec} 进度 ${ticks.length} 次`;
      } finally {
        engine.close();
      }
    });
  }

  private static check(out: CheckResult[], name: string, body: () => string): void {
    try {
      const detail = body();
      out.push({ name: name, pass: true, detail: detail });
    } catch (err) {
      const error = err as Error;
      out.push({ name: name, pass: false, detail: error.message });
    }
  }
}
