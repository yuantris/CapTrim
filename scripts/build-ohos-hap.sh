#!/usr/bin/env bash
# Build the HarmonyOS HAP that runs the device self-test page.
#
#   ./scripts/build-ohos-hap.sh                 # assembleHap (debug)
#   ./scripts/build-ohos-hap.sh install         # assembleHap, then hdc install
#   ./scripts/build-ohos-hap.sh clean           # remove build outputs
#
# Requirements:
#   * DevEco Studio installed (it ships hvigor, node, ohpm and the HarmonyOS SDK)
#   * libcaptrim.so already staged by build-rust.sh ohos. Without an FFmpeg
#     tree the stub backend is linked, which is enough for the self-test page
#     but cannot decode real media.
#
# DevEco no longer publishes hvigor as an npm package inside the project, so
# this script drives the copy bundled with the IDE instead of shipping a
# project-local hvigorw.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OHOS_DIR="$ROOT_DIR/platforms/ohos"
BUNDLE="com.captrim.demo"

log() { printf '[captrim] %s\n' "$*"; }
die() { printf '[captrim][error] %s\n' "$*" >&2; exit 1; }

# Locate DevEco Studio: explicit env var first, then the usual install roots.
find_deveco() {
    local candidates=()
    [ -n "${DEVECO_HOME:-}" ] && candidates+=("$DEVECO_HOME")
    if command -v hdc >/dev/null 2>&1; then
        # <deveco>/sdk/<api>/openharmony/toolchains/hdc
        candidates+=("$(cd "$(dirname "$(command -v hdc)")/../../../.." && pwd)")
    fi
    candidates+=(
        "/c/Program Files/Huawei/DevEco Studio"
        "/d/AppData/DevEco Studio"
        "/Applications/DevEco-Studio.app/Contents"
        "$HOME/Applications/DevEco-Studio.app/Contents"
        "$HOME/deveco"
    )
    local dir
    for dir in "${candidates[@]}"; do
        if [ -f "$dir/tools/hvigor/bin/hvigorw.js" ]; then
            printf '%s\n' "$dir"
            return 0
        fi
    done
    return 1
}

# DevEco exports DEVECO_HOME in Windows form (D:\AppData\DevEco Studio). That
# works for node, but not for $PATH lookups or any -f test in this shell, so
# normalise it to the POSIX form this script uses everywhere.
to_posix() {
    local path="$1"
    if [[ "$path" =~ ^([A-Za-z]):[\\/](.*)$ ]]; then
        local drive rest
        drive="$(printf '%s' "${BASH_REMATCH[1]}" | tr 'A-Z' 'a-z')"
        rest="${BASH_REMATCH[2]//\\//}"
        printf '/%s/%s\n' "$drive" "$rest"
    else
        printf '%s\n' "$path"
    fi
}

DEVECO="$(to_posix "$(find_deveco)")" ||
    die "DevEco Studio not found. Set DEVECO_HOME to the install root."
DEVECO="${DEVECO%/}"

export DEVECO_SDK_HOME="$DEVECO/sdk"
export PATH="$DEVECO/tools/node:$DEVECO/tools/ohpm/bin:$PATH"

command -v node >/dev/null 2>&1 || die "node not found under $DEVECO/tools/node"

ACTION="${1:-build}"
HVIGOR="$DEVECO/tools/hvigor/bin/hvigorw.js"

cd "$OHOS_DIR"

case "$ACTION" in
    clean)
        rm -rf "$OHOS_DIR/entry/build" "$OHOS_DIR/captrim/build" "$OHOS_DIR/build"
        log "removed build outputs"
        exit 0
        ;;
    build|install) ;;
    *) die "usage: build-ohos-hap.sh [build|install|clean]" ;;
esac

log "node $(node --version) | hvigor from $DEVECO"

# A real video is needed to prove that decode actually reaches the surface, and
# an app cannot read /data/local/tmp (shell does not own the app sandbox), so the
# sample rides inside the HAP as a rawfile and the app copies it out on first
# launch. The file is injected here, never committed.
RAWFILE_DIR="$OHOS_DIR/entry/src/main/resources/rawfile"
if [ -n "${CAPTRIM_SAMPLE_MP4:-}" ]; then
    [ -f "$CAPTRIM_SAMPLE_MP4" ] || die "CAPTRIM_SAMPLE_MP4 does not exist: $CAPTRIM_SAMPLE_MP4"
    mkdir -p "$RAWFILE_DIR"
    cp -f "$CAPTRIM_SAMPLE_MP4" "$RAWFILE_DIR/sample.mp4"
    log "bundled sample: $CAPTRIM_SAMPLE_MP4 -> rawfile/sample.mp4"
elif [ -f "$RAWFILE_DIR/sample.mp4" ]; then
    log "bundled sample: reusing rawfile/sample.mp4"
else
    log "no sample video (set CAPTRIM_SAMPLE_MP4); the page will run logic checks only"
fi

# The sticker has the same constraint (an app can only read its own sandbox) and
# a second one: it is a generated asset, not a source file. Draw it from the
# example that documents the ring-on-transparent-background design, so there is
# no mystery blob in the repo and the demo always matches the decoder tests.
if [ ! -f "$RAWFILE_DIR/sticker.png" ]; then
    mkdir -p "$RAWFILE_DIR"
    (cd "$ROOT_DIR" && cargo run -q -p captrim-core --example image_sticker -- "$RAWFILE_DIR/sticker.png") \
        || die "cannot generate rawfile/sticker.png (cargo run -p captrim-core --example image_sticker)"
    log "bundled sticker: generated rawfile/sticker.png"
else
    log "bundled sticker: reusing rawfile/sticker.png"
fi

# ohpm links file: dependencies (entry -> captrim HAR) into oh_modules.
if [ ! -d "$OHOS_DIR/oh_modules/captrim" ]; then
    log "resolving module dependencies"
    ( cd "$OHOS_DIR" && ohpm install --all >/dev/null )
fi

log "assembling entry HAP"
node "$HVIGOR" --mode module -p product=default -p module=entry@default assembleHap --no-daemon

OUT_DIR="$OHOS_DIR/entry/build/default/outputs/default"
# When a signingConfig is configured hvigor emits both; always prefer the signed one.
HAP="$(ls "$OUT_DIR"/*-signed.hap 2>/dev/null | head -n 1)"
[ -n "$HAP" ] || HAP="$(ls "$OUT_DIR"/*-unsigned.hap 2>/dev/null | head -n 1)"
[ -n "$HAP" ] && [ -f "$HAP" ] || die "no HAP produced"
log "HAP: $HAP"

if [ "$ACTION" = "install" ]; then
    HDC="$DEVECO/sdk/default/openharmony/toolchains/hdc"
    [ -x "$HDC" ] || HDC="hdc"
    log "installing on $( "$HDC" list targets | head -n 1)"
    # hdc wants a path it can resolve on the host, and MSYS rewrites absolute
    # ones; a relative name from the output directory sidesteps both.
    ( cd "$OUT_DIR" && MSYS_NO_PATHCONV=1 "$HDC" install -r "$(basename "$HAP")" )
    MSYS_NO_PATHCONV=1 "$HDC" shell aa start -a EntryAbility -b "$BUNDLE"
fi
