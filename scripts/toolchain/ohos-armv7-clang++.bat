@echo off
rem Windows C++ linker wrapper for armv7-unknown-linux-ohos (armeabi-v7a).
setlocal
if "%OHOS_NDK_HOME%"=="" (
    echo [captrim][error] OHOS_NDK_HOME is not set 1>&2
    exit /b 1
)
"%OHOS_NDK_HOME%\llvm\bin\clang++.exe" ^
    --target=arm-linux-ohos ^
    --sysroot="%OHOS_NDK_HOME%\sysroot" ^
    -D__MUSL__ ^
    -march=armv7-a -mfloat-abi=softfp -mtune=generic-armv7-a -mthumb ^
    %*
exit /b %ERRORLEVEL%
