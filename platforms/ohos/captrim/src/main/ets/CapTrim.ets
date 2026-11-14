import {
  createEngine,
  releaseEngine,
  apply as nativeApply,
  undo as nativeUndo,
  redo as nativeRedo,
  draft as nativeDraft,
  addMedia as nativeAddMedia,
  probe as nativeProbe,
  setMediaInfo as nativeSetMediaInfo,
  exportRemux as nativeExportRemux,
  version as nativeVersion,
  codeMessage as nativeCodeMessage,
  lastErrorMessage,
  planFrameAt as nativePlanFrameAt,
  planFrame as nativePlanFrame,
  duration as nativeDuration,
  previewNew,
  previewFree,
  previewSeek,
  previewSetPlaying,
  previewStep,
  previewPosition,
  previewSyncAudio,
  previewStats,
  presentCreate,
  presentStart,
  presentStop,
  presentControl,
  presentStatus,
  presentRelease
} from 'libcaptrim_napi.so';
import type {
  CodeResult,
  DurationResult,
  HandleResult,
  I64Result,
  IdResult,
  PresentResult,
  StringResult
} from 'libcaptrim_napi.so';

/** Raised for any non-zero status returned by the native CapTrim engine. */
export class CapTrimError extends Error {
  readonly code: number;

  constructor(code: number, message: string) {
    super(`${message} (code=${code})`);
    this.code = code;
  }
}

/** Return false to cancel an export. */
export type ProgressCallback = (value: number) => boolean;

function alwaysContinue(value: number): boolean {
  return true;
}

/**
 * What a layer's pixels are: run a video decoder, an image loader, or the text
 * renderer. Mirrors `LayerKind` in captrim-core.
 */
export type LayerKind = 'video' | 'subtitle' | 'overlay';

export interface Transform {
  scale: number;
  dx: number;
  dy: number;
  rotation_deg: number;
}

/** One composited layer of one output frame. */
export interface FrameLayer {
  clip_id: number;
  source_id: number;
  /** Empty means the layer is unavailable; skip it, do not fail the frame. */
  uri: string;
  /** Exact source instant to decode, already mapped through the clip speed. */
  source_us: number;
  /** Draw order, 0 is the bottom. */
  z: number;
  kind: LayerKind;
  transform: Transform;
  opacity: number;
  effects: string[];
}

/** One audio contributor of one output instant. */
export interface AudioMix {
  clip_id: number;
  source_id: number;
  uri: string;
  source_us: number;
  volume: number;
}

/** Everything needed to produce one output frame. */
export interface FramePlan {
  t_us: number;
  frame_index: number;
  width: number;
  height: number;
  layers: FrameLayer[];
  audio: AudioMix[];
}

/** What the platform render loop should do next. */
export type StepKind = 'present' | 'wait_frame' | 'idle' | 'end';

export interface Step {
  kind: StepKind;
  /** Set when kind is 'present'. */
  frame?: FramePlan;
  /** Set when kind is 'wait_frame': sleep until this wall time, then step again. */
  due_wall_us?: number;
  /** Set when kind is 'end'. */
  at_us?: number;
}

function parseStep(json: string): Step {
  return JSON.parse(json) as Step;
}

function parsePlan(json: string): FramePlan {
  return JSON.parse(json) as FramePlan;
}

/**
 * A CapTrim editing session. One instance owns one native engine; call
 * close() when the editor page is destroyed.
 *
 * Timeline editing is expressed as tagged JSON commands, e.g.
 * '{"type":"split_clip","clip_id":3,"at_us":4000000}'. Android uses the
 * exact same command format, so business code can be ported verbatim.
 */
export class CapTrim {
  private handle: number = 0;

  private constructor(handle: number) {
    this.handle = handle;
  }

  static create(draftJson: string = ''): CapTrim {
    const result: HandleResult = createEngine(draftJson);
    CapTrim.checkStatus(result.code);
    return new CapTrim(result.handle);
  }

  static version(): string {
    return nativeVersion().value;
  }

  static codeMessage(code: number): string {
    return nativeCodeMessage(code).value;
  }

  private static checkStatus(code: number): void {
    if (code !== 0) {
      throw new CapTrimError(code, lastErrorMessage().value);
    }
  }

  /** SDK internal: the native engine handle, for CapTrimPreview. */
  nativeHandle(): number {
    return this.handle;
  }

  /** Apply one command. Returns a new object id, or 0 when none was created. */
  apply(commandJson: string): number {
    const result: IdResult = nativeApply(this.handle, commandJson);
    CapTrim.checkStatus(result.code);
    return result.id;
  }

  undo(): void {
    CapTrim.checkStatus(nativeUndo(this.handle).code);
  }

  redo(): void {
    CapTrim.checkStatus(nativeRedo(this.handle).code);
  }

  draft(): string {
    const result: StringResult = nativeDraft(this.handle);
    CapTrim.checkStatus(result.code);
    return result.value;
  }

  addMedia(uri: string): number {
    const result: IdResult = nativeAddMedia(this.handle, uri);
    CapTrim.checkStatus(result.code);
    return result.id;
  }

  /** Probe a source via the FFmpeg backend and update the draft metadata. */
  probe(uri: string): string {
    const result: StringResult = nativeProbe(this.handle, uri);
    CapTrim.checkStatus(result.code);
    return result.value;
  }

  /** Inject metadata obtained from a platform decoder (AVCodec Kit etc.). */
  setMediaInfo(mediaId: number, infoJson: string): void {
    CapTrim.checkStatus(nativeSetMediaInfo(this.handle, mediaId, infoJson).code);
  }

  /** Timeline length in microseconds. */
  duration(): number {
    const result: DurationResult = nativeDuration(this.handle);
    CapTrim.checkStatus(result.code);
    return result.duration_us;
  }

  /** Total frames the exporter must write. Length in frames. */
  frameCount(): number {
    const result: DurationResult = nativeDuration(this.handle);
    CapTrim.checkStatus(result.code);
    return result.frame_count;
  }

  /**
   * Plan the frame covering t_us, for scrubbing. For export, walk frame
   * indices with planFrameAt instead: flooring a truncated PTS twice is what
   * makes NTSC exports repeat frames.
   */
  planFrame(tUs: number): FramePlan {
    const result: StringResult = nativePlanFrame(this.handle, tUs);
    CapTrim.checkStatus(result.code);
    return parsePlan(result.value);
  }

  /** Plan one frame by index. This is the export path. */
  planFrameAt(frameIndex: number): FramePlan {
    const result: StringResult = nativePlanFrameAt(this.handle, frameIndex);
    CapTrim.checkStatus(result.code);
    return parsePlan(result.value);
  }

  /**
   * P0 lossless remux. Runs synchronously on the calling thread, so invoke it
   * from a worker/task dispatcher. P1 will move export to a native thread and
   * use a thread-safe N-API callback.
   */
  exportRemux(srcUri: string, dstUri: string,
    progress: ProgressCallback = alwaysContinue): void {
    CapTrim.checkStatus(nativeExportRemux(this.handle, srcUri, dstUri, progress).code);
  }

  /** Open a playback session. Close the previous one first. */
  preview(): CapTrimPreview {
    const result: HandleResult = previewNew(this.handle);
    CapTrim.checkStatus(result.code);
    return new CapTrimPreview(result.handle);
  }

  close(): void {
    if (this.handle !== 0) {
      releaseEngine(this.handle);
      this.handle = 0;
    }
  }
}

/**
 * Playback session: owns the clock and the "frame on screen" state.
 *
 * The loop is:
 *   const now = monotonicUs();
 *   const step = preview.step(now);
 *   if (step.kind === 'present') { decode(step.frame); composite(); }
 *   else if (step.kind === 'wait_frame') { sleep until step.due_wall_us }
 *   else if (step.kind === 'end') { stop }
 *
 * `nowUs` must come from the same monotonic source on every call. Never pass a
 * wall-clock date: a clock that goes backwards stalls playback, one that jumps
 * forwards drops the whole buffer.
 *
 * The session picks up edits to the draft on the next step(), so after a command
 * the current frame repaints without any extra call.
 */
export class CapTrimPreview {
  private handle: number = 0;

  constructor(handle: number) {
    this.handle = handle;
  }

  seek(tUs: number, nowUs: number): void {
    CapTrimPreview.checkStatus(previewSeek(this.handle, tUs, nowUs).code);
  }

  /** Starting playback while parked on the last frame rewinds first. */
  setPlaying(playing: boolean, nowUs: number): void {
    CapTrimPreview.checkStatus(previewSetPlaying(this.handle, playing, nowUs).code);
  }

  step(nowUs: number): Step {
    const result: StringResult = previewStep(this.handle, nowUs);
    CapTrimPreview.checkStatus(result.code);
    return parseStep(result.value);
  }

  position(nowUs: number): number {
    const result: I64Result = previewPosition(this.handle, nowUs);
    CapTrimPreview.checkStatus(result.code);
    return result.value_us;
  }

  /**
   * Re-anchor to the audio renderer's position (the master clock). Returns the
   * drift that was corrected, which is the number to log when a device looks
   * out of sync.
   */
  syncAudio(audioPositionUs: number, nowUs: number): number {
    const result: I64Result = previewSyncAudio(this.handle, audioPositionUs, nowUs);
    CapTrimPreview.checkStatus(result.code);
    return result.value_us;
  }

  /** SDK internal: the native preview handle, for CapTrimPresenter. */
  nativeHandle(): number {
    return this.handle;
  }

  /** Frames skipped because the render loop fell behind. */
  droppedFrames(): number {
    const result: IdResult = previewStats(this.handle);
    CapTrimPreview.checkStatus(result.code);
    return result.id;
  }

  close(): void {
    if (this.handle !== 0) {
      previewFree(this.handle);
      this.handle = 0;
    }
  }

  private static checkStatus(code: number): void {
    if (code !== 0) {
      throw new CapTrimError(code, lastErrorMessage().value);
    }
  }
}

/** Numeric step kinds of the C ABI mapped back to the string union. */
const STEP_KINDS: StepKind[] = ['idle', 'present', 'wait_frame', 'end'];

/**
 * Presenter failures carry a status but no message: the message thread-local in
 * the Rust engine belongs to the editor, not to the platform decoder, and reading
 * it here would report a stale, unrelated error. Map the status instead.
 */
function presenterMessage(code: number): string {
  if (code === -1) {
    return 'presenter 参数无效';
  }
  if (code === -2) {
    return '媒体里没有视频轨';
  }
  if (code === -5) {
    return '打不开媒体文件';
  }
  if (code === -6) {
    return '解码器失败（无硬解或码流不支持）';
  }
  return '预览失败';
}

/** Snapshot of the render loop, in the units the UI wants to show. */
export interface PresentReport {
  kind: StepKind;
  /** wait_frame only: wall clock the loop sleeps until next. */
  due_us: number;
  /** Source instant that reached the surface, -1 when nothing was drawn. */
  source_us: number;
  layers: number;
  /** Layers the plan had beyond the fixed array; never silently dropped. */
  overflow: number;
  /** True when a decoded frame was rendered during the latest step. */
  drawn: boolean;
  /** True while the render thread is alive. */
  running: boolean;
  /** Sticky loop exit code: 0 while healthy. */
  error: number;
  /** Frames pushed to the surface, and frames the scheduler dropped. */
  frames: number;
  dropped: number;
  /** Playhead in timeline time. */
  position_us: number;
}

function toPresentReport(result: PresentResult): PresentReport {
  const report: PresentReport = {
    kind: STEP_KINDS[result.kind],
    due_us: result.due_us,
    source_us: result.source_us,
    layers: result.layers,
    overflow: result.overflow,
    drawn: result.drawn !== 0,
    running: result.running !== 0,
    error: result.error,
    frames: result.frames,
    dropped: result.dropped,
    position_us: result.position_us
  };
  return report;
}

/**
 * Draws the preview onto an XComponent surface with the platform decoder.
 *
 * This is the platform half of the P1 split: Rust decides which source instant
 * belongs on screen, this class gets it decoded and presented. Create it from
 * XComponent's onSurfaceCreated, which is the only place a valid surface id
 * exists, and close it in onSurfaceDestroyed.
 *
 * The decode loop runs on a native thread, not on the ArkTS event loop:
 * presenting a frame blocks until the hardware decoder hands it to the surface,
 * and doing that on the UI thread gets the app killed for being unresponsive
 * (observed: THREAD_BLOCK_6S). The page only sends transport commands and polls
 * status().
 *
 * Ownership: after start(), the native loop owns the preview session. Never
 * call seek()/setPlaying()/step() on that CapTrimPreview yourself -- going
 * around the loop races the decoder and tears the clock anchor. Use control().
 */
export class CapTrimPresenter {
  private handle: number = 0;

  private constructor(handle: number) {
    this.handle = handle;
  }

  /** Returns null when the surface is not ready yet; retry on the next callback. */
  static create(editor: CapTrim, surfaceId: string): CapTrimPresenter | null {
    const result: HandleResult = presentCreate(editor.nativeHandle(), surfaceId);
    if (result.code !== 0 || result.handle === 0) {
      return null;
    }
    return new CapTrimPresenter(result.handle);
  }

  /**
   * Hand `preview` to the render loop and start decoding on a native thread.
   * `nowUs` is the current wall clock; the loop advances it on its own.
   */
  start(preview: CapTrimPreview, nowUs: number): void {
    const result: CodeResult = presentStart(this.handle, preview.nativeHandle(), nowUs);
    if (result.code !== 0) {
      throw new CapTrimError(result.code, presenterMessage(result.code));
    }
  }

  /**
   * Queue a transport change. `playing` < 0 leaves the state alone, `seekUs` < 0
   * means "no seek". Returns immediately; the loop applies it on its next tick.
   */
  control(playing: number, seekUs: number): void {
    const result: CodeResult = presentControl(this.handle, playing, seekUs);
    if (result.code !== 0) {
      throw new CapTrimError(result.code, presenterMessage(result.code));
    }
  }

  /** Latest snapshot of the loop. Never blocks on a decode. */
  status(): PresentReport {
    const result: PresentResult = presentStatus(this.handle);
    if (result.code !== 0) {
      throw new CapTrimError(result.code, presenterMessage(result.code));
    }
    return toPresentReport(result);
  }

  /** Stop and join the render loop. Safe to call more than once. */
  stop(): void {
    if (this.handle !== 0) {
      presentStop(this.handle);
    }
  }

  close(): void {
    if (this.handle !== 0) {
      presentRelease(this.handle);
      this.handle = 0;
    }
  }
}
