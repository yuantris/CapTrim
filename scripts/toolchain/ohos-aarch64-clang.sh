#!/bin/sh
# Linker/CC wrapper for aarch64-unknown-linux-ohos.
# Matches the flags documented in the Rust OpenHarmony platform-support guide.
set -e
: "${OHOS_NDK_HOME:?OHOS_NDK_HOME is not set}"
exec "${OHOS_NDK_HOME}/llvm/bin/clang" \
    --target=aarch64-linux-ohos \
    --sysroot="${OHOS_NDK_HOME}/sysroot" \
    -D__MUSL__ "$@"
