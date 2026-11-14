#!/bin/sh
# Linker/CC wrapper for x86_64-unknown-linux-ohos (emulator).
set -e
: "${OHOS_NDK_HOME:?OHOS_NDK_HOME is not set}"
exec "${OHOS_NDK_HOME}/llvm/bin/clang" \
    --target=x86_64-linux-ohos \
    --sysroot="${OHOS_NDK_HOME}/sysroot" \
    -D__MUSL__ "$@"
