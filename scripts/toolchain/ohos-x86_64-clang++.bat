@echo off
rem Windows C++ linker wrapper for x86_64-unknown-linux-ohos (emulator).
setlocal
if "%OHOS_NDK_HOME%"=="" (
    echo [captrim][error] OHOS_NDK_HOME is not set 1>&2
    exit /b 1
)
"%OHOS_NDK_HOME%\llvm\bin\clang++.exe" ^
    --target=x86_64-linux-ohos ^
    --sysroot="%OHOS_NDK_HOME%\sysroot" ^
    -D__MUSL__ ^
    %*
exit /b %ERRORLEVEL%
