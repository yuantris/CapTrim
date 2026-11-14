@echo off
rem Windows linker wrapper for aarch64-unknown-linux-ohos.
rem
rem Cargo cannot exec the POSIX `ohos-aarch64-clang.sh` shim on Windows, and the
rem DevEco NDK ships only `clang.exe` plus `/bin/sh` shims. This wrapper applies
rem the same three flags the shim does (target, sysroot, __MUSL__) so the same
rem Rust source links identically on Windows, Linux and macOS.
rem
rem Keep this file at a path without spaces: Cargo passes the linker command
rem through a whitespace-split, while OHOS_NDK_HOME itself may contain spaces.
setlocal
if "%OHOS_NDK_HOME%"=="" (
    echo [captrim][error] OHOS_NDK_HOME is not set 1>&2
    exit /b 1
)
"%OHOS_NDK_HOME%\llvm\bin\clang.exe" ^
    --target=aarch64-linux-ohos ^
    --sysroot="%OHOS_NDK_HOME%\sysroot" ^
    -D__MUSL__ ^
    %*
exit /b %ERRORLEVEL%
