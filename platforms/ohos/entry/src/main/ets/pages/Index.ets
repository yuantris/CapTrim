import { common } from '@kit.AbilityKit';
import { fileIo as fs } from '@kit.CoreFileKit';
import { systemDateTime } from '@kit.BasicServicesKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { CapTrim, CapTrimPresenter, CapTrimPreview, PresentReport } from 'captrim';
import { CheckResult, SelfCheck } from './SelfCheck';

const DOMAIN: number = 0x0001;
const TAG: string = 'CapTrimDemo';
/**
 * Status poll interval. The render loop runs on a native thread at frame rate;
 * the UI only needs to refresh its labels, so polling at 4 Hz keeps the ArkTS
 * event loop idle without making the numbers look frozen.
 */
const POLL_MS: number = 250;
const CLIP_US: number = 4_000_000;

/** 单调时钟。换成 Date.now() 会被 NTP 回拨，播放会卡死。 */
function nowUs(): number {
  return systemDateTime.getUptime(systemDateTime.TimeType.ACTIVE) * 1000;
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

interface ClipSpec {
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

interface InsertClipCmd {
  type: string;
  track_id: number;
  clip: ClipSpec;
}

interface AddTrackCmd {
  type: string;
  kind: string;
}

interface SetTransformCmd {
  type: string;
  clip_id: number;
  transform: TransformJson;
}

function clipSpec(source: number, inUs: number, outUs: number, startUs: number,
                  transform?: TransformJson): ClipSpec {
  const clip: ClipSpec = {
    id: 0,
    source: source,
    in_us: inUs,
    out_us: outUs,
    start_us: startUs,
    speed: { num: 1, den: 1 },
    volume: 1,
    transform: transform ?? { scale: 1, dx: 0, dy: 0, rotation_deg: 0 },
    effects: []
  };
  return clip;
}

@Entry
@Component
struct Index {
  @State version: string = '';
  @State checks: CheckResult[] = [];
  @State summary: string = '点「运行自检」验证这台设备上的核心库';
  @State mediaState: string = '未准备';
  @State playing: boolean = false;
  @State transformed: boolean = false;
  @State positionMs: number = 0;
  @State dropped: number = 0;
  @State drawnFrames: number = 0;
  @State frameInfo: string = '等待画面';

  private controller: XComponentController = new XComponentController();
  private demo: CapTrim | null = null;
  private preview: CapTrimPreview | null = null;
  private presenter: CapTrimPresenter | null = null;
  private pollId: number = -1;
  private filesDir: string = '';
  private mediaReady: boolean = false;
  /** Ids of the two main-track demo clips. The transform button only touches
   * these, so the picture-in-picture clip keeps its own placement and the two
   * layers stay visually distinguishable. */
  private clipIds: number[] = [];
  /** The picture-in-picture clip: second decoder, second NativeImage. */
  private pipClipId: number = 0;
  /** Surface size in physical pixels, kept in step with onAreaChange. */
  private canvasWidth: number = 0;
  private canvasHeight: number = 0;

  aboutToAppear(): void {
    this.version = CapTrim.version();
    hilog.info(DOMAIN, TAG, 'core version %{public}s', this.version);
    this.prepareMedia();
    this.runChecks();
  }

  aboutToDisappear(): void {
    this.stopPolling();
    this.releasePresenter();
    if (this.preview !== null) {
      this.preview.close();
      this.preview = null;
    }
    if (this.demo !== null) {
      this.demo.close();
      this.demo = null;
    }
  }

  // ---- media -------------------------------------------------------------

  /**
   * Copy the bundled sample into the app sandbox.
   *
   * hdc cannot write the app's files dir (shell does not own it), so the sample
   * ships as a rawfile and lands here on first launch. Two copies are used so
   * playback crosses a real source switch, which is exactly the case that breaks
   * when the decoder is not torn down between clips.
   */
  private prepareMedia(): void {
    const ctx = getContext(this) as common.UIAbilityContext;
    this.filesDir = ctx.filesDir;
    const first = `${this.filesDir}/a.mp4`;
    const second = `${this.filesDir}/b.mp4`;
    try {
      if (!fs.accessSync(first)) {
        const data = ctx.resourceManager.getRawFileContentSync('sample.mp4');
        const file = fs.openSync(first, fs.OpenMode.READ_WRITE | fs.OpenMode.CREATE);
        fs.writeSync(file.fd, data.buffer);
        fs.closeSync(file);
      }
      if (!fs.accessSync(second)) {
        fs.copyFileSync(first, second);
      }
      this.mediaReady = true;
      this.mediaState = '样片就绪';
      hilog.info(DOMAIN, TAG, 'sample media ready under %{public}s', this.filesDir);
    } catch (err) {
      const error = err as Error;
      this.mediaReady = false;
      this.mediaState = '无样片（构建时未注入）';
      hilog.warn(DOMAIN, TAG, 'no sample media: %{public}s', error.message);
    }
  }

  /** Two clips back to back, so a source switch happens at 4 s. */
  /**
   * The demo timeline: two clips back to back on the main track (so a source
   * switch at 4 s exercises decoder teardown), plus a picture-in-picture clip on
   * an overlay track for the first 4 s.
   *
   * The PiP exists to prove the part that cannot be faked: two decoders running
   * at once, each writing into its own NativeImage, composited by z order. Its
   * transform is set at insert time so the picture is visibly not the main one
   * the moment the app opens -- if it were identity and full size, a broken
   * second decoder would be indistinguishable from a working one.
   */
  private ensureDemo(): CapTrim {
    if (this.demo !== null) {
      return this.demo;
    }
    const editor = CapTrim.create();
    const a = editor.addMedia(`file://${this.filesDir}/a.mp4`);
    const b = editor.addMedia(`file://${this.filesDir}/b.mp4`);
    const track: AddTrackCmd = { type: 'add_track', kind: 'main_video' };
    const trackId = editor.apply(JSON.stringify(track));
    const overlay: AddTrackCmd = { type: 'add_track', kind: 'overlay' };
    const overlayId = editor.apply(JSON.stringify(overlay));

    const first: InsertClipCmd = {
      type: 'insert_clip', track_id: trackId, clip: clipSpec(a, 0, CLIP_US, 0)
    };
    const second: InsertClipCmd = {
      type: 'insert_clip', track_id: trackId, clip: clipSpec(b, 0, CLIP_US, CLIP_US)
    };
    // Bottom-right, about 42% of the canvas wide, nudged down and right. Units
    // are canvas-relative (see Transform in captrim-core/model.rs), so this
    // looks the same whatever size the XComponent ends up being.
    const pipTransform: TransformJson = { scale: 0.42, dx: 0.28, dy: 0.33, rotation_deg: 0 };
    const pip: InsertClipCmd = {
      type: 'insert_clip', track_id: overlayId, clip: clipSpec(b, 0, CLIP_US, 0, pipTransform)
    };
    this.clipIds.push(editor.apply(JSON.stringify(first)));
    this.clipIds.push(editor.apply(JSON.stringify(second)));
    this.pipClipId = editor.apply(JSON.stringify(pip));
    this.demo = editor;
    hilog.info(DOMAIN, TAG, 'demo timeline %{public}dus / %{public}d frames, pip clip %{public}d',
      editor.duration(), editor.frameCount(), this.pipClipId);
    return editor;
  }

  // ---- surface -----------------------------------------------------------

  /**
   * The compositor works in physical pixels, ArkUI lays out in vp. Every layer's
   * scale and offset is relative to this canvas, so the conversion has to happen
   * once, here, rather than being guessed at from the window.
   */
  private updateCanvas(widthVp: number, heightVp: number): void {
    if (widthVp <= 0 || heightVp <= 0) {
      return;
    }
    const w = Math.round(this.getUIContext().vp2px(widthVp));
    const h = Math.round(this.getUIContext().vp2px(heightVp));
    if (w === this.canvasWidth && h === this.canvasHeight) {
      return;
    }
    this.canvasWidth = w;
    this.canvasHeight = h;
    if (this.presenter !== null) {
      try {
        this.presenter.resize(w, h);
      } catch (err) {
        const error = err as Error;
        hilog.error(DOMAIN, TAG, 'resize failed: %{public}s', error.message);
      }
    }
  }

  private attachSurface(): void {
    if (!this.mediaReady) {
      return;
    }
    const surfaceId = this.controller.getXComponentSurfaceId();
    if (surfaceId.length === 0) {
      this.frameInfo = 'surface 未就绪';
      return;
    }
    this.releasePresenter();
    // onAreaChange may not have run yet on the very first layout pass. A zero
    // canvas would make every layer degenerate, so fall back to the display size
    // and let the next layout report correct it.
    const width = this.canvasWidth > 0 ? this.canvasWidth : 1080;
    const height = this.canvasHeight > 0 ? this.canvasHeight : 1920;
    const presenter = CapTrimPresenter.create(this.ensureDemo(), surfaceId, width, height);
    if (presenter === null) {
      this.frameInfo = 'presenter 创建失败（surface 可能还没挂上）';
      hilog.error(DOMAIN, TAG, 'presenter create failed for surface %{public}s', surfaceId);
      return;
    }
    this.presenter = presenter;

    // The native loop owns the preview session from here on. That is the whole
    // point: decoding blocks until the frame is on the surface, and doing that
    // on the ArkTS thread is what got the app killed for being unresponsive.
    const preview = this.ensurePreview();
    try {
      presenter.start(preview, nowUs());
      this.frameInfo = '渲染线程已启动 · surface ' + surfaceId;
      hilog.info(DOMAIN, TAG, 'render loop started on surface %{public}s', surfaceId);
    } catch (err) {
      const error = err as Error;
      this.frameInfo = '启动渲染线程失败: ' + error.message;
      hilog.error(DOMAIN, TAG, 'present start failed: %{public}s', error.message);
      return;
    }
    // Draw the first frame at t=0 without waiting for the user to hit play.
    presenter.control(-1, 0);
    this.startPolling();
  }

  private releasePresenter(): void {
    if (this.presenter !== null) {
      this.presenter.close();
      this.presenter = null;
    }
  }

  // ---- playback ----------------------------------------------------------

  private ensurePreview(): CapTrimPreview {
    if (this.preview === null) {
      this.preview = this.ensureDemo().preview();
    }
    return this.preview;
  }

  private startPolling(): void {
    if (this.pollId !== -1) {
      return;
    }
    this.pollId = setInterval((): void => {
      this.poll();
    }, POLL_MS);
  }

  private stopPolling(): void {
    if (this.pollId !== -1) {
      clearInterval(this.pollId);
      this.pollId = -1;
    }
  }

  private togglePlay(): void {
    if (!this.mediaReady) {
      this.frameInfo = '没有样片，构建时需注入视频';
      return;
    }
    if (this.presenter === null) {
      this.frameInfo = '等 surface 挂载后再播放';
      return;
    }
    const playing = this.playing ? 0 : 1;
    this.presenter.control(playing, -1);
    this.playing = playing === 1;
    this.frameInfo = this.playing ? '播放中' : '已暂停';
  }

  private seekTo(tUs: number): void {
    if (!this.mediaReady) {
      return;
    }
    if (this.presenter === null) {
      this.frameInfo = '等 surface 挂载后再跳转';
      return;
    }
    // A seek while paused still redraws: the loop re-plans and presents the
    // frame under the playhead.
    this.presenter.control(-1, tUs);
    this.positionMs = tUs / 1000;
  }

  /**
   * Push a non-identity transform through the whole chain: the engine rewrites
   * the clip, the plan carries it, and the GL compositor applies it. If the
   * picture does not shrink and shift, one of those three is ignoring the
   * contract -- which is what this button exists to catch.
   *
   * No seek afterwards: the preview pulls the editor's timeline whenever its
   * revision changes, so the next step returns Present for the same instant and
   * the compositor redraws with the new geometry.
   */
  private toggleTransform(): void {
    if (this.demo === null || this.clipIds.length === 0) {
      return;
    }
    const next: boolean = !this.transformed;
    const transform: TransformJson = next
      ? { scale: 0.6, dx: 0.15, dy: 0.1, rotation_deg: 10 }
      : { scale: 1, dx: 0, dy: 0, rotation_deg: 0 };
    for (const id of this.clipIds) {
      const cmd: SetTransformCmd = { type: 'set_transform', clip_id: id, transform: transform };
      try {
        this.demo.apply(JSON.stringify(cmd));
      } catch (err) {
        const error = err as Error;
        this.frameInfo = '设置变换失败: ' + error.message;
        hilog.error(DOMAIN, TAG, 'set_transform failed: %{public}s', error.message);
        return;
      }
    }
    this.transformed = next;
    this.frameInfo = next ? '变换已应用（scale 0.6 / dx 0.15 / 旋转 10°）' : '变换已复位';
    hilog.info(DOMAIN, TAG, 'transform toggled: %{public}s', next ? 'on' : 'off');
  }

  /** Read the loop's latest snapshot. Never blocks: no decode happens here. */
  private poll(): void {
    const presenter = this.presenter;
    if (presenter === null) {
      return;
    }
    try {
      const report: PresentReport = presenter.status();
      this.positionMs = report.position_us / 1000;
      this.dropped = report.dropped;
      this.drawnFrames = report.frames;

      const source = report.source_us < 0 ? '-' : `${(report.source_us / 1000).toFixed(0)}ms`;
      if (report.error !== 0) {
        this.playing = false;
        this.frameInfo = `渲染线程已停止: ${report.error}`;
        hilog.error(DOMAIN, TAG, 'render loop error %{public}d', report.error);
        return;
      }
      this.frameInfo =
        `${report.kind} · 源 ${source} · 图层 ${report.layers} · ` +
        `已上屏 ${report.frames} · 丢帧 ${report.dropped}`;
      if (report.kind === 'end' && this.playing) {
        this.playing = false;
        this.frameInfo = '播放结束';
      }
    } catch (err) {
      const error = err as Error;
      this.frameInfo = '读取状态失败: ' + error.message;
      hilog.error(DOMAIN, TAG, 'status failed: %{public}s', error.message);
    }
  }

  // ---- self test ---------------------------------------------------------

  private pipelineScheduled: boolean = false;

  private appendChecks(extra: CheckResult[]): void {
    for (const item of extra) {
      hilog.info(DOMAIN, TAG, 'check %{public}s %{public}s | %{public}s',
        item.pass ? 'PASS' : 'FAIL', item.name, item.detail);
    }
    const all: CheckResult[] = this.checks.concat(extra);
    let passed = 0;
    for (const item of all) {
      if (item.pass) {
        passed++;
      }
    }
    this.checks = all;
    this.summary = `${passed}/${all.length} 项通过`;
    hilog.info(DOMAIN, TAG, 'selfcheck %{public}d/%{public}d passed', passed, all.length);
  }

  private runChecks(): void {
    this.summary = '运行中…';
    const results: CheckResult[] = SelfCheck.run(this.filesDir);
    let passed = 0;
    for (const item of results) {
      if (item.pass) {
        passed++;
      }
      hilog.info(DOMAIN, TAG, 'check %{public}s %{public}s | %{public}s',
        item.pass ? 'PASS' : 'FAIL', item.name, item.detail);
    }
    this.checks = results;
    this.summary = `${passed}/${results.length} 项通过`;
    hilog.info(DOMAIN, TAG, 'selfcheck %{public}d/%{public}d passed', passed, results.length);

    // The three hybrid-pipeline checks are deferred, one per UI task. SelfCheck
    // is synchronous and the 12 checks above already contain a ~17s software
    // transcode; chaining long jobs back-to-back makes OHOS report
    // ThreadBlock6S and kill the app with signal 9 (verified on Mate 60).
    // Letting the event loop breathe between tasks keeps the watchdog happy.
    if (!this.pipelineScheduled) {
      this.pipelineScheduled = true;
      const filesDir: string = this.filesDir;
      setTimeout((): void => {
        this.appendChecks(SelfCheck.runPipelineChecks(filesDir, 0));
        // Give the UI thread a real idle window before each heavy job.
        // Chaining them back-to-back still trips ThreadBlock6S even though
        // each job is short, because the 12 checks above already burned most
        // of the watchdog's tolerance with the ~17s software transcode.
        setTimeout((): void => {
          this.appendChecks(SelfCheck.runPipelineChecks(filesDir, 1));
          setTimeout((): void => {
            this.appendChecks(SelfCheck.runPipelineChecks(filesDir, 2));
            setTimeout((): void => {
              this.appendChecks(SelfCheck.runPipelineChecks(filesDir, 3));
            }, 8000);
          }, 8000);
        }, 8000);
      }, 800);
    }
  }

  @Builder
  resultRow(item: CheckResult) {
    Row() {
      Text(item.pass ? '✓' : '✗')
        .fontSize(16)
        .fontColor(item.pass ? '#1A9E5C' : '#D93025')
        .width(24)
      Column() {
        Text(item.name)
          .fontSize(14)
          .fontColor('#1B1B1F')
          .width('100%')
        Text(item.detail)
          .fontSize(11)
          .fontColor('#6B6B72')
          .width('100%')
          .maxLines(2)
          .textOverflow({ overflow: TextOverflow.Ellipsis })
      }
      .layoutWeight(1)
      .alignItems(HorizontalAlign.Start)
    }
    .width('100%')
    .padding({ top: 6, bottom: 6 })
    .alignItems(VerticalAlign.Top)
  }

  build() {
    Column() {
      Row() {
        Column() {
          Text('CapTrim 真机自检')
            .fontSize(20)
            .fontWeight(FontWeight.Bold)
            .fontColor('#1B1B1F')
          Text(`core ${this.version} · ${this.summary}`)
            .fontSize(11)
            .fontColor('#6B6B72')
            .margin({ top: 2 })
        }
        .layoutWeight(1)
        .alignItems(HorizontalAlign.Start)

        Button('运行自检')
          .height(32)
          .fontSize(13)
          .onClick((): void => {
            this.runChecks();
          })
      }
      .width('100%')
      .margin({ bottom: 8 })

      List({ space: 0 }) {
        ForEach(this.checks, (item: CheckResult) => {
          ListItem() {
            this.resultRow(item)
          }
        }, (item: CheckResult) => item.name)
      }
      .layoutWeight(1)
      .width('100%')
      .divider({ strokeWidth: 1, color: '#E6E6EA' })
      .scrollBar(BarState.Auto)

      Column() {
        Row() {
          Text('硬件解码 + GL 合成预览')
            .fontSize(15)
            .fontWeight(FontWeight.Medium)
            .fontColor('#1B1B1F')
          Blank()
          Text(`${this.mediaState} · ${this.positionMs.toFixed(0)}ms · 丢帧 ${this.dropped}`)
            .fontSize(11)
            .fontColor('#6B6B72')
        }
        .width('100%')
        .margin({ bottom: 8 })

        XComponent({
          id: 'captrimSurface',
          type: XComponentType.SURFACE,
          controller: this.controller
        })
          .width('100%')
          .height(200)
          .backgroundColor('#000000')
          .onLoad((): void => {
            this.attachSurface();
          })
          .onAreaChange((_old: Area, area: Area): void => {
            this.updateCanvas(Number(area.width), Number(area.height));
          })
          .onDestroy((): void => {
            this.releasePresenter();
          })

        Text(this.frameInfo)
          .fontSize(11)
          .fontColor('#3C3C43')
          .width('100%')
          .margin({ top: 6, bottom: 8 })
          .maxLines(2)

        Row({ space: 8 }) {
          Button(this.playing ? '暂停' : '播放')
            .height(32)
            .fontSize(13)
            .layoutWeight(1)
            .onClick((): void => {
              this.togglePlay();
            })
          Button('回到 0s')
            .height(32)
            .fontSize(13)
            .layoutWeight(1)
            .backgroundColor('#E8E8ED')
            .fontColor('#1B1B1F')
            .onClick((): void => {
              this.seekTo(0);
            })
          Button('跳到 4s')
            .height(32)
            .fontSize(13)
            .layoutWeight(1)
            .backgroundColor('#E8E8ED')
            .fontColor('#1B1B1F')
            .onClick((): void => {
              this.seekTo(4_000_000);
            })
        }
        .width('100%')

        Row({ space: 8 }) {
          Button(this.transformed ? '主视频复位' : '主视频缩放')
            .height(32)
            .fontSize(13)
            .layoutWeight(1)
            .backgroundColor('#E8E8ED')
            .fontColor('#1B1B1F')
            .onClick((): void => {
              this.toggleTransform();
            })
          Button('切到 2s')
            .height(32)
            .fontSize(13)
            .layoutWeight(1)
            .backgroundColor('#E8E8ED')
            .fontColor('#1B1B1F')
            .onClick((): void => {
              this.seekTo(2_000_000);
            })
        }
        .width('100%')
        .margin({ top: 8 })
      }
      .width('100%')
      .padding(12)
      .borderRadius(12)
      .backgroundColor('#FFFFFF')
      .margin({ top: 8 })
    }
    .width('100%')
    .height('100%')
    .padding(16)
    .backgroundColor('#F5F6F8')
  }
}
