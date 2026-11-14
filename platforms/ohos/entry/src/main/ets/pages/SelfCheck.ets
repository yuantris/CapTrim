// On-device self test for the CapTrim core.
//
// This page exists because unit tests on a build machine cannot prove three
// things that only the device knows: that the staged libcaptrim.so loads under
// the app's linker namespace, that every N-API entry point resolves, and that
// the integer timeline arithmetic produces the same frame grid on arm64 as it
// does on the host. Every check below fails loudly rather than returning a
// suspicious zero.

import { CapTrim, CapTrimPreview, FramePlan, Step } from 'captrim';

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
  static run(): CheckResult[] {
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

    return results;
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
