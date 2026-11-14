#!/usr/bin/env bash
# Build the trimmed LGPL FFmpeg shared libraries + the CapTrim C shim for
# HarmonyOS. Requires OHOS_NDK_HOME = <DevEco SDK>/default/openharmony/native.
#
# HarmonyOS requires every prebuilt .so to be produced by the HarmonyOS NDK
# toolchain and to carry a SONAME. The wrappers in scripts/toolchain/ supply
# --target=<arch>-linux-ohos, --sysroot and -D__MUSL__ exactly as documented
# by the HarmonyOS NDK build guide.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=ffmpeg-config-common.sh
source "${ROOT_DIR}/scripts/ffmpeg-config-common.sh"

need_env OHOS_NDK_HOME
export OHOS_NDK_HOME

ABIS="${ABIS:-arm64-v8a}"
FFMPEG_SRC="${FFMPEG_SRC:-${ROOT_DIR}/third_party/ffmpeg}"
FFMPEG_REF="${FFMPEG_REF:-n7.1}"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"

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

    CC="${ROOT_DIR}/scripts/toolchain/${WRAPPER}"
    CXX="${ROOT_DIR}/scripts/toolchain/${WRAPPER%.sh}++.sh"
    [ -x "$CC" ] || die "missing compiler wrapper: $CC"

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
        --cc="$CC"
        --cxx="$CXX"
        --nm="${LLVM}/llvm-nm"
        --ar="${LLVM}/llvm-ar"
        --ranlib="${LLVM}/llvm-ranlib"
        --strip="${LLVM}/llvm-strip"
        --extra-cflags="-D__MUSL__"
        --extra-ldflags="-D__MUSL__"
    )

    log "configure FFmpeg for HarmonyOS ${ABI}"
    ( cd "$BUILD" && "$FFMPEG_SRC/configure" "${CFG[@]}" )
    make -C "$BUILD" -j"$JOBS"
    make -C "$BUILD" install

    build_c_shim "$OUT" "$CC"
    write_build_receipt "$OUT" "$FFMPEG_SRC/configure" "${CFG[@]}"

    # Verify SONAME, per HarmonyOS "prebuilt library" constraints.
    if [ -x "${LLVM}/llvm-readelf" ]; then
        "${LLVM}/llvm-readelf" -d "${OUT}/libcaptrim_ffmpeg.so" | grep SONAME \
            || die "libcaptrim_ffmpeg.so is missing SONAME"
    fi
    log "HarmonyOS ${ABI} artifacts -> ${OUT}"
done
