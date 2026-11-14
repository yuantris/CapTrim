# CapTrim

Android 与 HarmonyOS 共用的视频剪辑核心 SDK。**Rust 描述时间线，FFmpeg 处理文件与滤镜，编解码和渲染交给系统硬件。**

一份 Rust 核心 + 一份 C ABI，两端只做薄桥接（Android JNI / 鸿蒙 Node-API），业务 API 只定义一次。目标是覆盖「剪映基础剪辑」：导入探测、分割、删除、排序、裁剪、变速、音量、音画分离、转场、滤镜、字幕、画中画、比例裁剪、导出。

当前状态：**P0 骨架与 P1 调度内核已跑通**（见[验证状态](#验证状态)）。预览的帧计划、时钟与状态机已实现并有测试覆盖；解码器与 GPU 合成器只定义了契约，需真机实现。

---

## 一、三条设计边界

划错任何一条，都会得到一个体积巨大、预览卡顿、导出缓慢、两端各有一套 bug 的 SDK。

| 边界 | 决策 | 理由 |
|---|---|---|
| 编解码 | **不用 FFmpeg，走系统硬件**：Android `AMediaCodec`，鸿蒙 AVCodec Kit（`OH_AVCodec*`） | 手机软编解码既慢又烫。鸿蒙官方明确 AVCodec Kit 提供 H.264/H.265 硬件加速，且按 MIME 创建时**优先**返回硬解实例 |
| FFmpeg 职责 | 只保留 `libavformat`（封装/解封装）、`libavfilter`、`libswresample`、`libswscale` | 这两块系统 API 反而更难用、格式覆盖更窄；裁剪后体积可控，且**不需要 libx264**，天然避开 GPL |
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
        │  native/captrim_ffmpeg.c（libavformat）   │
        │  probe 探测 · remux 无损导出               │
        └──────────────────────────────────────────┘

  预览/导出共用同一份 Timeline：渲染与解码由各端实现注入，
  Rust 核心不含任何 #cfg 平台分支。
```

**为什么预览和导出必须共用一份描述**：只要两端各自解析一次素材，变速、转场、B 帧处就一定会出现「预览看到的」与「导出的」不一致。调度策略可以完全相反（预览丢帧保实时，导出逐帧保正确），但时间线描述只能有一份。

## 三、目录

```
crates/captrim-core/      纯 Rust 核心：时间、模型、命令、草稿（零平台依赖）
crates/captrim-ffi/       稳定 C ABI + 后端抽象（captrim.h 是唯一契约）
native/                   FFmpeg C shim（probe / remux）
platforms/android/        Gradle AAR + CMake + JNI + Kotlin
platforms/ohos/           HAR 工程 + CMake + Node-API + ArkTS + .d.ts
platforms/ohos/entry/     真机自检 App（9 项检查，结果写 hilog）
scripts/                  FFmpeg 裁剪编译、Rust 双端构建、ABI 校验、HAP 构建
```

## 四、快速开始

前置：Rust（含 `rustup target add aarch64-unknown-linux-ohos`）。

**1. 只验证核心逻辑（不需要任何移动端环境）**

```bash
cargo test --workspace          # 26 个测试
cargo clippy --workspace --all-targets
bash scripts/check-native-abi.sh   # 头文件 == Rust 导出 == NAPI == d.ts
```

**2. 鸿蒙侧（先跑 stub 后端，不需要 FFmpeg）**

```bash
export OHOS_NDK_HOME="/path/to/sdk/default/openharmony/native"
FEATURES= bash scripts/build-rust.sh ohos     # 产出并暂存 libs/arm64-v8a/
```

Windows 下脚本会自动选用 `scripts/toolchain/*.bat`（Cargo 无法把 `.sh` 当链接器执行），macOS/Linux 用 `*.sh`。

**3. 接上 FFmpeg（真实探测与导出）**

```bash
bash scripts/build-ffmpeg-ohos.sh      # 裁剪编译 + 编译 C shim，产物进 dist/ohos/
bash scripts/build-rust.sh ohos        # 默认 FEATURES=ffmpeg
```

**4. Android**

```bash
export ANDROID_NDK_HOME="/path/to/ndk"
bash scripts/build-ffmpeg-android.sh
bash scripts/build-rust.sh android
cd platforms/android && ./gradlew :captrim:assembleRelease
```

**5. 鸿蒙真机自检**

`platforms/ohos/entry` 是个可直接装到手机上的 App：打开即在设备上跑 9 项检查，把每一行结果写进 hilog。

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

P1 的成果不是「能放视频」，而是**把预览和导出钉在同一条时间线上**。做法是把三件事从平台层收进 Rust：

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

**三条容易踩的实现约定**

1. **`nowUs` 必须是单调时钟**。传 `System.currentTimeMillis()` 会出两种故障：时钟回拨导致播放卡死，向前跳跃导致整段缓冲被丢弃。鸿蒙用 `OH_GetTimeSinceBoot` 系列的微秒值。
2. **导出按帧索引走，按时间戳走会重复帧**。`frame_pts_us(1)` 对 30000/1001 返回 33366，但第 1 帧真正的起点是 33366.67 µs；再把这个值喂给按下取整的接口就会塌回第 0 帧。所以 `plan_frame_at(frame_index)` 是导出 API，`plan_frame(t_us)` 只用于拖动和播放。这条有专门的集成测试守着。
3. **编辑后不需要额外通知预览**。`Editor` 有单调递增的 `revision`，`preview_step` 每次比对它；变化了就重规划**同一帧索引**。否则改完滤镜画面会停在旧图，直到播放头移动。

**平台需要自己实现的部分**（有接口契约、无实现，因为必须真机验证）：

- 解码器：`FrameLayer.source_us` 是精确的源时间，用它去 seek + 解码；空 `uri` 表示该层不可用，跳过而不是整帧失败。
- 合成器：按 `z` 从小到大画，`transform` / `opacity` / `effects` 直接消费；`kind` 区分视频、字幕、贴纸，决定走解码器还是文字引擎。
- 音频：`AudioMix` 是全轨混合后的输入，先统一重采样到 48kHz/立体声/f32 再叠加；播完后把音频位置回灌给 `syncAudio`。

## 七、已完成 / 未完成

**已完成**

- `Rational` 精确时间运算（含 NTSC 30000/1001 帧边界、无浮点比较）
- Timeline / Track / Clip 模型、重叠校验、分割（含变速片段的源时间映射）
- Command 栈 + 撤销重做 + 草稿 JSON 往返
- 稳定 C ABI（25 个函数）：句柄注册表、错误码、`catch_unwind`、UTF-8 校验
- FFmpeg probe / remux 的 C shim + 特征门控后端（未启用时返回 `NO_BACKEND` 而非崩溃）
- **P1 调度内核**：帧计划、音频主导时钟 + 漂移补偿、`step` 状态机、丢帧统计
- 双端工程：AAR（JNI + Kotlin）、HAR（Node-API + ArkTS + `.d.ts`）
- 鸿蒙真机自检 App：9 项端到端检查，含 29.97fps 帧栅格、丢帧统计、编辑后重绘
- 构建与校验脚本：FFmpeg 裁剪 configure、双端 Rust 交叉编译、ABI/NAPI 漂移检查、HAP 打包

**未完成（按优先级）**

- P1 收尾：硬解码器与 GPU 合成器**实现**（契约已定义、调度已在真机验证，缺 AVCodec 解码与上屏）
- P2：滤镜/转场/画中画/字幕渲染、音频混合、**重编码导出**（`ExportPlan`/`Segment` 抽象）
- P3：变速保调、功耗与机型兼容矩阵、崩溃符号化
- 倒放：要求反向解码 + 全段帧缓存，复杂度约等于其余功能之和，明确留给 v2

**逐帧数据不要走 JSON 往返**：当前 `planFrame` / `step` 返回 JSON，是为了让 P1 骨架在没有渲染器时也能跑通、也能两端对齐。真实的合成器应当从 C++ 直接调 C ABI（同一份 `captrim_preview_step`），避免每帧一次 JSON 解析与跨语言拷贝。ArkTS 侧已做类型化封装，Android 侧返回原始 JSON 是刻意的：AAR 保持零依赖，也不塞一个无法用单元测试验证的手写解析器。

## 八、验证状态

区分「本机实测通过」和「已写好但未在此环境验证」，避免误判成熟度。

| 项 | 状态 | 说明 |
|---|---|---|
| `cargo test --workspace` | ✅ 84 通过 | core 单元 55 + P0 集成 1 + P1 预览/导出一致性 5 + FFI 7 + P1 ABI 契约 16 |
| 预览与导出一致性 | ✅ 有测试 | 同一草稿分别走「播放循环」与「逐帧导出」，逐帧比对 `FramePlan` 必须完全相同 |
| `cargo clippy --workspace --all-targets` | ✅ 零告警 | 含 `--features ffmpeg`，`-D warnings` 可用 |
| `check-native-abi.sh` | ✅ 通过 | 25 个 C 函数、24 个 NAPI 函数，头文件 == Rust 导出 == bridge == d.ts == ArkTS 导入 |
| `captrim-core` → 鸿蒙 arm64 | ✅ 编译通过 | `aarch64-unknown-linux-ohos`（Rust 官方 Tier 2 + Host Tools） |
| `libcaptrim.so` 交叉链接 | ✅ 通过 | ELF64 / AArch64 / DYN，25 个符号（含 8 个 `captrim_preview_*`），SONAME 已设，仅依赖 `libc.so` |
| NAPI 桥接编译 | ✅ 通过 | 真实 DevEco NDK，`-Wall -Wextra -Werror` 零警告；依赖仅 `libcaptrim.so` / `libace_napi.z.so` / `libc.so` |
| ArkTS 静态检查 | ✅ 0 error | `devecocli check arkts`（`.ets` 与 `Index.ets` 均通过），剩 2 条「.so 未校验」告警（预期） |
| 鸿蒙构建脚本端到端 | ✅ 通过 | Windows + Git Bash 实测产出并暂存 |
| **鸿蒙 HAP 打包** | ✅ 真机装上了 | `scripts/build-ohos-hap.sh`：HAP 内仅 `libcaptrim.so` + `libcaptrim_napi.so`，无 `libc++_shared.so` |
| **鸿蒙真机自检** | ✅ 9/9 通过 | Mate 60（ALN-AL00）/ OpenHarmony 7.0.0 / API 26 / arm64-v8a，详见下节 |
| **Android AAR 编译** | ⚠️ 未验证 | 本机有 SDK/NDK/Gradle 但无 Android 真机；Kotlin 与 JNI 签名（含 P1 新增 11 个）已逐一手工核对 |
| **FFmpeg 实际编译** | ⚠️ 未验证 | 本机无 FFmpeg 源码树；FFmpeg 官方要求 make + 宿主 C 编译器 |

真机自检跑的是 **stub 后端**（无 FFmpeg），验证的是「原生库能加载、25 个 C 函数可用、整数时间轴在 arm64 上算得对、预览状态机不失控」，不含真实解码。9 项结果：

```
PASS 原生库加载与 N-API 绑定 | libcaptrim 0.1.0
PASS 时间线构建（2 轨 3 片段） | 9.000s / 270 帧
PASS 分割 / 撤销 / 重做 | 4 → 3 → 4
PASS 29.97fps 帧栅格不塌陷 | 270 帧全部严格递增，最大误差 ≤1us
PASS 逐帧源时间唯一（不重复帧） | 前 60 帧源时间严格递增
PASS 预览时钟与丢帧统计 | 呈现 45 次 / 等待 45 次 / 位置 1500ms / 丢帧 0
PASS 编辑后自动重绘 | 同帧不重复，改动后立即重绘
PASS 音频主导时钟校正 | 漂移 50000us 已被校正
PASS 错误码可读 | 0 → "ok"，未知片段正确报错
```

CI 已配置为在上述缺口处开工（Android 构建），见 `.github/workflows/ci.yml`。

## 九、许可与专利

- FFmpeg 默认 **LGPL-2.1+**。两个编译脚本显式禁用了 `--enable-gpl` / `--enable-nonfree` / `libx264` / `libx265`，产物附构建回执。
- 保持 LGPL 的条件：动态链接、随附 FFmpeg 对应源码与 configure 行、在 App 的 About/EULA 中声明。
- 因编码走系统硬件，**不需要 libx264**，从根上避免整体变成 GPL。
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
