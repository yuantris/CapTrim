# CapTrim

Android 与 HarmonyOS 共用的视频剪辑核心 SDK。**Rust 描述时间线，FFmpeg 处理文件与滤镜，编解码和渲染交给系统硬件。**

一份 Rust 核心 + 一份 C ABI，两端只做薄桥接（Android JNI / 鸿蒙 Node-API），业务 API 只定义一次。目标是覆盖「剪映基础剪辑」：导入探测、分割、删除、排序、裁剪、变速、音量、音画分离、转场、滤镜、字幕、画中画、比例裁剪、导出。

当前状态：**P0 骨架已跑通**（见[验证状态](#验证状态)），可在真机前完成双端联调的准备工作。

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
scripts/                  FFmpeg 裁剪编译、Rust 双端构建、ABI 校验
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
| `set_canvas` | `width` · `height` · `fps{num,den}` |

### 两端用法

```kotlin
// Android
CapTrim.create(draftJson).use { editor ->
    val clipId = editor.apply("""{"type":"split_clip","clip_id":3,"at_us":4000000}""")
    editor.undo()
    val draft = editor.draft()
}
```

```typescript
// HarmonyOS
const editor = CapTrim.create(draftJson);
editor.apply('{"type":"split_clip","clip_id":3,"at_us":4000000}');
editor.undo();
editor.close();
```

两端失败都抛异常并带 `CAPTRIM_ERR_*` 码。C ABI 的线程约定、字符串所有权写在 `crates/captrim-ffi/captrim.h` 头部，改代码前先读。

## 六、P0 已完成 / 未完成

**已完成**

- `Rational` 精确时间运算（含 NTSC 30000/1001 帧边界、无浮点比较）
- Timeline / Track / Clip 模型、重叠校验、分割（含变速片段的源时间映射）
- Command 栈 + 撤销重做 + 草稿 JSON 往返
- 稳定 C ABI：句柄注册表、错误码、`catch_unwind`、UTF-8 校验
- FFmpeg probe / remux 的 C shim + 特征门控后端（未启用时返回 `NO_BACKEND` 而非崩溃）
- 双端工程：AAR（JNI + Kotlin）、HAR（Node-API + ArkTS + `.d.ts`）
- 构建与校验脚本：FFmpeg 裁剪 configure、双端 Rust 交叉编译、ABI/NAPI 漂移检查

**未完成（按优先级）**

- P1：硬解码预览、音画同步、GPU 合成（先把画面跑起来）
- P2：滤镜/转场/画中画/字幕渲染、音频混合、**重编码导出**（`ExportPlan`/`Segment` 抽象）
- P3：变速保调、功耗与机型兼容矩阵、崩溃符号化
- 倒放：要求反向解码 + 全段帧缓存，复杂度约等于其余功能之和，明确留给 v2

## 七、验证状态

区分「本机实测通过」和「已写好但未在此环境验证」，避免误判成熟度。

| 项 | 状态 | 说明 |
|---|---|---|
| `cargo test --workspace` | ✅ 26 通过 | core 18 + 集成 1 + FFI 7 |
| `cargo clippy --workspace --all-targets` | ✅ 零告警 | 含 `--features ffmpeg` |
| `check-native-abi.sh` | ✅ 通过 | 14 个 C 函数、13 个 NAPI 函数三方一致 |
| `captrim-core` → 鸿蒙 arm64 | ✅ 编译通过 | `aarch64-unknown-linux-ohos`（Rust 官方 Tier 2 + Host Tools） |
| `libcaptrim.so` 交叉链接 | ✅ 通过 | ELF64 / AArch64 / DYN，SONAME=`libcaptrim.so`，仅依赖 `libc.so` |
| NAPI 桥接编译 | ✅ 通过 | 真实 DevEco NDK，`-Wall -Wextra` 零警告；依赖仅 `libcaptrim.so` / `libace_napi.z.so` / `libc.so` |
| ArkTS 静态检查 | ✅ 0 error | `devecocli check arkts`，剩 2 条「.so 未校验」告警（P0 预期） |
| 鸿蒙构建脚本端到端 | ✅ 通过 | Windows + Git Bash 实测产出并暂存 |
| **Android AAR 编译** | ⚠️ 未验证 | 本机无 Android SDK/NDK/Gradle；Kotlin 与 JNI 签名已逐一手工核对 |
| **FFmpeg 实际编译** | ⚠️ 未验证 | 本机无 FFmpeg 源码树；FFmpeg 官方要求 make + 宿主 C 编译器 |
| 真机运行 | ⚠️ 未验证 | 需真机；官方注明模拟器仅支持部分软解格式 |

CI 已配置为在上述缺口处开工（Android 构建），见 `.github/workflows/ci.yml`。

## 八、许可与专利

- FFmpeg 默认 **LGPL-2.1+**。两个编译脚本显式禁用了 `--enable-gpl` / `--enable-nonfree` / `libx264` / `libx265`，产物附构建回执。
- 保持 LGPL 的条件：动态链接、随附 FFmpeg 对应源码与 configure 行、在 App 的 About/EULA 中声明。
- 因编码走系统硬件，**不需要 libx264**，从根上避免整体变成 GPL。
- H.264 / H.265 另有**专利许可**问题（与开源许可无关），商业分发需单独评估。

## 九、已知坑

已踩过的，以及一定会踩的：

1. **鸿蒙预构建库有硬约束**：`.so` 必须由 HarmonyOS NDK 工具链编译、**必须带 SONAME**（`llvm-readelf -d | grep SONAME` 验证，否则打包后加载不到）、架构限 `armeabi-v7a / arm64-v8a / x86_64`。
2. **STL 一致性**：鸿蒙要求同一 App 内所有 Native 库的 `OHOS_STL` 一致。因此 NAPI 桥接刻意不依赖 `libc++_shared.so`（不用 `std::string`，`-fno-exceptions -fno-rtti -nostdlib++`），Android 侧用 `c++_static`。这是本仓库为规避该坑做的主动设计，不是随手写的编译选项。
3. **`napi_env` / `JNIEnv` 禁止跨线程缓存**。当前 P0 导出在调用线程同步执行所以安全；一旦导出移到工作线程，进度回调**必须**改成 `napi_create_threadsafe_function`，否则随机闪退。
4. **时间戳**：B 帧导致 DTS≠PTS；seek 必须退回关键帧再解码到目标帧；导出时自己生成单调递增 PTS，不要透传源文件时间戳。
5. **不要做 YUV↔RGB↔YUV 往返**。Surface 模式解码直出纹理，鸿蒙 `AVBuffer` 回调本身零拷贝，自己 memcpy 会白吃掉一半性能。
6. **音频统一先重采样**到 48kHz/立体声/f32 再混合与变速；每轨各做一次重采样是常见性能陷阱。
7. **变速保调**需要独立的 tempo（时间伸缩）与 pitch 处理，`libswresample` 只做采样率转换，不做变速保调。
8. **VFR 素材**（手机录屏、部分相机）没有恒定帧率。P0 的 remux 不受影响，但 P1 按帧率映射时间线时必须按 PTS 走，不能假设 `frame_index / fps`。
9. **素材会被用户删掉**：草稿里存 URI 不够，需持久化权限或拷贝到沙箱，否则重开草稿时一片空白。
10. **生命周期**：XComponent / Surface 的销毁必须早于解码器停止；长时导出走前台服务 / 后台长时任务。
