export interface CodeResult {
  code: number;
}

export interface IdResult extends CodeResult {
  id: number;
}

export interface HandleResult extends CodeResult {
  handle: number;
}

export interface StringResult extends CodeResult {
  value: string;
}

/** A microsecond value: a timeline position, or a drift correction. */
export interface I64Result extends CodeResult {
  value_us: number;
}

export interface DurationResult extends CodeResult {
  duration_us: number;
  frame_count: number;
}

export const createEngine: (draftJson: string) => HandleResult;
export const releaseEngine: (handle: number) => CodeResult;
export const apply: (handle: number, commandJson: string) => IdResult;
export const undo: (handle: number) => CodeResult;
export const redo: (handle: number) => CodeResult;
export const draft: (handle: number) => StringResult;
export const addMedia: (handle: number, uri: string) => IdResult;
export const probe: (handle: number, uri: string) => StringResult;
export const setMediaInfo: (handle: number, mediaId: number, infoJson: string) => CodeResult;
export const exportRemux: (
  handle: number,
  srcUri: string,
  dstUri: string,
  progress?: (value: number) => boolean
) => CodeResult;
/** Decode -> filter graph -> re-encode -> mp4 mux. Empty strings use defaults. */
export const exportTranscode: (
  handle: number,
  srcUri: string,
  dstUri: string,
  filterDesc: string,
  videoCodec: string,
  progress?: (value: number) => boolean
) => CodeResult;
export const version: () => StringResult;
export const codeMessage: (code: number) => StringResult;
export const lastErrorMessage: () => StringResult;

/** Plan one frame by index: the export path. Returns a FramePlan JSON. */
export const planFrameAt: (handle: number, frameIndex: number) => StringResult;
/** Plan the frame covering t_us: the scrub path. Returns a FramePlan JSON. */
export const planFrame: (handle: number, tUs: number) => StringResult;
/** Full export breakdown: frame-aligned segments with source-time ranges. */
export const exportPlan: (handle: number) => StringResult;
export const duration: (handle: number) => DurationResult;

export const previewNew: (handle: number) => HandleResult;
export const previewFree: (preview: number) => CodeResult;
export const previewSeek: (preview: number, tUs: number, nowWallUs: number) => CodeResult;
export const previewSetPlaying: (
  preview: number,
  playing: boolean,
  nowWallUs: number
) => CodeResult;
/** Returns a Step JSON: present / wait_frame / idle / end. */
export const previewStep: (preview: number, nowWallUs: number) => StringResult;
export const previewPosition: (preview: number, nowWallUs: number) => I64Result;
export const previewSyncAudio: (
  preview: number,
  audioPositionUs: number,
  nowWallUs: number
) => I64Result;
export const previewStats: (preview: number) => IdResult;

/** Snapshot of the render loop, as reported by the native presenter. */
export interface PresentResult extends CodeResult {
  /** 0 idle, 1 present, 2 wait_frame, 3 end (mirrors StepKind). */
  kind: number;
  /** Set when kind is 2: wall clock the loop sleeps until next. */
  due_us: number;
  /** Source instant that reached the surface, -1 when nothing was drawn. */
  source_us: number;
  /** Layers in the plan, and layers dropped because the fixed array is full. */
  layers: number;
  overflow: number;
  /**
   * 1 when the latest step changed the picture -- a newly decoded frame, or a
   * redraw of the current one after an edit. 0 when the window already showed
   * the right frame.
   */
  drawn: number;
  /** 1 while the render loop thread is alive. */
  running: number;
  /** Sticky loop exit code: 0 while healthy. */
  error: number;
  /** Frames composited onto the window, and frames the scheduler dropped. */
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

/**
 * Bind a presenter to an XComponent surface.
 *
 * `surfaceId` is the decimal string from getXComponentSurfaceId(); `width` and
 * `height` are that surface's size in physical pixels. They are the canvas every
 * layer's transform is relative to, so pass real values, not zeros.
 */
export const presentCreate: (
  engine: number,
  surfaceId: string,
  width: number,
  height: number
) => HandleResult;
/** Canvas size changed (rotation, split screen). Applied by the render loop. */
export const presentResize: (presenter: number, width: number, height: number) => CodeResult;
/**
 * Start the render loop on its own thread. The loop owns `preview` from then
 * on: drive it with presentControl, never by calling preview* directly.
 * `nowUs` is the caller's clock; the loop advances it from CLOCK_MONOTONIC.
 */
export const presentStart: (presenter: number, preview: number, nowUs: number) => CodeResult;
/** Stop and join the render loop. */
export const presentStop: (presenter: number) => CodeResult;
/**
 * Queue a transport change for the loop. `playing` < 0 keeps the current state,
 * `seekUs` < 0 means "no seek". Non-blocking, safe from the UI thread.
 */
export const presentControl: (
  presenter: number,
  playing: number,
  seekUs: number
) => CodeResult;
/** Latest snapshot. Non-blocking, safe from the UI thread. */
export const presentStatus: (presenter: number) => PresentResult;
export const presentRelease: (presenter: number) => CodeResult;
