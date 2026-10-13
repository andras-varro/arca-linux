/*
 * audio_alsa_lost_test.c — the ALSA backend must notice an unplugged USB card on
 * the very next service, even when the free-space query keeps answering.
 *
 * MEASURED on a unit with a probe holding plughw:1,0 (S16, 44100 Hz, 2 ch,
 * buffer 32768, period 2048): from the kernel's "USB disconnect" onward,
 * snd_pcm_avail_update() returned a FROZEN positive value (1543) with no error
 * for 25 s, while snd_pcm_avail() returned -ENODEV from the first sample and
 * snd_pcm_writei() returned -EBADFD.  A backend that sizes its writes from
 * avail_update sees a nearly full ring forever, so the pump asks for nothing,
 * writei is never called, and the loss never surfaces: silence, with no reopen.
 *
 * This compiles the REAL backend in common/audio_out.c (-DAUDIO_OUT_HAVE_ALSA)
 * against tests/alsa_stub/alsa/asoundlib.h, which only declares the ALSA calls;
 * every one is defined below, so the stub plays back exactly that transcript.
 *
 *   L1-L3  CONTROL: a healthy stream (avail_update == avail, positive) is
 *          serviced, written to, and NOT reported lost.
 *   U1-U3  the unplug transcript: one service call fails, reports the device
 *          lost and counts a refused service.
 *
 * Seen failing: against the pre-fix audio_out.c (avail_update in alsa_space)
 * U1-U3 fail and L1-L3 pass.
 *
 * The STALL half: a BlueALSA A2DP PCM was log-measured freezing with every call
 * succeeding — avail answering, the ring holding the lead, nothing consumed —
 * so no error ever reached device_lost and the stream sat silent on the pinned
 * headset.  The stub replays that against a fake monotonic clock
 * (--wrap=clock_gettime) and a fake /sys/class/bluetooth (--wrap=opendir):
 *   C1-C2  CONTROL: a running stream that consumes is never called stalled.
 *   N1-N3  NEGATIVE: a paused ring, a ring below the start threshold, and a
 *          service gap the device played through are not stalls.
 *   S1-S3  the freeze on the pinned headset: not before ALSA_STALL_MS, lost
 *          after it.
 *   R1-R4  the reopen leaves the stalled pin for the next tier, and a stall
 *          there falls to onboard.
 *   T1     a Bluetooth link change makes the pin eligible again.
 *
 * Seen failing: against the pre-fix audio_out.c (no stall detector)
 * 6 of 19 checks fail — S2, S3, R1-R4 — and every control passes.
 *
 * Build (from native_apps/, as tests/run-all.sh does):
 *   gcc -Wall -Wextra -Wno-unused-parameter -I common -DAUDIO_OUT_HAVE_ALSA \
 *       -I tests/alsa_stub -Wl,--wrap=clock_gettime -Wl,--wrap=opendir \
 *       -o build/audio_alsa_lost_test \
 *       tests/audio_alsa_lost_test.c common/audio_out.c common/audio_gen.c -lm
 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <time.h>
#include <dirent.h>
#include <unistd.h>

#include <alsa/asoundlib.h>
#include "../common/audio_out.h"

/* ── The stub device ────────────────────────────────────────────────────── */

#define STUB_RATE    44100u
#define STUB_CH      2u
#define STUB_PERIOD  2048ul
#define STUB_BUFFER  32768ul
#define STUB_FROZEN  1543      /* the measured frozen avail_update */

static struct {
    bool            unplugged;
    unsigned long   queued;      /* frames in the ring */
    unsigned        writes;      /* successful writei calls */
    unsigned long   drain;       /* frames the device plays per avail call */
    bool            no_writes;   /* writei answers -EAGAIN */
    snd_pcm_state_t state;
} stub;

static int stub_pcm_token;

static long healthy_avail(void)
{
    unsigned long d = stub.drain < stub.queued ? stub.drain : stub.queued;
    stub.queued -= d;
    return (long)(STUB_BUFFER - stub.queued);
}

snd_pcm_state_t snd_pcm_state(snd_pcm_t *pcm) { return stub.state; }
const char *snd_pcm_state_name(snd_pcm_state_t s)
{
    static const char *n[] = { "OPEN", "SETUP", "PREPARED", "RUNNING", "XRUN",
                               "DRAINING", "PAUSED", "SUSPENDED", "DISCONNECTED" };
    return (unsigned)s < sizeof(n) / sizeof(n[0]) ? n[s] : "?";
}

/* ── The fake clock and the fake Bluetooth sysfs ────────────────────────── */

static uint32_t fake_ms;

int __real_clock_gettime(clockid_t id, struct timespec *ts);
int __wrap_clock_gettime(clockid_t id, struct timespec *ts)
{
    if (id != CLOCK_MONOTONIC) return __real_clock_gettime(id, ts);
    ts->tv_sec  = (time_t)(fake_ms / 1000u);
    ts->tv_nsec = (long)(fake_ms % 1000u) * 1000000L;
    return 0;
}

static char bt_dir[64];
DIR *__real_opendir(const char *name);
DIR *__wrap_opendir(const char *name)
{
    if (name && strcmp(name, "/sys/class/bluetooth") == 0) name = bt_dir;
    return __real_opendir(name);
}

static void bt_link(const char *entry)
{
    char p[128];
    snprintf(p, sizeof(p), "%s/%s", bt_dir, entry);
    FILE *f = fopen(p, "w");
    if (f) fclose(f);
}

int snd_pcm_open(snd_pcm_t **pcm, const char *name, snd_pcm_stream_t s, int mode)
{
    *pcm = (snd_pcm_t *)&stub_pcm_token;
    return 0;
}
int snd_pcm_close(snd_pcm_t *pcm)   { return 0; }
int snd_pcm_prepare(snd_pcm_t *pcm) { return stub.unplugged ? -ENODEV : 0; }
int snd_pcm_resume(snd_pcm_t *pcm)  { return stub.unplugged ? -ENODEV : 0; }

snd_pcm_sframes_t snd_pcm_avail(snd_pcm_t *pcm)
{
    return stub.unplugged ? -ENODEV : healthy_avail();
}
snd_pcm_sframes_t snd_pcm_avail_update(snd_pcm_t *pcm)
{
    return stub.unplugged ? STUB_FROZEN : healthy_avail();
}
snd_pcm_sframes_t snd_pcm_writei(snd_pcm_t *pcm, const void *buf, snd_pcm_uframes_t n)
{
    if (stub.unplugged) return -EBADFD;
    if (stub.no_writes) return -EAGAIN;
    if (n > STUB_BUFFER - stub.queued) n = STUB_BUFFER - stub.queued;
    if (n == 0) return -EAGAIN;
    stub.queued += n;
    stub.writes++;
    return (snd_pcm_sframes_t)n;
}
const char *snd_strerror(int e)                 { return e < 0 ? strerror(-e) : strerror(e); }
int snd_pcm_format_width(snd_pcm_format_t f)    { return 16; }

int snd_pcm_hw_params_any(snd_pcm_t *p, snd_pcm_hw_params_t *h)                       { return 0; }
int snd_pcm_hw_params_set_access(snd_pcm_t *p, snd_pcm_hw_params_t *h, snd_pcm_access_t a) { return 0; }
int snd_pcm_hw_params_set_format(snd_pcm_t *p, snd_pcm_hw_params_t *h, snd_pcm_format_t f) { return 0; }
int snd_pcm_hw_params_set_rate_near(snd_pcm_t *p, snd_pcm_hw_params_t *h, unsigned *v, int *d)
{ *v = STUB_RATE; return 0; }
int snd_pcm_hw_params_set_channels_near(snd_pcm_t *p, snd_pcm_hw_params_t *h, unsigned *v)
{ *v = STUB_CH; return 0; }
int snd_pcm_hw_params_set_period_size_near(snd_pcm_t *p, snd_pcm_hw_params_t *h,
                                           snd_pcm_uframes_t *v, int *d)
{ *v = STUB_PERIOD; return 0; }
int snd_pcm_hw_params_set_buffer_size_near(snd_pcm_t *p, snd_pcm_hw_params_t *h,
                                           snd_pcm_uframes_t *v)
{ *v = STUB_BUFFER; return 0; }
int snd_pcm_hw_params(snd_pcm_t *p, snd_pcm_hw_params_t *h)   /* a real open: empty ring */
{ stub.queued = 0; return 0; }
int snd_pcm_hw_params_get_rate(const snd_pcm_hw_params_t *h, unsigned *v, int *d)
{ *v = STUB_RATE; return 0; }
int snd_pcm_hw_params_get_channels(const snd_pcm_hw_params_t *h, unsigned *v)
{ *v = STUB_CH; return 0; }
int snd_pcm_hw_params_get_period_size(const snd_pcm_hw_params_t *h, snd_pcm_uframes_t *v, int *d)
{ *v = STUB_PERIOD; return 0; }
int snd_pcm_hw_params_get_buffer_size(const snd_pcm_hw_params_t *h, snd_pcm_uframes_t *v)
{ *v = STUB_BUFFER; return 0; }

int snd_pcm_sw_params_current(snd_pcm_t *p, snd_pcm_sw_params_t *s) { return 0; }
int snd_pcm_sw_params_set_start_threshold(snd_pcm_t *p, snd_pcm_sw_params_t *s,
                                          snd_pcm_uframes_t v) { return 0; }
int snd_pcm_sw_params(snd_pcm_t *p, snd_pcm_sw_params_t *s) { return 0; }

/* ── Checks ─────────────────────────────────────────────────────────────── */

static int checks, failures;

static void check(bool ok, const char *what)
{
    checks++;
    if (!ok) failures++;
    printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
}

/* Service every 100 ms of fake time for `ms`: the elapsed ms at which the
 * stream was first reported lost, or -1 if it never was. */
static long run_for(AudioOut *o, long ms)
{
    for (long t = 100; t <= ms; t += 100) {
        fake_ms += 100;
        audio_out_service(o);
        if (audio_out_device_lost(o)) return t;
    }
    return -1;
}

static bool path_is(const char *path, const char *want)
{
    return path && strcmp(path, want) == 0;
}

/* The stall half (file header).  Returns nonzero only if the fixture fails. */
static int stall_cases(void)
{
    AudioOut out;
    const char *path = NULL;

    snprintf(bt_dir, sizeof(bt_dir), "/tmp/arca_alsa_stall_XXXXXX");
    if (!mkdtemp(bt_dir)) { printf("FAILED  mkdtemp\n"); return 1; }
    bt_link("hci0:11");                         /* one ACL link: the headset */
    audio_out_set_device_pref("auto");
    audio_out_set_bt_addr("00:11:22:33:44:55");
    memset(&stub, 0, sizeof(stub));
    fake_ms = 100000;

    printf("control: a running stream that consumes\n");
    stub.state = SND_PCM_STATE_RUNNING;
    stub.drain = STUB_PERIOD;
    if (audio_out_open_resolved(&out, 44100, 2, &path) != 0) {
        printf("FAILED  the stub stream would not open\n");
        return 1;
    }
    check(path_is(path, "bluealsa-pin"), "C1 the pinned headset is resolved first");
    check(run_for(&out, 4000) < 0, "C2 a consuming stream is not called stalled in 4 s");

    printf("negative: paused, below the start threshold, a gap played through\n");
    stub.drain = 0;
    stub.state = SND_PCM_STATE_PAUSED;
    check(run_for(&out, 4000) < 0, "N1 a paused ring is not a stall");
    stub.state     = SND_PCM_STATE_PREPARED;
    stub.no_writes = true;
    stub.queued    = STUB_PERIOD / 2;
    check(run_for(&out, 4000) < 0, "N2 a ring below the start threshold is not a stall");
    stub.state     = SND_PCM_STATE_RUNNING;
    stub.no_writes = false;
    run_for(&out, 1500);                        /* owed, nothing played for 1.4 s */
    fake_ms     += 5000;                        /* no service for 5 s ...          */
    stub.queued -= STUB_PERIOD;                 /* ... and the device played on    */
    audio_out_service(&out);
    check(!audio_out_device_lost(&out), "N3 a service gap the device played through is not a stall");

    printf("stall: the pinned headset's ring freezes, every call succeeding\n");
    check(run_for(&out, 1900) < 0, "S1 no stall is reported before 2 s without progress");
    check(run_for(&out, 400) > 0, "S2 the frozen stream is reported lost by 2.3 s");
    check(audio_out_service(&out) < 0, "S3 a stalled stream keeps failing until reopened");

    printf("reopen: the stalled pin is not resolved again\n");
    audio_out_close(&out);
    path = NULL;
    audio_out_open_resolved(&out, 44100, 2, &path);
    check(!path_is(path, "bluealsa-pin"), "R1 the reopen does not pick the stalled pin");
    check(path_is(path, "bluealsa"), "R2 the reopen takes the next tier, any Bluetooth sink");
    check(run_for(&out, 2500) > 0, "R3 that sink freezing too is reported lost");
    audio_out_close(&out);
    path = NULL;
    audio_out_open_resolved(&out, 44100, 2, &path);
    check(path_is(path, "/dev/dsp"), "R4 with both Bluetooth tiers stalled, onboard plays");

    printf("return: a link change makes the pin eligible again\n");
    bt_link("hci0:12");
    check(path_is(audio_out_device_path(), "bluealsa-pin"),
          "T1 after a Bluetooth link change the pinned headset resolves again");
    audio_out_close(&out);

    char p[128];
    snprintf(p, sizeof(p), "%s/hci0:11", bt_dir); unlink(p);
    snprintf(p, sizeof(p), "%s/hci0:12", bt_dir); unlink(p);
    rmdir(bt_dir);
    return 0;
}

int main(void)
{
    AudioOut out;
    setvbuf(stdout, NULL, _IONBF, 0);   /* keep the lines in step with the library's stderr */
    memset(&stub, 0, sizeof(stub));

    if (audio_out_open_alsa(&out, "plughw:1,0", 44100, 2) != 0) {
        printf("FAILED  the stub ALSA stream would not open\n");
        return 1;
    }

    printf("control: a healthy stream\n");
    /* The ring has been played down to one period: the pump has room to write. */
    stub.queued = STUB_PERIOD;
    unsigned before = stub.writes;
    long r = audio_out_service(&out);
    check(r >= 0, "L1 a healthy service call succeeds");
    check(stub.writes > before, "L2 a healthy service call writes to the device");
    check(!audio_out_device_lost(&out), "L3 a healthy stream is not reported lost");

    printf("unplug: avail_update frozen at %d, avail -ENODEV, writei -EBADFD\n",
           STUB_FROZEN);
    stub.unplugged = true;
    uint32_t refused_before = audio_out_refused(&out);
    errno = 0;
    r = audio_out_service(&out);
    check(r < 0, "U1 the first service after the unplug fails");
    check(audio_out_device_lost(&out), "U2 the first service after the unplug reports the device lost");
    check(audio_out_refused(&out) == refused_before + 1,
          "U3 the failed query is counted as a refused service");

    audio_out_close(&out);

    if (stall_cases() != 0) return 1;

    printf("\n%s  %d checks, %d failure(s)\n",
           failures ? "FAILED" : "PASSED", checks, failures);
    return failures ? 1 : 0;
}
