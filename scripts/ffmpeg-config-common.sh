#!/usr/bin/env bash
# Shared, FULL-FEATURED FFmpeg configuration for CapTrim.
#
# LICENSE: This build enables GPL codecs (libx264 / libx265), so the resulting
# binaries are GPLv2-or-later, NOT LGPL. CapTrim is a LEARNING project that
# prioritizes full functionality over license-minimalism; if you ever distribute
# the binaries you must comply with GPL: provide the corresponding FFmpeg / x264
# / x265 source AND the exact configure lines. See write_build_receipt ->
# dist/<platform>/<abi>/FFMPEG_BUILD.txt.
#
# Components kept: libavformat, libavcodec, libavfilter, libswscale,
# libswresample, libavutil, plus external libx264 / libx265 (built from source
# by this script). We start from --disable-everything and opt IN a broad, common
# set so the build stays tractable; remove --disable-everything for an even
# fuller build.

set -euo pipefail

log() { printf '[captrim] %s\n' "$*"; }
die() { printf '[captrim][error] %s\n' "$*" >&2; exit 1; }

need_env() {
    local name="$1"
    [ -n "${!name:-}" ] || die "$name is not set. See README.md -> Building native libraries."
}

# Robust clone: bump http buffer, retry up to 3x, and fall back to a GitHub
# mirror for FFmpeg if git.ffmpeg.org drops the connection (common with big repos).
git_clone_robust() {
    local url="$1" dest="$2" ref="${3:-}"
    [ -d "$dest" ] && { log "skip clone, exists: $dest"; return 0; }
    mkdir -p "$(dirname "$dest")"
    git config --global http.postBuffer 1048576000 2>/dev/null || true
    git config --global http.lowSpeedLimit 1000 2>/dev/null || true
    git config --global http.lowSpeedTime 60 2>/dev/null || true
    local i rc
    for i in 1 2 3; do
        log "clone attempt $i: $url @ ${ref:-HEAD}"
        if git clone --depth 1 ${ref:+--branch "$ref"} "$url" "$dest" 2>&1; then
            return 0
        fi
        log "clone attempt $i failed; cleaning and retrying"
        rm -rf "$dest"
        sleep 2
    done
    if [[ "$url" == *git.ffmpeg.org* ]]; then
        log "falling back to GitHub FFmpeg mirror"
        rm -rf "$dest"
        if git clone --depth 1 ${ref:+--branch "$ref"} https://github.com/FFmpeg/FFmpeg.git "$dest" 2>&1; then
            return 0
        fi
    fi
    return 1
}

# Optional external codec sources (auto-cloned if missing)
X264_SRC="${X264_SRC:-${ROOT_DIR}/third_party/x264}"
X265_SRC="${X265_SRC:-${ROOT_DIR}/third_party/x265}"
X264_REF="${X264_REF:-master}"
X265_REF="${X265_REF:-master}"
BUILD_X264="${BUILD_X264:-1}"
BUILD_X265="${BUILD_X265:-1}"

# Filled by resolve_external_codecs()
X264_PREFIX=""
X265_PREFIX=""
EXT_CFLAGS=""
EXT_LDFLAGS=""
FFMPEG_EXTRA_ENABLE=""

# Build static x264 for one ABI.
# Args: abi cc cxx arch sysroot prefix
build_x264() {
    [ "$BUILD_X264" = "1" ] || return 0
    local abi="$1" cc="$2" cxx="$3" arch="$4" sysroot="$5" prefix="$6"
    local src="$X264_SRC"
    # Reuse a completed install. The prefix lives INSIDE build/x264-<abi>/,
    # so an unconditional `rm -rf "$b"` used to wipe the very artifact this
    # check looks for and forced a full rebuild on every retry.
    if [ -f "$prefix/lib/libx264.a" ] && [ -f "$prefix/lib/pkgconfig/x264.pc" ]; then
        log "x264 already built -> $prefix"
        return 0
    fi
    if [ ! -d "$src" ]; then
        git_clone_robust https://code.videolan.org/videolan/x264.git "$src" "$X264_REF" \
            || log "x264 clone FAILED; continuing without libx264"
    fi
    mkdir -p "$prefix"
    # The NDK clang lives at <dir>aarch64-linux-androidNN-clang; the binutils
    # counterparts (llvm-ar/llvm-strip/llvm-ranlib/llvm-strings) sit in the
    # same bin/ directory, all with the .exe suffix. For HarmonyOS the CC is a
    # wrapper script under scripts/toolchain/, so resolve the binutils from the
    # OHOS NDK's own llvm/bin instead.
    local llvm_dir
    if [ -n "${OHOS_NDK_HOME:-}" ] && [[ "$cc" == *"ohos"* ]]; then
        llvm_dir="${OHOS_NDK_HOME}/llvm/bin/"
    else
        llvm_dir="$(dirname "$cc")/"
    fi
    local ar="${llvm_dir}llvm-ar.exe" strip="${llvm_dir}llvm-strip.exe" ranlib="${llvm_dir}llvm-ranlib.exe"
    # x264's ./configure does NOT accept CC=... as a flag; pass the toolchain
    # via environment variables. It also needs `strings` to inspect the
    # endian-test object; NDK ships it as llvm-strings.exe, so expose a shim.
    local shimdir="${ROOT_DIR}/build/_shims"
    mkdir -p "$shimdir"
    cat > "$shimdir/strings" <<SHIM
#!/bin/sh
exec "${llvm_dir}llvm-strings.exe" "\$@"
SHIM
    chmod +x "$shimdir/strings"
    export PATH="$shimdir:$PATH"
    # x264 builds in-tree in the shared source dir, which is reused across
    # targets (Android then HarmonyOS). A leftover config.h / objects from the
    # previous target poison cc_check ("No working C compiler found"), so purge
    # the generated build artifacts before reconfiguring.
    ( cd "$src" && rm -f config.h x264_config.h config.mk libx264.a 2>/dev/null; \
      find . -name '*.o' -delete 2>/dev/null; find . -name '*.d' -delete 2>/dev/null ) || true
    # x264's configure re-splits --extra-cflags on whitespace, so a sysroot path
    # containing a space (e.g. the HarmonyOS NDK's "DevEco Studio") would be
    # torn apart. The CC wrapper already injects --sysroot, so only pass it here
    # when the path is space-free (covers Android, whose NDK path has no space).
    local x264_cflags="-fPIC"
    case "$sysroot" in *" "*) ;; *) x264_cflags="--sysroot=$sysroot -fPIC" ;; esac
    log "building x264 for ${abi}"
    if ( cd "$src" && \
         CC="$cc" AR="$ar" STRIP="$strip" RANLIB="$ranlib" \
         ./configure --prefix="$prefix" \
             --host="${arch}-linux" \
             --enable-static --enable-pic --disable-cli --disable-opencl \
             --extra-cflags="$x264_cflags" && \
         make -j"$JOBS" && make install ); then
        log "x264 built -> $prefix"
    else
        log "x264 build FAILED; continuing without libx264"
    fi
}

# Stage x265 artifacts into its install prefix.
# x265's CMakeLists wraps BOTH the shared-library install rule and the x265.pc
# install rule in `if(X265_LATEST_TAG)` (derived from `git describe`).
# git_clone_robust makes a --depth 1 clone with no tags, and `git fetch --tags`
# does not reliably add any on this host either, so `make install` silently
# installs headers + the static archive but NO libx265.so and NO x265.pc.
# FFmpeg configure then drops --enable-libx265 entirely. Stage the artifacts
# ourselves so the build works with a tag-less shallow clone.
# Args: build_dir prefix shared src_dir
install_x265_artifacts() {
    local b="$1" prefix="$2" shared="$3" src="$4"
    mkdir -p "$prefix/include" "$prefix/lib/pkgconfig" || return 1
    if [ -f "$src/source/x265.h" ]; then
        cp -f "$src/source/x265.h" "$prefix/include/" || return 1
    fi
    if [ -f "$b/x265_config.h" ]; then
        cp -f "$b/x265_config.h" "$prefix/include/" || return 1
    fi
    if [ "$shared" = "ON" ]; then
        # Android: install ONLY the real shared library. Leaving libx265.a on
        # the same -L path can make the linker pick the archive, which drops
        # api.cpp.o and leaves x265_api_get_198 unresolved in libavcodec.so.
        [ -f "$b/libx265.so" ] || return 1
        rm -f "$prefix/lib/libx265.a"
        cp -L -f "$b/libx265.so" "$prefix/lib/libx265.so" || return 1
    else
        [ -f "$b/libx265.a" ] || return 1
        cp -f "$b/libx265.a" "$prefix/lib/libx265.a" || return 1
    fi
    # Write a clean .pc: the generated one carries a malformed Libs.private
    # (-l-l:libunwind.a) and is not installed at all when no tag exists.
    local pc_prefix="$prefix"
    command -v cygpath >/dev/null 2>&1 && pc_prefix="$(cygpath -m -- "$prefix")"
    cat > "$prefix/lib/pkgconfig/x265.pc" <<EOF
prefix=$pc_prefix
exec_prefix=\${prefix}
libdir=\${exec_prefix}/lib
includedir=\${exec_prefix}/include

Name: x265
Description: H.265/HEVC video encoder
Version: 3.4
Libs: -L\${libdir} -lx265
Cflags: -I\${includedir}
EOF
    return 0
}

# Build x265 for one ABI (cmake cross-build; may need tuning per NDK).
# SHARED decides ENABLE_SHARED: Android needs a SHARED libx265.so, everything
# else keeps the static archive.
# Args: abi cc cxx arch sysroot prefix
build_x265() {
    [ "$BUILD_X265" = "1" ] || return 0
    local abi="$1" cc="$2" cxx="$3" arch="$4" sysroot="$5" prefix="$6"
    local src="$X265_SRC"
    # Reuse a completed install: cmake+make for x265 costs several minutes
    # and occasionally hangs on the SVE assembly step under heavy -j. The
    # required artifact depends on the mode: Android needs the SHARED .so,
    # everything else the static archive.
    local shared=OFF
    case "$cc" in *android*) shared=ON ;; esac
    if [ "$shared" = "ON" ] && [ -f "$prefix/lib/libx265.so" ] &&
       [ -f "$prefix/lib/pkgconfig/x265.pc" ]; then
        log "x265 already built (shared) -> $prefix"
        return 0
    fi
    if [ "$shared" = "OFF" ] && [ -f "$prefix/lib/libx265.a" ]; then
        log "x265 already built (static) -> $prefix"
        return 0
    fi
    if [ ! -d "$src" ]; then
        git_clone_robust https://github.com/videolan/x265.git "$src" "$X265_REF" \
            || log "x265 clone FAILED; continuing without libx265"
    fi
    # x265's CMake installs x265-shared only when a git tag exists
    # (X265_LATEST_TAG comes from `git describe`), but git_clone_robust makes
    # a --depth 1 clone that carries no tags. Fetch them non-fatally or the
    # shared build would compile yet silently install nothing.
    ( cd "$src" && git fetch origin --tags --depth 1 >/dev/null 2>&1 ) \
        || log "x265 tag fetch failed; shared library install may be skipped"
    local b="${ROOT_DIR}/build/x265-${abi}"
    # Never `rm -rf "$b"`: prefix IS "$b/prefix". Wiping the build dir also
    # destroyed completed installs, so the cache check above could never fire
    # after an interrupted run and every retry recompiled x265 from scratch.
    mkdir -p "$b" "$prefix"
    # Resume after an interrupted run: the compiled library often exists even
    # though make install never ran (or installed nothing due to a missing git
    # tag). Stage it directly instead of paying for another full cmake+make.
    if [ -f "$b/CMakeCache.txt" ] && [ -f "$b/x265_config.h" ] && {
           { [ "$shared" = "ON" ]  && [ -f "$b/libx265.so" ]; } ||
           { [ "$shared" = "OFF" ] && [ -f "$b/libx265.a" ]; }
       }; then
        if install_x265_artifacts "$b" "$prefix" "$shared" "$src"; then
            log "x265 installed from existing build -> $prefix"
            return 0
        fi
        log "staging cached x265 artifacts failed; rebuilding"
        rm -f "$b/CMakeCache.txt"
    fi
    log "building x265 for ${abi}"
    # Android links libx265 as a SHARED library. The static archive's api.cpp.o
    # (which defines the versioned x265_api_get_198 accessor FFmpeg calls) is
    # skipped by this host's archive linker, leaving libavcodec.so with an
    # undefined symbol the Android loader rejects. Every way to force the
    # object in (--undefined is eaten by FFmpeg's version script, --whole-
    # archive duplicates compiler-rt symbols, a direct .o duplicates it again
    # in libavformat) breaks somewhere else. A shared libx265.so carries its
    # own SONAME dependency, which the Android loader resolves normally.
    local shared=OFF
    case "$cc" in *android*) shared=ON ;; esac
    log "x265 ENABLE_SHARED=$shared for ${abi}"
    # On Windows cmake defaults to the NMake generator (needs nmake, absent) and
    # can't identify the NDK clang, so force Unix Makefiles + the platform's
    # cmake toolchain file (X265_CMAKE_TOOLCHAIN) when provided.
    local tc="${X265_CMAKE_TOOLCHAIN:-}"
    local extra="${X265_CMAKE_EXTRA:-}"
    # Resolve the binutils dir so we can hand cmake an explicit archiver. When
    # the compiler is a wrapper script (OHOS), cmake cannot infer llvm-ar from
    # it, so libx265.a link fails with "link command: file not found".
    local llvm_dir
    if [ -n "${OHOS_NDK_HOME:-}" ] && [[ "$cc" == *"ohos"* ]]; then
        llvm_dir="${OHOS_NDK_HOME}/llvm/bin/"
    else
        llvm_dir="$(dirname "$cc")/"
    fi
    # cmake must be able to *identify* the compiler (it probes --version and does
    # a test compile). A POSIX .sh shim can't be exec'd by cmake at all, and a
    # .bat shim is launched via cmd yet cmake still fails to parse its
    # CMAKE_C_COMPILER_ID (x265's CMakeLists aborts at `if(ID STREQUAL "Clang")`).
    # For HarmonyOS the wrapper only injects --target/--sysroot/-D__MUSL__, so we
    # point cmake at the real OHOS clang.exe and bake those flags into
    # CMAKE_*_FLAGS instead. That lets cmake detect the compiler as Clang.
    local cmake_cc="$cc" cmake_cxx="$cxx" cmake_cflags="" cmake_cxxflags=""
    case "$cc" in
        *.sh)
            if [ -n "${OHOS_NDK_HOME:-}" ] && [[ "$cc" == *"ohos"* ]]; then
                cmake_cc="${OHOS_NDK_HOME}/llvm/bin/clang.exe"
                cmake_cxx="${OHOS_NDK_HOME}/llvm/bin/clang++.exe"
                # The OHOS sysroot is musl-based; its <string.h> already provides
                # strtok_r, but x265's param.cpp defines its own static copy unless
                # HAVE_STRTOK_R is set, which then collides ("static declaration
                # follows non-static declaration"). Tell x265 to use the libc one.
                cmake_cflags="--target=aarch64-linux-ohos --sysroot=${OHOS_NDK_HOME}/sysroot -D__MUSL__ -DHAVE_STRTOK_R"
                cmake_cxxflags="--target=aarch64-linux-ohos --sysroot=${OHOS_NDK_HOME}/sysroot -D__MUSL__ -DHAVE_STRTOK_R"
            else
                cmake_cc="${cc%.sh}.bat"; cmake_cxx="${cxx%.sh}.bat"
            fi ;;
    esac
    local cmake_flags=(
        -G "Unix Makefiles"
        -DCMAKE_BUILD_TYPE=Release
        -DENABLE_SHARED=$shared -DENABLE_CLI=OFF -DENABLE_PIC=ON
        -DENABLE_ASSEMBLY=OFF
        -DCMAKE_INSTALL_PREFIX="$prefix"
    )
    if [ "$shared" = "ON" ]; then
        # The Android NDK/bionic has no libpthread: pthread symbols live in
        # libc. x265's CMakeLists unconditionally appends `pthread` to
        # PLATFORM_LIBS for UNIX-like targets (Android included), so the
        # shared-library link fails with "unable to find library -lpthread".
        # Ship an EMPTY static archive on the link path: -lpthread resolves to
        # an archive with no members, nothing is pulled in and libx265.so
        # carries no bogus NEEDED entry — bionic's libc provides the real
        # pthread_* symbols at runtime.
        local shims="${ROOT_DIR}/build/_shims/android-nostl"
        mkdir -p "$shims"
        [ -f "$shims/libpthread.a" ] || "${llvm_dir}llvm-ar.exe" rc "$shims/libpthread.a"
        cmake_flags+=(-DCMAKE_SHARED_LINKER_FLAGS="-L${shims}")
    fi
    if [ -n "$tc" ]; then
        cmake_flags+=(-DCMAKE_TOOLCHAIN_FILE="$tc" $extra)
    else
        cmake_flags+=(
            -DCMAKE_C_COMPILER="$cmake_cc" -DCMAKE_CXX_COMPILER="$cmake_cxx"
            -DCMAKE_C_FLAGS="$cmake_cflags" -DCMAKE_CXX_FLAGS="$cmake_cxxflags"
            -DCMAKE_SYSROOT="$sysroot"
            -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR="$arch"
            -DCMAKE_AR="${llvm_dir}llvm-ar.exe"
            -DCMAKE_RANLIB="${llvm_dir}llvm-ranlib.exe"
            -DCMAKE_STRIP="${llvm_dir}llvm-strip.exe"
            $extra
        )
    fi
    if ( cd "$b" && cmake "$src/source" "${cmake_flags[@]}" && \
         make -j"$JOBS" ); then
        log "x265 compiled -> $b"
        # make install is best-effort: its shared-library and .pc rules are
        # gated on X265_LATEST_TAG, which a shallow clone does not have.
        ( cd "$b" && make install ) \
            || log "x265 make install incomplete (no git tag?); staging manually"
    else
        log "x265 build FAILED; continuing without libx265"
    fi
    install_x265_artifacts "$b" "$prefix" "$shared" "$src" \
        || log "x265 artifacts not staged; FFmpeg will build without libx265"
}

# Build (or skip) the external codecs and accumulate FFmpeg configure flags.
# Args: abi cc cxx arch sysroot
resolve_external_codecs() {
    local abi="$1" cc="$2" cxx="$3" arch="$4" sysroot="$5"
    # Reset per-ABI so repeated calls don't duplicate --enable flags.
    X264_PREFIX=""; X265_PREFIX=""
    EXT_CFLAGS=""; EXT_LDFLAGS=""; FFMPEG_EXTRA_ENABLE=""
    local p264="${ROOT_DIR}/build/x264-${abi}/prefix"
    local p265="${ROOT_DIR}/build/x265-${abi}/prefix"
    build_x264 "$abi" "$cc" "$cxx" "$arch" "$sysroot" "$p264"
    if [ -f "$p264/lib/libx264.a" ] || [ -f "$p264/lib/libx264.so" ]; then
        X264_PREFIX="$p264"
        EXT_CFLAGS="${EXT_CFLAGS:+$EXT_CFLAGS }-I$p264/include"
        EXT_LDFLAGS="${EXT_LDFLAGS:+$EXT_LDFLAGS }-L$p264/lib"
        FFMPEG_EXTRA_ENABLE="${FFMPEG_EXTRA_ENABLE:-} --enable-gpl --enable-libx264"
    fi
    build_x265 "$abi" "$cc" "$cxx" "$arch" "$sysroot" "$p265"
    if [ -f "$p265/lib/libx265.a" ] || [ -f "$p265/lib/libx265.so" ]; then
        X265_PREFIX="$p265"
        EXT_CFLAGS="${EXT_CFLAGS:+$EXT_CFLAGS }-I$p265/include"
        EXT_LDFLAGS="${EXT_LDFLAGS:+$EXT_LDFLAGS }-L$p265/lib"
        FFMPEG_EXTRA_ENABLE="${FFMPEG_EXTRA_ENABLE:-} --enable-libx265"
    fi

    # FFmpeg's configure locates x264/x265 via pkg-config; point it at the
    # freshly built .pc files (the build host has no system pkg-config, so the
    # build scripts place a minimal shim on PATH).
    local pc_path=""
    [ -n "$X264_PREFIX" ] && pc_path="${X264_PREFIX}/lib/pkgconfig"
    [ -n "$X265_PREFIX" ] && pc_path="${pc_path:+$pc_path:}${X265_PREFIX}/lib/pkgconfig"
    if [ -n "$pc_path" ]; then
        export PKG_CONFIG_PATH="${pc_path}${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
    fi
}

# Ensure make + cmake are reachable. NDK ships make; cmake is usually alongside
# an Android SDK. Override CMAKE_BIN if your cmake lives elsewhere.
ensure_build_tools() {
    local ndk_prebuilt="${1:-}"
    local extra_cmake="${CMAKE_BIN:-}"
    if ! command -v make >/dev/null 2>&1; then
        if [ -n "$ndk_prebuilt" ] && { [ -x "$ndk_prebuilt/make.exe" ] || [ -x "$ndk_prebuilt/make" ]; }; then
            export PATH="$ndk_prebuilt:$PATH"
        else
            log "WARNING: make not on PATH and no NDK prebuilt dir; build may fail"
        fi
    fi
    if [ -n "$extra_cmake" ]; then
        export PATH="$(dirname "$extra_cmake"):$PATH"
    elif ! command -v cmake >/dev/null 2>&1; then
        local found
        found="$(ls -d /d/Android/Sdk/cmake/*/bin 2>/dev/null | sort -V | tail -1)"
        [ -n "$found" ] && export PATH="$found:$PATH"
    fi
    command -v make  >/dev/null 2>&1 || log "WARNING: make still not on PATH"
    command -v cmake >/dev/null 2>&1 || log "WARNING: cmake not on PATH (x265 build will be skipped)"
}

# Components required by native/captrim_ffmpeg.c and by the P1 preview path,
# plus the learning-project "full functionality" set.
FFMPEG_COMMON_CFG=(
    --disable-everything
    --disable-programs
    --disable-doc
    --disable-network
    --disable-autodetect
    --disable-avdevice
    --enable-gpl
    --enable-shared
    --disable-static
    --enable-pic
    --enable-avfilter
    --enable-swscale
    --enable-swresample
    --enable-protocol=file
    --enable-protocol=concat
    --enable-demuxer=mov
    --enable-demuxer=matroska
    --enable-demuxer=mpegts
    --enable-demuxer=flv
    --enable-demuxer=avi
    --enable-demuxer=aac
    --enable-demuxer=mp3
    --enable-demuxer=wav
    --enable-demuxer=ogg
    --enable-demuxer=mp4
    --enable-demuxer=m4v
    --enable-demuxer=image2
    --enable-demuxer=gif
    --enable-muxer=mp4
    --enable-muxer=mov
    --enable-muxer=matroska
    --enable-muxer=mpegts
    --enable-muxer=flv
    --enable-muxer=avi
    --enable-muxer=aac
    --enable-muxer=mp3
    --enable-muxer=wav
    --enable-muxer=ogg
    --enable-muxer=image2
    --enable-muxer=gif
    --enable-parser=h264
    --enable-parser=hevc
    --enable-parser=aac
    --enable-parser=mpeg4video
    --enable-parser=mpegaudio
    --enable-parser=vp9
    --enable-parser=opus
    --enable-decoder=h264
    --enable-decoder=hevc
    --enable-decoder=aac
    --enable-decoder=mp3
    --enable-decoder=mpeg4
    --enable-decoder=vp9
    --enable-decoder=opus
    --enable-decoder=flac
    --enable-decoder=pcm_s16le
    --enable-decoder=pcm_s24le
    --enable-encoder=aac
    --enable-encoder=mpeg4
    --enable-encoder=flac
    --enable-encoder=rawvideo
    # CRITICAL: with --disable-everything the libx264/libx265 ENCODER components
    # are OFF even though --enable-libx264/--enable-libx265 (added in
    # resolve_external_codecs) only enables the *libraries*. The encoders must be
    # turned on explicitly or libx264.o/libx265.o are never compiled into
    # libavcodec (verified: CONFIG_LIBX264_ENCODER/265_ENCODER were 0 -> 0 symbols
    # in libavcodec.so). libx264rgb is the RGB-pixel-format variant of libx264.
    --enable-encoder=libx264
    --enable-encoder=libx264rgb
    --enable-encoder=libx265
    # ---- Filters for the middle "FFmpeg processing" stage of the transcode
    # pipeline. With --disable-everything each filter is OFF unless opted in.
    # buffer/buffersink are the graph anchors (frame-in / frame-out); scale and
    # format are the workhorses (resize + pixel-format conversion); fps/crop/
    # trim/setpts/null/hflip/vflip/pad/overlay/rotate make up a useful "complex
    # edit command" vocabulary shared by both software and hardware paths.
    --enable-filter=buffer
    --enable-filter=buffersink
    --enable-filter=scale
    --enable-filter=format
    --enable-filter=fps
    --enable-filter=crop
    --enable-filter=trim
    --enable-filter=setpts
    --enable-filter=null
    --enable-filter=hflip
    --enable-filter=vflip
    --enable-filter=pad
    --enable-filter=overlay
    --enable-filter=rotate
)

# FFmpeg's configure writes MSYS-style absolute paths (/c/Users/...) into the
# generated Makefiles and config.mak (it computes SRC_PATH via `pwd`). The NDK's
# native make.exe cannot resolve those paths from inside a file (MSYS bash only
# translates command-line arguments, not file contents). Rewrite them to
# Windows-style (C:/Users/...) which both make and the NDK clang understand.
# Call this once, right after configure and before make.
fix_ffmpeg_generated_paths() {
    local builddir="$1"
    local winroot msysroot
    winroot="$(cygpath -w "$ROOT_DIR" 2>/dev/null)" || return 0
    winroot="${winroot//\\//}"          # backslash -> forward slash (make/clang safe)
    msysroot="$ROOT_DIR"                # already /c/Users/.../CapTrim
    [ -n "$winroot" ] || return 0
    log "rewriting MSYS paths -> ${winroot} in generated build files"
    find "$builddir" \( -name 'Makefile' -o -name 'config.mak' -o -name '*.mak' \) -type f 2>/dev/null \
        | while read -r f; do
            sed -i "s|${msysroot}|${winroot}|g" "$f"
        done
}

# Writes the exact configure line next to the binaries. GPL compliance requires
# distributing the matching source AND the build recipe (incl. x264/x265 refs).
write_build_receipt() {
    local out_dir="$1"
    shift
    {
        echo "# CapTrim FFmpeg build receipt"
        echo "# FFmpeg ref: ${FFMPEG_REF:-unknown}"
        echo "# FFmpeg source: ${FFMPEG_SRC:-not recorded}"
        echo "# x264 source: ${X264_SRC:-not recorded} (ref ${X264_REF:-unknown})"
        echo "# x265 source: ${X265_SRC:-not recorded} (ref ${X265_REF:-unknown})"
        echo "# Built at: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
        echo "# License: GPL v2.1 or later (--enable-gpl / --enable-libx264 / --enable-libx265)"
        echo "# NOTE: distributing the binaries requires shipping FFmpeg + x264 + x265 source."
        echo "# configure:"
        printf '%s\n' "$*"
    } > "$out_dir/FFMPEG_BUILD.txt"
    cp "${ROOT_DIR}/native/captrim_ffmpeg.c" "$out_dir/captrim_ffmpeg.c.shipped"
}

# $1 = output dir, $2 = compiler executable (a wrapper script is fine),
# remaining args are extra compiler flags. Produces libcaptrim_ffmpeg.so with
# a proper SONAME, which HarmonyOS requires for prebuilt libraries.
build_c_shim() {
    local out_dir="$1"
    local cc="$2"
    shift 2
    log "building libcaptrim_ffmpeg.so"
    # FFmpeg's `make install` under --disable-everything creates
    # prefix/include/libavutil/ but installs ZERO libavutil public headers into
    # it (verified empty; libavcodec/libavformat install fine). Meanwhile the
    # source tree's libavutil/avutil.h needs the GENERATED headers avconfig.h /
    # config.h, which only exist in the build dir (not in the source tree).
    # Resolution: take ALL public headers from the FFmpeg SOURCE tree (single,
    # version-matched source of truth), then add the build dir for the generated
    # headers, and keep prefix/include last as a fallback for anything already
    # correctly installed (libavcodec / libavformat / ...).
    local build_dir="${PREFIX%/prefix}"
    [ -d "$build_dir/libavutil" ] || build_dir="$(dirname "$PREFIX")"
    "$cc" -shared -fPIC -O2 \
        -I"${ROOT_DIR}/native" \
        -I"${FFMPEG_SRC:-${ROOT_DIR}/third_party/ffmpeg}" \
        -I"${build_dir}/libavutil" -I"${build_dir}" \
        -I"${PREFIX}/include" \
        "${ROOT_DIR}/native/captrim_ffmpeg.c" \
        "${ROOT_DIR}/native/captrim_pipeline.c" \
        -o "${out_dir}/libcaptrim_ffmpeg.so" \
        -L"${PREFIX}/lib" -lavformat -lavcodec -lavutil -lavfilter -lswscale \
        -Wl,-soname,libcaptrim_ffmpeg.so "$@" \
        || die "C shim compilation failed"

    # Keep the original FFmpeg SONAMEs: do not rename libav*.so / libsw*.so.
    for lib in "${PREFIX}"/lib/libav*.so* "${PREFIX}"/lib/libsw*.so*; do
        [ -e "$lib" ] || continue
        cp -a "$lib" "$out_dir/"
    done

    # External codecs: Android ships the REAL libx265.so. X265_PREFIX is set
    # by resolve_external_codecs() for the current ABI; FFmpeg's own PREFIX
    # only holds libav*/libsw* libs, so searching it here always found nothing
    # and left an old 5 KB placeholder in dist. Copy by SONAME (libx265.so —
    # a tag-less build has no versioned suffix). OHOS builds x265 static-only,
    # so its prefix has no .so and nothing is copied there.
    if [ -n "${X265_PREFIX:-}" ] && [ -e "${X265_PREFIX}/lib/libx265.so" ]; then
        cp -L -f "${X265_PREFIX}/lib/libx265.so" "$out_dir/libx265.so"
    fi
}
