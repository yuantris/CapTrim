@echo off
rem Windows linker wrapper for x86_64-unknown-linux-ohos. See ohos-aarch64-clang.bat.
setlocal
if "%OHOS_NDK_HOME%"=="" (
    echo [captrim][error] OHOS_NDK_HOME is not set 1>&2
    exit /b 1
)
"%OHOS_NDK_HOME%\llvm\bin\clang.exe" ^
    --target=x86_64-linux-ohos ^
    --sysroot="%OHOS_NDK_HOME%\sysroot" ^
    -D__MUSL__ ^
    %*
exit /b %ERRORLEVEL%
