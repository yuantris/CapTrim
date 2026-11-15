#!/usr/bin/env bash
# Build the FULL GPL FFmpeg shared libraries (incl. libx264 / libx265) + the
# CapTrim C shim for HarmonyOS. Requires OHOS_NDK_HOME = <DevEco SDK>/default/openharmony/native.
#
# HarmonyOS requires every prebuilt .so to be produced by the HarmonyOS NDK
# toolchain and to carry a SONAME. The wrappers in scripts/toolchain/ supply
# --target=<arch>-linux-ohos, --sysroot and -D__MUSL__ exactly as documented
# by the HarmonyOS NDK build guide.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=ffmpeg-config-common.sh
source "${ROOT_DIR}/scripts/ffmpeg-config-common.sh"

# Put our minimal pkg-config shim (scripts/pkg-config) on PATH so FFmpeg's
# configure can locate the bundled x264/x265 .pc files on this host.
export PATH="${ROOT_DIR}/scripts:$PATH"

# FFmpeg's configure writes temp test files under TMPDIR; the default Windows
# path with backslashes gets mangled, so point it at Git Bash's /tmp.
export TMPDIR=/tmp

need_env OHOS_NDK_HOME
export OHOS_NDK_HOME
# HarmonyOS NDK ships no `make`; reuse the Android NDK's make (or system make).
ensure_build_tools "${ANDROID_NDK_HOME:+${ANDROID_NDK_HOME}/prebuilt/windows-x86_64/bin}"

ABIS="${ABIS:-arm64-v8a}"
FFMPEG_SRC="${FFMPEG_SRC:-${ROOT_DIR}/third_party/ffmpeg}"
FFMPEG_REF="${FFMPEG_REF:-n7.1}"
# Cap parallelism: the OHOS clang wrapper is a shell script; each parallel compile
# forks an MSYS bash and too many exhaust the MSYS fork heap on Windows.
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"; [ "$JOBS" -gt 6 ] && JOBS=6

# Host compiler for FFmpeg's build-time tools (they must RUN on this Windows
# build machine; the OHOS NDK clang cross-compiles and cannot). Use a standalone
# llvm-mingw clang toolchain, same as the Android build.
LLVM_MINGW_ZIP="${ROOT_DIR}/build/llvm-mingw.zip"
LLVM_MINGW_HOME="${LLVM_MINGW_HOME:-${ROOT_DIR}/build/llvm-mingw}"
if [ -f "$LLVM_MINGW_ZIP" ] && [ ! -x "$LLVM_MINGW_HOME/bin/clang.exe" ]; then
    log "extracting llvm-mingw host toolchain from $LLVM_MINGW_ZIP"
    rm -rf "${ROOT_DIR}/build/llvm-mingw-tmp"
    mkdir -p "${ROOT_DIR}/build/llvm-mingw-tmp"
    unzip -q "$LLVM_MINGW_ZIP" -d "${ROOT_DIR}/build/llvm-mingw-tmp" \
        || die "failed to unzip llvm-mingw (is 'unzip' available?)"
    _lm=$(ls -d "${ROOT_DIR}/build/llvm-mingw-tmp"/llvm-mingw-* 2>/dev/null | head -1)
    [ -n "$_lm" ] && mv "$_lm" "$LLVM_MINGW_HOME"
    rm -rf "${ROOT_DIR}/build/llvm-mingw-tmp"
fi
HOSTCC="${LLVM_MINGW_HOME}/bin/clang.exe"
[ -x "$HOSTCC" ] || die "host compiler not found at $HOSTCC; FFmpeg needs it to build+run build-time tools."

LLVM="${OHOS_NDK_HOME}/llvm/bin"
[ -x "${LLVM}/clang" ] || die "HarmonyOS NDK clang not found at ${LLVM}/clang"
[ -d "${OHOS_NDK_HOME}/sysroot" ] || die "HarmonyOS NDK sysroot missing"

if [ ! -d "$FFMPEG_SRC" ]; then
    log "cloning FFmpeg ${FFMPEG_REF} into ${FFMPEG_SRC}"
    mkdir -p "$(dirname "$FFMPEG_SRC")"
    git clone --depth 1 --branch "$FFMPEG_REF" https://git.ffmpeg.org/ffmpeg.git "$FFMPEG_SRC"
fi

for ABI in $ABIS; do
    case "$ABI" in
        arm64-v8a)
            ARCH="aarch64"; WRAPPER="ohos-aarch64-clang.sh" ;;
        armeabi-v7a)
            ARCH="arm"; WRAPPER="ohos-armv7-clang.sh" ;;
        x86_64)
            ARCH="x86_64"; WRAPPER="ohos-x86_64-clang.sh" ;;
        *) die "unsupported ABI: $ABI" ;;
    esac

    # x264 and FFmpeg are built by Git Bash's make/sh, which execs a POSIX .sh
    # shim directly. x265 is built by cmake, a native Windows binary that can only
    # launch .bat/.cmd/.exe — so build_x265 converts the .sh wrapper to .bat for
    # the cmake step internally. (Using .bat for x264/FFmpeg fails because sh
    # tries to interpret the .bat as a shell script and the NDK path's space
    # truncates it.)
    CC="${ROOT_DIR}/scripts/toolchain/${WRAPPER}"
    CXX="${ROOT_DIR}/scripts/toolchain/${WRAPPER%.sh}++.sh"
    [ -f "$CC" ] || die "missing compiler wrapper: $CC"
    [ -f "$CXX" ] || die "missing C++ compiler wrapper: $CXX"

    SYSROOT="${OHOS_NDK_HOME}/sysroot"
    resolve_external_codecs "$ABI" "$CC" "$CXX" "$ARCH" "$SYSROOT"

    # x265 is a C++ static archive; FFmpeg's link test for it needs the C++
    # runtime resolvable. The OHOS NDK provides libc++/libc++abi (statically
    # linked from its llvm/lib). (Set FFMPEG_EXTRA_LIBS to override.)
    FFMPEG_EXTRA_LIBS="${FFMPEG_EXTRA_LIBS:--lc++ -lc++abi -lm}"

    BUILD="${ROOT_DIR}/build/ffmpeg-ohos/${ABI}"
    PREFIX="${BUILD}/prefix"
    OUT="${ROOT_DIR}/dist/ohos/${ABI}"
    mkdir -p "$BUILD" "$OUT"

    # FFmpeg has no dedicated OHOS target; --target-os=linux plus the NDK
    # clang wrapper (which injects --target/--sysroot/-D__MUSL__) is the
    # documented cross-compilation approach for HarmonyOS third-party libs.
    CFG=(
        "${FFMPEG_COMMON_CFG[@]}"
        --prefix="$PREFIX"
        --target-os=linux
        --arch="$ARCH"
        --enable-cross-compile
        # The OHOS NDK path contains a space ("DevEco Studio"); FFmpeg's install
        # rule runs $(STRIP) unquoted, which splits on that space. Stripping is
        # cosmetic (size only), so disable it to keep the toolchain path safe.
        --disable-stripping
        --cc="$CC"
        --cxx="$CXX"
        --host-cc="$HOSTCC"
        --nm="${LLVM}/llvm-nm.exe"
        --ar="${LLVM}/llvm-ar.exe"
        --ranlib="${LLVM}/llvm-ranlib.exe"
        --strip="${LLVM}/llvm-strip.exe"
        --extra-cflags="-D__MUSL__"
        --extra-ldflags="-D__MUSL__"
    )
    CFG+=(
        ${FFMPEG_EXTRA_ENABLE:-}
        --extra-cflags="-D__MUSL__ ${EXT_CFLAGS:-}"
        --extra-ldflags="-D__MUSL__ ${EXT_LDFLAGS:-}"
        --extra-libs="${FFMPEG_EXTRA_LIBS}"
    )

    log "configure FFmpeg for HarmonyOS ${ABI}"
    ( cd "$BUILD" && "$FFMPEG_SRC/configure" "${CFG[@]}" )
    fix_ffmpeg_generated_paths "$BUILD"
    make -C "$BUILD" -j"$JOBS"
    make -C "$BUILD" install

    # The platform-neutral three-stage orchestrator (captrim_pipeline.c) is
    # added by build_c_shim for BOTH platforms. OHOS additionally links the
    # hardware first/last mile: AVCodec Kit Buffer mode + OH_AVMuxer + hilog.
    # Android keeps the same pipeline with weak factories resolving to NULL,
    # i.e. it automatically runs the software fallback until AMediaCodec is
    # wired - this is the intended "same ABI, platform swap" design.
    build_c_shim "$OUT" "$CC" \
        "${ROOT_DIR}/native/captrim_ohos_hw.c" \
        -DCAPTRIM_OHOS_HW=1 \
        -lnative_media_codecbase -lnative_media_core \
        -lnative_media_vdec -lnative_media_venc \
        -lnative_media_avsource -lnative_media_avdemuxer \
        -lnative_media_avmuxer -lhilog_ndk.z
    write_build_receipt "$OUT" "$FFMPEG_SRC/configure" "${CFG[@]}"

    # libavcodec links against the OHOS C++ runtime libc++_shared.so. That
    # runtime is NOT present on the device system image for third-party apps, so
    # it must ship inside the HAP or the NAPI module fails to dlopen at runtime
    # ("Error loading shared library libc++_shared.so: needed by libavcodec.so").
    # Bundle the NDK build matching this ABI next to the FFmpeg libs; the
    # build-rust.sh stage_libs step then copies it into the HAP automatically.
    case "$ABI" in
        arm64-v8a) _cxx_rt="${OHOS_NDK_HOME}/llvm/lib/aarch64-linux-ohos/libc++_shared.so" ;;
        *) _cxx_rt="$(find "${OHOS_NDK_HOME}/llvm/lib" -path "*${ARCH}-linux-ohos*" -name libc++_shared.so 2>/dev/null | head -1)" ;;
    esac
    if [ -n "${_cxx_rt:-}" ] && [ -f "$_cxx_rt" ]; then
        cp -f "$_cxx_rt" "${OUT}/libc++_shared.so"
        log "bundled C++ runtime -> ${OUT}/libc++_shared.so"
    else
        log "WARN: libc++_shared.so not found for ${ABI}; the HAP will fail to load FFmpeg at runtime"
    fi

    # Verify SONAME, per HarmonyOS "prebuilt library" constraints.
    if [ -x "${LLVM}/llvm-readelf" ]; then
        "${LLVM}/llvm-readelf" -d "${OUT}/libcaptrim_ffmpeg.so" | grep SONAME \
            || die "libcaptrim_ffmpeg.so is missing SONAME"
    fi
    log "HarmonyOS ${ABI} artifacts -> ${OUT}"
done
