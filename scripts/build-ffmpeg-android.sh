#!/usr/bin/env bash
# Build the trimmed LGPL FFmpeg shared libraries + the CapTrim C shim for
# Android. Requires ANDROID_NDK_HOME (NDK r25+). Recommended host: Linux/macOS.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=ffmpeg-config-common.sh
source "${ROOT_DIR}/scripts/ffmpeg-config-common.sh"

need_env ANDROID_NDK_HOME

ANDROID_API="${ANDROID_API:-26}"
ABIS="${ABIS:-arm64-v8a}"
FFMPEG_SRC="${FFMPEG_SRC:-${ROOT_DIR}/third_party/ffmpeg}"
FFMPEG_REF="${FFMPEG_REF:-n7.1}"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"

case "$(uname -s)" in
    Linux*) HOST_TAG="linux-x86_64" ;;
    Darwin*) HOST_TAG="darwin-x86_64" ;;
    MINGW*|MSYS*|CYGWIN*) HOST_TAG="windows-x86_64" ;;
    *) die "unsupported host: $(uname -s)" ;;
esac

TOOLCHAIN="${ANDROID_NDK_HOME}/toolchains/llvm/prebuilt/${HOST_TAG}"
[ -d "$TOOLCHAIN" ] || die "NDK toolchain not found at $TOOLCHAIN"

if [ ! -d "$FFMPEG_SRC" ]; then
    log "cloning FFmpeg ${FFMPEG_REF} into ${FFMPEG_SRC}"
    mkdir -p "$(dirname "$FFMPEG_SRC")"
    git clone --depth 1 --branch "$FFMPEG_REF" https://git.ffmpeg.org/ffmpeg.git "$FFMPEG_SRC"
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

    CC="${TOOLCHAIN}/bin/${TRIPLE}${ANDROID_API}-clang"
    CXX="${CC}++"
    [ -x "$CC" ] || die "missing compiler: $CC"

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
        --nm="${TOOLCHAIN}/bin/llvm-nm"
        --ar="${TOOLCHAIN}/bin/llvm-ar"
        --ranlib="${TOOLCHAIN}/bin/llvm-ranlib"
        --strip="${TOOLCHAIN}/bin/llvm-strip"
        --sysroot="${TOOLCHAIN}/sysroot"
    )

    log "configure FFmpeg for ${ABI} (API ${ANDROID_API})"
    ( cd "$BUILD" && "$FFMPEG_SRC/configure" "${CFG[@]}" )
    make -C "$BUILD" -j"$JOBS"
    make -C "$BUILD" install

    build_c_shim "$OUT" "$CC"
    write_build_receipt "$OUT" "$FFMPEG_SRC/configure" "${CFG[@]}"
    log "Android ${ABI} artifacts -> ${OUT}"
done
