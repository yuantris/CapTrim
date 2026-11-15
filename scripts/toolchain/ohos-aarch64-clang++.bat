@echo off
rem Windows C++ linker wrapper for aarch64-unknown-linux-ohos.
rem Mirrors ohos-aarch64-clang.bat but invokes clang++.exe so C++ sources
rem (e.g. libx265) and cmake compiler detection work on native Windows.
setlocal
if "%OHOS_NDK_HOME%"=="" (
    echo [captrim][error] OHOS_NDK_HOME is not set 1>&2
    exit /b 1
)
"%OHOS_NDK_HOME%\llvm\bin\clang++.exe" ^
    --target=aarch64-linux-ohos ^
    --sysroot="%OHOS_NDK_HOME%\sysroot" ^
    -D__MUSL__ ^
    %*
exit /b %ERRORLEVEL%
