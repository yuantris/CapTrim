#!/usr/bin/env bash
# Build libcaptrim.so for Android, HarmonyOS, or the host, and stage every
# native library into the corresponding platform package.
#
#   ./scripts/build-rust.sh host                 # CI / local verification
#   ./scripts/build-rust.sh android             # requires ANDROID_NDK_HOME
#   ./scripts/build-rust.sh ohos                # requires OHOS_NDK_HOME
#
# The mobile builds enable the `ffmpeg` feature, so run build-ffmpeg-*.sh first.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

log() { printf '[captrim] %s\n' "$*"; }
die() { printf '[captrim][error] %s\n' "$*" >&2; exit 1; }

# On Windows / Git-Bash the NDK clang.exe is a *native* Windows binary and does
# not understand MSYS-style paths like /c/Users/... (it would look for
# C:\c\Users\... and never find libcaptrim_ffmpeg.so). Convert a path to a
# Windows "mixed" form (C:/...) before handing it to the native linker. On
# Linux/macOS `cygpath` is absent, so the path is returned unchanged.
native_path() {
    if command -v cygpath >/dev/null 2>&1; then
        cygpath -w -m -- "$1"
    else
        printf '%s' "$1"
    fi
}

PLATFORM="${1:-}"
[ -n "$PLATFORM" ] || die "usage: build-rust.sh host|android|ohos"

stage_libs() {
    local src_dir="$1"
    local dst_dir="$2"
    mkdir -p "$dst_dir"
    local found=0
    # Use *.so* so versioned FFmpeg libraries (libavcodec.so.61.19.100 and the
    # libavcodec.so / libavcodec.so.61 symlinks) are staged too. Android ships
    # unversioned .so files, where *.so* collapses to *.so, so this is safe for
    # both platforms.
    #
    # -L dereferences the SONAME symlinks: on Windows hosts Git Bash often
    # cannot create symlinks at all ("cannot create symbolic link ..."), and a
    # dereferenced copy is what the packager wants anyway. The trap it avoids
    # is a stale libavfilter.so left from an older build shadowing the fresh
    # libavfilter.so.10.4.100 in the HAP.
    for lib in "$src_dir"/*.so*; do
        [ -e "$lib" ] || continue
        cp -aL "$lib" "$dst_dir/"
        found=1
    done
    [ "$found" = "1" ] || die "no .so found in $src_dir"
    log "staged native libs -> $dst_dir"
}

if [ "$PLATFORM" = "host" ]; then
    log "building host cdylib (stub backend)"
    cargo build --release -p captrim-ffi
    exit 0
fi

# `FEATURES=` (explicitly empty) builds the stub backend, which needs no FFmpeg
# and is what CI and first-time integration use.
FEATURES="${FEATURES-ffmpeg}"
ABIS="${ABIS:-arm64-v8a}"
case "$(uname -s)" in
    Linux*) HOST_TAG="linux-x86_64"; WRAPPER_EXT=".sh" ;;
    Darwin*) HOST_TAG="darwin-x86_64"; WRAPPER_EXT=".sh" ;;
    MINGW*|MSYS*|CYGWIN*) HOST_TAG="windows-x86_64"; WRAPPER_EXT=".bat" ;;
    *) die "unsupported host: $(uname -s)" ;;
esac

for ABI in $ABIS; do
    case "$PLATFORM:$ABI" in
        android:arm64-v8a)
            RUST_TARGET="aarch64-linux-android"; CLANG_NAME="aarch64-linux-android" ;;
        android:armeabi-v7a)
            RUST_TARGET="armv7-linux-androideabi"; CLANG_NAME="armv7a-linux-androideabi" ;;
        android:x86_64)
            RUST_TARGET="x86_64-linux-android"; CLANG_NAME="x86_64-linux-android" ;;
        ohos:arm64-v8a)
            RUST_TARGET="aarch64-unknown-linux-ohos"; WRAPPER="ohos-aarch64-clang" ;;
        ohos:armeabi-v7a)
            RUST_TARGET="armv7-unknown-linux-ohos"; WRAPPER="ohos-armv7-clang" ;;
        ohos:x86_64)
            RUST_TARGET="x86_64-unknown-linux-ohos"; WRAPPER="ohos-x86_64-clang" ;;
        *) die "unsupported platform/ABI: $PLATFORM/$ABI" ;;
    esac

    if [ "$PLATFORM" = "android" ]; then
        [ -n "${ANDROID_NDK_HOME:-}" ] || die "ANDROID_NDK_HOME is not set"
        ANDROID_API="${ANDROID_API:-26}"
        LINKER_BASE="${ANDROID_NDK_HOME}/toolchains/llvm/prebuilt/${HOST_TAG}/bin/${CLANG_NAME}${ANDROID_API}-clang"
        # On Windows the NDK ships the clang wrapper as a .cmd. rustc only spawns
        # a linker through `cmd /c` for *.cmd/*.bat linkers, so the bare-name
        # (shell-script) wrapper would be exec'd directly and fail.
        if [ "$HOST_TAG" = "windows-x86_64" ]; then
            LINKER="${LINKER_BASE}.cmd"
        else
            LINKER="${LINKER_BASE}"
        fi
        NATIVE_DIR="${ROOT_DIR}/dist/android/${ABI}"
        STAGE_DIR="${ROOT_DIR}/platforms/android/captrim/src/main/jniLibs/${ABI}"
    else
        [ -n "${OHOS_NDK_HOME:-}" ] || die "OHOS_NDK_HOME is not set"
        export OHOS_NDK_HOME
        LINKER="${ROOT_DIR}/scripts/toolchain/${WRAPPER}${WRAPPER_EXT}"
        NATIVE_DIR="${ROOT_DIR}/dist/ohos/${ABI}"
        STAGE_DIR="${ROOT_DIR}/platforms/ohos/captrim/libs/${ABI}"
    fi

    # A .bat/.cmd has no executable bit under Git Bash, so presence is enough.
    case "$LINKER" in
        *.bat|*.cmd) [ -f "$LINKER" ] || die "linker wrapper not found: $LINKER" ;;
        *) [ -x "$LINKER" ] || die "linker not found or not executable: $LINKER" ;;
    esac
    if [ -n "$FEATURES" ]; then
        [ -f "${NATIVE_DIR}/libcaptrim_ffmpeg.so" ] || \
            die "run build-ffmpeg-${PLATFORM}.sh first, or set FEATURES= to build the stub backend (missing ${NATIVE_DIR}/libcaptrim_ffmpeg.so)"
    fi

    LINKER_VAR="CARGO_TARGET_$(echo "$RUST_TARGET" | tr '[:lower:]-' '[:upper:]_')_LINKER"
    export "$LINKER_VAR"="$LINKER"
    # HarmonyOS requires a SONAME on every prebuilt .so.
    # NATIVE_DIR may be an MSYS path (/c/...); the native linker only accepts a
    # Windows path, so convert it.
    export RUSTFLAGS="${RUSTFLAGS:-} -L native=$(native_path "$NATIVE_DIR") -C link-arg=-Wl,-soname,libcaptrim.so"

    FEATURE_ARGS=()
    if [ -n "$FEATURES" ]; then
        FEATURE_ARGS=(--features "$FEATURES")
    fi

    log "building ${RUST_TARGET} (features=${FEATURES:-none})"
    cargo build --release --target "$RUST_TARGET" -p captrim-ffi ${FEATURE_ARGS[@]+"${FEATURE_ARGS[@]}"}

    mkdir -p "$NATIVE_DIR"
    cp -a "target/${RUST_TARGET}/release/libcaptrim.so" "${NATIVE_DIR}/"
    stage_libs "$NATIVE_DIR" "$STAGE_DIR"
done
