#!/usr/bin/env bash
# Static check for the two licence profiles (see ffmpeg-config-common.sh).
#
# Why this exists: a GPL flag leaking into the LGPL profile is invisible until
# someone redistributes the SDK, and a full FFmpeg rebuild costs ~10 minutes per
# ABI. Everything asserted here is decidable from the sources in a second:
#
#   1. CAPTRIM_LICENSE=lgpl drops --enable-gpl, libx264 and libx265 from the
#      configure line, never builds x264/x265, and writes to *-lgpl directories.
#   2. CAPTRIM_LICENSE=gpl keeps all three, plus the two encoder components
#      without which --enable-libx264 compiles the library but not the encoder.
#   3. An unknown profile is rejected instead of silently defaulting to GPL.
#   4. --enable-gpl appears nowhere else in the tree: one stray flag in a
#      platform script would make an "LGPL" artifact GPL again.
#   5. The core encoder table still knows libx264 / libx265 and marks them
#      GPL-only, so an LGPL build reports them as unavailable instead of
#      failing at encode time.
#
# Run from CI and locally; exits non-zero on any violation.
set -uo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
COMMON="${ROOT_DIR}/scripts/ffmpeg-config-common.sh"
ENCODERS="${ROOT_DIR}/crates/captrim-core/src/encoders.rs"

fails=0
note() { printf '[captrim] %s\n' "$*"; }
ok()   { printf '[captrim] ok: %s\n' "$*"; }
bad()  { printf '[captrim][error] %s\n' "$*" >&2; fails=$((fails + 1)); }

# Evaluate the shared config under a given profile and print the facts that
# decide the licence: the configure line, whether x264/x265 get built, and the
# artifact directories.
probe_profile() {
    local profile="$1"
    CAPTRIM_LICENSE="$profile" ROOT_DIR="$ROOT_DIR" bash -c '
        set -euo pipefail
        source "$ROOT_DIR/scripts/ffmpeg-config-common.sh" >/dev/null 2>&1 || exit 9
        printf "CFG=%s\n" "${FFMPEG_COMMON_CFG[*]}"
        printf "BUILD_X264=%s\n" "$BUILD_X264"
        printf "BUILD_X265=%s\n" "$BUILD_X265"
        read -r b o <<< "$(license_dirs ohos arm64-v8a)"
        printf "BUILD_DIR=%s\n" "$b"
        printf "OUT_DIR=%s\n" "$o"
    ' 2>/dev/null
}

# ---- 1/2. The two profiles differ exactly where they must ------------------
gpl="$(probe_profile gpl)" || bad "CAPTRIM_LICENSE=gpl could not be evaluated"
lgpl="$(probe_profile lgpl)" || bad "CAPTRIM_LICENSE=lgpl could not be evaluated"

gpl_cfg="$(printf '%s\n' "$gpl" | sed -n 's/^CFG=//p')"
lgpl_cfg="$(printf '%s\n' "$lgpl" | sed -n 's/^CFG=//p')"

for flag in --enable-gpl --enable-encoder=libx264 --enable-encoder=libx264rgb --enable-encoder=libx265; do
    case " $gpl_cfg " in
        *" $flag "*) ;;
        *) bad "gpl profile lost '$flag' (software H.264/HEVC would disappear)" ;;
    esac
done

for flag in --enable-gpl libx264 libx265; do
    case "$lgpl_cfg" in
        *"$flag"*) bad "lgpl profile still contains '$flag': the artifact would be GPL" ;;
    esac
done

case "$lgpl" in
    *'BUILD_X264=0'*) ;;
    *) bad "lgpl profile would still build x264" ;;
esac
case "$lgpl" in
    *'BUILD_X265=0'*) ;;
    *) bad "lgpl profile would still build x265" ;;
esac
# The GPL profile must keep building them by default, or its feature set silently
# shrinks (BUILD_X264=0 on the command line is still an allowed override).
case "$gpl" in
    *'BUILD_X264=1'*) ;;
    *) bad "gpl profile no longer builds x264 by default" ;;
esac

# Separate artifact trees, so a LGPL build cannot overwrite the GPL one that the
# on-device self-test uses as its baseline.
case "$lgpl" in
    *'BUILD_DIR='*'ffmpeg-ohos-lgpl/'*'OUT_DIR='*'dist/ohos-lgpl/'*) ;;
    *) bad "lgpl profile does not use its own build/dist directories" ;;
esac
case "$gpl" in
    *'BUILD_DIR='*'ffmpeg-ohos/'*'OUT_DIR='*'dist/ohos/'*) ;;
    *) bad "gpl profile no longer uses the historical build/dist directories" ;;
esac
if [ "$fails" -eq 0 ]; then
    ok "profiles: gpl keeps x264/x265 + --enable-gpl, lgpl drops all three"
fi

# ---- 3. An unknown profile is an error, not a default ---------------------
set +e
( CAPTRIM_LICENSE=bsd ROOT_DIR="$ROOT_DIR" bash -c '
    source "$ROOT_DIR/scripts/ffmpeg-config-common.sh"
  ' >/dev/null 2>&1 )
rc=$?
set -e
if [ "$rc" -eq 0 ]; then
    bad "CAPTRIM_LICENSE=bsd was accepted; validate the value instead of defaulting"
else
    ok "unknown CAPTRIM_LICENSE rejected (exit $rc)"
fi

# ---- 4. --enable-gpl lives in exactly one place ---------------------------
# Every other script (platform builds, gradle/hvigor wrappers, CI) must inherit
# the profile from ffmpeg-config-common.sh. A second occurrence means some path
# can produce a GPL artifact without being asked.
stray="$(grep -rIl --enable-gpl \
    "${ROOT_DIR}/scripts" "${ROOT_DIR}/native" "${ROOT_DIR}/crates" \
    "${ROOT_DIR}/platforms" "${ROOT_DIR}/.github" 2>/dev/null \
    | grep -v 'ffmpeg-config-common.sh' || true)"
if [ -n "$stray" ]; then
    bad "--enable-gpl appears outside ffmpeg-config-common.sh:"$'\n'"$stray"
else
    ok "--enable-gpl is confined to ffmpeg-config-common.sh"
fi

# ---- 5. The encoder table still describes both profiles -------------------
[ -f "$ENCODERS" ] || bad "missing $ENCODERS"
if [ -f "$ENCODERS" ]; then
    for name in libx264 libx264rgb libx265 libopenh264 mpeg4; do
        grep -q "\"$name\"" "$ENCODERS" \
            || bad "encoder table does not list '$name'"
    done
    # The three that drag the build under GPL must be marked as such, otherwise
    # resolve() would happily hand an LGPL build an encoder it does not have.
    if ! grep -q 'needs_gpl' "$ENCODERS"; then
        bad "encoder table lost needs_gpl(); GPL-only encoders would look available"
    fi
    ok "encoder table lists the candidates and their GPL status"
fi

if [ "$fails" -ne 0 ]; then
    printf '[captrim][error] licence profile check failed (%d problem(s))\n' "$fails" >&2
    exit 1
fi
printf '[captrim] licence profile check ok\n'
