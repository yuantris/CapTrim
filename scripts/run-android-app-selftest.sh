#!/usr/bin/env bash
# Build the Kotlin self-check app (:app), install it on a connected device,
# push the sample clip into its sandbox, run it and report the results.
# Requires: ANDROID_HOME (or ANDROID_NDK_HOME) + adb + a cached Gradle 8.13.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SAMPLE="$ROOT/platforms/ohos/entry/src/main/resources/rawfile/sample.mp4"
PKG="io.captrim.selfcheck"
LOG_TAG="CapTrimSelfCheck"
[ -f "$SAMPLE" ] || { echo "sample not found: $SAMPLE" >&2; exit 1; }

# Git-Bash rewrites /data/local/tmp etc. into Windows paths; only the adb
# calls need that disabled.
adbcmd() { MSYS_NO_PATHCONV=1 command adb "$@"; }
wpath() {
    if command -v cygpath >/dev/null 2>&1; then cygpath -w "$1"; else printf '%s' "$1"; fi
}

adbcmd get-state >/dev/null 2>&1 || { echo "no Android device connected" >&2; exit 1; }

# The repo ships no gradle wrapper yet, so use a locally cached distribution.
GRADLE_BIN=""
for c in "$HOME/.gradle/wrapper/dists"/gradle-8.13-bin/*/gradle-8.13/bin/gradle; do
    [ -f "$c" ] && GRADLE_BIN="$c" && break
done
[ -n "$GRADLE_BIN" ] || { echo "no cached gradle 8.13 under ~/.gradle/wrapper/dists" >&2; exit 1; }

# Locate the Android SDK: prefer explicit vars, else derive from the NDK path.
SDK_DIR="${ANDROID_SDK_ROOT:-${ANDROID_HOME:-}}"
if [ -z "$SDK_DIR" ] && [ -n "${ANDROID_NDK_HOME:-}" ]; then
    # ANDROID_NDK_HOME is set in Windows form (D:\Android\Sdk\ndk\27.x) just as
    # often as in MSYS form; strip the ndk suffix only after normalising, or
    # the pattern never matches and ANDROID_HOME ends up pointing at the NDK,
    # which makes Gradle look for licences under the NDK instead of the SDK.
    ndk_unix="$(cygpath -u -- "$ANDROID_NDK_HOME" 2>/dev/null || printf '%s' "$ANDROID_NDK_HOME")"
    SDK_DIR="${ndk_unix%/ndk/*}"
fi
[ -n "$SDK_DIR" ] || SDK_DIR="/d/Android/Sdk"
export ANDROID_HOME="$(cygpath -m -- "$SDK_DIR" 2>/dev/null || printf '%s' "$SDK_DIR")"

echo "== building app-debug.apk (first run downloads AGP) =="
BUILD_LOG="$ROOT/platforms/android/build-app.log"
if ! (cd "$ROOT/platforms/android" && "$GRADLE_BIN" --console=plain :app:assembleDebug) \
        > "$BUILD_LOG" 2>&1; then
    tail -40 "$BUILD_LOG" >&2
    echo "gradle build failed; full log: $BUILD_LOG" >&2
    exit 1
fi
APK="$ROOT/platforms/android/app/build/outputs/apk/debug/app-debug.apk"
[ -f "$APK" ] || { echo "apk not produced: $APK" >&2; exit 1; }

echo "== installing $APK =="
adbcmd install -r "$(wpath "$APK")" >/dev/null

echo "== pushing sample into app sandbox =="
# adb push (binary protocol) to /data/local/tmp, then pipe on-device into
# run-as. Two earlier approaches are broken on Windows:
#   - `adb shell run-as ... sh -c 'cat > file' < sample`: the device shell
#     re-parses the joined command, so an unquoted `>` fires in the wrong
#     directory, and stdin forwarding through adb shell truncates the 21 MB
#     stream to a few hundred bytes;
#   - quoting the whole remote command fixes the redirect but not the
#     truncation.
push_sample() {
    adbcmd shell mkdir -p /data/local/tmp/captrim-selftest
    adbcmd push "$(wpath "$SAMPLE")" /data/local/tmp/captrim-selftest/sample.mp4 >/dev/null
    # mkdir -p first: files/ is created lazily by the app's first getFilesDir().
    adbcmd shell "cat /data/local/tmp/captrim-selftest/sample.mp4 | run-as $PKG sh -c 'mkdir -p files && cat > files/a.mp4'"
}
if ! push_sample 2>/dev/null; then
    # run-as needs the app data dir, which only exists after the first launch.
    adbcmd shell am start -n "$PKG/.MainActivity" >/dev/null
    sleep 3
    adbcmd shell am force-stop "$PKG"
    push_sample
fi

echo "== running selfcheck =="
adbcmd logcat -c
adbcmd shell am start -n "$PKG/.MainActivity" >/dev/null

OUT=""
for _ in $(seq 1 90); do
    OUT="$(adbcmd logcat -d -s "$LOG_TAG" 2>/dev/null || true)"
    printf '%s' "$OUT" | grep -q "== selfcheck" && break
    sleep 2
done

printf '%s\n' "$OUT"
SUMMARY="$(printf '%s\n' "$OUT" | grep -o "== selfcheck [0-9]*/[0-9]* passed ==" | tail -1 || true)"
if [ -z "$SUMMARY" ]; then
    echo "selfcheck did not finish within the timeout" >&2
    exit 1
fi
echo "$SUMMARY"

PASSED="${SUMMARY#== selfcheck }"; PASSED="${PASSED%%/*}"
TOTAL="${SUMMARY#*/}"; TOTAL="${TOTAL%% *}"
[ "$PASSED" = "$TOTAL" ] || exit 1
