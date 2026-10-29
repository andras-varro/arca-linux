/* retro audio: core stereo -> mono, linear resample to the device rate with a
 * small rate correction that holds a mono ring at its target level, drained by
 * audio_out's fill callback from the main loop's audio_out_service() (no thread). */
#include "retro.h"
#include "audio_out.h"
#include "config.h"
#include <stdio.h>
#include <string.h>

#define RING 65536            /* mono samples, power of two */
static AudioOut out;
static int16_t ring[RING];
static volatile unsigned r_w, r_r;     /* free-running indices */
static int live, want, primed;      /* live: a device is open; want: the session wants one (reopen when lost) */
static double core_rate_s, retry_at;
static double step, ratio = 1.0, pos;   /* source samples per device sample */
static int16_t last_s, prev_s;
static unsigned target, underruns, underruns_all, ub_seen;   /* underruns_all: never reset, for aud_buffer_status */
static uint32_t starved0;

static unsigned level(void) { return r_w - r_r; }

static long fill(void *ctx, int16_t *buf, long frames, int ch)
{
    (void)ctx;
    long got = 0;
    if (!primed) return 0;               /* silence until the ring reaches its target */
    while (got < frames && level() > 0) {
        int16_t s = ring[r_r & (RING - 1)]; r_r++;
        for (int c = 0; c < ch; c++) buf[got * ch + c] = s;
        got++;
    }
    if (got < frames) { underruns++; underruns_all++; }
    return got;
}

static void dev_close(void)
{
    if (!live) return;
    audio_out_set_fill(&out, NULL, NULL, NULL);
    audio_out_close(&out);
    live = 0;
}

static int dev_open(void)
{
    const char *path = NULL;
    /* 2 ch, as the games ask: mono on plug:bluealsa broke up on a VT360 speaker and was clean as
     * stereo (heard on the panel, 2026-10-29); fill() copies each sample to every channel */
    if (audio_out_open_resolved(&out, 44100, 2, &path) != 0) return -1;
    audio_out_set_shift(&out, 1);        /* the -6 dB every client here applies */
    int rate = audio_out_rate(&out);
    step = core_rate_s / rate;           /* the resampler follows whatever this sink granted */
    long per = audio_out_period(&out);
    target = (unsigned)(per > 0 ? per * 3 / 2 : 3072) + (unsigned)(rate / 25);
    if (target > RING / 2) target = RING / 2;
    r_r = r_w; primed = 0; pos = 0;
    audio_out_set_fill(&out, fill, NULL, "retro");
    starved0 = audio_out_starved(&out);
    live = 1;
    fprintf(stderr, "retro: audio %s %d Hz x%d (core %.0f Hz), period %ld, ring target %u\n",
            path ? path : "?", rate, audio_out_channels(&out), core_rate_s, per, target);
    return 0;
}

int aud_open(double core_rate, double core_fps)
{
    (void)core_fps;
    audio_out_set_device_pref(config_audio_device_stored());
    audio_out_set_bt_addr(config_audio_bt_addr_stored());   /* the pinned headset, as audio.c does; without it plug:bluealsa picks the most recently connected sink */
    core_rate_s = core_rate; want = 1;
    if (dev_open() == 0) return 0;
    fprintf(stderr, "retro: audio open failed, retrying each second\n");
    retry_at = now_s() + 1.0;
    return -1;
}

void aud_push(const int16_t *st, size_t frames)
{
    if (!live) return;
    /* rate correction: +-0.5 % pulling the ring toward its target */
    double err = ((double)target - (double)level()) / (double)target;
    if (err > 1) err = 1;
    if (err < -1) err = -1;
    ratio = 1.0 + 0.005 * err;
    double st_eff = step / ratio;        /* fewer source samples per output = more output */
    for (size_t i = 0; i < frames; i++) {
        prev_s = last_s;
        last_s = (int16_t)(((int)st[2 * i] + (int)st[2 * i + 1]) >> 1);
        while (pos < 1.0) {
            int v = prev_s + (int)((last_s - prev_s) * pos);
            if (level() < RING - 1) { ring[r_w & (RING - 1)] = (int16_t)v; r_w++; }
            pos += st_eff;
        }
        pos -= 1.0;
    }
    if (!primed && level() >= target) primed = 1;
}

void aud_service(void)
{
    if (!want) return;
    /* Failover and take-back, ScummVM's oss-mixer.cpp loop: audio_out decides (the lost flag, and
     * audio_out_usb_returned() for a replugged USB card or the pinned headset coming back); the
     * owner closes and reopens through its own path.  No device: one open attempt per second. */
    if (live && audio_out_device_lost(&out)) { fprintf(stderr, "retro: audio device lost, reopening\n"); dev_close(); }
    else if (live && audio_out_usb_returned(&out)) dev_close();
    if (!live) {
        double t = now_s();
        if (t < retry_at) return;
        if (dev_open() != 0) { retry_at = t + 1.0; return; }
    }
    audio_out_service(&out);
}

void aud_stats(unsigned *ru, unsigned *ds, double *lms, double *rt)
{
    *ru = underruns; underruns = 0;
    uint32_t s = live ? audio_out_starved(&out) : 0;
    *ds = s - starved0; starved0 = s;
    *lms = live ? 1000.0 * level() / audio_out_rate(&out) : 0;
    *rt = ratio;
}

void aud_close(void) { want = 0; dev_close(); }

void aud_pause(void)
{
    r_r = r_w; primed = 0; pos = 0; last_s = prev_s = 0;   /* silence until aud_push reaches the target again */
}

/* For the core's audio-buffer-status callback (env 62): fill as % of the ring target, clamped to 100;
 * *underrun = the device drained the ring dry since the last call.  Returns false until a device is open and primed.
 * One divide per call, once per core run (~50/s). */
bool aud_buffer_status(unsigned *pct, bool *underrun)
{
    unsigned u = underruns_all;
    *underrun = u != ub_seen; ub_seen = u;
    if (!live || !primed || !target) { *pct = 0; return false; }
    unsigned p = level() * 100 / target;
    *pct = p > 100 ? 100 : p;
    return true;
}
