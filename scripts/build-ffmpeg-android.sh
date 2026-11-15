#!/usr/bin/env bash
# Build the FULL GPL FFmpeg shared libraries (incl. libx264 / libx265) + the
# CapTrim C shim for Android. Requires ANDROID_NDK_HOME (NDK r25+). Because we
# enable GPL codecs, the binaries are GPLv2-or-later, NOT LGPL.
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

need_env ANDROID_NDK_HOME

ANDROID_API="${ANDROID_API:-26}"
ABIS="${ABIS:-arm64-v8a}"
FFMPEG_SRC="${FFMPEG_SRC:-${ROOT_DIR}/third_party/ffmpeg}"
FFMPEG_REF="${FFMPEG_REF:-n7.1}"
# Cap parallelism: the NDK clang is a wrapper script and each parallel compile
# forks an MSYS bash; too many concurrent forks exhaust the MSYS fork heap
# ("Resource temporarily unavailable"). Keep this low on Windows.
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"; [ "$JOBS" -gt 6 ] && JOBS=6

# Host compiler for FFmpeg's build-time tools (they must RUN on this Windows
# build machine). The Android NDK clang cannot do that, so we use a standalone
# llvm-mingw clang toolchain (downloaded as build/llvm-mingw.zip).
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

case "$(uname -s)" in
    Linux*) HOST_TAG="linux-x86_64" ;;
    Darwin*) HOST_TAG="darwin-x86_64" ;;
    MINGW*|MSYS*|CYGWIN*) HOST_TAG="windows-x86_64" ;;
    *) die "unsupported host: $(uname -s)" ;;
esac

TOOLCHAIN="${ANDROID_NDK_HOME}/toolchains/llvm/prebuilt/${HOST_TAG}"
[ -d "$TOOLCHAIN" ] || die "NDK toolchain not found at $TOOLCHAIN"
ensure_build_tools "${ANDROID_NDK_HOME}/prebuilt/${HOST_TAG}/bin"

if [ ! -d "$FFMPEG_SRC" ]; then
    git_clone_robust https://git.ffmpeg.org/ffmpeg.git "$FFMPEG_SRC" "$FFMPEG_REF" \
        || die "FFmpeg clone failed after retries (set FFMPEG_SRC to a pre-populated dir)"
fi

for ABI in $ABIS; do
    case "$ABI" in
        arm64-v8a)
            ARCH="aarch64"; TRIPLE="aarch64-linux-android"; CPU="armv8-a" ;;
        armeabi-v7a)
            ARCH="arm"; TRIPLE="armv7a-linux-androideabi"; CPU="armv7-a" ;;
        x86_64)
            ARCH="x86_64"; TRIPLE="x86_64-linux-android"; CPU="x86_64" ;;
        *) die "unsupported ABI: $ABI" ;;
    esac

    # Use the .cmd wrappers (not the bash wrappers): they launch clang via
    # cmd.exe, avoiding the extra MSYS bash fork per compile (fork-heap-safe).
    CC="${TOOLCHAIN}/bin/${TRIPLE}${ANDROID_API}-clang.cmd"
    CXX="${TOOLCHAIN}/bin/${TRIPLE}${ANDROID_API}-clang++.cmd"
    [ -f "$CC" ] || die "missing compiler: $CC"

    SYSROOT="${TOOLCHAIN}/sysroot"
    # x265 cross-build: use the NDK's official cmake toolchain file so cmake can
    # identify the Clang compiler and target the right ABI.
    export X265_CMAKE_TOOLCHAIN="${ANDROID_NDK_HOME}/build/cmake/android.toolchain.cmake"
    export X265_CMAKE_EXTRA="-DANDROID_ABI=${ABI} -DANDROID_PLATFORM=android-${ANDROID_API}"
    resolve_external_codecs "$ABI" "$CC" "$CXX" "$ARCH" "$SYSROOT"

    # x265 is a C++ static archive; FFmpeg's link test for it needs the C++
    # runtime resolvable. The NDK provides libc++/libc++abi; -ldl/-lm cover the
    # rest of x265's private deps. (Set FFMPEG_EXTRA_LIBS to override.)
    FFMPEG_EXTRA_LIBS="${FFMPEG_EXTRA_LIBS:--lc++ -lc++abi -ldl -lm}"

    BUILD="${ROOT_DIR}/build/ffmpeg-android/${ABI}"
    PREFIX="${BUILD}/prefix"
    OUT="${ROOT_DIR}/dist/android/${ABI}"
    mkdir -p "$BUILD" "$OUT"

    CFG=(
        "${FFMPEG_COMMON_CFG[@]}"
        --prefix="$PREFIX"
        --target-os=android
        --arch="$ARCH"
        --cpu="$CPU"
        --enable-cross-compile
        --cc="$CC"
        --cxx="$CXX"
        --host-cc="$HOSTCC"
        --nm="${TOOLCHAIN}/bin/llvm-nm.exe"
        --ar="${TOOLCHAIN}/bin/llvm-ar.exe"
        --ranlib="${TOOLCHAIN}/bin/llvm-ranlib.exe"
        --strip="${TOOLCHAIN}/bin/llvm-strip.exe"
        --sysroot="${TOOLCHAIN}/sysroot"
    )
    CFG+=(
        ${FFMPEG_EXTRA_ENABLE:-}
        --extra-cflags="${EXT_CFLAGS:-}"
        --extra-ldflags="${EXT_LDFLAGS:-}"
        --extra-libs="${FFMPEG_EXTRA_LIBS}"
    )

    [ -x "$HOSTCC" ] || die "host compiler not found at $HOSTCC; FFmpeg needs it to build+run build-time tools. Place llvm-mingw zip at build/llvm-mingw.zip (auto-extracted) or set LLVM_MINGW_HOME."

    log "configure FFmpeg for ${ABI} (API ${ANDROID_API})"
    ( cd "$BUILD" && "$FFMPEG_SRC/configure" "${CFG[@]}" )
    fix_ffmpeg_generated_paths "$BUILD"
    make -C "$BUILD" -j"$JOBS"
    make -C "$BUILD" install

    build_c_shim "$OUT" "$CC"
    write_build_receipt "$OUT" "$FFMPEG_SRC/configure" "${CFG[@]}"
    log "Android ${ABI} artifacts -> ${OUT}"
done
