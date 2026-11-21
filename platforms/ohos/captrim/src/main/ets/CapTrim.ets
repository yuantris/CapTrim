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
  exportTranscode as nativeExportTranscode,
  exportBurnSubtitles as nativeExportBurnSubtitles,
  exportTimeline as nativeExportTimeline,
  version as nativeVersion,
  codeMessage as nativeCodeMessage,
  lastErrorMessage,
  planFrameAt as nativePlanFrameAt,
  planFrame as nativePlanFrame,
  exportPlan as nativeExportPlan,
  effectsToFilterDesc as nativeEffectsToFilterDesc,
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
  presentResize,
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

/** Integer ratio, used for frame rates and per-clip playback speed. */
export interface Rational {
  num: number;
  den: number;
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

/** One video source active for a whole export segment. */
export interface ExportVideoItem {
  clip_id: number;
  source_id: number;
  uri: string;
  z: number;
  kind: LayerKind;
  source_start_us: number;
  source_end_us: number;
  speed: Rational;
  transform: Transform;
  effects: string[];
}

/** One audio source active for a whole export segment. */
export interface ExportAudioItem {
  clip_id: number;
  source_id: number;
  uri: string;
  source_start_us: number;
  source_end_us: number;
  speed: Rational;
  volume: number;
}

/** A frame-aligned range whose active sources do not change. */
export interface ExportSegment {
  index: number;
  start_frame: number;
  end_frame: number;
  start_us: number;
  end_us: number;
  video: ExportVideoItem[];
  audio: ExportAudioItem[];
}

/** Full export breakdown. Segments cover [0, frame_count) with no holes. */
export interface ExportPlan {
  width: number;
  height: number;
  frame_rate: Rational;
  duration_us: number;
  frame_count: number;
  segments: ExportSegment[];
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

function parseExportPlan(json: string): ExportPlan {
  return JSON.parse(json) as ExportPlan;
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

  /**
   * Translate an ExportVideoItem.effects list into the FFmpeg filter_desc for
   * exportTranscode(). The vocabulary and constants live in the Rust core, so
   * the export path never duplicates them: read `effects` from a cached export
   * plan, pass it here, and feed the result to exportTranscode(). An empty list
   * yields an empty string (passthrough).
   */
  static effectsToFilterDesc(effects: string[]): string {
    const result: StringResult = nativeEffectsToFilterDesc(JSON.stringify(effects));
    CapTrim.checkStatus(result.code);
    return result.value;
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

  /** Full export breakdown: frame ranges with stable source lists. */
  exportPlan(): ExportPlan {
    const result: StringResult = nativeExportPlan(this.handle);
    CapTrim.checkStatus(result.code);
    return parseExportPlan(result.value);
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

  /**
   * Full transcode: decode -> FFmpeg filter graph -> re-encode -> mp4 mux.
   * This is the software backbone of the editing pipeline (and the fallback
   * when a platform hardware codec is unavailable).
   *
   * @param filterDesc libavfilter graph for the VIDEO track, e.g.
   *   "scale=640:-2,fps=24,hflip". Empty string keeps frames as-is (still
   *   re-encoded). Audio tracks are losslessly stream-copied.
   * @param videoCodec encoder name, e.g. "libx264" (default), "libx265",
   *   "mpeg4". Empty string uses the default.
   *
   * Runs synchronously on the calling thread -- invoke from a worker/task
   * dispatcher to avoid blocking the UI thread on long clips.
   */
  exportTranscode(srcUri: string, dstUri: string,
    filterDesc: string = '', videoCodec: string = '',
    progress: ProgressCallback = alwaysContinue): void {
    CapTrim.checkStatus(
      nativeExportTranscode(this.handle, srcUri, dstUri, filterDesc, videoCodec, progress).code);
  }

  /**
   * Export the whole timeline as one file (M1).
   *
   * Unlike exportTranscode(), which runs ONE source through one filter, this
   * consumes the timeline: every segment, every picture layer (transform,
   * effects, opacity), picture-in-picture and dissolve, every transition seam
   * and every audio track. The core compiles one FFmpeg filter graph over the
   * distinct sources from the same export plan the preview reads, so the file
   * matches what was on screen -- there is no second renderer.
   *
   * Subtitle / sticker stills are not in this graph yet (burn them over the
   * result). An opacity ramp other than 0->1 / 1->0 is refused with the
   * segment and layer named instead of exporting the wrong picture.
   *
   * @param videoCodec '' = libx264; @param crf <= 0 = 20;
   * @param preset '' = backend default; @param audioBitrateKbps <= 0 = 192.
   * Runs synchronously on the calling thread.
   */
  exportTimeline(dstUri: string, videoCodec: string = '', crf: number = 20,
    preset: string = 'veryfast', audioBitrateKbps: number = 192,
    progress: ProgressCallback = alwaysContinue): void {
    CapTrim.checkStatus(
      nativeExportTimeline(this.handle, dstUri, videoCodec, crf, preset, audioBitrateKbps,
        progress).code);
  }

  /**
   * Export with this timeline's subtitles burned into the picture.
   *
   * The core rasterises every subtitle clip into `workDir` (one .pam per clip),
   * builds the overlay graph and transcodes; the platform supplies only the
   * chains around it:
   *
   * @param filterPrefix runs first and must trim the source to the timeline
   *   time range, put the picture on the timeline canvas
   *   (`width` x `height`) and reset PTS to zero (`setpts=PTS-STARTPTS`):
   *   subtitle windows are timeline microseconds from that zero.
   * @param filterSuffix runs after the last overlay, e.g. "fps=15,scale=320:180".
   * @param workDir absolute cache directory for the rendered images. Its path
   *   must not contain ':' , quotes, commas, semicolons, brackets or '%' --
   *   FFmpeg filter graphs carry paths verbatim.
   */
  exportBurnSubtitles(srcUri: string, dstUri: string, filterPrefix: string,
    filterSuffix: string, workDir: string, videoCodec: string = '',
    progress: ProgressCallback = alwaysContinue): void {
    CapTrim.checkStatus(
      nativeExportBurnSubtitles(this.handle, srcUri, dstUri, filterPrefix, filterSuffix,
        videoCodec, workDir, progress).code);
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
  /** Position the audio device actually rendered, and total samples written. */
  audio_position_us: number;
  audio_frames: number;
  /** Decoded frames withheld because their PTS was ahead of the playhead. */
  held: number;
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
    position_us: result.position_us,
    audio_position_us: result.audio_position_us,
    audio_frames: result.audio_frames,
    held: result.held
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

  /**
   * Returns null when the surface is not ready yet; retry on the next callback.
   * `width` / `height` are the surface's size in physical pixels -- the canvas
   * that every layer's scale and offset are relative to, so they must be real
   * values rather than zeros.
   */
  static create(editor: CapTrim, surfaceId: string, width: number, height: number): CapTrimPresenter | null {
    const result: HandleResult = presentCreate(editor.nativeHandle(), surfaceId, width, height);
    if (result.code !== 0 || result.handle === 0) {
      return null;
    }
    return new CapTrimPresenter(result.handle);
  }

  /**
   * Tell the loop the canvas changed, e.g. on rotation or a split-screen relayout.
   * Layer geometry follows immediately; the next composed frame is what shows.
   */
  resize(width: number, height: number): void {
    const result: CodeResult = presentResize(this.handle, width, height);
    if (result.code !== 0) {
      throw new CapTrimError(result.code, presenterMessage(result.code));
    }
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
