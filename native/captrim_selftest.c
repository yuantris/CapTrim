/*
 * Native end-to-end self-check for the CapTrim shared libraries.
 *
 * Built by scripts/run-android-selftest.sh (or by hand with the NDK clang)
 * and executed directly on an Android device via adb. It exercises the same
 * C ABI that the JNI/Kotlin layer calls, so a failure here isolates the
 * native stack from the Java side.
 *
 * Usage: captrim_selftest <source.mp4> <writable-dir>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "captrim_ffmpeg.h"
#include "captrim_pipeline.h"

static int g_pass = 0;
static int g_fail = 0;

static void check(int ok, const char *name, const char *detail) {
    if (ok) {
        ++g_pass;
        printf("PASS %-46s | %s\n", name, detail ? detail : "");
    } else {
        ++g_fail;
        printf("FAIL %-46s | %s\n", name, detail ? detail : "");
    }
    fflush(stdout);
}

typedef struct {
    int ticks;
    float last;
} progress_t;

static int32_t on_progress(float v, void *user_data) {
    progress_t *p = (progress_t *)user_data;
    p->ticks++;
    p->last = v;
    return 0;
}

static char *probe_info(const char *uri, char *buf, size_t cap, int *ok) {
    char *json = NULL;
    char *err = NULL;
    int32_t rc = ct_ff_probe(uri, &json, &err);
    *ok = (rc == 0 && json != NULL);
    if (!*ok) {
        snprintf(buf, cap, "rc=%d err=%s", (int)rc, err ? err : "(null)");
    } else {
        snprintf(buf, cap, "%s", json);
    }
    ct_ff_string_free(json);
    ct_ff_string_free(err);
    return buf;
}

static int has(const char *json, const char *needle) {
    return json && strstr(json, needle) != NULL;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <source.mp4> <writable-dir>\n", argv[0]);
        return 2;
    }
    const char *src = argv[1];
    const char *dir = argv[2];
    char dst[512];
    char json[8192];
    char detail[2048];
    int ok = 0;

    printf("== CapTrim native selftest (Android) ==\n");
    printf("source=%s workdir=%s\n", src, dir);
    fflush(stdout);

    /* 1. Probe the bundled sample. */
    char *src_info = probe_info(src, json, sizeof(json), &ok);
    check(ok && has(src_info, "video"), "FFmpeg probe", src_info);

    /* 2. Lossless remux: a stream-copy smoke test of the muxer. */
    snprintf(dst, sizeof(dst), "%s/selftest_remux.mp4", dir);
    char *err = NULL;
    progress_t remux_p = {0, 0.0f};
    int32_t rc = ct_ff_remux(src, dst, on_progress, &remux_p, &err);
    check(rc == 0, "FFmpeg remux",
          rc == 0 ? "stream copy ok" : (err ? err : "unknown error"));
    ct_ff_string_free(err);
    if (rc == 0) {
        char *remux_info = probe_info(dst, json, sizeof(json), &ok);
        check(ok && has(remux_info, "video"), "Remux output probe", remux_info);
    } else {
        check(0, "Remux output probe", "skipped");
    }

    /* 3. Fully software transcode: decode -> filter -> libx264 -> mp4. */
    snprintf(dst, sizeof(dst), "%s/selftest_soft.mp4", dir);
    err = NULL;
    progress_t soft_p = {0, 0.0f};
    rc = ct_ff_transcode(src, dst,
                         "trim=duration=2,setpts=PTS-STARTPTS,scale=320:180,hflip,fps=15",
                         "libx264", on_progress, &soft_p, &err);
    check(rc == 0, "FFmpeg complex transcode (software)",
          rc == 0 ? "filter chain ok" : (err ? err : "unknown error"));
    ct_ff_string_free(err);
    if (rc == 0) {
        char *soft_info = probe_info(dst, json, sizeof(json), &ok);
        snprintf(detail, sizeof(detail), "%s progress=%d last=%.2f",
                 soft_info, soft_p.ticks, (double)soft_p.last);
        check(ok && has(soft_info, "h264"), "Software output codec", detail);
    } else {
        check(0, "Software output codec", "skipped");
    }

    /* 3b. libx265 (HEVC): proves the GPL HEVC encoder is linked and usable on
     * Android — the codec whose static archive historically broke linking. */
    snprintf(dst, sizeof(dst), "%s/selftest_x265.mp4", dir);
    err = NULL;
    progress_t x265_p = {0, 0.0f};
    rc = ct_ff_transcode(src, dst,
                         "trim=duration=2,setpts=PTS-STARTPTS,scale=320:180,hflip,fps=15",
                         "libx265", on_progress, &x265_p, &err);
    check(rc == 0, "FFmpeg complex transcode (libx265 HEVC)",
          rc == 0 ? "filter chain ok" : (err ? err : "unknown error"));
    ct_ff_string_free(err);
    if (rc == 0) {
        char *x265_info = probe_info(dst, json, sizeof(json), &ok);
        snprintf(detail, sizeof(detail), "%s progress=%d last=%.2f",
                 x265_info, x265_p.ticks, (double)x265_p.last);
        check(ok && has(x265_info, "hevc"), "x265 output codec", detail);
    } else {
        check(0, "x265 output codec", "skipped");
    }

    /* 4. Three-stage orchestration + hardware-first routing. Without an
     *    AMediaCodec backend linked, this must fall back cleanly to software. */
    snprintf(dst, sizeof(dst), "%s/selftest_pipeline.mp4", dir);
    err = NULL;
    ct_pipeline_report_t report;
    progress_t pipe_p = {0, 0.0f};
    memset(&report, 0, sizeof(report));
    rc = ct_pipeline_run(src, dst,
                         "trim=duration=2,setpts=PTS-STARTPTS,scale=320:180,hflip,fps=15",
                         "libx264", NULL, on_progress, &pipe_p, &err, &report);
    check(rc == 0, "Three-stage pipeline (software baseline)",
          rc == 0 ? "pipeline ok" : (err ? err : "unknown error"));
    ct_ff_string_free(err);
    if (rc == 0) {
        snprintf(detail, sizeof(detail),
                 "decoder=%d encoder=%d decoded=%d encoded=%d reason=%s",
                 (int)report.decoder, (int)report.encoder,
                 report.frames_decoded, report.frames_encoded,
                 report.fallback_reason[0] ? report.fallback_reason : "none");
        check(report.frames_decoded > 0 && report.frames_encoded > 0,
              "Pipeline frame flow", detail);
    } else {
        check(0, "Pipeline frame flow", "skipped");
    }

    /* 5. hw: route with no platform factory: must still produce a valid mp4. */
    snprintf(dst, sizeof(dst), "%s/selftest_hwroute.mp4", dir);
    err = NULL;
    progress_t hw_p = {0, 0.0f};
    memset(&report, 0, sizeof(report));
    rc = ct_pipeline_run(src, dst,
                         "trim=duration=2,setpts=PTS-STARTPTS,scale=320:180,fps=15",
                         "hw:libx264", NULL, on_progress, &hw_p, &err, &report);
    check(rc == 0, "Hardware-first route with fallback",
          rc == 0 ? "route ok" : (err ? err : "unknown error"));
    ct_ff_string_free(err);
    if (rc == 0) {
        char *hw_info = probe_info(dst, json, sizeof(json), &ok);
        snprintf(detail, sizeof(detail),
                 "%s decoder=%d encoder=%d progress=%d reason=%s",
                 hw_info, (int)report.decoder, (int)report.encoder,
                 hw_p.ticks,
                 report.fallback_reason[0] ? report.fallback_reason : "none");
        check(ok && has(hw_info, "h264"), "Hardware-route output", detail);
    } else {
        check(0, "Hardware-route output", "skipped");
    }

    /* 6. Re-decode the hardware-produced mp4 with FFmpeg software. Container
     *    metadata alone does not prove the hardware bitstream is valid. */
    if (rc == 0) {
        char verify_dst[512];
        snprintf(verify_dst, sizeof(verify_dst), "%s/selftest_verify.mp4", dir);
        err = NULL;
        progress_t verify_p = {0, 0.0f};
        int32_t vrc = ct_ff_transcode(dst, verify_dst,
                                      "scale=160:90,fps=10", "libx264",
                                      on_progress, &verify_p, &err);
        snprintf(detail, sizeof(detail), "software re-decode rc=%d progress=%d %s",
                 (int)vrc, verify_p.ticks, err ? err : "");
        check(vrc == 0, "Hardware output software re-decode", detail);
        ct_ff_string_free(err);
    } else {
        check(0, "Hardware output software re-decode", "skipped");
    }

    /* 7. Hardware capability query must not crash without a platform backend. */
    {
        char *caps = NULL;
        int32_t crc = ct_pipeline_hardware_caps(&caps);
        check(crc == 0 && caps != NULL, "Hardware capability query",
              crc == 0 && caps ? caps : "unavailable");
        ct_pipeline_string_free(caps);
    }

    printf("== selftest %d/%d passed ==\n", g_pass, g_pass + g_fail);
    return g_fail == 0 ? 0 : 1;
}
