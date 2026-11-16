#!/usr/bin/env bash
# Build the FULL GPL FFmpeg shared libraries (incl. libx264 / libx265) + the
# CapTrim C shim for HarmonyOS. Requires OHOS_NDK_HOME = <DevEco SDK>/default/openharmony/native.
#
# HarmonyOS requires every prebuilt .so to be produced by the HarmonyOS NDK
# toolchain and to carry a SONAME. The wrappers in scripts/toolchain/ supply
# --target=<arch>-linux-ohos, --sysroot and -D__MUSL__ exactly as documented
# by the HarmonyOS NDK build guide.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=ffmpeg-config-common.sh
source "${ROOT_DIR}/scripts/ffmpeg-config-common.sh"

# Put our minimal pkg-config shim (scripts/pkg-config) on PATH so FFmpeg's
# configure can locate the bundled x264/x265 .pc files on this host.
export PATH="${ROOT_DIR}/scripts:$PATH"

# FFmpeg's configure writes temp test files under TMPDIR; the default Windows
# path with backslashes gets mangled. The linker shim runs under Git Bash's sh
# but calls native clang.exe, so hand both a Windows-style path.
export TMPDIR="$(cygpath -m -- "${TMPDIR:-/tmp}")"

need_env OHOS_NDK_HOME
export OHOS_NDK_HOME
# HarmonyOS NDK ships no `make`; reuse the Android NDK's make (or system make).
ensure_build_tools "${ANDROID_NDK_HOME:+${ANDROID_NDK_HOME}/prebuilt/windows-x86_64/bin}"

ABIS="${ABIS:-arm64-v8a}"
FFMPEG_SRC="${FFMPEG_SRC:-${ROOT_DIR}/third_party/ffmpeg}"
FFMPEG_REF="${FFMPEG_REF:-n7.1}"
# Cap parallelism: the OHOS clang wrapper is a shell script; each parallel compile
# forks an MSYS bash and too many exhaust the MSYS fork heap on Windows.
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"; [ "$JOBS" -gt 6 ] && JOBS=6
build_env_preflight ohos

# Host compiler for FFmpeg's build-time tools (they must RUN on this Windows
# build machine; the OHOS NDK clang cross-compiles and cannot). Use a standalone
# llvm-mingw clang toolchain, same as the Android build.
LLVM_MINGW_ZIP="${ROOT_DIR}/build/llvm-mingw.zip"
LLVM_MINGW_HOME="${LLVM_MINGW_HOME:-${ROOT_DIR}/build/llvm-mingw}"
if [ -f "$LLVM_MINGW_ZIP" ] && [ ! -x "$LLVM_MINGW_HOME/bin/clang.exe" ]; then
    log "extracting llvm-mingw host toolchain from $LLVM_MINGW_ZIP"
    rm -rf "${ROOT_DIR}/build/llvm-mingw-tmp"
    mkdir -p "${ROOT_DIR}/build/llvm-mingw-tmp"
    unzip -q "$LLVM_MINGW_ZIP" -d "${ROOT_DIR}/build/llvm-mingw-tmp" \
        || die "failed to unzip llvm-mingw (is 'unzip' available?)"
    _lm=$(ls -d "${ROOT_DIR}/build/llvm-mingw-tmp"/llvm-mingw-* 2>/dev/null | head -1)
    [ -n "$_lm" ] && mv "$_lm" "$LLVM_MINGW_HOME"
    rm -rf "${ROOT_DIR}/build/llvm-mingw-tmp"
fi
HOSTCC="${LLVM_MINGW_HOME}/bin/clang.exe"
[ -x "$HOSTCC" ] || die "host compiler not found at $HOSTCC; FFmpeg needs it to build+run build-time tools."

LLVM="${OHOS_NDK_HOME}/llvm/bin"
[ -x "${LLVM}/clang" ] || die "HarmonyOS NDK clang not found at ${LLVM}/clang"
[ -d "${OHOS_NDK_HOME}/sysroot" ] || die "HarmonyOS NDK sysroot missing"

# The inline libavcodec link line is ~8207 chars, just over the Windows 8191
# limit (the tail "-lm" was clipped to "-l"). Route it through a response file
# and call the OHOS clang.exe directly, as the Android build already does.
# Use Git Bash's own sh.exe as make's SHELL: a copied sh.exe loses the MSYS
# root and cannot resolve /bin/sh for FFmpeg's version.sh wrapper scripts.
export CAPTRIM_MAKE_SHELL="$(cygpath -m -- "$(command -v sh)")"
LD_RSP="${ROOT_DIR}/build/_shims/ld-rsp-ohos.sh"
mkdir -p "$(dirname "$LD_RSP")"
cat > "$LD_RSP" <<'WRAP'
#!/bin/sh
# Route a potentially huge linker command through a response file, then call
# the OHOS native clang.exe directly. CAPTRIM_LD_SYSROOT is already a Windows
# path; response-file arguments receive no MSYS conversion, so convert here.
: "${CAPTRIM_LD_CC_EXE:?CAPTRIM_LD_CC_EXE must point at clang.exe}"
: "${CAPTRIM_LD_TARGET:?CAPTRIM_LD_TARGET must be the OHOS target triple}"
: "${CAPTRIM_LD_SYSROOT:?CAPTRIM_LD_SYSROOT must be a Windows-form sysroot}"
conv() {
    case "$1" in
        /*) cygpath -m -- "$1" 2>/dev/null || printf '%s' "$1" ;;
        -[LIRF]/*)
            rest="${1#-[LIRF]}"
            printf '%s%s' "${1%"$rest"}" "$(cygpath -m -- "$rest" 2>/dev/null || printf '%s' "$rest")" ;;
        --sysroot=/*)
            rest="${1#--sysroot=}"
            printf '%s%s' "--sysroot=" "$(cygpath -m -- "$rest" 2>/dev/null || printf '%s' "$rest")" ;;
        *) printf '%s' "$1" ;;
    esac
}
rsp="$(mktemp "${TMPDIR:-/tmp}/captrim-ld-ohos-XXXXXX")"
rspw="$rsp"
if command -v cygpath >/dev/null 2>&1; then
    rspw="$(cygpath -m -- "$rsp")"
fi
for arg in "$@"; do
    case "$arg" in
        @*) printf '%s\n' "$arg" ;;
        *" "*|*'"'*|*'&'*|*'|'*) printf '"%s"\n' "$(conv "$arg")" ;;
        *) printf '%s\n' "$(conv "$arg")" ;;
    esac
done > "$rsp"
"$CAPTRIM_LD_CC_EXE" "--target=$CAPTRIM_LD_TARGET" "--sysroot=$CAPTRIM_LD_SYSROOT" \
    -D__MUSL__ "@$rspw"
rc=$?
rm -f "$rsp"
exit $rc
WRAP
chmod +x "$LD_RSP"
export CAPTRIM_LD_CMD="$(cygpath -m -- "$LD_RSP")"
ld_invoke() { printf '%s %s' "$CAPTRIM_MAKE_SHELL" "$CAPTRIM_LD_CMD"; }

if [ ! -d "$FFMPEG_SRC" ]; then
    log "cloning FFmpeg ${FFMPEG_REF} into ${FFMPEG_SRC}"
    mkdir -p "$(dirname "$FFMPEG_SRC")"
    git clone --depth 1 --branch "$FFMPEG_REF" https://git.ffmpeg.org/ffmpeg.git "$FFMPEG_SRC"
fi

for ABI in $ABIS; do
    case "$ABI" in
        arm64-v8a)
            ARCH="aarch64"; WRAPPER="ohos-aarch64-clang.sh"; OHOS_TARGET="aarch64-linux-ohos" ;;
        armeabi-v7a)
            ARCH="arm"; WRAPPER="ohos-armv7-clang.sh"; OHOS_TARGET="arm-linux-ohos" ;;
        x86_64)
            ARCH="x86_64"; WRAPPER="ohos-x86_64-clang.sh"; OHOS_TARGET="x86_64-linux-ohos" ;;
        *) die "unsupported ABI: $ABI" ;;
    esac

    # x264 and FFmpeg are built by Git Bash's make/sh, which execs a POSIX .sh
    # shim directly. x265 is built by cmake, a native Windows binary that can only
    # launch .bat/.cmd/.exe — so build_x265 converts the .sh wrapper to .bat for
    # the cmake step internally. (Using .bat for x264/FFmpeg fails because sh
    # tries to interpret the .bat as a shell script and the NDK path's space
    # truncates it.)
    CC="${ROOT_DIR}/scripts/toolchain/${WRAPPER}"
    CXX="${ROOT_DIR}/scripts/toolchain/${WRAPPER%.sh}++.sh"
    [ -f "$CC" ] || die "missing compiler wrapper: $CC"
    [ -f "$CXX" ] || die "missing C++ compiler wrapper: $CXX"

    SYSROOT="${OHOS_NDK_HOME}/sysroot"
    export CAPTRIM_LD_CC_EXE="$(cygpath -m -- "${LLVM}/clang.exe")"
    export CAPTRIM_LD_TARGET="$OHOS_TARGET"
    export CAPTRIM_LD_SYSROOT="$(cygpath -m -- "$SYSROOT")"
    resolve_external_codecs "$ABI" "$CC" "$CXX" "$ARCH" "$SYSROOT"

    # x265 is a C++ static archive; FFmpeg's link test for it needs the C++
    # runtime resolvable. The OHOS NDK provides libc++/libc++abi (statically
    # linked from its llvm/lib). (Set FFMPEG_EXTRA_LIBS to override.)
    FFMPEG_EXTRA_LIBS="${FFMPEG_EXTRA_LIBS:--lc++ -lc++abi -lm}"

    BUILD="${ROOT_DIR}/build/ffmpeg-ohos/${ABI}"
    PREFIX="${BUILD}/prefix"
    OUT="${ROOT_DIR}/dist/ohos/${ABI}"
    mkdir -p "$BUILD" "$OUT"

    # FFmpeg has no dedicated OHOS target; --target-os=linux plus the NDK
    # clang wrapper (which injects --target/--sysroot/-D__MUSL__) is the
    # documented cross-compilation approach for HarmonyOS third-party libs.
    CFG=(
        "${FFMPEG_COMMON_CFG[@]}"
        --prefix="$PREFIX"
        --target-os=linux
        --arch="$ARCH"
        --enable-cross-compile
        # The OHOS NDK path contains a space ("DevEco Studio"); FFmpeg's install
        # rule runs $(STRIP) unquoted, which splits on that space. Stripping is
        # cosmetic (size only), so disable it to keep the toolchain path safe.
        --disable-stripping
        --cc="$CC"
        --cxx="$CXX"
        --ld="$(ld_invoke)"
        --host-cc="$HOSTCC"
        --nm="${LLVM}/llvm-nm.exe"
        --ar="${LLVM}/llvm-ar.exe"
        --ranlib="${LLVM}/llvm-ranlib.exe"
        --strip="${LLVM}/llvm-strip.exe"
        --extra-cflags="-D__MUSL__"
        --extra-ldflags="-D__MUSL__"
    )
    CFG+=(
        ${FFMPEG_EXTRA_ENABLE:-}
        --extra-cflags="-D__MUSL__ ${EXT_CFLAGS:-}"
        --extra-ldflags="-D__MUSL__ ${EXT_LDFLAGS:-}"
        --extra-libs="${FFMPEG_EXTRA_LIBS}"
    )

    # Both platforms need @rsp: the inline libavcodec link line is ~8.2 KB,
    # just over the Windows 8191-char limit. The OHOS ld shim expands the rsp
    # and calls clang.exe directly, so no wrapper has to resolve a relative
    # @file. Drop stale configure output before switching the link rule.
    patch_ffmpeg_link_rsp
    rm -f "$BUILD/ffbuild/config.mak"

    log "configure FFmpeg for HarmonyOS ${ABI}"
    ( cd "$BUILD" && "$FFMPEG_SRC/configure" "${CFG[@]}" )
    fix_ffmpeg_generated_paths "$BUILD"
    make -C "$BUILD" -j"$JOBS" SHELL="$CAPTRIM_MAKE_SHELL"
    make -C "$BUILD" install SHELL="$CAPTRIM_MAKE_SHELL"

    # The platform-neutral three-stage orchestrator (captrim_pipeline.c) is
    # added by build_c_shim for BOTH platforms. OHOS additionally links the
    # hardware first/last mile: AVCodec Kit Buffer mode + OH_AVMuxer + hilog.
    # Android keeps the same pipeline with weak factories resolving to NULL,
    # i.e. it automatically runs the software fallback until AMediaCodec is
    # wired - this is the intended "same ABI, platform swap" design.
    build_c_shim "$OUT" "$CC" \
        "${ROOT_DIR}/native/captrim_ohos_hw.c" \
        -DCAPTRIM_OHOS_HW=1 \
        -lnative_media_codecbase -lnative_media_core \
        -lnative_media_vdec -lnative_media_venc \
        -lnative_media_avsource -lnative_media_avdemuxer \
        -lnative_media_avmuxer -lhilog_ndk.z
    write_build_receipt "$OUT" "$FFMPEG_SRC/configure" "${CFG[@]}"

    # libavcodec links against the OHOS C++ runtime libc++_shared.so. That
    # runtime is NOT present on the device system image for third-party apps, so
    # it must ship inside the HAP or the NAPI module fails to dlopen at runtime
    # ("Error loading shared library libc++_shared.so: needed by libavcodec.so").
    # Bundle the NDK build matching this ABI next to the FFmpeg libs; the
    # build-rust.sh stage_libs step then copies it into the HAP automatically.
    case "$ABI" in
        arm64-v8a) _cxx_rt="${OHOS_NDK_HOME}/llvm/lib/aarch64-linux-ohos/libc++_shared.so" ;;
        *) _cxx_rt="$(find "${OHOS_NDK_HOME}/llvm/lib" -path "*${ARCH}-linux-ohos*" -name libc++_shared.so 2>/dev/null | head -1)" ;;
    esac
    if [ -n "${_cxx_rt:-}" ] && [ -f "$_cxx_rt" ]; then
        cp -f "$_cxx_rt" "${OUT}/libc++_shared.so"
        log "bundled C++ runtime -> ${OUT}/libc++_shared.so"
    else
        log "WARN: libc++_shared.so not found for ${ABI}; the HAP will fail to load FFmpeg at runtime"
    fi

    # Verify SONAME, per HarmonyOS "prebuilt library" constraints.
    if [ -x "${LLVM}/llvm-readelf" ]; then
        "${LLVM}/llvm-readelf" -d "${OUT}/libcaptrim_ffmpeg.so" | grep SONAME \
            || die "libcaptrim_ffmpeg.so is missing SONAME"
    fi
    log "HarmonyOS ${ABI} artifacts -> ${OUT}"
done
