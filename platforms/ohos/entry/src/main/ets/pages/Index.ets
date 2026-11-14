import { systemDateTime } from '@kit.BasicServicesKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { CapTrim, CapTrimPreview, FramePlan, Step } from 'captrim';
import { CheckResult, Fixture, SelfCheck } from './SelfCheck';

const DOMAIN: number = 0x0001;
const TAG: string = 'CapTrimDemo';

/** 单调递增的渲染时钟。绝不能换成 Date.now()：系统时间会跳变。 */
function nowUs(): number {
  return systemDateTime.getUptime(systemDateTime.TimeType.ACTIVE) * 1000;
}

const TICK_MS: number = 16;

@Entry
@Component
struct Index {
  @State version: string = '';
  @State checks: CheckResult[] = [];
  @State summary: string = '点「运行自检」验证这台设备上的核心库';
  @State playing: boolean = false;
  @State positionMs: number = 0;
  @State dropped: number = 0;
  @State frameInfo: string = '-';
  private fixture: Fixture | null = null;
  private preview: CapTrimPreview | null = null;
  private timerId: number = -1;

  aboutToAppear(): void {
    this.version = CapTrim.version();
    hilog.info(DOMAIN, TAG, 'core version %{public}s', this.version);
    // Run once on open so a headless install (hdc install + aa start) proves the
    // native core on the device without anyone tapping the screen.
    this.runChecks();
  }

  aboutToDisappear(): void {
    this.stopTicking();
    if (this.preview !== null) {
      this.preview.close();
      this.preview = null;
    }
    if (this.fixture !== null) {
      this.fixture.close();
      this.fixture = null;
    }
  }

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

  private ensurePreview(): CapTrimPreview {
    if (this.fixture === null) {
      this.fixture = new Fixture();
    }
    if (this.preview === null) {
      this.preview = this.fixture.editor.preview();
    }
    return this.preview;
  }

  private startTicking(): void {
    if (this.timerId !== -1) {
      return;
    }
    this.timerId = setInterval((): void => {
      this.tick();
    }, TICK_MS);
  }

  private stopTicking(): void {
    if (this.timerId !== -1) {
      clearInterval(this.timerId);
      this.timerId = -1;
    }
  }

  private togglePlay(): void {
    const preview = this.ensurePreview();
    const now = nowUs();
    if (this.playing) {
      preview.setPlaying(false, now);
      this.playing = false;
      this.stopTicking();
      this.frameInfo = '已暂停';
      return;
    }
    preview.setPlaying(true, now);
    this.playing = true;
    this.startTicking();
  }

  private seekTo(tUs: number): void {
    const preview = this.ensurePreview();
    preview.seek(tUs, nowUs());
    this.positionMs = preview.position(nowUs()) / 1000;
    this.frameInfo = '已跳转';
    if (this.playing) {
      this.tick();
    }
  }

  private tick(): void {
    if (this.preview === null) {
      return;
    }
    const preview = this.preview;
    const now = nowUs();
    const step: Step = preview.step(now);
    this.positionMs = preview.position(now) / 1000;
    this.dropped = preview.droppedFrames();
    if (step.kind === 'present' && step.frame !== undefined) {
      const frame: FramePlan = step.frame;
      this.frameInfo = `帧 #${frame.frame_index} · ${frame.layers.length} 层 · ${frame.width}x${frame.height}`;
    } else if (step.kind === 'idle') {
      this.frameInfo = 'idle（画面已是最新）';
    } else if (step.kind === 'end') {
      this.frameInfo = '播放结束';
      this.playing = false;
      this.stopTicking();
    }
  }

  @Builder
  resultRow(item: CheckResult) {
    Row() {
      Text(item.pass ? '✓' : '✗')
        .fontSize(18)
        .fontColor(item.pass ? '#1A9E5C' : '#D93025')
        .width(28)
      Column() {
        Text(item.name)
          .fontSize(15)
          .fontColor('#1B1B1F')
          .width('100%')
        Text(item.detail)
          .fontSize(12)
          .fontColor('#6B6B72')
          .width('100%')
          .maxLines(3)
          .textOverflow({ overflow: TextOverflow.Ellipsis })
      }
      .layoutWeight(1)
      .alignItems(HorizontalAlign.Start)
    }
    .width('100%')
    .padding({ top: 10, bottom: 10 })
    .alignItems(VerticalAlign.Top)
  }

  build() {
    Column() {
      Row() {
        Column() {
          Text('CapTrim 真机自检')
            .fontSize(22)
            .fontWeight(FontWeight.Bold)
            .fontColor('#1B1B1F')
          Text(`core ${this.version} · ${this.summary}`)
            .fontSize(12)
            .fontColor('#6B6B72')
            .margin({ top: 4 })
        }
        .layoutWeight(1)
        .alignItems(HorizontalAlign.Start)

        Button('运行自检')
          .height(36)
          .fontSize(14)
          .onClick((): void => {
            this.runChecks();
          })
      }
      .width('100%')
      .margin({ bottom: 12 })

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

      Divider().color('#E6E6EA').margin({ top: 8, bottom: 12 })

      Column() {
        Row() {
          Text('预览循环')
            .fontSize(15)
            .fontWeight(FontWeight.Medium)
            .fontColor('#1B1B1F')
          Blank()
          Text(`${this.positionMs.toFixed(0)} ms · 丢帧 ${this.dropped}`)
            .fontSize(12)
            .fontColor('#6B6B72')
        }
        .width('100%')

        Text(this.frameInfo)
          .fontSize(12)
          .fontColor('#3C3C43')
          .width('100%')
          .margin({ top: 6 })

        Row({ space: 8 }) {
          Button(this.playing ? '暂停' : '播放')
            .height(34)
            .fontSize(14)
            .layoutWeight(1)
            .onClick((): void => {
              this.togglePlay();
            })
          Button('回到 0s')
            .height(34)
            .fontSize(14)
            .layoutWeight(1)
            .backgroundColor('#E8E8ED')
            .fontColor('#1B1B1F')
            .onClick((): void => {
              this.seekTo(0);
            })
          Button('跳到 4s')
            .height(34)
            .fontSize(14)
            .layoutWeight(1)
            .backgroundColor('#E8E8ED')
            .fontColor('#1B1B1F')
            .onClick((): void => {
              this.seekTo(4_000_000);
            })
        }
        .width('100%')
        .margin({ top: 10 })
      }
      .width('100%')
      .padding(12)
      .borderRadius(12)
      .backgroundColor('#FFFFFF')
    }
    .width('100%')
    .height('100%')
    .padding(16)
    .backgroundColor('#F5F6F8')
  }
}
