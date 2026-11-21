// On-device self test for the CapTrim core.
//
// This page exists because unit tests on a build machine cannot prove three
// things that only the device knows: that the staged libcaptrim.so loads under
// the app's linker namespace, that every N-API entry point resolves, and that
// the integer timeline arithmetic produces the same frame grid on arm64 as it
// does on the host. Every check below fails loudly rather than returning a
// suspicious zero.

import { CapTrim, CapTrimError, CapTrimPreview, FramePlan, Step } from 'captrim';
import { fileIo as fs } from '@kit.CoreFileKit';
import { taskpool } from '@kit.ArkTS';
import { TimelineExportResult, exportTimelineTask, BurnResult, burnTask } from './TimelineExportTask';

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

interface InsertTextClipCmd {
  type: string;
  track_id: number;
  text: string;
  start_us: number;
  duration_us: number;
}

interface SetTransformCmd {
  type: string;
  clip_id: number;
  transform: TransformJson;
}

interface SetVolumeCmd {
  type: string;
  clip_id: number;
  volume: number;
}

interface InsertImageClipCmd {
  type: string;
  track_id: number;
  uri: string;
  start_us: number;
  duration_us: number;
}

interface TrackJson {
  clips: ClipJson[];
}

interface DraftJson {
  tracks: TrackJson[];
}

/** Ink statistics of one PAM written by the core (caption or still). */
interface InkCount {
  /** Pixels with any alpha at all. */
  total: number;
  /** The subset bright enough to be white caption fill, not the outline. */
  bright: number;
  /** Edges: `0 < alpha < 255`, i.e. anti-aliased rather than hard cut. */
  partial: number;
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

    // Which licence profile this build is: GPL carries libx264 / libx265, LGPL
    // carries neither (and is the only one a closed-source SDK may distribute).
    SelfCheck.check(results, '软件编码器清单（许可 profile 自报）', (): string => {
      const encoders = CapTrim.encoders();
      expect(encoders.length > 0, 'encoders() 为空：FFmpeg 构建没有任何软件编码器');
      const isGpl = CapTrim.hasEncoder('libx264') || CapTrim.hasEncoder('libx265');
      return `${isGpl ? 'GPL' : 'LGPL'} profile，可用: ${encoders}`;
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
      // platform never duplicates the vocabulary or its constants. It is also
      // the capability probe: `boxblur` / `eq` are GPL filters inside FFmpeg,
      // so an LGPL build must return -11 naming the effect instead of a chain
      // libavfilter would reject at graph-parse time.
      const base = CapTrim.effectsToFilterDesc(['invert', 'hflip']);
      expect(base === 'hflip,negate', `effects 翻译应为 hflip,negate，实际 "${base}"`);
      expect(CapTrim.effectsToFilterDesc([]) === '', '空 effects 应翻成空链');

      let chain: string = '';
      let unsupportedNote: string = '';
      try {
        chain = CapTrim.effectsToFilterDesc(['blur', 'invert', 'hflip']);
      } catch (e) {
        const err = e as Error;
        const unsupported = (err as CapTrimError).code === -11;
        expect(unsupported, `缺少 boxblur 时应报 -11，实际 ${err.message}`);
        expect(err.message.includes('blur') && err.message.includes('available effects'),
          `错误信息应点名 effect 与可用项，实际 "${err.message}"`);
        unsupportedNote = err.message;
      }
      if (unsupportedNote === '') {
        expect(chain === 'hflip,negate,boxblur=2:1',
          `effects 翻译应为 hflip,negate,boxblur=2:1，实际 "${chain}"`);
      }

      // set_effects survives into the export plan as the per-segment effects
      // list, which is exactly what the exporter feeds back to the translation.
      const fx = new Fixture();
      try {
        fx.editor.apply(`{"type":"set_effects","clip_id":${fx.firstClip},` +
          '"effects":["hflip","invert"]}');
        const plan = JSON.stringify(fx.editor.exportPlan());
        expect(plan.includes('"hflip"'), `exportPlan 未携带 effects: ${plan}`);
        expect(plan.includes('"invert"'), `exportPlan 未携带 invert: ${plan}`);
        return unsupportedNote === ''
          ? `desc="${chain}"，plan 携带 effects`
          : `desc 未生成（${unsupportedNote}），plan 携带 effects`;
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
    SelfCheck.check(results, 'FFmpeg 复杂剪辑（滤镜+H.264 重编码）', (): string => {
      // '' = whichever software H.264 encoder this build carries (libx264 in the
      // GPL profile). The LGPL profile has none, so report it instead of failing.
      if (!CapTrim.hasEncoder('libx264') && !CapTrim.hasEncoder('libopenh264')) {
        return '跳过：本构建无软件 H.264 编码器（LGPL profile）';
      }
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
          '',
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
    // The encoder is whatever software encoder this build carries: an LGPL build
    // has no H.264 one, and mpeg4 still exercises the same three stages end to
    // end, so the profile difference stays visible instead of hiding the case.
    const softCodec: string = SelfCheck.softwareEncoder();
    const softH264: boolean = softCodec === 'libx264' || softCodec === 'libopenh264';
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
          `hw:sw:${softCodec}`,
          (v: number): boolean => { ticks.push(Math.round(v * 100)); return true; }
        );
        const stat = fs.statSync(dst);
        expect(stat.size > 0, `管线输出为空 (${stat.size} bytes)`);
        expect(ticks.length > 0, '管线进度回调未触发');
        const info = JSON.parse(engine.probe(`file://${dst}`)) as MediaInfoJson;
        const video = info.streams.find((s: StreamJson) => s.kind === 'video');
        expect(video !== undefined, '管线产物没有视频流');
        const outVideo: StreamJson = video!;
        expect(outVideo.codec === (softH264 ? 'h264' : 'mpeg4'),
          `编码器应为 ${softH264 ? 'h264' : 'mpeg4'}，实际 ${outVideo.codec}`);
        expect(outVideo.width === 320 && outVideo.height === 180,
          `分辨率应为 320x180，实际 ${outVideo.width}x${outVideo.height}`);
        return `decode→filter→encode(${outVideo.codec} ${softCodec}) ` +
          `${(stat.size / 1_000_000).toFixed(2)}MB 进度 ${ticks.length} 次`;
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

  /**
   * Caption + sticker burn, off the UI thread.
   *
   * The two transcodes (burned and baseline) are the longest jobs in this
   * self check — 16 s together — and a blocked UI thread past 6 s is enough for
   * the system to kill the app. The draft is built here, the pixels are
   * rasterised and encoded on a worker, and only the cheap assertions (file
   * sizes, PAM ink counts, probe metrics) run here.
   */
  static async runBurnAsync(filesDir: string): Promise<CheckResult> {
    const name: string = '字幕与贴图烧录（core 栅格化 + FFmpeg overlay）';
    const src = `file://${filesDir}/a.mp4`;
    const dst = `${filesDir}/burn.mp4`;
    const base = `${filesDir}/burn-base.mp4`;
    const workDir = `${filesDir}/burn`;
    // Both worker tasks ask for '' (any software H.264 encoder). An LGPL build
    // has none, so a burn would end in CAPTRIM_ERR_UNSUPPORTED; report it as a
    // skip so the profile difference reads as a fact, not as a broken app.
    if (!CapTrim.hasEncoder('libx264') && !CapTrim.hasEncoder('libopenh264')) {
      return { name: name, pass: true, detail: '跳过：本构建无软件 H.264 编码器（LGPL profile）' };
    }
    try {
      for (const path of [dst, base]) {
        try {
          fs.unlinkSync(path);
        } catch (e) {
          // 首次运行产物不存在，忽略
        }
      }
      // The PAM count below must describe this run only, so the previous
      // run's renders (different clip ids) cannot inflate it.
      try {
        fs.rmdirSync(workDir);
      } catch (e) {
        // 目录不存在或非空？plan_burn 会重建；旧文件在下面按名字过滤。
      }
      // 540x960 keeps the overlay at 1:1 with the canvas (subtitle geometry is
      // canvas-relative); running on a worker means the size no longer has to
      // fit a UI-thread budget.
      const engine = CapTrim.create();
      let draft: string = '';
      try {
        const canvas: SetCanvasCmd = {
          type: 'set_canvas', width: 540, height: 960,
          frame_rate: { num: 30, den: 1 }
        };
        engine.apply(JSON.stringify(canvas));
        const track: AddTrackCmd = { type: 'add_track', kind: 'subtitle' };
        const trackId = engine.apply(JSON.stringify(track));
        // 0.4s..1.6s only: frames outside the window must stay clean, which is
        // what the enable=between(t,...) expression is for.
        const subtitle: InsertTextClipCmd = {
          type: 'insert_text_clip', track_id: trackId,
          text: '字幕烧录 · Overlay', start_us: 400_000, duration_us: 1_200_000
        };
        engine.apply(JSON.stringify(subtitle));

        // The sticker is the second half of this check: its pixels come from
        // a PNG decoded in Rust (no libz on this device, so FFmpeg could not
        // decode it), then fitted, scaled and rotated by the same core code
        // the GL preview uploads.
        const imageTrack: AddTrackCmd = { type: 'add_track', kind: 'image' };
        const imageTrackId = engine.apply(JSON.stringify(imageTrack));
        const stickerCmd: InsertImageClipCmd = {
          type: 'insert_image_clip', track_id: imageTrackId,
          uri: `file://${filesDir}/sticker.png`,
          start_us: 400_000, duration_us: 1_200_000
        };
        const stickerId = engine.apply(JSON.stringify(stickerCmd));
        expect(stickerId > 0, '贴图没有插入，sticker.png 可能不存在');
        engine.apply(JSON.stringify({
          type: 'set_transform', clip_id: stickerId,
          transform: { scale: 0.25, dx: 0.3, dy: -0.35, rotation_deg: 0 }
        } as SetTransformCmd));
        draft = engine.draft();
      } finally {
        engine.close();
      }

      const prefix = 'trim=duration=2,setpts=PTS-STARTPTS,' +
        'scale=540:960:force_original_aspect_ratio=decrease,' +
        'pad=540:960:(ow-iw)/2:(oh-ih)/2';
      const raw: string = (await taskpool.execute(
        burnTask, draft, src, dst, base, prefix, workDir)) as string;
      const burned = fs.statSync(dst).size;
      expect(burned > 0, '烧录产物为空');
      // Same picture pipeline, no captions or stickers: if overlay did anything
      // at all the encoded result cannot be byte-identical.
      const plain = fs.statSync(base).size;
      expect(plain > 0, '基线产物为空');
      expect(burned !== plain,
        `烧录前后文件大小相同 (${burned} bytes)，overlay 可能没有生效`);

      const info = JSON.parse(raw) as BurnResult;
      const probed = JSON.parse(info.probe) as MediaInfoJson;
      const video = probed.streams.find((s: StreamJson) => s.kind === 'video');
      expect(video !== undefined, '烧录产物没有视频流');
      const outVideo: StreamJson = video!;
      expect(outVideo.width === 540 && outVideo.height === 960,
        `分辨率应为 540x960，实际 ${outVideo.width}x${outVideo.height}`);

      const entries = fs.listFileSync(workDir);
      const pams = entries.filter((name: string): boolean => name.endsWith('.pam'));
      const captions = pams.filter((name: string): boolean => name.startsWith('sub_'));
      const stills = pams.filter((name: string): boolean => name.startsWith('img_'));
      expect(captions.length === 1, `应渲染 1 张字幕 PAM，实际 ${captions.length}`);
      expect(stills.length === 1, `应渲染 1 张贴图 PAM，实际 ${stills.length}`);
      // The overlay consumes exactly these files, so prove they carry real
      // pixels on device rather than an empty canvas — the half FFmpeg cannot
      // tell us about.
      const ink = SelfCheck.pamInk(`${workDir}/${captions[0]}`);
      expect(ink.total > 500, `字幕 PAM 里只有 ${ink.total} 个墨迹像素`);
      expect(ink.bright > 300, `字幕 PAM 里只有 ${ink.bright} 个白色像素`);

      // The sticker is 256x256 fitted into 540x960 (scale 2.1) and then drawn
      // at 0.25, i.e. 135x135; the ring covers ~47% of that square.
      const stickerInk = SelfCheck.pamInk(`${workDir}/${stills[0]}`);
      expect(stickerInk.total > 3000, `贴图 PAM 里只有 ${stickerInk.total} 个墨迹像素`);
      // A ring on a transparent background: partial alpha at its edge is what
      // proves the PNG's alpha channel survived the decode and the resample.
      // Flattened alpha would leave a solid square instead.
      expect(stickerInk.partial > 200,
        `贴图 PAM 只有 ${stickerInk.partial} 个半透明边缘像素，alpha 可能被压平`);

      return {
        name: name,
        pass: true,
        detail: `540x960 burn ${(burned / 1_000).toFixed(0)}KB vs base ` +
          `${(plain / 1_000).toFixed(0)}KB，PAM ${pams.length} 张 ` +
          `(字幕 ${ink.bright} 白 / ${ink.total} 墨迹，贴图 ${stickerInk.total} 墨迹 / ` +
          `${stickerInk.partial} 半透明)，进度 ${info.ticks} 次（worker 线程）`
      };
    } catch (e) {
      const detail: string = e instanceof Error ? (e as Error).message : `${e}`;
      return { name: name, pass: false, detail: detail };
    }
  }

  /**
   * M1 end to end, off the UI thread.
   *
   * The timeline is built here (cheap, and `draft()` is exactly the JSON an
   * app would save), then the export itself runs on a taskpool worker. A real
   * export blocks far longer than OHOS ThreadBlock6S tolerates: running it
   * inline got the app killed with a THREAD_BLOCK_6S fault right after the
   * burn check, before any result line could be printed.
   */
  static async runTimelineExportAsync(filesDir: string): Promise<CheckResult> {
    const name: string = '时间线导出（两段拼接 + 画中画 + 字幕贴图 + 双音轨）';
    const srcA = `file://${filesDir}/a.mp4`;
    const srcB = `file://${filesDir}/b.mp4`;
    const dst = `${filesDir}/timeline.mp4`;
    // The worker asks for '' (any software H.264 encoder); an LGPL build has
    // none and would fail with CAPTRIM_ERR_UNSUPPORTED here.
    if (!CapTrim.hasEncoder('libx264') && !CapTrim.hasEncoder('libopenh264')) {
      return { name: name, pass: true, detail: '跳过：本构建无软件 H.264 编码器（LGPL profile）' };
    }
    try {
      try {
        fs.unlinkSync(dst);
      } catch (e) {
        // 首次运行产物不存在，忽略
      }
      // Second source for the concat + picture-in-picture, derived from the
      // pushed sample so the check stays self-contained.
      try {
        fs.accessSync(`${filesDir}/b.mp4`);
      } catch (e) {
        const seeder = CapTrim.create();
        try {
          seeder.exportRemux(srcA, `${filesDir}/b.mp4`);
        } finally {
          seeder.close();
        }
      }
      expect(fs.statSync(`${filesDir}/b.mp4`).size > 0, 'b.mp4 素材没有生成');

      const engine = CapTrim.create();
      let draft: string = '';
      try {
        const a = engine.addMedia(srcA);
        const b = engine.addMedia(srcB);
        engine.setMediaInfo(a, mediaInfo(srcA, 10_000_000));
        engine.setMediaInfo(b, mediaInfo(srcB, 8_000_000));
        const canvas: SetCanvasCmd = {
          type: 'set_canvas', width: 360, height: 640,
          frame_rate: { num: 30, den: 1 }
        };
        engine.apply(JSON.stringify(canvas));
        const main: AddTrackCmd = { type: 'add_track', kind: 'main_video' };
        const overlay: AddTrackCmd = { type: 'add_track', kind: 'overlay' };
        const mainId = engine.apply(JSON.stringify(main));
        const overlayId = engine.apply(JSON.stringify(overlay));
        const first: InsertClipCmd = {
          type: 'insert_clip', track_id: mainId, clip: clipOf(a, 0, 2_000_000, 0)
        };
        const second: InsertClipCmd = {
          type: 'insert_clip', track_id: mainId, clip: clipOf(b, 0, 2_000_000, 2_000_000)
        };
        engine.apply(JSON.stringify(first));
        engine.apply(JSON.stringify(second));
        const pip: InsertClipCmd = {
          type: 'insert_clip', track_id: overlayId, clip: clipOf(b, 0, 4_000_000, 0)
        };
        const pipId = engine.apply(JSON.stringify(pip));
        engine.apply(
          `{"type":"set_transform","clip_id":${pipId},` +
          `"transform":{"scale":0.25,"dx":0.3,"dy":-0.35,"rotation_deg":0}}`
        );
        // Captions and stickers are stills the core rasterises into filesDir
        // and overlays in the same graph: this also puts `pam`, `movie` and
        // `loop` on the device-side proof list.
        const subtitle: AddTrackCmd = { type: 'add_track', kind: 'subtitle' };
        const subtitleId = engine.apply(JSON.stringify(subtitle));
        const caption: InsertTextClipCmd = {
          type: 'insert_text_clip', track_id: subtitleId,
          text: '字幕随片导出', start_us: 500_000, duration_us: 2_500_000
        };
        engine.apply(JSON.stringify(caption));
        const image: AddTrackCmd = { type: 'add_track', kind: 'image' };
        const imageId = engine.apply(JSON.stringify(image));
        expect(fs.statSync(`${filesDir}/sticker.png`).size > 0,
          'sticker.png 不在沙箱里，贴图用例无法运行');
        const badge: InsertImageClipCmd = {
          type: 'insert_image_clip', track_id: imageId,
          uri: `file://${filesDir}/sticker.png`, start_us: 1_000_000, duration_us: 3_000_000
        };
        const badgeId = engine.apply(JSON.stringify(badge));
        engine.apply(
          `{"type":"set_transform","clip_id":${badgeId},` +
          `"transform":{"scale":0.15,"dx":0.35,"dy":-0.35,"rotation_deg":0}}`
        );
        expect(engine.duration() === 4_000_000, `时间线应为 4.000s，实际 ${engine.duration()}us`);
        draft = engine.draft();
      } finally {
        engine.close();
      }

      const raw: string =
        (await taskpool.execute(exportTimelineTask, draft, dst, filesDir)) as string;
      const exported = JSON.parse(raw) as TimelineExportResult;
      const stat = fs.statSync(dst);
      expect(stat.size > 0, `导出产物为空 (${stat.size} bytes)`);
      expect(exported.ticks >= 30, `进度回调次数过少 (${exported.ticks})`);
      expect(exported.lastProgress > 0.9, `进度未走到尾 (${exported.lastProgress})`);

      const info = JSON.parse(exported.probe) as MediaInfoJson;
      const video = info.streams.find((s: StreamJson) => s.kind === 'video');
      expect(video !== undefined, '导出产物没有视频流');
      const outVideo: StreamJson = video!;
      expect(outVideo.codec === 'h264', `编码器应为 h264，实际 ${outVideo.codec}`);
      expect(outVideo.width === 360 && outVideo.height === 640,
        `画布应为 360x640，实际 ${outVideo.width}x${outVideo.height}`);
      const audio = info.streams.find((s: StreamJson) => s.kind === 'audio');
      expect(audio !== undefined, '导出产物没有音轨');
      const outAudio: StreamJson = audio!;
      expect(outAudio.sample_rate === 48000 && outAudio.channels === 2,
        `音轨应为 48k 立体声，实际 ${outAudio.sample_rate}Hz/${outAudio.channels}ch`);
      expect(Math.abs(info.duration_us - 4_000_000) <= 34_000,
        `时长应为 4.000s（±AAC 尾帧），实际 ${info.duration_us}us`);
      return {
        name: name,
        pass: true,
        detail: `360x640 h264 / aac ${outAudio.channels}ch ${outAudio.sample_rate}Hz，` +
          `${exported.ticks} 次进度 至 ${exported.lastProgress.toFixed(2)}（worker 线程）`
      };
    } catch (e) {
      const detail: string = e instanceof Error ? (e as Error).message : `${e}`;
      return { name: name, pass: false, detail: detail };
    }
  }

  /**
   * The software encoder this build actually carries, for cases that must run
   * on both licence profiles. GPL: libx264. LGPL: mpeg4 (the only software
   * encoder left once --enable-gpl is gone). Never invent one: the pipeline
   * test asks for a name the binary really has, so a profile without H.264
   * still proves decode→filter→encode instead of skipping the whole stage.
   */
  private static softwareEncoder(): string {
    if (CapTrim.hasEncoder('libx264')) {
      return 'libx264';
    }
    if (CapTrim.hasEncoder('libopenh264')) {
      return 'libopenh264';
    }
    return 'mpeg4';
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
        const codec = SelfCheck.softwareEncoder();
        engine.exportTranscode(
          `file://${src}`, dst, 'scale=64:64', codec,
          (v: number): boolean => { ticks.push(Math.round(v * 100)); return true; }
        );
        expect(fs.statSync(dst).size > 0, '软解再编码产物为空');
        expect(ticks.length >= 2, `可解码帧数异常 (进度 ${ticks.length} 次)`);
        return `FFmpeg 软解→重编码(${codec})成功，进度 ${ticks.length} 次`;
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

  /**
   * Count ink in a PAM (P7, RGB_ALPHA, written by the core): pixels whose
   * alpha is non-zero, and the subset that is bright enough to be the white
   * fill rather than the black outline. ArkTS cannot decode video, but it can
   * read this file — the exact bytes FFmpeg's `movie` filter overlays.
   */
  private static pamInk(path: string): InkCount {
    const stat = fs.statSync(path);
    const file = fs.openSync(path, fs.OpenMode.READ_ONLY);
    const buffer = new ArrayBuffer(stat.size);
    try {
      fs.readSync(file.fd, buffer);
    } finally {
      fs.closeSync(file);
    }
    const bytes = new Uint8Array(buffer);
    // Header ends with the ASCII "ENDHDR\n" line; pixels follow immediately.
    let headerEnd = -1;
    const tag = 'ENDHDR\n';
    for (let i = 0; i + tag.length <= bytes.length; i++) {
      let match = true;
      for (let j = 0; j < tag.length; j++) {
        if (bytes[i + j] !== tag.charCodeAt(j)) {
          match = false;
          break;
        }
      }
      if (match) {
        headerEnd = i + tag.length;
        break;
      }
    }
    expect(headerEnd > 0, `${path} 没有 PAM 头`);
    const result: InkCount = { total: 0, bright: 0, partial: 0 };
    for (let p = headerEnd; p + 3 < bytes.length; p += 4) {
      if (bytes[p + 3] === 0) {
        continue;
      }
      result.total++;
      if (bytes[p + 3] < 255) {
        result.partial++;
      }
      if (bytes[p] > 200 && bytes[p + 1] > 200 && bytes[p + 2] > 200) {
        result.bright++;
      }
    }
    return result;
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
