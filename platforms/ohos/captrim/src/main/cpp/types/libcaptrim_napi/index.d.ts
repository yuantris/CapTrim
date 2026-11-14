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
export const version: () => StringResult;
export const codeMessage: (code: number) => StringResult;
export const lastErrorMessage: () => StringResult;

/** Plan one frame by index: the export path. Returns a FramePlan JSON. */
export const planFrameAt: (handle: number, frameIndex: number) => StringResult;
/** Plan the frame covering t_us: the scrub path. Returns a FramePlan JSON. */
export const planFrame: (handle: number, tUs: number) => StringResult;
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
