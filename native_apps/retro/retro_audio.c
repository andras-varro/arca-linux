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
static int open_ok, primed;
static double step, ratio = 1.0, pos;   /* source samples per device sample */
static int16_t last_s, prev_s;
static unsigned target, underruns;
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
    if (got < frames) underruns++;
    return got;
}

int aud_open(double core_rate, double core_fps)
{
    (void)core_fps;
    audio_out_set_device_pref(config_audio_device_stored());
    const char *path = NULL;
    if (audio_out_open_resolved(&out, 44100, 1, &path) != 0) {
        fprintf(stderr, "retro: audio open failed, running silent\n"); return -1;
    }
    audio_out_set_shift(&out, 1);        /* the -6 dB every client here applies */
    int rate = audio_out_rate(&out);
    step = core_rate / rate;
    long per = audio_out_period(&out);
    target = (unsigned)(per > 0 ? per * 3 / 2 : 3072) + (unsigned)(rate / 25);
    if (target > RING / 2) target = RING / 2;
    audio_out_set_fill(&out, fill, NULL, "retro");
    starved0 = audio_out_starved(&out);
    open_ok = 1;
    fprintf(stderr, "retro: audio %s %d Hz x%d (core %.0f Hz), period %ld, ring target %u\n",
            path ? path : "?", rate, audio_out_channels(&out), core_rate, per, target);
    return 0;
}

void aud_push(const int16_t *st, size_t frames)
{
    if (!open_ok) return;
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
    if (!open_ok) return;
    if (audio_out_device_lost(&out)) { fprintf(stderr, "retro: audio device lost\n"); aud_close(); return; }
    audio_out_service(&out);
}

void aud_stats(unsigned *ru, unsigned *ds, double *lms, double *rt)
{
    *ru = underruns; underruns = 0;
    uint32_t s = open_ok ? audio_out_starved(&out) : 0;
    *ds = s - starved0; starved0 = s;
    *lms = open_ok ? 1000.0 * level() / audio_out_rate(&out) : 0;
    *rt = ratio;
}

void aud_close(void)
{
    if (!open_ok) return;
    audio_out_set_fill(&out, NULL, NULL, NULL);
    audio_out_close(&out);
    open_ok = 0;
}

void aud_pause(void)
{
    r_r = r_w; primed = 0; pos = 0; last_s = prev_s = 0;   /* silence until aud_push reaches the target again */
}
