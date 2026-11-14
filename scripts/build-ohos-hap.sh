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

DEVECO="$(find_deveco)" || die "DevEco Studio not found. Set DEVECO_HOME to the install root."

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

# ohpm links file: dependencies (entry -> captrim HAR) into oh_modules.
if [ ! -d "$OHOS_DIR/oh_modules/captrim" ]; then
    log "resolving module dependencies"
    ohpm install --all >/dev/null
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
    "$HDC" install -r "$HAP"
    "$HDC" shell aa start -a EntryAbility -b "$BUNDLE"
fi
