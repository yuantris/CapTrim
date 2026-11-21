// Export one timeline on a taskpool worker, not on the UI thread.
//
// The self check used to call exportTimeline synchronously, and a real
// multi-clip export (decode + filter graph + x264 encode) blocks the UI
// thread for far longer than OHOS ThreadBlock6S tolerates: the app was
// killed with a THREAD_BLOCK_6S fault before the result line was printed.
// The export itself is the SDK's normal job, so this file is also the
// reference shape for an app: build the draft on the UI thread, hand it to
// a worker that owns its own engine handle, get plain JSON back.

import { CapTrim } from 'captrim';

/** Serializable result of one export; assertions stay on the calling thread. */
export interface TimelineExportResult {
  ticks: number;
  lastProgress: number;
  probe: string;
}

/** Serializable result of one burn export pair; file sizes are read by the caller. */
export interface BurnResult {
  ticks: number;
  probe: string;
}

/**
 * Burn this timeline's captions and stickers into the picture, plus the same
 * pipeline without any still layer as a baseline.
 *
 * Both transcodes belong on a worker for the same reason the timeline export
 * does: 2 s of 540x960 through x264 takes long enough to trip ThreadBlock6S on
 * the UI thread. The baseline runs the identical chain, so the only difference
 * between the two files is the overlay itself.
 */
@Concurrent
export function burnTask(draftJson: string, srcUri: string, dstUri: string, baseUri: string,
                         prefix: string, workDir: string): string {
  const engine: CapTrim = CapTrim.create(draftJson);
  try {
    let ticks: number = 0;
    engine.exportBurnSubtitles(srcUri, dstUri, prefix, 'fps=15', workDir, '',
      (value: number): boolean => {
        ticks++;
        return true;
      });
    engine.exportTranscode(srcUri, baseUri, `${prefix},fps=15`, '',
      (value: number): boolean => true);
    const result: BurnResult = {
      ticks: ticks,
      probe: engine.probe(`file://${dstUri}`)
    };
    return JSON.stringify(result);
  } finally {
    engine.close();
  }
}

/** Export one timeline; returns the progress tick count, last value and probe. */
@Concurrent
export function exportTimelineTask(draftJson: string, dstUri: string, workDir: string): string {
  // A worker has no UI, so it builds its own engine from the draft the UI
  // thread serialised. Nothing but primitives crosses the thread boundary.
  const engine: CapTrim = CapTrim.create(draftJson);
  try {
    let ticks: number = 0;
    let last: number = 0;
    // `workDir` is where the core rasterises caption and sticker bitmaps for
    // this export; it is the same sandbox directory the burn check uses.
    engine.exportTimeline(dstUri, workDir, '', 22, 'veryfast', 160,
      (value: number): boolean => {
        ticks++;
        last = value;
        return true;
      });
    const result: TimelineExportResult = {
      ticks: ticks,
      lastProgress: last,
      probe: engine.probe(`file://${dstUri}`)
    };
    return JSON.stringify(result);
  } finally {
    engine.close();
  }
}
