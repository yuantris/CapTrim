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
  lastErrorMessage
} from 'libcaptrim_napi.so';
import type { HandleResult, IdResult, StringResult } from 'libcaptrim_napi.so';

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

  /**
   * P0 lossless remux. Runs synchronously on the calling thread, so invoke it
   * from a worker/task dispatcher. P1 will move export to a native thread and
   * use a thread-safe N-API callback.
   */
  exportRemux(srcUri: string, dstUri: string,
    progress: ProgressCallback = alwaysContinue): void {
    CapTrim.checkStatus(nativeExportRemux(this.handle, srcUri, dstUri, progress).code);
  }

  close(): void {
    if (this.handle !== 0) {
      releaseEngine(this.handle);
      this.handle = 0;
    }
  }
}
