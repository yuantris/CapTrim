#!/bin/sh
set -e
: "${OHOS_NDK_HOME:?OHOS_NDK_HOME is not set}"
exec "${OHOS_NDK_HOME}/llvm/bin/clang++" \
    --target=arm-linux-ohos \
    --sysroot="${OHOS_NDK_HOME}/sysroot" \
    -D__MUSL__ \
    -march=armv7-a -mfloat-abi=softfp -mtune=generic-armv7-a -mthumb "$@"
