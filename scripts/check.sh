#!/usr/bin/env bash
# Every gate CI enforces, in the order CI runs them.
#
# This exists because a red run is expensive and easy to earn. Commit b2d3158
# failed at `cargo fmt --all --check` and GitHub skipped the four gates behind
# it, so the tests, clippy and ABI results that had been checked locally were
# never reported -- the push looked verified and was not. The fix is not more
# discipline, it is one command that cannot be run partially.
#
# What it does NOT cover (no DevEco NDK or device here): the NAPI/HAR/HAP build
# and anything that needs a real decoder. CI does not cover those either. The
# Android FFmpeg check is a type-check, not a link: dist/ is generated and not
# checked in, so only the on-device native self-test proves the real link.
#
# Usage:
#   bash scripts/check.sh          # everything
#   bash scripts/check.sh --fast   # skip the HarmonyOS cross-check
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

FAST=0
for arg in "$@"; do
    case "$arg" in
        --fast) FAST=1 ;;
        *) echo "[captrim][error] unknown argument: $arg" >&2; exit 2 ;;
    esac
done

step() {
    printf '\n[captrim] == %s ==\n' "$1"
}

step "fmt (cargo fmt --all --check)"
# Not `cargo fmt`: a check must not repair what it is meant to report.
cargo fmt --all --check

step "clippy"
# The feature spelling matters. CI enables the FFmpeg backend through the
# dependency (`captrim-ffi/ffmpeg`), which is not the same set of cfg flags as
# `--features ffmpeg` on the workspace root.
cargo clippy --workspace --all-targets --features captrim-ffi/ffmpeg -- -D warnings

step "test"
cargo test --workspace

step "ABI / NAPI contract"
bash scripts/check-native-abi.sh

if [ "$FAST" -eq 0 ]; then
    step "cross-check core for HarmonyOS arm64"
    # Type-checks against the HarmonyOS std without a linker, so no NDK needed.
    if rustup target list --installed | grep -qx 'aarch64-unknown-linux-ohos'; then
        cargo check -p captrim-core --target aarch64-unknown-linux-ohos
    else
        echo "[captrim] skipped: target aarch64-unknown-linux-ohos not installed"
        echo "[captrim]   rustup target add aarch64-unknown-linux-ohos"
    fi
fi

step "check FFmpeg backend for Android arm64"
# Same command as the CI job. Needs the Rust target but neither an NDK nor the
# generated FFmpeg libraries, because cargo check does not invoke the linker.
if rustup target list --installed | grep -qx 'aarch64-linux-android'; then
    cargo check -p captrim-ffi --features ffmpeg --target aarch64-linux-android
else
    echo "[captrim] skipped: target aarch64-linux-android not installed"
    echo "[captrim]   rustup target add aarch64-linux-android"
fi

printf '\n[captrim] all gates passed\n'
