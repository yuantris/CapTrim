#!/usr/bin/env bash
# Build and run the native CapTrim self-check on a connected Android device.
# Requires ANDROID_NDK_HOME and a device listed by `adb devices`.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
: "${ANDROID_NDK_HOME:?ANDROID_NDK_HOME is not set}"

case "$(uname -s)" in
    Linux*)   HOST_TAG=linux-x86_64 ;;
    Darwin*)  HOST_TAG=darwin-x86_64 ;;
    MINGW*|MSYS*|CYGWIN*) HOST_TAG=windows-x86_64 ;;
    *) echo "unsupported host" >&2; exit 1 ;;
esac

CC="${ANDROID_NDK_HOME}/toolchains/llvm/prebuilt/${HOST_TAG}/bin/aarch64-linux-android26-clang"
[ -x "$CC" ] || CC="${CC}.cmd"
DIST="$ROOT/dist/android/arm64-v8a"
SAMPLE="${1:-$ROOT/platforms/ohos/entry/src/main/resources/rawfile/sample.mp4}"
DEVICE_DIR=/data/local/tmp/captrim-selftest

# Git-Bash rewrites /data/local/tmp into a Windows path. Only the adb calls
# need that disabled; the NDK compiler still needs MSYS to convert -I/-L paths.
adbcmd() { MSYS_NO_PATHCONV=1 command adb "$@"; }
wpath() {
    if command -v cygpath >/dev/null 2>&1; then cygpath -w "$1"; else printf '%s' "$1"; fi
}

if ! adbcmd get-state >/dev/null 2>&1; then
    echo "no Android device connected" >&2
    exit 1
fi
[ -f "$SAMPLE" ] || { echo "sample not found: $SAMPLE" >&2; exit 1; }

echo "== building native selftest =="
"$CC" -std=gnu11 -O2 -Wall -Wextra -Wno-unused-parameter \
    -I"$ROOT/native" \
    "$ROOT/native/captrim_selftest.c" \
    -L"$DIST" -Wl,-rpath,'$ORIGIN' -lcaptrim_ffmpeg \
    -o "$DIST/captrim_selftest"

echo "== pushing libraries + sample to $DEVICE_DIR =="
adbcmd shell "mkdir -p $DEVICE_DIR"
for f in "$DIST"/lib*.so "$DIST/captrim_selftest" "$SAMPLE"; do
    adbcmd push "$(wpath "$f")" "$DEVICE_DIR/$(basename "$f")" >/dev/null
done

echo "== running on device =="
adbcmd shell "chmod 755 $DEVICE_DIR/captrim_selftest"
# libx265.so is a real shared dependency of libavcodec.so (built by
# build-ffmpeg-android.sh) and lives in the same directory, so
# LD_LIBRARY_PATH=. is enough for the Android loader to resolve it.
adbcmd shell "cd $DEVICE_DIR && LD_LIBRARY_PATH=. ./captrim_selftest sample.mp4 ."
