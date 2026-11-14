#!/usr/bin/env bash
# Host-side verification, no NDK required. CI runs this on every commit.
#  1. C header declarations == Rust #[no_mangle] exports
#  2. platform header copies are identical
#  3. NAPI registrations == index.d.ts declarations == ArkTS wrapper calls
#  4. all shell scripts parse cleanly
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
HEADER="${ROOT_DIR}/crates/captrim-ffi/captrim.h"
FFI_SRC="${ROOT_DIR}/crates/captrim-ffi/src"

header_funcs="$(grep -E '^[[:space:]]*(const char \*|void|int32_t)[[:space:]]*.*captrim_[a-z_0-9]+[[:space:]]*\(' "$HEADER" \
    | grep -oE 'captrim_[a-z_0-9]+\(' | sed 's/(//' | sort -u)"
rust_funcs="$(grep -hoE 'pub extern "C" fn captrim_[a-z_0-9]+' "$FFI_SRC"/*.rs \
    | grep -oE 'captrim_[a-z_0-9]+' | sort -u)"

if [ -z "$header_funcs" ] || [ -z "$rust_funcs" ]; then
    echo "[captrim][error] failed to extract symbols" >&2
    exit 1
fi

if ! diff -u <(printf '%s\n' "$header_funcs") <(printf '%s\n' "$rust_funcs"); then
    echo "[captrim][error] captrim.h and the Rust C ABI disagree" >&2
    exit 1
fi
printf '[captrim] ABI ok: %s exported functions\n' "$(printf '%s\n' "$header_funcs" | wc -l | tr -d ' ')"

for copy in \
    "$ROOT_DIR/platforms/android/captrim/src/main/cpp/include/captrim.h" \
    "$ROOT_DIR/platforms/ohos/captrim/src/main/cpp/include/captrim.h"; do
    cmp -s "$HEADER" "$copy" || {
        echo "[captrim][error] stale header copy: $copy (run scripts/sync-headers.sh)" >&2
        exit 1
    }
done
printf '[captrim] platform header copies in sync\n'

# The HarmonyOS bridge is easy to drift: adding a function to the C ABI needs a
# registration in napi_bridge.cpp, a declaration in index.d.ts, and a caller in
# CapTrim.ets. Checking all three here turns a runtime "is not a function" into
# a CI failure.
NAPI_DIR="${ROOT_DIR}/platforms/ohos/captrim/src/main/cpp"
dts_funcs="$(grep -oE 'export const [A-Za-z_0-9]+' "${NAPI_DIR}/types/libcaptrim_napi/index.d.ts" \
    | awk '{print $3}' | sort -u)"
napi_funcs="$(grep -oE '\{"[A-Za-z_0-9]+", *nullptr, *[A-Za-z_0-9]+,' "${NAPI_DIR}/napi_bridge.cpp" \
    | sed 's/^{"//; s/",.*//' | sort -u)"
if [ -z "$dts_funcs" ] || [ -z "$napi_funcs" ]; then
    echo "[captrim][error] failed to extract NAPI surface" >&2
    exit 1
fi
if ! diff -u <(printf '%s\n' "$dts_funcs") <(printf '%s\n' "$napi_funcs"); then
    echo "[captrim][error] index.d.ts and napi_bridge.cpp disagree" >&2
    exit 1
fi

# Each declared function must actually be used by the ArkTS wrapper, and the
# wrapper must not call anything that is not declared.
ets_imports="$(sed -n '/from .libcaptrim_napi.so.;/q;p' \
    "${ROOT_DIR}/platforms/ohos/captrim/src/main/ets/CapTrim.ets" \
    | grep -oE '^[[:space:]]*[a-zA-Z]+( as [a-zA-Z]+)?,?[[:space:]]*$' \
    | sed 's/ as .*//; s/[ ,]//g' | grep -v '^import$' | grep -v '^$' | sort -u)"
if [ -n "$ets_imports" ] && ! diff -u <(printf '%s\n' "$dts_funcs") <(printf '%s\n' "$ets_imports"); then
    echo "[captrim][error] CapTrim.ets does not import the full NAPI surface" >&2
    exit 1
fi
printf '[captrim] NAPI surface ok: %s functions (d.ts == bridge == ArkTS)\n' \
    "$(printf '%s\n' "$dts_funcs" | wc -l | tr -d ' ')"

# The Android AAR cannot be compiled without an NDK, so at least guarantee that
# every JNI method in the RegisterNatives table has a matching definition and
# vice versa. A missing registration is a runtime UnsatisfiedLinkError that only
# shows up on a device.
JNI="${ROOT_DIR}/platforms/android/captrim/src/main/cpp/jni_bridge.cpp"
jni_registered="$(grep -oE '\{"native[A-Za-z0-9]+",' "$JNI" | sed 's/^{"//; s/",$//' | sort -u)"
jni_defined="$(grep -oE '^(jstring|jobject|jboolean|jint|jlong|void) native[A-Za-z0-9]+' "$JNI" \
    | awk '{print $2}' | sort -u)"
if [ -z "$jni_registered" ] || [ -z "$jni_defined" ]; then
    echo "[captrim][error] failed to extract the JNI surface" >&2
    exit 1
fi
if ! diff -u <(printf '%s\n' "$jni_registered") <(printf '%s\n' "$jni_defined"); then
    echo "[captrim][error] RegisterNatives and jni_bridge.cpp definitions disagree" >&2
    exit 1
fi
printf '[captrim] JNI surface ok: %s methods registered\n' \
    "$(printf '%s\n' "$jni_registered" | wc -l | tr -d ' ')"

for script in "$ROOT_DIR"/scripts/*.sh "$ROOT_DIR"/scripts/toolchain/*.sh; do
    bash -n "$script" || { echo "[captrim][error] syntax error in $script" >&2; exit 1; }
done
printf '[captrim] shell syntax ok\n'
