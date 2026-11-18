#!/usr/bin/env bash
# Build the FULL GPL FFmpeg shared libraries (incl. libx264 / libx265) + the
# CapTrim C shim for Android. Requires ANDROID_NDK_HOME (NDK r25+). Because we
# enable GPL codecs, the binaries are GPLv2-or-later, NOT LGPL.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=ffmpeg-config-common.sh
source "${ROOT_DIR}/scripts/ffmpeg-config-common.sh"

# Put our minimal pkg-config shim (scripts/pkg-config) on PATH so FFmpeg's
# configure can locate the bundled x264/x265 .pc files on this host.
export PATH="${ROOT_DIR}/scripts:$PATH"

# FFmpeg's configure writes temp test files under TMPDIR; the default Windows
# path with backslashes gets mangled, so point it at Git Bash's /tmp.
# Recipes (and the ld response-file shim) run under make's POSIX shell, whose
# MSYS root may differ from this script's: an MSYS-style /tmp path would
# resolve against the WRONG root there, so hand every consumer a Windows path.
export TMPDIR="$(cygpath -m -- "${TMPDIR:-/tmp}")"

# Windows cmd.exe caps a single BATCH invocation at 8191 characters. FFmpeg
# invokes its linker — the aarch64-*-clang.cmd batch wrapper — with the full
# object list, and libavcodec's line crosses that limit (~8.3 KB) once
# libx265.o joins it; cmd.exe then dies with "命令行太长" (command line too
# long), which is the root cause of the flaky libavcodec/libavformat link
# failures in earlier build attempts. This shim funnels the whole command
# into a response file and calls the real driver through @file, so the
# process command line stays tiny. FFmpeg uses it via --ld below.
LD_RSP="${ROOT_DIR}/build/_shims/ld-rsp.sh"
mkdir -p "$(dirname "$LD_RSP")"
cat > "$LD_RSP" <<'WRAP'
#!/bin/sh
# Route a potentially huge linker command through a response file.
# CAPTRIM_LD_CC_EXE must point at the native clang.exe and
# CAPTRIM_LD_TARGET at the triple (e.g. aarch64-linux-android26).
#
# Two Windows quirks are worked around here:
# 1. cmd.exe caps a BATCH invocation at 8191 characters and the native
#    make.exe funnels recipes through it; FFmpeg's libavcodec link line is
#    ~8.3 KB once libx265.o joins it, so the tail of the argument list
#    silently disappeared ("-pthre" was a clipped "-pthread"). We call
#    clang.exe DIRECTLY — a native executable, CreateProcess-limited
#    (~32 KB) — and inject the --target the .cmd wrapper would have added.
#    (build-ffmpeg-android.sh additionally rewrites FFmpeg's shared-library
#    link rule to hand the object list over as an @rsp file, so no recipe
#    line is long in the first place.)
# 2. When arguments travel on a real command line, the MSYS runtime
#    silently rewrites POSIX-style paths (/c/Users/..., /tmp/...) into
#    Windows form for native executables. Arguments inside a response file
#    get NO such treatment, so every path-bearing argument shape (bare
#    path, -L/-I/-R/-F<path>, --sysroot=<path>) is converted with cygpath.
# Arguments that are ALREADY @response files pass through verbatim.
: "${CAPTRIM_LD_CC_EXE:?CAPTRIM_LD_CC_EXE must point at clang.exe}"
: "${CAPTRIM_LD_TARGET:?CAPTRIM_LD_TARGET must be the target triple}"
conv() {
    case "$1" in
        /*)
            cygpath -m -- "$1" 2>/dev/null || printf '%s' "$1" ;;
        -[LIRF]/*)
            rest="${1#-[LIRF]}"
            printf '%s%s' "${1%"$rest"}" "$(cygpath -m -- "$rest" 2>/dev/null || printf '%s' "$rest")" ;;
        --sysroot=/*)
            rest="${1#--sysroot=}"
            printf '%s%s' "--sysroot=" "$(cygpath -m -- "$rest" 2>/dev/null || printf '%s' "$rest")" ;;
        *)
            printf '%s' "$1" ;;
    esac
}
rsp="$(mktemp "${TMPDIR:-/tmp}/captrim-ld-XXXXXX")"
rspw="$rsp"
if command -v cygpath >/dev/null 2>&1; then
    # clang.exe is native: hand it a Windows path, it cannot open /tmp/...
    rspw="$(cygpath -m -- "$rsp")"
fi
at_args=""
for arg in "$@"; do
    case "$arg" in
        # Already a response file: clang expands @args found INSIDE a
        # response file (nested expansion, verified with this NDK), so
        # embedding it verbatim is safe. Quoting tricks (re-parsing quote
        # characters from an expanded variable) are NOT: POSIX shells treat
        # quotes in expanded values as data, and the quotes then leak into
        # the argument and clang/lld stop recognizing the @.
        @*) printf '%s\n' "$arg" ;;
        *" "*|*'"'*|*'&'*|*'|'*) printf '"%s"\n' "$(conv "$arg")" ;;
        *) printf '%s\n' "$(conv "$arg")" ;;
    esac
done > "$rsp"
"$CAPTRIM_LD_CC_EXE" "--target=$CAPTRIM_LD_TARGET" "@$rspw"
rc=$?
rm -f "$rsp"
exit $rc
WRAP
chmod +x "$LD_RSP"

# Native make.exe cannot resolve "/bin/sh" and silently falls back to
# cmd.exe — which TRUNCATES recipe lines longer than 8191 characters. That
# cut FFmpeg's ~8.3 KB libavcodec link line mid-token ("-pthre") before it
# ever reached the response-file shim above. Give make the Git Bash sh.exe
# itself: it shares this script's MSYS runtime, so /bin/sh (used by the
# ffbuild/*.sh wrappers) and environment handoff into ld-rsp.sh both work.
# A COPIED sh.exe instead gets a standalone MSYS root with no /bin/sh, so
# make dies with "ffbuild/version.sh: cannot execute: required file not
# found ... Error 127" after configure has already succeeded. On this host
# Git lives at a space-free path, so CreateProcess splits it correctly;
# build-ffmpeg-ohos.sh uses the same approach. Use "$(command -v sh)" so the
# sh always matches the running bash's own MSYS runtime (version skew breaks
# the spawn handshake), never a copy.
export CAPTRIM_MAKE_SHELL="$(cygpath -m -- "$(command -v sh)")"
# Pass the linker as "sh.exe ld-rsp.sh" so the recipe never relies on the
# script's shebang resolution either.
export CAPTRIM_LD_CMD="$(cygpath -m -- "$LD_RSP")"
ld_invoke() { printf '%s %s' "$CAPTRIM_MAKE_SHELL" "$CAPTRIM_LD_CMD"; }

need_env ANDROID_NDK_HOME

ANDROID_API="${ANDROID_API:-26}"
ABIS="${ABIS:-arm64-v8a}"
FFMPEG_SRC="${FFMPEG_SRC:-${ROOT_DIR}/third_party/ffmpeg}"
FFMPEG_REF="${FFMPEG_REF:-n7.1}"
# Cap parallelism: the NDK clang is a wrapper script and each parallel compile
# forks an MSYS bash; too many concurrent forks exhaust the MSYS fork heap
# ("Resource temporarily unavailable"). Keep this low on Windows.
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"; [ "$JOBS" -gt 6 ] && JOBS=6

# Host compiler for FFmpeg's build-time tools (they must RUN on this Windows
# build machine). The Android NDK clang cannot do that, so we use a standalone
# llvm-mingw clang toolchain (downloaded as build/llvm-mingw.zip).
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

case "$(uname -s)" in
    Linux*) HOST_TAG="linux-x86_64" ;;
    Darwin*) HOST_TAG="darwin-x86_64" ;;
    MINGW*|MSYS*|CYGWIN*) HOST_TAG="windows-x86_64" ;;
    *) die "unsupported host: $(uname -s)" ;;
esac

TOOLCHAIN="${ANDROID_NDK_HOME}/toolchains/llvm/prebuilt/${HOST_TAG}"
[ -d "$TOOLCHAIN" ] || die "NDK toolchain not found at $TOOLCHAIN"
ensure_build_tools "${ANDROID_NDK_HOME}/prebuilt/${HOST_TAG}/bin"
build_env_preflight android

if [ ! -d "$FFMPEG_SRC" ]; then
    git_clone_robust https://git.ffmpeg.org/ffmpeg.git "$FFMPEG_SRC" "$FFMPEG_REF" \
        || die "FFmpeg clone failed after retries (set FFMPEG_SRC to a pre-populated dir)"
fi

for ABI in $ABIS; do
    case "$ABI" in
        arm64-v8a)
            ARCH="aarch64"; TRIPLE="aarch64-linux-android"; CPU="armv8-a" ;;
        armeabi-v7a)
            ARCH="arm"; TRIPLE="armv7a-linux-androideabi"; CPU="armv7-a" ;;
        x86_64)
            ARCH="x86_64"; TRIPLE="x86_64-linux-android"; CPU="x86_64" ;;
        *) die "unsupported ABI: $ABI" ;;
    esac

    # Use the .cmd wrappers (not the bash wrappers): they launch clang via
    # cmd.exe, avoiding the extra MSYS bash fork per compile (fork-heap-safe).
    CC="${TOOLCHAIN}/bin/${TRIPLE}${ANDROID_API}-clang.cmd"
    CXX="${TOOLCHAIN}/bin/${TRIPLE}${ANDROID_API}-clang++.cmd"
    [ -f "$CC" ] || die "missing compiler: $CC"
    # The ld-rsp.sh shim calls the native clang.exe directly (no cmd.exe in
    # the chain) and injects the --target the .cmd wrapper would have added.
    export CAPTRIM_LD_CC_EXE="${TOOLCHAIN}/bin/clang.exe"
    export CAPTRIM_LD_TARGET="aarch64-linux-android${ANDROID_API}"
    [ "$ARCH" != "aarch64" ] && export CAPTRIM_LD_TARGET="${ARCH}-linux-android${ANDROID_API}"

    SYSROOT="${TOOLCHAIN}/sysroot"
    # x265 is a C++ library; FFmpeg's link test for it needs the C++ runtime
    # resolvable. The NDK provides libc++/libc++abi; -ldl/-lm cover the
    # rest of x265's private deps. (Set FFMPEG_EXTRA_LIBS to override.)
    # libx265 is built as a SHARED library for Android (see build_x265 in
    # ffmpeg-config-common.sh): the static archive leaves x265_api_get_198
    # undefined in libavcodec.so on this host's toolchain, and every way to
    # force the object in breaks the configure probe or duplicate symbols.
    # Keep FFMPEG_EXTRA_LIBS free of link flags: configure uses it for its own
    # probes but does not carry it into EXTRALIBS-avcodec.
    FFMPEG_EXTRA_LIBS="${FFMPEG_EXTRA_LIBS:--lc++ -lc++abi -ldl -lm}"

    # Single shared C++ library: embed libc++ statically into libx265.so so
    # no extra libc++_shared.so needs shipping (the C API never throws).
    export X265_CMAKE_TOOLCHAIN="${ANDROID_NDK_HOME}/build/cmake/android.toolchain.cmake"
    export X265_CMAKE_EXTRA="-DANDROID_ABI=${ABI} -DANDROID_PLATFORM=android-${ANDROID_API} -DANDROID_STL=c++_static"
    resolve_external_codecs "$ABI" "$CC" "$CXX" "$ARCH" "$SYSROOT"

    BUILD="${ROOT_DIR}/build/ffmpeg-android/${ABI}"
    PREFIX="${BUILD}/prefix"
    OUT="${ROOT_DIR}/dist/android/${ABI}"
    mkdir -p "$BUILD" "$OUT"

    CFG=(
        "${FFMPEG_COMMON_CFG[@]}"
        --prefix="$PREFIX"
        --target-os=android
        --arch="$ARCH"
        --cpu="$CPU"
        --enable-cross-compile
        # Tool COMMANDS must be in Windows form: native make.exe launches
        # them via CreateProcess, which cannot split /c/, /d/... command
        # paths. ARGUMENT paths are auto-converted by the msys runtime when
        # it spawns a native process, so flags like --sysroot=/d/... may stay.
        --cc="$(cygpath -m -- "$CC")"
        --cxx="$(cygpath -m -- "$CXX")"
        --ld="$(ld_invoke)"
        --host-cc="$(cygpath -m -- "$HOSTCC")"
        --nm="$(cygpath -m -- "${TOOLCHAIN}/bin/llvm-nm.exe")"
        --ar="$(cygpath -m -- "${TOOLCHAIN}/bin/llvm-ar.exe")"
        --ranlib="$(cygpath -m -- "${TOOLCHAIN}/bin/llvm-ranlib.exe")"
        --strip="$(cygpath -m -- "${TOOLCHAIN}/bin/llvm-strip.exe")"
        --sysroot="$(cygpath -m -- "${TOOLCHAIN}/sysroot")"
    )
    CFG+=(
        ${FFMPEG_EXTRA_ENABLE:-}
        --extra-cflags="${EXT_CFLAGS:-}"
        --extra-ldflags="${EXT_LDFLAGS:-}"
        --extra-libs="${FFMPEG_EXTRA_LIBS}"
    )

    [ -x "$HOSTCC" ] || die "host compiler not found at $HOSTCC; FFmpeg needs it to build+run build-time tools. Place llvm-mingw zip at build/llvm-mingw.zip (auto-extracted) or set LLVM_MINGW_HOME."

    log "configure FFmpeg for ${ABI} (API ${ANDROID_API})"
    ( cd "$BUILD" && "$FFMPEG_SRC/configure" "${CFG[@]}" )
    fix_ffmpeg_generated_paths "$BUILD"
    patch_ffmpeg_link_rsp
    make -C "$BUILD" -j"$JOBS" SHELL="$CAPTRIM_MAKE_SHELL"
    make -C "$BUILD" install SHELL="$CAPTRIM_MAKE_SHELL"

    # Link the Android hardware first/last mile into the same shim. Without
    # this file the weak factories resolve to NULL and the pipeline runs
    # fully in software (the pre-AMediaCodec baseline).
    build_c_shim "$OUT" "$CC" \
        "${ROOT_DIR}/native/captrim_android_hw.c" \
        -lmediandk -llog

    # Guard: libavcodec.so must not carry an x265 symbol it cannot resolve at
    # runtime. With the shared libx265.so the versioned symbol is expected to
    # stay UND in the dynamic table (the loader resolves it from the
    # dependency), so only a missing NEEDED entry — with an x265 reference
    # still present — means the link went wrong.
    if "${TOOLCHAIN}/bin/llvm-readelf.exe" -d "$OUT/libavcodec.so" 2>/dev/null | grep -q 'NEEDED.*libx265.so'; then
        log "libavcodec.so links against shared libx265.so"
    elif "${TOOLCHAIN}/bin/llvm-nm.exe" -D "$OUT/libavcodec.so" 2>/dev/null | grep -q 'x265_api_get_198'; then
        die "libavcodec.so references x265 but has no libx265.so dependency"
    else
        log "libavcodec.so carries no x265 dependency (x265 disabled?)"
    fi
    # An early experiment shipped a 5 KB placeholder libx265.so plus LD_PRELOAD
    # so the loader saw a fake x265_api_get_198. Reject any non-real build:
    # a genuine x265 shared library is several MB on arm64.
    x265_size="$(stat -c %s "$OUT/libx265.so" 2>/dev/null || echo 0)"
    [ "$x265_size" -gt 1000000 ] \
        || die "dist libx265.so is not a real x265 build (${x265_size} bytes)"
    log "dist libx265.so is a real ${x265_size}-byte shared library"
    write_build_receipt "$OUT" "$FFMPEG_SRC/configure" "${CFG[@]}"
    log "Android ${ABI} artifacts -> ${OUT}"
done
