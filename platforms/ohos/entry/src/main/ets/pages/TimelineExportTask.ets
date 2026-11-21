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

@Concurrent
export function exportTimelineTask(draftJson: string, dstUri: string): string {
  // A worker has no UI, so it builds its own engine from the draft the UI
  // thread serialised. Nothing but primitives crosses the thread boundary.
  const engine: CapTrim = CapTrim.create(draftJson);
  try {
    let ticks: number = 0;
    let last: number = 0;
    engine.exportTimeline(dstUri, 'libx264', 22, 'veryfast', 160,
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
