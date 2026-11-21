#!/usr/bin/env bash
# Rebuild ONLY libcaptrim_ffmpeg.so (the C shim) for an already-installed
# FFmpeg prefix, without re-running FFmpeg's configure/make.
# Usage: CAPTRIM_LICENSE=gpl|lgpl bash scripts/relink-cshim.sh [ohos|android]
#
# Why this exists: native/*.c is compiled into libcaptrim_ffmpeg.so by
# build-ffmpeg-<platform>.sh, and build-rust.sh only stages that file. Editing
# the C sources and re-running build-rust.sh therefore ships a stale shim whose
# missing symbol kills the app at dlopen (on device it surfaces as a JS
# SyntaxError about a missing export, not as a link error). make inside the
# FFmpeg script is not incremental either (it wipes the build dir), so this
# relink is the ~1 min alternative to a ~6 min full FFmpeg rebuild.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export CAPTRIM_LICENSE="${CAPTRIM_LICENSE:-gpl}"
source "${ROOT_DIR}/scripts/ffmpeg-config-common.sh"

PLATFORM="${1:-ohos}"
ABI=arm64-v8a

case "$PLATFORM" in
    ohos)
        need_env OHOS_NDK_HOME
        export OHOS_NDK_HOME
        ARCH=aarch64
        CC="${ROOT_DIR}/scripts/toolchain/ohos-aarch64-clang.sh"
        HW_ARGS=(
            "${ROOT_DIR}/native/captrim_ohos_hw.c"
            -DCAPTRIM_OHOS_HW=1
            -lnative_media_codecbase -lnative_media_core
            -lnative_media_vdec -lnative_media_venc
            -lnative_media_avsource -lnative_media_avdemuxer
            -lnative_media_avmuxer -lhilog_ndk.z
        )
        NM="${OHOS_NDK_HOME}/llvm/bin/llvm-nm"
        ;;
    android)
        need_env ANDROID_NDK_HOME
        export ANDROID_NDK_HOME
        HOST_TAG=windows-x86_64
        ANDROID_API="${ANDROID_API:-21}"
        ARCH=aarch64
        CC="${ANDROID_NDK_HOME}/toolchains/llvm/prebuilt/${HOST_TAG}/bin/aarch64-linux-android${ANDROID_API}-clang.cmd"
        HW_ARGS=("${ROOT_DIR}/native/captrim_android_hw.c" -lmediandk -llog)
        NM="${ANDROID_NDK_HOME}/toolchains/llvm/prebuilt/${HOST_TAG}/bin/llvm-nm.exe"
        ;;
    *) die "unknown platform: $PLATFORM" ;;
esac

[ -f "$CC" ] || die "missing compiler: $CC"

read -r BUILD OUT <<< "$(license_dirs "$PLATFORM" "$ABI")"
PREFIX="${BUILD}/prefix"
FFMPEG_SRC="${FFMPEG_SRC:-${ROOT_DIR}/third_party/ffmpeg}"
[ -d "${PREFIX}/lib" ] || die "no installed FFmpeg prefix at ${PREFIX}"
mkdir -p "$OUT"

log "relinking C shim for ${CAPTRIM_LICENSE} profile (${PLATFORM}) -> ${OUT}"
build_c_shim "$OUT" "$CC" "${HW_ARGS[@]}"

"$NM" -D --defined-only "${OUT}/libcaptrim_ffmpeg.so" \
    | grep -E "ct_ff_(encoder|filter)_available" \
    || die "new symbols missing from ${OUT}/libcaptrim_ffmpeg.so"
log "ok: ${OUT}/libcaptrim_ffmpeg.so exports ct_ff_encoder_available + ct_ff_filter_available"
