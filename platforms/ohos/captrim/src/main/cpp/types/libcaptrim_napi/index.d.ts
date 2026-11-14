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
