#!/usr/bin/env bash
# Shared, deliberately minimal FFmpeg configuration for CapTrim P0.
#
# LEGAL: this configuration stays LGPL v2.1+. Never add --enable-gpl,
# --enable-nonfree, --enable-libx264 or --enable-libx265. Encoding is done by
# the platform hardware codecs, so CapTrim does not need a software encoder.
# FFmpeg is built as shared libraries and linked dynamically; see
# https://ffmpeg.org/legal.html for the full compliance checklist.

set -euo pipefail

log() { printf '[captrim] %s\n' "$*"; }
die() { printf '[captrim][error] %s\n' "$*" >&2; exit 1; }

need_env() {
    local name="$1"
    [ -n "${!name:-}" ] || die "$name is not set. See README.md -> Building native libraries."
}

# Components required by native/captrim_ffmpeg.c and by the P1 preview path.
# --disable-everything turns off every codec/muxer/demuxer/parser/protocol;
# everything below is an explicit opt-in so the resulting .so stays small.
FFMPEG_COMMON_CFG=(
    --disable-everything
    --disable-programs
    --disable-doc
    --disable-network
    --disable-autodetect
    --disable-avdevice
    --disable-avfilter
    --disable-postproc
    --disable-swscale
    --disable-swresample
    --disable-asm
    --enable-shared
    --disable-static
    --enable-pic
    --enable-protocol=file
    --enable-demuxer=mov
    --enable-demuxer=matroska
    --enable-demuxer=mpegts
    --enable-demuxer=flv
    --enable-demuxer=avi
    --enable-demuxer=aac
    --enable-demuxer=mp3
    --enable-demuxer=wav
    --enable-demuxer=ogg
    --enable-muxer=mp4
    --enable-parser=h264
    --enable-parser=hevc
    --enable-parser=aac
    --enable-parser=mpeg4video
    --enable-decoder=h264
    --enable-decoder=hevc
    --enable-decoder=aac
    --enable-decoder=mp3
)

# Writes the exact configure line next to the binaries. LGPL compliance
# requires distributing the matching source AND the build recipe.
write_build_receipt() {
    local out_dir="$1"
    shift
    {
        echo "# CapTrim FFmpeg build receipt"
        echo "# FFmpeg ref: ${FFMPEG_REF:-unknown}"
        echo "# FFmpeg source: ${FFMPEG_SRC:-not recorded}"
        echo "# Built at: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
        echo "# License: LGPL v2.1 or later (no --enable-gpl / --enable-nonfree)"
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
    "$cc" -shared -fPIC -O2 \
        -I"${ROOT_DIR}/native" -I"${PREFIX}/include" \
        "${ROOT_DIR}/native/captrim_ffmpeg.c" \
        -o "${out_dir}/libcaptrim_ffmpeg.so" \
        -L"${PREFIX}/lib" -lavformat -lavcodec -lavutil \
        -Wl,-soname,libcaptrim_ffmpeg.so "$@" \
        || die "C shim compilation failed"

    # Keep the original FFmpeg SONAMEs: do not rename libav*.so.
    for lib in "${PREFIX}"/lib/libav*.so* "${PREFIX}"/lib/libsw*.so*; do
        [ -e "$lib" ] || continue
        cp -a "$lib" "$out_dir/"
    done
}
