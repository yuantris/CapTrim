# CapTrim

Android 和鸿蒙共用的视频剪辑核心 SDK。Rust 描述时间线，FFmpeg 管封装/解封装、滤镜、缩放/重采样；编解码首选各平台硬件，FFmpeg 只是软件兜底。

一份 Rust 核心 + 一份 C ABI，两端只做薄桥接（Android JNI、鸿蒙 Node-API），业务 API 只定义一次。目标是覆盖剪映的基础剪辑能力：导入探测、分割、删除、排序、裁剪、变速、音量、音画分离、转场、滤镜、字幕、画中画、比例裁剪、导出。

现在的状态：P0 骨架、P1 调度内核、鸿蒙硬解 + GL 多图层合成、FFmpeg 复杂剪辑软链路、「硬解 → FFmpeg 复杂命令处理 → 硬编 → mp4」三段式导出，双端真机都跑通了——鸿蒙自检 17/17，Android native 自检 18/18（含 P2 音频 5 项），Android Kotlin 层自检 16/16，明细见[验证状态](#验证状态)。P2 音频输出与 hold-and-present 已在鸿蒙 HAP 真机验证：音频位置/帧数持续增长，播放头与音频主时钟差 7~25 ms，`held` 随预读增长。三段式的首尾两段各自独立回退到 FFmpeg 软解/软编，中间段两端共用一份实现，只靠弱符号工厂切换。OHOS 的同步/回调两种 Buffer 模式、Android 的 AMediaCodec Buffer 模式都在真机验证过，`fallback=[none]`，产物能被 FFmpeg 软解复验。预览的帧计划、时钟、状态机在 Rust 核心里，有测试覆盖。鸿蒙侧每层一个硬解码器，输出进各自的 `OH_NativeImage`，GLES 按 transform 合成到 XComponent，画中画双路解码也是实时的。

---

## 一、三条设计边界

这三条是地基，定错哪条都会立刻出问题：FFmpeg 职责不裁清楚，SDK 体积收不住；编解码不首选系统硬件，预览卡、导出慢；时间不用整数比，长视频必然音画漂移。另外，时间换算、调度这类只能有一份的逻辑，一旦两端各写一遍，就会长出两套 bug——所以核心全收进 Rust。

| 边界 | 决策 | 理由 |
|---|---|---|
| 编解码 | **首选系统硬件**：Android `AMediaCodec`，鸿蒙 AVCodec Kit（`OH_AVCodec*`）；FFmpeg（`libavcodec` + `libx264`/`libx265`）作为软件兜底 | 手机硬编解码更快更省电；但 FFmpeg 覆盖硬解不支持的格式，且本学习项目要求功能做全，故同时编译 x264/x265 |
| FFmpeg 职责 | `libavformat`（封装/解封装）、`libavcodec`（软解 + `libx264`/`libx265` 软编）、`libavfilter`、`libswscale`、`libswresample` | 文件层面系统 API 难用、格式窄；滤镜/缩放/重采样用 FFmpeg；编码兜底用 x264/x265；整体因此采用 **GPL** 而非 LGPL |
| 时间 | 全程整数比 `Rational`，微秒只在 FFI/JSON 边界出现 | `f64` 累加在长视频上必然产生音画漂移；变速重映射用浮点会累积误差 |

## 二、架构

```
┌──────────────────────────────────────────────────────────────┐
│  业务层（剪辑 App / 编辑页 / 时间线 UI）                        │
└───────────────┬──────────────────────────┬───────────────────┘
                │ Kotlin                   │ ArkTS
┌───────────────▼──────────────┐ ┌─────────▼───────────────────┐
│ Android AAR                  │ │ HarmonyOS HAR               │
│  CapTrim.kt   (API)          │ │  CapTrim.ets  (API)         │
│  jni_bridge.cpp (薄桥接)      │ │  napi_bridge.cpp (薄桥接)    │
└───────────────┬──────────────┘ └─────────┬───────────────────┘
                │  JNI                     │  Node-API
        ┌───────▼──────────────────────────▼───────┐
        │  libcaptrim.so  —  稳定 C ABI（14 个函数） │
        │  crates/captrim-ffi                      │
        │  · 句柄注册表（野指针 → 错误码，不是 UB）   │
        │  · 每个入口 catch_unwind                  │
        │  · 字符串所有权 + 统一错误码               │
        └───────┬──────────────────────────────────┘
                │
        ┌───────▼──────────────────────────────────┐
        │  crates/captrim-core  —  纯 Rust，零平台依赖│
        │  Rational 时间 · Timeline/Track/Clip      │
        │  Command 栈（撤销重做）· 草稿 JSON          │
        │  render 帧计划 · clock 时钟 · preview 状态机│
        └───────┬──────────────────────────────────┘
                │  MediaBackend trait
        ┌───────▼──────────────────────────────────┐
        │  native/captrim_ffmpeg.c（FFmpeg 软后端）  │
        │  probe · remux · 滤镜/重编码 transcode      │
        └──────────────────────────────────────────┘

  预览/导出共用同一份 Timeline：渲染与解码由各端实现注入，
  Rust 核心不含任何 #cfg 平台分支。
```

**为什么预览和导出必须共用一份描述**：只要两端各自解析一次素材，变速、转场、B 帧处就一定会出现「预览看到的」与「导出的」不一致。调度策略可以完全相反（预览丢帧保实时，导出逐帧保正确），但时间线描述只能有一份。

### 三段式导出管线（契约见 `native/captrim_pipeline.h`）

目标形态是 **硬解 → FFmpeg 复杂命令处理 → 硬编**，首尾各自独立回退，中间段全平台共用：

| 段 | 鸿蒙 | Android | 失败回退 |
| --- | --- | --- | --- |
| ① 解码 → `ct_frame_t` | `OH_VideoDecoder`（Buffer 模式） | `AMediaCodec`（ByteBuffer） | FFmpeg `libavcodec` 软解 |
| ② 滤镜 / 缩放 / 裁剪 / 帧率 | FFmpeg `libavfilter` + `libswscale` | 同左（唯一实现） | — |
| ③ 编码 + mp4 封装 | `OH_VideoEncoder` + `OH_AVMuxer` | `AMediaCodec` + `AMediaMuxer` | `libx264`/`libx265` + `libavformat` |

`ct_decoder_t` / `ct_processor_t` / `ct_encoder_t` 是纯 C 函数表，工厂函数返回 `NULL` 即表示该后端不可用，由 `ct_pipeline_run()` **按段**降级，不会一段失败就整条链路退回软件。

**2026-11-15 落地，鸿蒙自检 17/17**：`native/captrim_pipeline.c` 实现编排 + FFmpeg 软解/中间滤镜/软编三工厂 + `ct_pipeline_hardware_caps()`；`native/captrim_ohos_hw.c` 实现 OHOS 硬解（`OH_AVSource`/`OH_AVDemuxer`/`OH_VideoDecoder`）与硬编（`OH_VideoEncoder`/`OH_AVMuxer`）工厂，编进 OHOS 版 `libcaptrim_ffmpeg.so`。Android 构建编入 `native/captrim_android_hw.c`（`AMediaExtractor` + `AMediaCodec` + `AMediaMuxer`），真机验证 `decoder=2 encoder=2`（即 `CT_BACKEND_ANDROID_HW`）、`reason=none` 无回退；两端共用同一份编排层，只靠弱符号工厂切换。`ct_ff_transcode()` 按 `video_codec` 前缀路由：`libx264`/空 = 纯软直连；`hw:<encoder>` = 三段式硬件优先；`hw:sw:<encoder>` = 强制全软；`hw:cb:<encoder>` = 强制 OHOS 回调式 Buffer 模式（诊断用）。硬件段在流式阶段失败时，`ct_ff_transcode()` 整单重试全软，保证导出不失败。

**硬段的两种 Buffer 模式**：AVCodec Kit 的两个互斥形态都在 `captrim_ohos_hw.c` 里，共用同一套流式循环，只把「取空输入 buffer / 取已填充输出 buffer」两个原语换成不同实现：

| 模式 | 适用 API | 开启方式 | 真机结果 |
| --- | --- | --- | --- |
| 同步（快速路径） | 20+ | Configure 阶段设 `OH_MD_KEY_ENABLE_SYNC_MODE=1`，然后 `QueryInputBuffer`/`QueryOutputBuffer`/`Get*Buffer` 轮询；**不能注册回调** | `decoder=OHOS_HW encoder=OHOS_HW decoded=100 encoded=29 fallback=[none]` |
| 回调（官方示例） | 11+ | `RegisterCallback` + `OnNeedInputBuffer`/`OnNewOutputBuffer`，回调把 `(index, OH_AVBuffer*)` 推入 mutex/condvar 队列 | 同上，`in=29 out=29` |

之前 `AV_ERR_OPERATE_NOT_PERMIT(2)` 的根因不是「设备不支持同步模式」，而是默认就是异步模式：`Query*Buffer` 的头文件注释写了「仅同步模式可用 / 异步模式下返回 `AV_ERR_OPERATE_NOT_PERMIT`」。同步键用 `dlsym` 运行时解析，不产生硬符号依赖，老设备自动走回调；设备仍拒绝轮询的话，`open()` 销毁 codec 就地重建为回调模式。

Mate 60（ALN-AL00 / OpenHarmony 7.0.0 / API 26）上的日志：
```text
CapTrim OHOS: hardware decoder ACTIVE mode=sync NV12 1260x2720 dur=22729000us
CapTrim OHOS: hardware encoder ACTIVE mode=sync NV12 320x180 -> mp4
CapTrim OHOS: encoder input layout=padded w=320 h=180 stride=320 slice=180 cap=90112 need=86400
CapTrim OHOS: encoder finished mode=sync in=29 out=29
CapTrim pipeline: decoder=OHOS_HW encoder=OHOS_HW decoded=100 encoded=29 3448ms fallback=[none]
```
回调模式（`hw:cb:`）同样 `fallback=[none]`。两个硬编产物再被 FFmpeg 软 H.264 解码器重新解码 + 重编码成功（各 26 次进度回调），证明不是「只有容器元数据」的空壳 mp4。

YUV 跨距对齐是硬段桥接的第一坑：硬件解码器输出的每行按 16/32/64 字节对齐，`stride > width`，不能按 `width*height` 连续拷贝。`ct_frame_t.stride[4]` 必须逐平面填入，第②段用 `AVFrame.linesize[] = stride[]` 包装后再交给 libswscale 归一到 yuv420p。

依据的鸿蒙官方文档（2026-11-15 查阅）：文档中心索引 `https://developer.huawei.com/config/llms.txt` → [API Reference](https://developer.huawei.com/consumer/cn/llms/docs-references.txt) / [Guides](https://developer.huawei.com/consumer/cn/llms/docs-capabilities.txt) → [基于Buffer模式进行视频转码](https://developer.huawei.com/consumer/cn/doc/doccenter-capabilities/video-transcoding-buffer.md)、[视频编解码宽高、跨距与裁剪信息说明](https://developer.huawei.com/consumer/cn/doc/doccenter-capabilities/video-dimension-guide.md)、[视频编码](https://developer.huawei.com/consumer/cn/doc/doccenter-capabilities/video-encoding.md)、[视频解码](https://developer.huawei.com/consumer/cn/doc/doccenter-capabilities/video-decoding.md)、[VideoEncoder C API](https://developer.huawei.com/consumer/cn/doc/doccenter-references/api/capi-videoencoder.md)、[VideoDecoder C API](https://developer.huawei.com/consumer/cn/doc/doccenter-references/api/capi-videodecoder.md)、[OH_AVCodecCallback](https://developer.huawei.com/consumer/cn/doc/doccenter-references/api/capi-codecbase-oh-avcodeccallback.md)、[AVMuxer C API](https://developer.huawei.com/consumer/cn/doc/doccenter-references/api/capi-avmuxer.md)（其中回调结构体与跨距指南经 `devecocli docs search/read` 复核）。具体函数签名以本机 OHOS NDK 头文件 `native_avcodec_base.h`（`OH_AVCodecCallback`、`OH_MD_KEY_ENABLE_SYNC_MODE`）、`native_avcodec_videoencoder.h`、`native_avcodec_videodecoder.h`、`native_avmuxer.h` 为准。

## 三、目录

```
crates/captrim-core/      纯 Rust 核心：时间、模型、命令、草稿（零平台依赖）
crates/captrim-ffi/       稳定 C ABI + 后端抽象（captrim.h 是唯一契约）
native/                   FFmpeg C shim（probe / remux / 滤镜转码）+ 三段式管线 + 双端硬段工厂
platforms/android/        Gradle AAR + CMake + JNI + Kotlin
platforms/android/app/    Kotlin 层真机自检 App（16 项，结果写 logcat）
platforms/ohos/           HAR 工程 + CMake + Node-API + ArkTS + .d.ts
platforms/ohos/entry/     真机自检 App（17 项检查，结果写 hilog）
scripts/                  FFmpeg 裁剪编译、Rust 双端构建、ABI 校验、HAP 构建、双端真机自检、本地 CI 全套（check.sh）
```

## 四、快速开始

前置：Rust（含 `rustup target add aarch64-unknown-linux-ohos aarch64-linux-android`）。

**1. 只验证核心逻辑（不需要任何移动端环境）**

```bash
bash scripts/check.sh           # 本地把 CI 的每一关（除需要 NDK/Gradle 的 AAR 构建）按同样顺序跑一遍
```

单独跑某一步时：

```bash
cargo fmt --all --check            # CI 的第一关；漏跑它会让后面四关在 CI 上被跳过
cargo test --workspace             # 102 个测试
cargo clippy --workspace --all-targets --features captrim-ffi/ffmpeg -- -D warnings
bash scripts/check-native-abi.sh   # 头文件 == Rust 导出 == NAPI == d.ts
```

`check.sh` 的由来很具体：`b2d3158` 在 `cargo fmt --all --check` 上失败，GitHub 就把后面的测试、clippy、ABI 三步全跳过了——本地跑过的结果一次都没报出来，推送看着验证过，其实没有。所以把 CI 的每一关按同样顺序收进一个脚本，一次跑完。

**2. 鸿蒙侧（先跑 stub 后端，不需要 FFmpeg）**

```bash
export OHOS_NDK_HOME="/path/to/sdk/default/openharmony/native"
FEATURES= bash scripts/build-rust.sh ohos     # 产出并暂存 libs/arm64-v8a/
```

Windows 下脚本会自动选用 `scripts/toolchain/*.bat`（Cargo 无法把 `.sh` 当链接器执行），macOS/Linux 用 `*.sh`。

**3. 编译 FFmpeg（含 libx264 / libx265 + 滤镜/缩放/重采样，GPL 构建）**

脚本会自动拉取并交叉编译 x264 / x265，再编译 FFmpeg 与 C shim。需要 NDK 的 `make`（Android NDK 自带；鸿蒙 NDK 没有，可复用 Android NDK 的 `make` 或系统 `make`）以及 `cmake`（x265 用，Android SDK 自带）。Android 侧 x265 编成共享库 `libx265.so` 随 dist 分发（OHOS 侧静态编入）；两个 codec 都有安装缓存，构建被中断后重跑会自动续上，不会重新编译。Windows 上建议 `JOBS=3`（脚本上限 6）。

```bash
# HarmonyOS（需先设 OHOS_NDK_HOME；注意 DevEco 默认路径含空格会导致 make/sh 截断，
# 本机实测使用无空格路径 /d/Environment/command-line-tools/sdk/default/openharmony/native）
export OHOS_NDK_HOME="/d/Environment/command-line-tools/sdk/default/openharmony/native"
bash scripts/build-ffmpeg-ohos.sh
# Android（需先设 ANDROID_NDK_HOME）
export ANDROID_NDK_HOME="/d/Android/Sdk/ndk/27.3.13750724"
bash scripts/build-ffmpeg-android.sh
bash scripts/build-rust.sh ohos        # 或 android，默认 FEATURES=ffmpeg
```

**4. Android**

```bash
export ANDROID_NDK_HOME="/path/to/ndk"
bash scripts/build-ffmpeg-android.sh
bash scripts/build-rust.sh android
```

仓库没带 gradlew，本机用缓存的 Gradle 构建；两个真机自检脚本会自己找 Gradle 8.13：

```bash
bash scripts/run-android-selftest.sh        # native 层 18 项，推 /data/local/tmp 跑
bash scripts/run-android-app-selftest.sh    # Kotlin/JNI 层 16 项，装 App 跑
```

**5. 鸿蒙真机自检**

`platforms/ohos/entry` 是个可直接装到手机上的 App：打开即在设备上跑 17 项检查，把每一行结果写进 hilog。下半部分是真机预览：`播放 / 回到 0s / 跳到 4s` 走时间线与源切换，`缩放 + 平移` 通过 `set_transform` 改草稿，验证 `engine → plan → C ABI → GL 合成` 整条链路都按 `Transform` 契约工作。

```bash
export DEVECO_HOME="/path/to/DevEco Studio"   # 未设置时按 hdc 位置推断
bash scripts/build-ohos-hap.sh               # 产出 entry/build/.../entry-default-signed.hap
bash scripts/build-ohos-hap.sh install       # 构建 + hdc install + 拉起页面

hdc shell "hilog -x" | grep CapTrimDemo      # 看 check PASS/FAIL
```

首次构建需签名：在 DevEco Studio 打开 `platforms/ohos`，File → Project Structure → Signing Configs 勾选自动签名（需登录华为账号），签名材料写入同一份 `build-profile.json5`。未配置签名时 hvigor 只产出 unsigned HAP，`hdc install` 会报 `9568320 no signature file`。

## 五、API

### 命令即数据

所有编辑操作都是带标签的 JSON，两端完全一致，业务代码可直接搬运：

```json
{"type":"split_clip","clip_id":3,"at_us":4000000}
```

| `type` | 关键字段 |
|---|---|
| `add_media` | `uri` |
| `add_track` / `remove_track` | `kind`(video/audio/text/overlay) · `track_id` |
| `insert_clip` | `track_id` · `clip{source,in_point,out_point,timeline_start,speed,volume}` |
| `split_clip` | `clip_id` · `at_us` |
| `remove_clip` | `clip_id` |
| `move_clip` | `clip_id` · `timeline_start` · `track_id` |
| `set_speed` | `clip_id` · `speed{num,den}` |
| `set_volume` | `clip_id` · `volume` |
| `set_out_point` | `clip_id` · `out_us` |
| `set_transform` | `clip_id` · `transform{scale,dx,dy,rotation_deg}` |
| `set_transition` | `clip_id` · `transition{kind,duration_us}`（`null` 清除） |
| `set_effects` | `clip_id` · `effects[]`（整表替换，名字必须在 v1 词汇内） |
| `set_canvas` | `width` · `height` · `frame_rate{num,den}` |

### 两端用法

```kotlin
// Android
CapTrim.create(draftJson).use { editor ->
    val clipId = editor.apply("""{"type":"split_clip","clip_id":3,"at_us":4000000}""")
    editor.undo()
    val draft = editor.draft()

    // 预览：时钟与帧调度在 native 侧，这里只消费 step
    editor.preview().use { preview ->
        preview.setPlaying(true, System.nanoTime() / 1000)
        val step = JSONObject(preview.step(System.nanoTime() / 1000))
        if (step.getString("kind") == "present") render(step.getJSONObject("frame"))
    }
}
```

```typescript
// HarmonyOS
const editor = CapTrim.create(draftJson);
editor.apply('{"type":"split_clip","clip_id":3,"at_us":4000000}');
editor.undo();

const preview = editor.preview();
preview.setPlaying(true, monotonicUs());
const step: Step = preview.step(monotonicUs());
if (step.kind === 'present') render(step.frame);
preview.close();
editor.close();
```

ArkTS 侧 `Step` / `FramePlan` 是类型化的（`JSON.parse` 是原生实现）；Android 侧返回原始 JSON，解析交给宿主 App 已有的序列化库——理由见[第七节](#七已完成--未完成)末尾。

两端失败都抛异常并带 `CAPTRIM_ERR_*` 码。C ABI 的线程约定、字符串所有权写在 `crates/captrim-ffi/captrim.h` 头部，改代码前先读。

## 六、P1：预览管线

P1 做的事是把预览和导出钉在同一条时间线上，而不只是「能放视频」。做法是把三件事从平台层收进 Rust：

| 模块 | 职责 | 为什么必须在 Rust |
|---|---|---|
| `render` | 给定时刻，算出这一帧要画什么：图层次序（`z`）、源时间（已过变速映射并钳到 `[in,out)`）、音频混合项 | 导出逐帧走的是同一函数，两端不可能各解析一遍 |
| `clock` | 音频主导的播放时钟，漂移小于阈值时微调速率（slew），超过阈值硬对齐 | 漂移补偿策略写两遍，就是两端体验不一致的开始 |
| `preview` | `step(now)` 状态机：`present` / `wait_frame` / `idle` / `end`，并统计丢帧 | 平台只管解码和上屏，节奏由一处决定 |

平台侧因此只剩一件事：

```kotlin
// 每帧调用；nowUs 必须是单调时钟（System.nanoTime() / 1000）
when (step.kind) {
    "present"    -> { decode(step.frame); composite(step.frame.layers); present() }
    "wait_frame" -> sleepUntil(step.due_wall_us)
    "idle"       -> awaitUiEvent()
    "end"        -> stop()
}
```

三条实现约定，都踩过：

1. **`nowUs` 必须是单调时钟**。传 `System.currentTimeMillis()` 会出两种故障：时钟回拨导致播放卡死，向前跳跃导致整段缓冲被丢弃。鸿蒙用 `OH_GetTimeSinceBoot` 系列的微秒值。
2. **导出按帧索引走，按时间戳走会重复帧**。`frame_pts_us(1)` 对 30000/1001 返回 33366，但第 1 帧真正的起点是 33366.67 µs；再把这个值喂给按下取整的接口就会塌回第 0 帧。所以 `plan_frame_at(frame_index)` 是导出 API，`plan_frame(t_us)` 只用于拖动和播放。这条有专门的集成测试守着。
3. **编辑后不需要额外通知预览**。`Editor` 有单调递增的 `revision`，`preview_step` 每次比对它；变化了就重规划**同一帧索引**。否则改完滤镜画面会停在旧图，直到播放头移动。

**平台需要自己实现的部分**（鸿蒙侧已实现，见下一节；Android 侧仍是契约）：

- 解码器：`FrameLayer.source_us` 是精确的源时间，用它去 seek + 解码；空 `uri` 表示该层不可用，跳过而不是整帧失败。
- 合成器：按 `z` 从小到大画，`transform` / `opacity` / `effects` 直接消费；`kind` 区分视频、字幕、贴纸，决定走解码器还是文字引擎。
- 音频（P2 已实现）：`plan_audio_at(t_us)` 按**原始时间**（不吸附帧栅格）给出全轨 `AudioMix`，`captrim_audio` 按 URI 缓存解码器、统一重采样到 48kHz/立体声/f32 再叠加；鸿蒙 `OH_AudioRenderer` 把真实播放位置回灌 `syncAudio`，音频成为主时钟。

`Transform` 的单位是跨端契约，写在 `crates/captrim-core/src/model.rs`：`scale` 是**相对「适配画布后的尺寸」**的倍数（1.0 = 不裁切地填满，与分辨率无关），`dx`/`dy` 是画布归一化平移（1.0 = 一整个画布宽/高，+dy 向下），`rotation_deg` 绕层中心顺时针。预览与导出必须用同一套，否则「预览所见」和「文件所得」会不一致。

### 鸿蒙侧实现：渲染线程 + AVCodec + GL 合成

`platforms/ohos/captrim/src/main/cpp/` 是 P1 的平台半边，做法和 Android 将要用的一致：

- `presenter.{h,cpp}`：解码器生命周期、demuxer、喂帧节奏、渲染线程、命令队列
- `gl_compositor.{h,cpp}`：EGL 上下文、OES 纹理、按 transform 画 quad

```
ArkTS 页面                        native 渲染线程                  解码器回调线程
presentStart(preview, now) ───▶ LoopMain ──┐
presentControl(playing,seek) ─▶ 命令队列 ──┤
                                          ├─ step_frame() → PRESENT
                                          ├─ ComposePlan() ──────────▶ eglSwapBuffers
                                          │    ▲ UpdateSurfaceImage          ▲
                                          ├─ RequestFrame(source_us) ──▶ 喂帧 / Stop-Start 定位
                                          │    ▲ reached / eos               │
                                          │                              RenderOutputBuffer
                                          │                                    │
                                          └──────────── OH_NativeImage ◀───────┘
presentStatus() ◀── 快照(非阻塞) ──────────┘
```

这里的关键是解码器不直接写 XComponent：它写到 `OH_NativeImage`（鸿蒙的 SurfaceTexture），帧变成 GPU 纹理，再由合成器按计划画成 quad。直出更快，但只能画一个全屏画面——一旦有缩放、位移、第二个图层，就必须合成。

**每个视频图层一个解码器**。`presenter.cpp` 里没有「当前那个解码器」，只有一组 `SourceSlot`（demuxer + codec + NativeImage + texture，外加自己的 target/eos/reached）。一次 present 会按计划里出现的图层逐个要槽位：没有就开一个，开不出来就记一笔并跳过这一层（SoC 解码器实例用尽是真实可能），槽位不够就淘汰「最久没出现在计划里」的那个。画中画因此不再需要 core 做任何调度——计划早就在每一层上写了 `source_id` 和 `source_us`，平台侧照着开解码器即可。

有六条约束决定了这个形状：

1. **`present` 会阻塞**。它要等解码器把帧交给 surface（最多 ~120 ms）。早期版本在 ArkUI 的 `setInterval` 里直接调，系统判定主线程无响应并杀掉进程（`THREAD_BLOCK_6S`, `signal:9`）。现在循环跑在自己的线程，页面只发命令 + 读快照。
2. **预览会话只有一个所有者**。`seek` / `setPlaying` 若从 UI 线程直接打进去，会和正在解码的循环抢时钟锚点。命令通过队列交给渲染线程执行（`PresenterControl` 只是入队 + 唤醒条件变量）。
3. **不能跨 codec 调用持锁**。`OH_VideoDecoder_Flush/Stop` 会等回调线程返回，而回调要拿同一把锁去 signal —— 死锁后循环彻底静默。现在 `lock` 只保护状态与条件变量，每个槽位有自己的 `demux_lock` 保护自己的 demuxer（两路解码时共用一把锁会把两条互不相干的流水线串起来）。
4. **surface 队列很浅，先取帧再等帧**。`NativeImage` 只缓冲两三帧，若先在 `RenderAt` 里等「目标帧」，队列会一直满着，解码器被反压停住，而它要产的正是那个目标帧 —— 互相等待。实测表现为 8 秒素材只上屏 51 帧；改成「先 `ComposePlan` 排空队列，再请求下一帧」后恢复到 460+ 帧。
5. **不在计划里的图层也要继续排空**。某层的画面暂时离场时，如果就此不再 latch，它的解码器会把这轮喂进去的帧填满自己的 image 队列，然后卡在 `RenderOutputBuffer` 里 —— 而这个调用发生在回调线程上，之后的 `OH_VideoDecoder_Stop()` 会一直等它。所以每轮都把**所有活着的槽位**排空一遍，只是不画。
6. **离场再回来的图层要 seek 一次，且只能靠标志位判断**。播放中绝不 seek 追赶（那会自锁，见第 4 条）；但一个离屏两秒的图层，解码器就停在两秒前，靠「目标位置差得远」是区分不出「刚回到计划」和「解码器慢」的 —— 前者需要 seek，后者 seek 就会更慢。渲染循环知道真相，所以由它置 `resume` 标志，让这个槽位恰好 seek 一次。

### P2：音频输出与 hold-and-present

音频规划从帧计划里独立出来：`plan_audio_at(t_us)` 按原始时间读时间线，不吸附视频帧栅格。音频回调通常 10 ms 一次，视频帧 33 ms 一次，两边各走各的节奏，但读的是同一份剪辑；帧计划也复用这个函数，所以预览与导出不会听到不同的源时间。

`native/captrim_audio.c` 是跨平台混音器：按 URI 缓存解码器，FFmpeg 解出 PCM 后经 libswresample 统一到 48kHz/立体声/f32，再按 volume 叠加。音轨缺失或打不开就是一段静音，不让整段预览失败。

鸿蒙侧 `audio_output.cpp` 用 `OH_AudioRenderer`（`AUDIOSTREAM_SAMPLE_F32LE`）输出。回调里 `OH_AudioRenderer_GetTimestamp()` 取实际播放位置并写进原子量，渲染线程每步读它、调 `captrim_preview_sync_audio`——preview 会话仍然只有一个所有者。暂停时只写静音，恢复播放或 seek 时重新锚定。

hold-and-present 处理解码器的 300 ms 喂帧前瞻：播放中 PTS 超过播放头的帧先留在 codec buffer（最多 4 帧），等音频时钟追上再由渲染线程交给 surface；队列满就先释放最旧的，避免反压把解码器卡死。暂停、seek、销毁统一清空。状态快照新增 `audio_position_us` / `audio_frames` / `held` 三个数，能直接看出音频是否在走、hold 是否生效。

真机验证（最终 HAP，默认暂停启动）：自检仍 17/17。暂停态视频 `pos=0`，音频位置从 6.69 s 增到 10.69 s、音频帧从 311296 增到 487424，说明 renderer 回调一直在写 PCM；播放态日志 `pos=2023739 audio=1998250 frames=106496 held=131` → `pos=4864731 audio=4857375 frames=233472 held=285`，播放头与音频位置差 7~25 ms，`held` 随预读从 131 增到 285。Android native 18/18 另覆盖音频解码/重采样/seek/音量/静音。

官方依据（均从 `https://developer.huawei.com/config/llms.txt` 的 API Reference 索引进入）：《native_audiorenderer.h》 https://developer.huawei.com/consumer/cn/doc/doccenter-references/api/capi-native-audiorenderer-h.md ；《native_audiostreambuilder.h》 https://developer.huawei.com/consumer/cn/doc/doccenter-references/api/capi-native-audiostreambuilder-h.md 。索引页 https://developer.huawei.com/consumer/cn/llms/docs-references.txt 的同步时间为 2026-09-17。

### ExportPlan / Segment

导出不再逐帧向平台扔 JSON。`plan_export()` 从同一份时间线找出活跃片段发生变化的帧边界，切成连续的 `ExportSegment`：每段给出 `[start_frame, end_frame)`、活跃的视频/音频源、段首源时间和保留为 `Rational` 的速度。段内源时间是输出帧的线性函数，`ExportSegment::video_source_us_at()` / `audio_source_us_at()` 负责精确换算，避免平台把速度先转成 float 后慢慢漂移。段边界用帧索引而非裸微秒：片段起点落在两帧之间时，取**第一个 PTS 不小于起点**的帧；用 floor 会把片段提前一帧画出来。空洞也是合法段，只是源数组为空，渲染成黑场/静音。

`captrim_engine_export_plan()` 把整份计划作为 `ExportPlan` JSON 返回，Android Kotlin/JNI 和鸿蒙 Node-API/ArkTS 都暴露了同名入口。导出器可以先拉一次计划、缓存解码器，再按段推进；`plan_frame_at_index()` 仍是逐帧复核的唯一真相，两者的活跃源集合由同一组测试约束。

### 转场：Fade 交叉溶解

模型上转场是切入片段自己的属性：`Clip.transition_in: Option<Transition{kind, duration_us}>`（`Option` 缺省为硬切，`#[serde(default)]` 让旧草稿照常解析），配套 `set_transition` 命令（`transition: null` 即清除）。转场窗是**切入片起始处的半开区间** `[start, start + d)`，窗口长必须为正且短于片段本身。

窗口内的排布只有一条派生规则：**同轨相邻前驱的视频延长到 `end + d`**（`video_extension_us` 按「下一片段 start == 本片段 end 且带 transition」推导），源时间越过 `out_us` 后由 `clamp_source_us` 冻结在最后一帧；前驱 opacity 恒 1，切入片 opacity 从 0 线性升到 1，绘制序按 `(z, start_us, clip_id)` 让前驱垫底——合成结果恰好是 `B·u + A·(1-u)`，前驱若也渐变会二次衰减成 `A(1-u)²`。无前驱（首片段或空隙）时同一段 ramp 就是从黑淡入；邻接关系一消失（Move/Trim/Remove 都不用知道转场的存在）规则自动降级，草稿永远不会因为转场而加载失败。

选「窗口在切入片之内」而不是之前：片段位置、时长、split 语义全部不动，总时长不变，代价是前驱被多画 `d` 长（冻结帧）。音频 v1 刻意**硬切**：`plan_audio_at` 走 `contains`，前驱声音在切点准时结束、切入片满音量进入，避免冻结帧配持续出声的直流感；转场期音频交叉渐变留作后续项。`SplitClip` 里右半恒清转场（它开的是新切点），左半只有在窗放得下时才保留，放不下就丢弃转场而不是让整个 split 失败。

导出侧把同一窗口翻译成段边界（窗末多切一刀），`ExportVideoItem` 新增 `opacity_start` / `opacity_end`（serde 缺省 1.0），段内按帧号线性插值。两端数学上同源：预览是 `(t - start)/d`，导出是两个端点的 lerp，而 PTS 对帧索引是线性的，所以逐帧相等（测试里只留 1e-6 的 f32 舍入余量）。**零 ABI 变更、零 FFmpeg filter 白名单变更、GL 合成器零改动**——`opacity` 管道在 P1 就已铺到着色器。

### 滤镜：v1 词汇

滤镜的名字表只有一份：`effects.rs` 的 `CANONICAL_ORDER`，八个名字 `hflip / vflip / brightness / contrast / saturation / grayscale / invert / blur`。`effects` 是**集合**——顺序和重复都不携带语义，两端都按 canonical order 应用（几何翻转最先、色彩居中、模糊最后），重复项折叠成一次。`Clip::validate` 拒绝词汇外的名字，草稿在模型边界就带着一个没人渲染的效果加载不进来；`set_effects` 命令整表替换，和其余命令一样走撤销栈。

导出侧的唯一真相是 `effects_to_filter_desc()`：按 canonical order 拼成 FFmpeg 链，空表给空串，调用方可以无条件拼接。各名字的落点（与 GLSL 共享同一组常数，改动必须双边同步）：

| 名字 | FFmpeg | GLSL（合成器） |
|---|---|---|
| `hflip` / `vflip` | `hflip` / `vflip` | 局部坐标镜像 |
| `brightness` | `eq=brightness=0.15` | `c + 0.15` |
| `contrast` | `eq=contrast=1.30` | `(c - 0.5) * 1.30 + 0.5` |
| `saturation` | `eq=saturation=1.30` | `mix(luma, c, 1.30)` |
| `grayscale` | `hue=s=0` | `vec3(luma)` |
| `invert` | `negate` | `1.0 - c` |
| `blur` | `boxblur=2:1` | 5×5 box（半径 2，每轴一趟） |

FFmpeg 走 YUV、着色器走 RGB，两边是**近似相等**而非逐位相等（和 `scale`/`fit` 既有取舍一致）；必须相等的是「跑哪些效果、什么顺序」。这四个 filter（`eq`/`hue`/`negate`/`boxblur`）还没进 FFmpeg 裁剪白名单，预览侧合成器也还没读 `effects`（`CaptrimLayerDesc` 要扩效果位）——所以词汇层、校验、翻译器先落地并被测试钉死，白名单重编、GLSL 分支与导出接线是接下来的三步。

## 七、已完成 / 未完成

**已完成**

- `Rational` 精确时间运算（含 NTSC 30000/1001 帧边界、无浮点比较）
- Timeline / Track / Clip 模型、重叠校验、分割（含变速片段的源时间映射）
- Command 栈 + 撤销重做 + 草稿 JSON 往返
- 稳定 C ABI（30 个函数）：句柄注册表、错误码、`catch_unwind`、UTF-8 校验、二进制帧描述
- FFmpeg 后端（`libavformat` 探测/remux、`libavfilter`+`libswscale` 复杂剪辑、`libavcodec`/`libx264`/`libx265` 软编解兜底）+ 特征门控（无 `ffmpeg` feature 时返回 `NO_BACKEND` 而非崩溃）
- `ExportPlan` / `Segment`：帧对齐导出分段、段内稳定源集合、有理数速度与精确源时间换算，双端已暴露
- 转场 Fade 交叉溶解：`set_transition` 命令 + 切入片起始的窗口，前驱视频派生延长（冻结帧）、切入片 opacity ramp、绘制序 `(z, start_us, clip_id)`；导出翻译成段边界 + `opacity_start/opacity_end`，与预览逐帧同源；音频 v1 硬切
- 滤镜 v1 词汇层：`effects.rs` 单一词汇表（8 个名字，集合语义 + canonical order），`Clip::validate` 拒绝词汇外名字，`set_effects` 命令走撤销栈，`effects_to_filter_desc()` 把词汇翻译成 FFmpeg filter 链（与 GLSL 分支共享同一组常数）
- P1 调度内核：帧计划、音频主导时钟 + 漂移补偿、`step` 状态机、丢帧统计
- P2 音频预览：`plan_audio_at` 按原始时间规划音频（不吸附帧栅格）、native 混音器统一 48kHz/立体声/f32、鸿蒙 `OH_AudioRenderer` 输出并把真实播放位置回灌主时钟、播放态 hold-and-present 压住超前解码帧
- 双端工程：AAR（JNI + Kotlin）、HAR（Node-API + ArkTS + `.d.ts`）
- 鸿蒙真机自检 App：17 项端到端检查，含 29.97fps 帧栅格、丢帧统计、编辑后重绘、三段式管线的软件基线 / 同步硬段 / 回调式硬段与回退，以及硬编产物的 FFmpeg 软解复验；P2 音频输出与 hold-and-present 另有真机日志验证
- Android native 自检：18 项，含 P2 音频解码/重采样/seek/音量/静音、真实 x265 HEVC 编码复探和 AMediaCodec 硬解硬编链路（`scripts/run-android-selftest.sh`）
- Android Kotlin/JNI 层自检 App：16 项，走 `CapTrim.kt` 公开 API，和鸿蒙 `SelfCheck.ets` 同构（`scripts/run-android-app-selftest.sh`）
- 鸿蒙硬解 + GL 多图层合成上屏：每层一个解码器（`SourceSlot`：demuxer + codec + NativeImage + texture），解码器输出到各自的 `OH_NativeImage`（等同 Android SurfaceTexture），`gl_compositor` 用 EGL/GLES3 按 z 序把每层画成带缩放/平移/旋转/透明度的 quad，native 渲染线程 + 命令队列；源切换、跳转、暂停落点、transform、画中画双路实时播放都在真机验证过
- 构建与校验脚本：FFmpeg 裁剪 configure、双端 Rust 交叉编译、ABI/NAPI 漂移检查、HAP 打包、本地 CI 全套

**未完成（按优先级）**

- 补滤镜（词汇表/校验/FFmpeg 翻译层已完成，剩 FFmpeg 白名单重编、GLSL 消费、导出接线）、字幕渲染、图片与贴纸图层（转场 Fade 交叉溶解、P2 音频与 hold-and-present、`ExportPlan`/`Segment` 已完成）
- 转场后续：转场期音频交叉渐变（v1 刻意硬切）、更多转场类型（擦除/推拉等，复用同一窗口模型）、前驱素材越出裁剪点时用 handle material 代替冻结帧
- P3：变速保调、功耗与机型兼容矩阵、崩溃符号化
- 倒放：要求反向解码 + 全段帧缓存，复杂度约等于其余功能之和，明确留给 v2

逐帧数据不要走 JSON 往返：当前 `planFrame` / `step` 返回 JSON，是为了让 P1 骨架在没有渲染器时也能跑通、也能两端对齐。真实的合成器应当从 C++ 直接调 C ABI（同一份 `captrim_preview_step`），避免每帧一次 JSON 解析与跨语言拷贝。ArkTS 侧已做类型化封装，Android 侧返回原始 JSON 是刻意的：AAR 保持零依赖，也不塞一个无法用单元测试验证的手写解析器。

## 八、验证状态

只记在本机或真机上实际跑过的；没验证过的会单独标出来，别当成成熟度。

| 项 | 状态 | 说明 |
|---|---|---|
| `cargo test --workspace` | ✅ 120 通过 | core 单元 86（含 P2 音频规划 2、ExportPlan 10、转场 11、滤镜词汇 7）+ P0 集成 1 + P1 预览/导出一致性 5 + 二进制帧描述单元 5 + FFI ABI 7 + P1 ABI 契约 16；滤镜词汇 7 项本轮新增 |
| 预览与导出一致性 | ✅ 有测试 | 同一草稿分别走「播放循环」与「逐帧导出」，逐帧比对 `FramePlan` 必须完全相同 |
| `ExportPlan` / `Segment` | ✅ 10 项通过 | 帧对齐边界、相邻片段/空洞/NTSC、变速源时间映射、与 `FramePlan` 活跃源集合一致、JSON 序列化（含转场窗边界与 `opacity_start/opacity_end` ramp、缺字段兼容）；C ABI/NAPI/JNI 均已暴露 |
| `cargo clippy --workspace --all-targets` | ✅ 零告警 | 与 CI 同一组参数（`--features captrim-ffi/ffmpeg`），`-D warnings` 可用 |
| `check-native-abi.sh` | ✅ 通过 | 30 个 C 函数、33 个 NAPI 函数、26 个 JNI 方法，头文件 == Rust 导出 == bridge == d.ts == ArkTS 导入，平台头文件副本一致 |
| `captrim-core` → 鸿蒙 arm64 | ✅ 编译通过 | `aarch64-unknown-linux-ohos`（Rust 官方 Tier 2 + Host Tools） |
| `captrim-ffi/ffmpeg` → Android arm64 | ✅ 类型检查通过 | `cargo check -p captrim-ffi --features ffmpeg --target aarch64-linux-android`，CI 独立 job 与 `check.sh` 同一命令；不链接（dist/ 不入仓库），真实链接由真机 native 自检覆盖 |
| `libcaptrim.so` 交叉链接（stub） | ✅ 通过 | 无 FFmpeg 时：ELF64 / AArch64 / DYN，30 个符号（含 9 个 `captrim_preview_*`），SONAME 已设，仅依赖 `libc.so`（host / CI 默认） |
| `libcaptrim.so`（ffmpeg feature）双端链接 | ✅ 通过 | OHOS + Android 双端：`libcaptrim.so` → NEEDED `libcaptrim_ffmpeg.so`（OHOS 另 NEEDED `libc.so`；Android 另 NEEDED `libdl.so`+`libc.so`）；FFI 符号 `ct_ff_probe`/`ct_ff_remux`/`ct_ff_transcode`/`ct_ff_string_free` 由 shim 导出并被 cdylib 引用；`platforms/<os>/captrim/{libs,jniLibs}/arm64-v8a` 已暂存全部动态库 |
| NAPI 桥接编译 | ✅ 通过 | 真实 DevEco NDK；依赖包含 `libcaptrim.so` / `libcaptrim_ffmpeg.so` / `libohaudio.so` / `libace_napi.z.so` / `libhilog_ndk.z.so` / 媒体与窗口系统库 |
| ArkTS 静态检查 | ✅ 0 error | `devecocli check arkts`（`.ets` 与 `Index.ets` 均通过），剩 3 条「.so 未校验」告警（预期） |
| 鸿蒙构建脚本端到端 | ✅ 通过 | Windows + Git Bash 实测产出、暂存、`hdc install` 一条命令 |
| 鸿蒙 HAP 打包 | ✅ 真机装上了 | `scripts/build-ohos-hap.sh`：HAP 内含 `libcaptrim.so`(NAPI) + `libcaptrim_ffmpeg.so` + FFmpeg 全套(`libav*`/`libsw*`) + `libc++_shared.so`(NDK C++ 运行时，由 `build-ffmpeg-ohos.sh` 自动打入 `dist/ohos/...` 再随 `stage_libs` 进 HAP) |
| 鸿蒙真机自检 | ✅ 17/17 通过 | Mate 60（ALN-AL00）/ OpenHarmony 7.0.0 / API 26 / arm64-v8a；含 FFmpeg 软后端 probe + remux + 复杂剪辑、三段式管线的软件基线 / 同步硬段 / 回调式硬段，以及两个硬编 mp4 的 FFmpeg 软解复验，详见下节 |
| P2 鸿蒙 HAP 真机验证 | ✅ 通过 | 最终 HAP 自检 17/17；暂停态音频位置 6.69 s → 10.69 s、音频帧 311296 → 487424；播放态播放头与音频主时钟差 7~25 ms，`held` 131 → 285，证明 `OH_AudioRenderer` 输出、时钟回灌与 hold-and-present 均生效 |
| 鸿蒙硬解 + GL 合成 | ✅ 真机出画面 | 截图确认 XComponent 上确有视频帧；8 s 时间线实时播完（468 次合成，解码 458 帧仅 7 帧未上屏）；暂停落点误差 ≤1 帧；缩放/平移/旋转经 `set_transform` 全链路生效（截图对比） |
| 画中画（双路解码） | ✅ 真机出画面 | 主轨 + overlay 轨同时解码：`slots=2`、两路各自 latch 并合成，截图可见主画面与右下角内嵌画面；8 s 时间线双路播放 `pos` 与壁钟同为 6012 ms（1.00× 实时）；只改主视频的 transform 时内嵌画面保持原位（截图对比），证明两个图层互不干扰 |
| Android AAR 编译 | ✅ 构建通过 | `platforms/android` 没有 gradlew，本机用缓存的 Gradle 8.13 构建（`scripts/run-android-app-selftest.sh` 会自动找）；AGP 8.7.3 / Kotlin 2.0.21 / compileSdk 35 / NDK 27.0.12077973 |
| Android 真机自检（Kotlin/JNI 层） | ✅ 16/16 通过 | 小米 2211133C / Android 16 / API 36；走 `CapTrim.kt` 公开 API，覆盖库加载、时间线、分割撤销重做、29.97fps 栅格、预览时钟、probe / remux / x264 / 真实 x265（复探 `codec=hevc`）/ 三段式管线软硬链路 / 硬编产物软解复验，详见下节 |
| Android 真机自检（native） | ✅ 18/18 通过 | 小米 2211133C（fuxi）/ Android 16 / API 36 / arm64-v8a：`scripts/run-android-selftest.sh` 一条命令编译+推送+运行；probe / remux / 软件复杂剪辑 / libx265 真实 HEVC 软编（输出复探 `codec=hevc`）/ 三段式管线（`decoder=2 encoder=2`，即 `CT_BACKEND_ANDROID_HW`，`reason=none` 无回退）/ 硬编产物 FFmpeg 软解复验 / 硬件能力查询 / P2 音频解码重采样 seek 音量静音全通过，详见下节 |
| FFmpeg 实际编译（Android） | ✅ 已编译验证 | `dist/android/arm64-v8a`：libavcodec/format/util/filter/swscale/swresample + `libcaptrim_ffmpeg.so` + 真实交叉编译的 `libx265.so`（7.45 MB，119 个 `x265_*` 导出符号）；`libavcodec.so` 的 `DT_NEEDED` 含 `libx265.so`，`x265_api_get_198` 运行时由它解析；12 个 `_FILTER 1` 全开，shim 编入 `captrim_pipeline.c` 和 `captrim_android_hw.c`。构建脚本会校验 dist 里的 `libx265.so` 大于 1 MB，防止桩再混进来。回执见 `FFMPEG_BUILD.txt` |
| FFmpeg 实际编译（HarmonyOS） | ✅ 已编译验证 | `dist/ohos/arm64-v8a`：libavcodec/format/util/filter/swscale/swresample/postproc + `libcaptrim_ffmpeg.so`，含 libx264 + libx265（GPLv2+）；`libavcodec.so` 实际编入 `ff_libx265_encoder`/`ff_libx264_encoder`（995 个 x265 相关符号）；构建回执见 `FFMPEG_BUILD.txt` |

鸿蒙自检同时覆盖原生库加载、30 个 C 函数、整数时间轴、预览状态机和 FFmpeg 软后端（probe、remux、复杂剪辑 decode → libavfilter → libx264 → mp4）。FFmpeg 那部分要求 HAP 里随附 NDK 的 `libc++_shared.so`：`libavcodec.so.61` 的 NEEDED 指向它，而设备系统镜像里没有，缺了 `captrim_napi` 模块直接加载失败，ArkTS 报 `does not provide an export name 'version'`。`build-ffmpeg-ohos.sh` 会自动拷，已在真机验证。HAP 内带了真实 mp4（`CAPTRIM_SAMPLE_MP4`），页面拷进沙箱给 probe/remux/transcode 用。17 项结果：

```
PASS 原生库加载与 N-API 绑定 | libcaptrim CapTrim 0.1.0
PASS 时间线构建（2 轨 3 片段） | 9.000s / 270 帧
PASS 分割 / 撤销 / 重做 | 4 → 3 → 4
PASS 29.97fps 帧栅格不塌陷 | 270 帧全部严格递增，最大误差 ≤1us
PASS 逐帧源时间唯一（不重复帧） | 前 60 帧源时间严格递增
PASS 预览时钟与丢帧统计 | 呈现 45 次 / 等待 45 次 / 位置 1500ms / 丢帧 0
PASS 编辑后自动重绘 | 同帧不重复，改动后立即重绘
PASS 音频主导时钟校正 | 漂移 50000us 已被校正
PASS 错误码可读 | 0 → "ok"，未知片段正确报错
PASS FFmpeg 探测（软后端 probe） | format=mov,mp4,m4a,3gp,3g2,mj2 dur=22.73s streams=1[video]
PASS FFmpeg 重封装（软后端 remux） | 写回 21.34MB，复探 1 路流
PASS FFmpeg 复杂剪辑（滤镜+libx264重编码） | 1260x2720→320x180 codec=h264 fps≈15 0.06MB 进度 22 次
PASS 三段式管线（软件基线 / 回退链路） | decode→filter→encode 0.06MB 进度 29 次
PASS 三段式管线（硬解/硬编优先，失败自动回退） | 硬件优先→回退就绪 0.03MB codec=h264 进度 29 次
PASS 三段式管线（OHOS 回调式 Buffer 硬解/硬编） | 回调式 Buffer 硬件优先 0.03MB codec=h264 进度 29 次
PASS 硬编产物可被 FFmpeg 软解（pipeline_hw.mp4） | FFmpeg 软解→重编码成功，进度 26 次
PASS 硬编产物可被 FFmpeg 软解（pipeline_cb.mp4） | FFmpeg 软解→重编码成功，进度 26 次
```

> 五项转码自检都用 `trim=duration=2` 只处理前 2 秒。原因是 OHOS 的 `ThreadBlock6S` watchdog：自检同步跑在 UI 线程，一次完整 22 秒转码（约 17 s）会耗尽主线程阻塞预算，之后任何重任务都会让 App 被 `signal 9` 杀死（`AppKilledReporter: reason: ThreadBlock6S`）。因此 `ct_ff_transcode()` / `ct_pipeline_run()` 都支持「终止型滤镜提前关闭滤镜图」并立即停止解码；四项重管线检查（软件基线 / 同步硬段 / 回调硬段 / 软解复验）分别放到延迟的 UI 任务中分批执行，每批之间留 8 s 空闲窗口。

**复杂剪辑（`exportTranscode`）的实现要点**：`ct_ff_transcode` 走 `av_read_frame → 解码 → libavfilter graph → libx264 → mp4`，音频流无损复制，支持进度回调与取消。三个要记住的坑：

- 滤镜图、编码器、muxer **必须从第一帧解码结果延迟构建**：容器 `codecpar->format` 通常是 `AV_PIX_FMT_NONE(-1)`，拿它描述 buffer 源会在格式协商里解引用空描述符而 SIGSEGV。
- 给 buffersink 设 `pix_fmts` 必须用 `av_opt_set_int_list(..., AV_PIX_FMT_NONE, ...)`（在哨兵处截断）；用 `av_opt_set_bin(sizeof(array))` 会把 `-1` 当成真实格式写进列表，同样导致 `av_pix_fmt_desc_get(-1) == NULL` 崩溃。
- `--disable-everything` 下每个 filter 都要显式 `--enable-filter=`，且 shim 必须链接 `-lavfilter -lswscale`。

### Android 真机自检（native，18/18）

Android 侧没有 demo App，自检做成可直接 push 到 `/data/local/tmp` 运行的 native 程序：`scripts/run-android-selftest.sh <sample.mp4>` 一条命令完成 NDK 编译 → 推送库和素材 → adb shell 运行（`native/captrim_selftest.c`）。它调用与 JNI 完全相同的 C ABI，因此失败能直接隔离到 native 层。其中两项专测**真实 x265**：先用 `libx265` 走一遍完整 decode → libavfilter → HEVC 编码 → mp4，再复探输出确认 `codec=hevc`，桩实现过不了这一关。P2 又补了 5 项音频检查，直接生成 1 秒 440 Hz stereo s16le WAV，走真实 FFmpeg 解码 + swresample 重采样。小米 2211133C（Android 16 / API 36 / arm64-v8a）结果：

```
PASS FFmpeg probe                          | dur=22.73s 1260x2720 h264
PASS FFmpeg remux                          | stream copy ok
PASS Remux output probe                    | 复探 1 路视频流
PASS FFmpeg complex transcode (software)   | →320x180 h264 进度 22 次
PASS Software output codec                 | h264
PASS FFmpeg complex transcode (libx265 HEVC) | 真实 x265 编码 29 帧 / 12.82 fps / 137.28 kb/s
PASS x265 output codec                     | 320x180 codec=hevc
PASS Three-stage pipeline (software baseline) | pipeline ok
PASS Pipeline frame flow                   | decoder=2 encoder=2 decoded=100 encoded=29 reason=none
PASS Hardware-first route with fallback    | route ok
PASS Hardware-route output                 | 320x180 h264 进度 29 次 reason=none
PASS Hardware output software re-decode    | FFmpeg 软解→重编码成功，进度 15 次
PASS Hardware capability query             | vendor=Android MediaCodec decoderAvc=true encoderAvc=true
PASS Audio mixer create                     | 48000/2/f32
PASS Audio decode/resample to f32 PCM       | rc=0 peak=0.5000 rms=0.3214
PASS Audio seek to source instant           | source_us=500000 peak=0.5000
PASS Audio volume scaling                   | volume=0.25 peak=0.1250
PASS Audio silence with no items            | rc=0 peak=0.0000
== selftest 18/18 passed ==
```

`decoder=2 / encoder=2` 即 `CT_BACKEND_ANDROID_HW`，`reason=none` 表示**没有发生任何软回退**：`native/captrim_android_hw.c` 用 `AMediaExtractor` 喂 `AMediaCodec` 硬解、`libavfilter` 处理、`AMediaCodec` 硬编 + `AMediaMuxer` 封装。与 OHOS 版共用 `captrim_pipeline.c` 的编排和逐段回退，仅通过弱符号工厂 `ct_decoder_create_android()` / `ct_encoder_create_android()` 切换。

Android 侧踩过三个坑：① NDK 不导出 `COLOR_FormatYUV420SemiPlanar` 这些颜色格式常量，按 AOSP 数值硬编码（NV12=21）；`AMediaCodec_getInputFormat` 是 API 28+，minSdk 26 下 stride 只能从输入 buffer 容量反推，slice 高度也不能按 16 对齐（`input layout invalid (cap=86400 stride=0 slice=0)` 就是对齐多了）。② x265 静态 archive 在本机 Windows/NDK 工具链下会给 `libavcodec.so` 留下 `x265_api_get_198` 未定义符号，Android 加载器直接拒绝加载；早期用 5 KB 装载桩加 `LD_PRELOAD` 绕过，后来换成真实交叉编译的 7.45 MB `libx265.so`，`libavcodec.so` 通过 `DT_NEEDED` 正常依赖它，自检脚本只需要 `LD_LIBRARY_PATH=.`。③ x265 的 CMakeLists 把 shared 库和 `.pc` 的 install 规则包在 `if(X265_LATEST_TAG)` 里，浅克隆没有 tag 时 `make install` 静默不装；构建脚本改为手动把产物装进 prefix，再用 `NEEDED libx265.so` 加文件大小大于 1 MB 双重校验。详见第十节坑 38–43。

### Android 真机自检（Kotlin/JNI 层，16/16）

native 自检验的是 C ABI，这一层验的是宿主 App 实际走的路：`CapTrim.kt` → JNI → Rust → FFmpeg。`platforms/android/app` 是个最小 Activity，把 16 项检查跑在后台线程，结果打到 logcat（tag `CapTrimSelfCheck`）、屏幕和 `files/selfcheck.txt`。检查项和鸿蒙 `SelfCheck.ets` 同构：时间线、分割撤销重做、29.97fps 栅格、预览时钟、probe、remux、x264 复杂剪辑、真实 x265（复探 `codec=hevc`）、三段式管线软件基线与硬解硬编优先、硬编产物软解复验。

```bash
bash scripts/run-android-app-selftest.sh
# 构建 app-debug.apk（自动找本机缓存的 Gradle 8.13）→ adb install →
# 把 sample.mp4 推进 App 沙箱 → 拉起 → 轮询 logcat 里的 == selfcheck N/M passed ==
```

小米 2211133C（Android 16 / API 36）实测 16/16：

```
PASS 原生库加载与 JNI 绑定 | libcaptrim CapTrim 0.1.0
PASS 时间线构建（2 轨 3 片段） | 9.000s / 270 帧
PASS 分割 / 撤销 / 重做 | 4 → 3 → 4
PASS 29.97fps 帧栅格不塌陷 | 270 帧全部严格递增，最大误差 ≤1us
PASS 逐帧源时间唯一（不重复帧） | 前 60 帧源时间严格递增
PASS 预览时钟与丢帧统计 | 呈现 45 次 / 等待 45 次 / 位置 1499ms / 丢帧 0
PASS 编辑后自动重绘 | 同帧不重复，改动后立即重绘
PASS 音频主导时钟校正 | 漂移 50000us 已被校正
PASS 错误码可读 | 0 → "ok"，未知片段正确报错
PASS FFmpeg 探测（软后端 probe） | format=mov,mp4,m4a,3gp,3g2,mj2 dur=22.73s streams=1[video]
PASS FFmpeg 重封装（软后端 remux） | 写回 21.34MB，复探 1 路流
PASS FFmpeg 复杂剪辑（滤镜+libx264重编码） | 1260x2720→320x180 codec=h264 fps≈15 0.06MB 进度 22 次
PASS FFmpeg 复杂剪辑（libx265 HEVC） | x265 真实编码 0.04MB codec=hevc 进度 21 次
PASS 三段式管线（软件基线 / 回退链路） | decode→filter→encode 0.06MB codec=h264 进度 29 次
PASS 三段式管线（硬解/硬编优先，失败自动回退） | decode→filter→encode 0.06MB codec=h264 进度 29 次
PASS 硬编产物可被 FFmpeg 软解（pipeline_hw.mp4） | FFmpeg 软解→重编码成功，进度 26 次
== selfcheck 16/16 passed ==
```

推 sample 进 App 沙箱不能把 `adb shell` 当管道用：多参数会被设备 shell 重新解析，`>` 会落在错误的目录，stdin 转发 21 MB 还会被截断到几百字节。可靠做法是 `adb push` 到 `/data/local/tmp`，再在设备内 `cat ... | run-as ... sh -c 'cat > files/a.mp4'`，`scripts/run-android-app-selftest.sh` 里就是这么做的。

CI 有两个 Android job：`android` 打 stub 后端 AAR（已去掉 `continue-on-error`，失败即红），`android-ffmpeg` 用 `cargo check` 检查 FFmpeg 后端在 Android arm64 上的类型正确性（不链接）；见 `.github/workflows/ci.yml`。

## 九、许可与专利

- 本构建显式启用 **GPL**：编译脚本打开 `--enable-gpl` / `--enable-libx264` / `--enable-libx265`，因此产物（含 `libcaptrim_ffmpeg.so` 与 FFmpeg 共享库）整体为 **GPLv2-or-later**。这是学习项目「功能做全」的取舍；若未来要闭源/转 LGPL，需移除 x264/x265 并改回软解/硬件编码。
- GPL 合规义务（如分发）：随附 FFmpeg + x264 + x265 对应源码、完整 configure 行（见 `dist/<平台>/<abi>/FFMPEG_BUILD.txt`）、以及本仓库链接。
- H.264 / H.265 另有**专利许可**问题（与开源许可无关），商业分发需单独评估。

## 十、已知坑

已踩过的，以及一定会踩的：

1. **鸿蒙预构建库有硬约束**：`.so` 必须由 HarmonyOS NDK 工具链编译、**必须带 SONAME**（`llvm-readelf -d | grep SONAME` 验证，否则打包后加载不到）、架构限 `armeabi-v7a / arm64-v8a / x86_64`。
2. **STL 一致性**：鸿蒙要求同一 App 内所有 Native 库的 `OHOS_STL` 一致。HAR 侧通过 `-DOHOS_STL=none` + 桥接层不用 `std::string`（改用 `stdint.h`/`stdlib.h` 与 `malloc` RAII）做到零 C++ 运行时依赖，`-nostdlib++` 兜底；`llvm-readelf -d` 只剩 `libcaptrim.so / libace_napi.z.so / libc.so`。默认的 `c++_shared` 会额外往 HAP 里塞一个 1.2MB 的 `libc++_shared.so`，并把「宿主 App 必须也用 c++_shared」这个隐形要求传递出去。Android 侧用 `c++_static`。
3. **`napi_env` / `JNIEnv` 禁止跨线程缓存**。当前 P0 导出在调用线程同步执行所以安全；一旦导出移到工作线程，进度回调**必须**改成 `napi_create_threadsafe_function`，否则随机闪退。
4. **时间戳**：B 帧导致 DTS≠PTS；seek 必须退回关键帧再解码到目标帧；导出时自己生成单调递增 PTS，不要透传源文件时间戳。
5. **不要做 YUV↔RGB↔YUV 往返**。Surface 模式解码直出纹理，鸿蒙 `AVBuffer` 回调本身零拷贝，自己 memcpy 会白吃掉一半性能。
6. **音频统一先重采样**到 48kHz/立体声/f32 再混合与变速；每轨各做一次重采样是常见性能陷阱。
7. **变速保调**需要独立的 tempo（时间伸缩）与 pitch 处理，`libswresample` 只做采样率转换，不做变速保调。
8. **VFR 素材**（手机录屏、部分相机）没有恒定帧率。P0 的 remux 不受影响，但 P1 按帧率映射时间线时必须按 PTS 走，不能假设 `frame_index / fps`。
9. **素材会被用户删掉**：草稿里存 URI 不够，需持久化权限或拷贝到沙箱，否则重开草稿时一片空白。
10. **生命周期**：XComponent / Surface 的销毁必须早于解码器停止；长时导出走前台服务 / 后台长时任务。
11. **不要把截断后的 PTS 再喂给按下取整的接口**。这是 P1 实际踩到的坑：`frame_pts_us(1)` 对 30000/1001 返回 33366，而第 1 帧真正的起点是 33366.67 µs，再取整一次就回到第 0 帧，导出会静默重复帧。导出必须按帧索引走 `plan_frame_at`。
12. **播放时钟不能用墙钟**。`System.currentTimeMillis()` 会被 NTP 校正，回拨会让播放卡死、前跳会让整段缓冲作废；必须用单调时钟（`System.nanoTime()`、`OH_GetTimeSinceBoot`）。
13. **音画不同步先看 drift 再猜**。`syncAudio` 返回它修正了多少微秒，这个数持续变大说明音频输出设备的实际延迟与标称不符，而不是渲染慢——两者的修复方向完全相反。
14. **DevEco 的 hvigor 不在工程里**。新版 IDE 把 hvigor 打包在 `tools/hvigor/`，工程根放一个 `hvigorw.js` 只会让它把依赖解析到 `<工程>/hvigor`（那里只有配置），报 `ENOENT ... workspace/node_modules/@ohos/hvigor/bin/hvigor.js`。正确做法是直接 `node <DevEco>/tools/hvigor/bin/hvigorw.js`，见 `scripts/build-ohos-hap.sh`。缓存坏了（同一报错但路径存在）删掉 `~/.hvigor/project_caches/<hash>/workspace` 即可。
15. **`AppScope` 的资源目录是 `resources/base/`**，不是模板里的 `AppScope/res/`。写错时报的是 `The resource reference '$string:app_name' is not defined`（看着像少了字符串，其实是整棵资源树没被识别）。
16. **未签名 HAP 装不上真机**：`hdc install` 报 `9568320 no signature file`，必须先在 IDE 里配好 Signing Configs（自动签名需登录华为账号）。
17. **硬解有流水线深度，喂帧太少它会一帧都不吐**。同一颗解码器在 surface 模式下，喂 150 ms（约 5 帧）输入只换来「接受 11 个 sample、0 个 output」，每次 `present` 都等满预算、画面永远不动；喂到 300 ms 才立刻出帧。所以「喂帧前瞻量」不是吞吐调参，它的下限由解码器决定，上限决定画面领先音频时钟多少——真机实测 300 ms 是这颗芯片的可用值。等音频输出落地，要么做 hold-and-present，要么接受这个偏移。
18. **seek 用 `Stop`+`Start`，不要用 `Flush`**。`OH_VideoDecoder_Flush` 之后这颗解码器不再请求输入，`RenderAt` 等满预算就放弃，表现为「跳转后画面停在旧帧、已上屏计数冻结」。`Stop`/`Start` 是官方生命周期，`Start` 会重新发起输入回调。
19. **绝不要跨 codec 调用持锁**。`Stop`/`Flush` 会等回调线程返回，而回调要拿同一把锁去唤醒等待者——死锁后循环彻底静默（现象：日志停在 `Demuxer seek` 一行，之后没有任何心跳）。锁要拆成「状态锁」和「demuxer 锁」，且都不跨 codec 调用。
20. **暂停要主动回跳一次**。解码器总比播放头快一个喂帧前瞻，用户在「播放中途」按下暂停时，surface 上还停着领先的那一帧且之后再也不会有新帧（暂停时 `step` 返回 idle）。暂停时按当前播放位置做一次 seek，才能让画面落在用户按下的那一帧。
21. **present 必须离开 UI 线程**。ArkUI 的 `setInterval` 里同步调阻塞的 `present`，主线程被卡住，系统直接杀进程（`THREAD_BLOCK_6S` + `signal:9`，日志里没有任何崩溃栈，很容易误判成「解码崩了」）。判断依据是 `pidof` 点击前后是否一致——进程消失才是被杀，进程还在就是逻辑问题。
22. **hilog 的 `%s` 会被打码成 `<private>`**，诊断字符串参数需要用 `%{public}s`；但 `%{public}s` 对 `OH_LOG_Print` 的字符串参数在部分版本会输出乱码，最稳的办法是用 `OH_AVFormat_DumpInfo` 直接打印整份结构体文本。
23. **surface 模式下 `FreeOutputBuffer` 只归还、不渲染**。把 `RenderOutputBuffer` 改成 `FreeOutputBuffer` 想「不直接上屏、改成自己合成」是错的：解码器照样出帧（`output #1 pts=0` 照常打印），但帧根本进不了 `NativeImage` 队列，表现为 `UpdateSurfaceImage` 一直返回 `40601000`。要自己合成，仍然用 `RenderOutputBuffer`——只是把 `SetSurface` 的目标从 XComponent 换成 `OH_NativeImage_AcquireNativeWindow()`。
24. **`OH_NativeImage` 的队列很浅，等帧会把自己等死**。先在 `RenderAt` 里等「目标帧」再去合成，队列始终满着→解码器被反压→它产不出那个目标帧。实测：8 秒素材只上屏 51 帧。改成「每次循环先 `UpdateSurfaceImage` 排空队列，再请求下一帧」后恢复到 207 帧。同理，用 `OnFrameAvailable` 回调做门限也不行——回调按队列刷新触发，中间的帧全被漏掉（25 帧）。
25. **`UpdateSurfaceImage` 没有 buffer 时是立即返回，不是阻塞**。返回 `NATIVE_ERROR_NO_BUFFER`（`40601000`）就能当作「这轮没新帧」，所以可以每帧无条件调，不必维护 frame-available 状态机。
26. **`OH_AVFormat_GetStringValue` 在本机取不到 track mime**（返回非空指针但内容是乱码），要靠 `OH_AVFormat_DumpInfo` 的文本兜底解析，见 `FormatString()`。
27. **overlay 轨道上的图层不一定是贴纸**。`layer_kind` 原来只看轨道类型，视频画中画因此被标成 `Overlay`，而平台按 `kind` 选渲染器时就不去解码——结果是「画中画静默消失」，日志里没有任何错误，`fail=0`、`slots=1`，只能靠数图层发现。现在按源是否有视频流决定（未探测 = 按视频处理，因为解码是唯一能看见它的方式）。
28. **「有图层可画」不能当成 latch 的前置条件**。把 `UpdateSurfaceImage` 的排空放进「有计划才执行」的分支后，首帧永远到不了：打开解码器的那一步返回 Present 时帧还没产出，`count == 0` 于是没记住 `last_desc`，之后的空闲路径又因为 `has_last_desc == 0` 提前返回，于是再也不排空、画面停在黑色。排空必须在所有 early return 之前。
29. **改了 Rust 核心就要重跑 `build-rust.sh ohos`**。HAP 打包只复制 `platforms/ohos/captrim/libs/` 里已经交叉编译好的 `libcaptrim.so`；只跑 `build-ohos-hap.sh` 装上去的还是旧核心。本轮为此多跑了一轮：C++ 已改对，日志里依然只有 1 个解码器。
30. **槽位不会因为图层离场而关闭**。图层的画面走了，它的解码器留在槽位里空转（`target_us` 不再推进，输入回调自己就停在背压循环里）。这是刻意的：反复开关解码器比留着更贵，而需要腾地方时按 LRU 回收。`kMaxSources = 4` 是这个策略的上限，不是硬件的上限——真正的上限是 SoC 愿意发多少个实例，只能靠 `CreateByMime` 返回 null 发现。
31. **同步跑长转码会被 OHOS 的 `ThreadBlock6S` watchdog 杀掉**。真机自检在 UI 线程同步执行，一次完整 22 s 素材的软转码约 17 s，会耗尽主线程阻塞预算；之后再接任何重任务，系统会以 `AppKilledReporter: reason: ThreadBlock6S` + `exit with signal: 9` 直接杀进程，且因为自检日志是 `run()` 返回后统一打印，**一条 check 日志都看不到**（很容易误判成 native SIGSEGV）。解法有三层：自检用 `trim=duration=2` 只处理 2 s；`ct_ff_transcode()`/`ct_pipeline_run()` 在 `av_buffersrc_add_frame_flags` 返回 `AVERROR_EOF` 时视为「终止型滤镜已关闭图」，停止继续解码（否则 `trim` 反而被当成错误）；重检查拆到延迟 UI 任务分批执行。生产导出仍应放到 worker 线程或 NAPI async_work。
32. **`trim` 这类终止型滤镜会让 buffersrc 再写入时返回 `AVERROR_EOF`**。它不是错误，而是滤镜图已经结束；同时要主动向 buffersrc 送一次 EOF，让 `fps`/`scale` 里缓冲的帧能被 drain 出来，再由编排层停止解码。漏掉这一步的现象是导出报 `decode error: av_buffersrc_add_frame_flags: End of file (-541478725)`。
33. **AVCodec Kit 默认是异步模式，直接调同步 `Query*Buffer` 必然返回 `AV_ERR_OPERATE_NOT_PERMIT(2)`**。曾在 Mate 60 / OpenHarmony 7.0.0 / API 26 上把 `CreateByMime`/`Configure`/`Prepare`/`Start` 全跑通、却在 `OH_VideoDecoder_QueryInputBuffer`/`QueryOutputBuffer` 拿到 `2`，据此误判「该设备不支持同步 Buffer 模式，只能改回调」。真正原因写在 `native_avcodec_videodecoder.h` 里：这些 API `@since 20`、注释明确「仅同步模式可用」「异步模式下返回 `AV_ERR_OPERATE_NOT_PERMIT`」。**必须在 Configure 阶段设 `OH_MD_KEY_ENABLE_SYNC_MODE = 1`**（`native_avcodec_base.h`：值为 1 启用、默认 0；启用后不能注册回调、必须用 buffer query API、且只能在配置阶段设置）。该键是 API 20+，用 `dlsym(RTLD_DEFAULT, ...)` 运行时解析，避免老设备上 shim 因未定义符号加载失败；解析不到就自动走回调模式。现在两条路径都真机跑通，`fallback=[none]`。
34. **Buffer 模式下 EOS 必须写在输入 `OH_AVBuffer` 的 flag 上，`NotifyEndOfStream()` 只用于 Surface 模式**。`OH_VideoEncoder_NotifyEndOfStream` 的头文件注释写明「This function is used only in surface mode. In buffer mode, OH_AVBuffer is used to carry the EOS information」。原先在 `finish()` 里调它，编码器收不到结束信号，收尾只能靠超时猜；正确做法是再取一个输入 buffer，设 `attr.size=0` + `AVCODEC_BUFFER_FLAGS_EOS`，`OH_AVBuffer_SetBufferAttr` 后 `PushInputBuffer`。解码侧同理：解封装读失败时把 EOS flag 写进输入 buffer 再 push，不要直接停。
35. **编码器输入 buffer 的跨距别硬猜，也别盲信 description**。官方《视频编解码宽高、跨距与裁剪信息说明》说编码器输入有 `stride`/`sliceHeight` 填充，但官方转码示例 `CopyStrideYUV420SP` 却按 `width` 连续行写。实测 Mate 60 的 320×180 NV12 输入：`OH_VideoEncoder_GetInputDescription` 给 `stride=320 slice=180`、`OH_AVBuffer_GetCapacity=90112`、按填充布局算 `need=86400` 能装下；但更早一版在某些 buffer 上 `need > cap` 直接失败（`encoder input buffer too small / invalid stride`），表现为首帧成功、第二帧起整单全软重试。稳妥做法是**同时查 description 与真实 capacity**：填充布局放不下就退回连续布局，仍放不下才报错；并把 `w/h/stride/slice/cap/need` 打到 hilog，一次就能定位。
36. **硬件解码器要给编排层一个非零 `duration_us`，否则进度回调只剩收尾那一次**。轨道级 `OH_MD_KEY_DURATION` 可能取不到；容器级 `OH_AVSource_GetSourceFormat` 才有（本机 `22729000us`）。`ct_pipeline_run()` 的进度计算有 `total_us > 0` 的守卫，缺时长就会从「进度 29 次」退化成「进度 1 次」，看起来像导出卡住。另外输入/输出 PTS 必须用两个独立计数器（`input_index` / `output_index`），共用一个会让编码样本时间戳乱跳。
37. **`probe` 只能证明容器元数据合法，不能证明硬编码出的码流可解码**。自检里额外加一步：把硬编 mp4 再交给 FFmpeg 软 H.264 解码器走 `exportTranscode(..., 'scale=64:64', 'libx264')`，看到多次进度回调才算数；否则 muxer 写出一个结构完好、帧却全废的 mp4 也会一路 PASS。
38. **Android NDK 不导出 `COLOR_FormatYUV420SemiPlanar` 等颜色格式常量**。`NdkMediaCodec.h`/`NdkMediaFormat.h` 里找不到这些 OMX 数值，只能按 AOSP 定义硬编码（NV12=21、I420=19、PackedSemiPlanar=39、Flexible=42）——它们的稳定性由平台 ABI 保证，不是 NDK API。
39. **`AMediaCodec_getInputFormat` 是 API 28+，minSdk 26 下拿不到编码器 stride**。要么按 buffer 容量反推（连续布局 `cap/(w*h*3/2)`），要么直接用未对齐的 `width`/`height`。踩过的坑：把 slice 高度按 16 对齐后 `320x180` 需要 `320*192*3/2=92160` 字节，而编码器只给了 `86400`（`stride=0 slice=0` 时按连续布局分配），于是报 `hardware encoder input layout invalid (cap=86400 stride=0 slice=0)`。`stride` 可用时再按填充布局写，且 NV12 的 UV 平面起点是 `stride*slice`，不是 `stride*height`。
40. **Android 版 x265 必须是真实动态库，不能用装载桩**。静态 archive 在本机 Windows/NDK 工具链下会省略 `api.cpp.o`（`x265_api_get_198` 就在里面），`-Wl,--undefined` 被 FFmpeg 的 version script 吃掉、`--whole-archive` 带来 compiler-rt 重复符号、直接加 `.o` 又在 libavformat 重复定义，`libavcodec.so` 于是留下未解析符号，Android 加载器拒绝加载。早期用 5 KB 的 `libx265.so` 桩加 `LD_PRELOAD` 骗过加载器——自检 11/11 全绿，但 HEVC 一次都没真编过，已废弃。现在的做法：x265 用 `-DENABLE_SHARED=ON` 编成 7.45 MB 真库（`-DANDROID_STL=c++_static` 静态嵌入 C++ 运行时；x265 的 CMakeLists 硬编码 `-lpthread` 而 bionic 的 pthread 在 libc 里，所以垫一个空的 `libpthread.a`），FFmpeg 以 `-lx265` 链上。判据是 `libavcodec.so` 的 `DT_NEEDED` 含 `libx265.so`、`x265_api_get_198` 保持 `UND` 交给加载器解析，真机自检实际编码出 `codec=hevc` 才算数。
41. **浅克隆没有 git tag 时，x265 的 `make install` 什么都不装**。x265 `source/CMakeLists.txt` 把 shared 库和 `x265.pc` 的 install 规则都包在 `if(X265_LATEST_TAG)`（来自 `git describe`）里，`git_clone_robust` 用 `--depth 1` 克隆不带 tag，`git fetch --tags` 在本机也补不上。现象是 cmake+make 打印了 `[100%] Built target x265-shared`、真库就在构建目录里，`prefix/lib` 却是空的，FFmpeg configure 拿不到 `--enable-libx265`。现在由 `install_x265_artifacts()` 手动把 `libx265.so`、头文件和干净的 `x265.pc` 放进 prefix（x265 自己生成的 `.pc` 还带畸形 `Libs.private: -l-l:libunwind.a`），shared 模式下同时删掉同目录的 `libx265.a`，免得链接器又去挑静态库。
42. **`prefix` 就在 `build/x265-<abi>/` 里，无条件 `rm -rf "$b"` 会把装好的产物一起删掉**。`build_x264()` / `build_x265()` 原先开头都是 `rm -rf "$b"; mkdir -p "$b"`，而传进来的 prefix 恰好是 `$b/prefix`。中断一次（Windows 上 x265 要编十几分钟，经常被取消）再重跑，缓存检查永远为假，于是每次从头全量重编，看起来就是「一直卡住」。现在只 `mkdir -p`，并加了断点续跑：`CMakeCache.txt` 和 `libx265.so` 都在就直接 stage 进 prefix 返回。
43. **Windows 上 libavcodec 的链接行约 8.3 KB，超过 cmd.exe 的 8191 字符上限**。`libx265.o` 一进来，参数尾部的 `-L/-l` 就被静默截断（日志里出现过半截的 `-pthre`），x264/x265 符号全 unresolved。分两层解决：`ld-rsp.sh` 直接调原生 `clang.exe` 绕开 `.cmd` 批处理（CreateProcess 上限约 32 KB），参数写成 response file；`patch_ffmpeg_link_rsp()` 再改写 FFmpeg `ffbuild/library.mak` 的 shared 库链接规则，用 make 的 `$(file >$@.rsp,...)` 生成 rsp、以 `$(LD) @$@.rsp` 链接（`@` 不能漏，漏了 lld 把 rsp 当普通目标文件）。另外 `build_c_shim()` 原先从 FFmpeg 的 `PREFIX/lib` 找 `libx265.so`，那里永远没有，dist 里一直残留旧桩；改成从 `X265_PREFIX/lib` 拷。
