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

function clipSpec(source: number, inUs: number, outUs: number, startUs: number): ClipSpec {
  const clip: ClipSpec = {
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

@Entry
@Component
struct Index {
  @State version: string = '';
  @State checks: CheckResult[] = [];
  @State summary: string = '点「运行自检」验证这台设备上的核心库';
  @State mediaState: string = '未准备';
  @State playing: boolean = false;
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
  private ensureDemo(): CapTrim {
    if (this.demo !== null) {
      return this.demo;
    }
    const editor = CapTrim.create();
    const a = editor.addMedia(`file://${this.filesDir}/a.mp4`);
    const b = editor.addMedia(`file://${this.filesDir}/b.mp4`);
    const track: AddTrackCmd = { type: 'add_track', kind: 'main_video' };
    const trackId = editor.apply(JSON.stringify(track));
    const first: InsertClipCmd = {
      type: 'insert_clip', track_id: trackId, clip: clipSpec(a, 0, CLIP_US, 0)
    };
    const second: InsertClipCmd = {
      type: 'insert_clip', track_id: trackId, clip: clipSpec(b, 0, CLIP_US, CLIP_US)
    };
    editor.apply(JSON.stringify(first));
    editor.apply(JSON.stringify(second));
    this.demo = editor;
    hilog.info(DOMAIN, TAG, 'demo timeline %{public}dus / %{public}d frames',
      editor.duration(), editor.frameCount());
    return editor;
  }

  // ---- surface -----------------------------------------------------------

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
    const presenter = CapTrimPresenter.create(this.ensureDemo(), surfaceId);
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

  private runChecks(): void {
    this.summary = '运行中…';
    const results: CheckResult[] = SelfCheck.run();
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
          Text('硬件解码预览')
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
