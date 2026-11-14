#!/usr/bin/env bash
# The C ABI header is authored once. Copy it into both platform Native
# projects so an AAR/HAR can be built standalone; check-native-abi.sh fails
# CI if any copy drifts.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
INCLUDE_ANDROID="${ROOT_DIR}/platforms/android/captrim/src/main/cpp/include"
INCLUDE_OHOS="${ROOT_DIR}/platforms/ohos/captrim/src/main/cpp/include"

for target in "$INCLUDE_ANDROID" "$INCLUDE_OHOS"; do
    mkdir -p "$target"
    cp "${ROOT_DIR}/crates/captrim-ffi/captrim.h" "$target/captrim.h"
    cp "${ROOT_DIR}/native/captrim_ffmpeg.h" "$target/captrim_ffmpeg.h"
done

printf '[captrim] headers synced to Android and HarmonyOS Native projects\n'
