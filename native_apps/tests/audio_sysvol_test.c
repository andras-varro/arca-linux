/*
 * audio_sysvol_test — the ONE system volume reads and writes the right place, on the host
 *
 * audio_system_volume_get() and audio_system_volume_set() are the two calls every
 * app uses for the device-wide volume step.  On a LIVE bus the step lives in the
 * AudioOut and get() reads it from there; on a CLOSED bus (NULL, or an Audio that
 * never opened) get() reads the `volume_step` key from the config file and set()
 * clamps, writes the key and saves.  This asserts the observable half of that:
 *
 *   A1  closed bus, no config file: get() returns the default (AUDIO_VOLUME_DEFAULT)
 *   A2  closed bus: set(7) returns 7, the file holds `volume_step=7`, get() returns 7
 *   A3  closed bus: set(-3) clamps to 0 (return and file), set(99) clamps to
 *       AUDIO_VOLUME_MAX (return and file)
 *   A4  closed bus: an unrelated key already in the file survives a set()
 *   A5  closed bus: a stored 99 reads back as AUDIO_VOLUME_MAX, a stored -5 as 0
 *   L1  live bus: set(12) moves audio_out_get_volume_step() to 12, get() returns 12,
 *       and the file says 12
 *   L2  live bus: get() returns the LIVE step even when the file says something else
 *
 * ⚠️ **The device is FAKED at one seam, and the config path is FAKED at one seam —
 * both are LINK seams, not source edits.**  `-Wl,--wrap=audio_out_open_resolved`
 * sends audio.c's one device open to __wrap_audio_out_open_resolved() below, which
 * opens the REAL generic layer over an injected AudioOutDev (the same fake
 * audio_carry_test.c drives).  `-Wl,--wrap=config_init` sends every config_init()
 * call made from audio.c to __wrap_config_init() below, which runs the real
 * config_init() and then points cfg->filepath at a temp file.  config.c's own
 * internal calls (e.g. config_audio_device_stored() inside audio_open) are NOT
 * wrapped: they read the real /opt/games path, read-only, and find nothing on a
 * host.  Nothing in this test writes that path.
 *
 * ⚠️ **What this cannot see.**  (1) The real config file format on a unit, the
 * atomic-rename write path under a full /opt, and a failed save (only the message
 * is printed; the return value is not asserted).  (2) The real OSS/ALSA open and
 * the speaker's actual loudness at a given step: the step is a number here, not a
 * sound.  (3) Whether the control panel's volume bar and the launcher agree is not
 * exercised; this is the library's answer only.
 *
 * Build and run (host gcc, from native_apps/):
 *   gcc -Wall -Wextra -Wno-unused-parameter -I. \
 *       -Wl,--wrap=audio_out_open_resolved -Wl,--wrap=config_init \
 *       -o build/audio_sysvol_test tests/audio_sysvol_test.c \
 *       common/audio.c common/audio_gen.c common/audio_out.c common/audio_eq.c \
 *       common/audio_enh.c common/audio_wav.c common/config.c -lm && ./build/audio_sysvol_test
 */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "common/audio.h"
#include "common/audio_out.h"
#include "common/config.h"

static int failures = 0;
static int checks   = 0;

static void check(bool ok, const char *what)
{
    checks++;
    if (ok) printf("    ok   %s\n", what);
    else  { failures++; printf("    FAIL: %s\n", what); }
}

/* ── The config seam ────────────────────────────────────────────────────── */

static char g_cfg_path[128];       /* the temp file every config_init() from audio.c gets */

void __real_config_init(Config *cfg);
void __wrap_config_init(Config *cfg)
{
    __real_config_init(cfg);
    snprintf(cfg->filepath, sizeof cfg->filepath, "%s", g_cfg_path);
}

/* ── The fake device ────────────────────────────────────────────────────── */

static struct {
    bool gone;
    int  grant_rate;
    int  opens;
    int  closes;
    long bytes;
} g_fake;

static int fake_open(void *ctx, int rate_req, int ch_req,
                     int *rate, int *bits, int *ch)
{
    (void)ctx; (void)rate_req;
    *rate = g_fake.grant_rate;
    *bits = 16;
    *ch   = ch_req;
    return 0;
}

static int fake_space(void *ctx, int frame_bytes, AudioOutSpace *sp)
{
    (void)ctx; (void)frame_bytes;
    if (g_fake.gone) { errno = ENODEV; return -1; }
    sp->period_frames = 1024;
    sp->ring_frames   = 8192;
    sp->in_flight     = 0;
    sp->space         = 8192;
    return 0;
}

static ssize_t fake_write(void *ctx, const void *buf, size_t n, bool *again)
{
    (void)ctx; (void)buf;
    *again = false;
    if (g_fake.gone) { errno = ENODEV; return -1; }
    g_fake.bytes += (long)n;
    return (ssize_t)n;
}

static void fake_wait(void *ctx, int usec) { (void)ctx; (void)usec; }
static void fake_close(void *ctx)          { (void)ctx; g_fake.closes++; }

static const AudioOutDev FAKE_DEV = {
    fake_open, fake_space, fake_write, fake_wait, fake_close
};

/* audio.c's only device open. */
int __wrap_audio_out_open_resolved(AudioOut *out, int rate_req, int channels_req,
                                   const char **path_out);
int __wrap_audio_out_open_resolved(AudioOut *out, int rate_req, int channels_req,
                                   const char **path_out)
{
    g_fake.gone = false;
    g_fake.opens++;
    if (audio_out_open(out, &FAKE_DEV, NULL, rate_req, channels_req) != 0) return -1;
    if (path_out) *path_out = "fake-dsp";
    return 0;
}

/* ── File helpers ───────────────────────────────────────────────────────── */

static void write_text(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    if (!f) return;
    fputs(text, f);
    fclose(f);
}

static bool file_exists(const char *path) { return access(path, F_OK) == 0; }

/* Whole file into `buf`; "" if absent.  Config files here are a few lines. */
static void read_text(const char *path, char *buf, size_t cap)
{
    buf[0] = '\0';
    FILE *f = fopen(path, "r");
    if (!f) return;
    size_t n = fread(buf, 1, cap - 1, f);
    buf[n] = '\0';
    fclose(f);
}

/* True if the file has a line that is exactly `line` (no trailing spaces). */
static bool file_has_line(const char *path, const char *line)
{
    char buf[1024];
    read_text(path, buf, sizeof buf);
    size_t len = strlen(line);
    for (const char *p = buf; *p; ) {
        const char *nl = strchr(p, '\n');
        size_t this_len = nl ? (size_t)(nl - p) : strlen(p);
        if (this_len == len && strncmp(p, line, len) == 0) return true;
        if (!nl) break;
        p = nl + 1;
    }
    return false;
}

/* ── main ───────────────────────────────────────────────────────────────── */

int main(void)
{
    /* A temp path that is FREE at the start: mkstemp to claim a name, then drop
     * the file so case A1 really sees "no config file". */
    char tmpl[] = "/tmp/audio_sysvol_XXXXXX";
    int fd = mkstemp(tmpl);
    if (fd < 0) { perror("mkstemp"); return 2; }
    close(fd);
    unlink(tmpl);
    snprintf(g_cfg_path, sizeof g_cfg_path, "%s", tmpl);

    printf("audio_sysvol_test — the system volume, closed and live bus\n");

    /* ── A: closed bus (zeroed Audio = never opened) ─────────────────────── */
    Audio closed;
    memset(&closed, 0, sizeof closed);

    printf("  A1 closed bus, no config file -> the default\n");
    check(audio_system_volume_get(&closed) == AUDIO_VOLUME_DEFAULT,
          "A1: get() with no file returns AUDIO_VOLUME_DEFAULT");
    check(audio_system_volume_get(NULL) == AUDIO_VOLUME_DEFAULT,
          "A1: get(NULL) returns AUDIO_VOLUME_DEFAULT");
    check(!file_exists(g_cfg_path), "A1: get() did not create the file");

    printf("  A2 closed bus: set(7) writes the key and get() reads it back\n");
    check(audio_system_volume_set(&closed, 7) == 7, "A2: set(7) returns 7");
    check(file_has_line(g_cfg_path, "volume_step=7"), "A2: file contains volume_step=7");
    check(audio_system_volume_get(&closed) == 7, "A2: get() returns 7");

    printf("  A3 closed bus: out-of-range steps clamp, return and file agree\n");
    check(audio_system_volume_set(&closed, -3) == 0, "A3: set(-3) returns 0");
    check(file_has_line(g_cfg_path, "volume_step=0"), "A3: file contains volume_step=0");
    check(audio_system_volume_set(&closed, 99) == AUDIO_VOLUME_MAX,
          "A3: set(99) returns AUDIO_VOLUME_MAX");
    check(file_has_line(g_cfg_path, "volume_step=14"), "A3: file contains volume_step=14");

    printf("  A4 closed bus: an unrelated key in the file survives a set\n");
    write_text(g_cfg_path, "foo=bar\n");
    check(audio_system_volume_set(&closed, 5) == 5, "A4: set(5) returns 5");
    check(file_has_line(g_cfg_path, "foo=bar"), "A4: foo=bar survives the set");
    check(file_has_line(g_cfg_path, "volume_step=5"), "A4: file contains volume_step=5");

    printf("  A5 closed bus: a stored value is clamped on read\n");
    write_text(g_cfg_path, "volume_step=99\n");
    check(audio_system_volume_get(&closed) == AUDIO_VOLUME_MAX,
          "A5: stored 99 reads back as AUDIO_VOLUME_MAX");
    write_text(g_cfg_path, "volume_step=-5\n");
    check(audio_system_volume_get(&closed) == 0, "A5: stored -5 reads back as 0");

    /* ── L: live bus ─────────────────────────────────────────────────────── */
    printf("  L1 live bus: set(12) moves the live step and the file\n");
    unlink(g_cfg_path);
    memset(&g_fake, 0, sizeof g_fake);
    g_fake.grant_rate = 44100;
    Audio live;
    check(audio_init_unchecked(&live) == 0 && live.available,
          "L1: the fake stream opens (live bus)");
    check(audio_system_volume_set(&live, 12) == 12, "L1: set(12) returns 12");
    check(audio_out_get_volume_step(&live.out) == 12,
          "L1: audio_out_get_volume_step() == 12 on the live out");
    check(audio_system_volume_get(&live) == 12, "L1: get() returns 12");
    check(file_has_line(g_cfg_path, "volume_step=12"), "L1: file contains volume_step=12");

    printf("  L2 live bus: get() returns the live step, not the file\n");
    audio_out_set_volume_step(&live.out, 9);
    write_text(g_cfg_path, "volume_step=3\n");
    check(audio_system_volume_get(&live) == 9,
          "L2: get() returns the live 9 while the file says 3");
    audio_close(&live);

    unlink(g_cfg_path);
    printf("\n%s  %d check(s), %d failure(s)\n",
           failures ? "FAILED" : "PASSED", checks, failures);
    return failures ? 1 : 0;
}
