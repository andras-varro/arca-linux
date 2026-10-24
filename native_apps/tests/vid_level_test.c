/*
 * vid_level_test.c — host regression for jfvideo/vid_level.c.  Speech-like fixture:
 * 10 harmonics of a wandering ~120 Hz voice, ~4 Hz syllabic amplitude modulation,
 * a noise floor, and a pause every 4 s (alternately digital silence and low noise),
 * normalised to -18 dBFS PEAK (what the server's downmix delivers).  44100 Hz.
 *
 * Build (from native_apps):
 *   gcc -Wall -Wextra -O1 -fsanitize=address,undefined -I jfvideo tests/vid_level_test.c jfvideo/vid_level.c -lm
 */
#include "vid_level.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RATE 44100
#define BLK  1152
#define CEIL_S 29204

static int fails, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static unsigned rng = 777;
static double urand(void) { rng = rng * 1664525u + 1013904223u; return ((rng >> 8) & 0xffff) / 32768.0 - 1.0; }
static double dbfs(double ms) { return 10.0 * log10(ms > 1e-20 ? ms : 1e-20); }

/* `secs` of speech, normalised to peak_db; pauses are digital silence / -65 dBFS noise.
 * Returns interleaved floats (nch copies with different noise and a 0.8 scale on ch 1). */
static float *speech(double secs, int nch, double peak_db, int *frames_out)
{
    int n = (int)(secs * RATE);
    float *m = malloc(sizeof(float) * n), *buf = malloc(sizeof(float) * n * nch);
    double ph[10]; for (int k = 0; k < 10; k++) ph[k] = urand() * 3.14159;
    double pk = 0;
    for (int i = 0; i < n; i++) {
        double t = (double)i / RATE, cyc = fmod(t, 4.0);
        double v;
        if (cyc >= 3.0) {
            int digital = ((int)(t / 4.0)) & 1;
            v = digital ? 0.0 : 0.00056 * urand() * 1.732;      /* -65 dBFS rms */
        } else {
            double f0 = 120.0 * (1.0 + 0.1 * sin(2 * 3.14159265 * 0.3 * t));
            double env = 0.5 + 0.5 * sin(2 * 3.14159265 * 4.0 * t); env *= env;
            v = 0;
            for (int k = 1; k <= 10; k++) v += sin(2 * 3.14159265 * f0 * k * t + ph[k - 1]) / k;
            v = v * env + 0.0008 * urand() * 1.732;             /* ~ -62 dBFS floor */
        }
        m[i] = (float)v; if (fabs(v) > pk) pk = fabs(v);
    }
    double sc = pow(10.0, peak_db / 20.0) / pk;
    for (int i = 0; i < n; i++) {
        buf[i * nch] = (float)(m[i] * sc);
        if (nch == 2) buf[i * nch + 1] = (float)(m[i] * sc * 0.8 + 0.0003 * urand());
    }
    free(m); *frames_out = n; return buf;
}

typedef struct { double act_pw; int act_blocks; int maxabs; float gmin, gmax; float gend; } Run;

/* Process buf block by block; statistics over blocks starting at or after t_from. */
static Run run(VidLevel *l, const float *buf, int frames, int nch, double t_from)
{
    Run r = { 0, 0, 0, 1e9f, -1e9f, 0 };
    int16_t *out = malloc(sizeof(int16_t) * BLK * nch);
    for (int f = 0; f + BLK <= frames; f += BLK) {
        vid_level_process(l, buf + (size_t)f * nch, BLK, out);
        double sq = 0, osq = 0;
        for (int i = 0; i < BLK * nch; i++) {
            sq += (double)buf[(size_t)f * nch + i] * buf[(size_t)f * nch + i];
            double o = out[i] / 32767.0; osq += o * o;
            if (abs(out[i]) > r.maxabs) r.maxabs = abs(out[i]);
        }
        if ((double)f / RATE >= t_from) {
            float g = vid_level_gain(l);
            if (g < r.gmin) r.gmin = g;
            if (g > r.gmax) r.gmax = g;
            if (dbfs(sq / (BLK * nch)) > -55.0) { r.act_pw += osq / (BLK * nch); r.act_blocks++; }
        }
    }
    r.gend = vid_level_gain(l);
    free(out);
    return r;
}

static void test_speech(int nch)
{
    int n; float *x = speech(28.0, nch, -18.0, &n);
    /* check the fixture is what it claims: crest of the active part */
    double sq = 0; long cnt = 0; float pk = 0;
    for (int i = 0; i < n * nch; i++) { double a = fabs(x[i]); if (a > pk) pk = (float)a; if (a > 0.002) { sq += a * a; cnt++; } }
    double crest = 20 * log10(pk) - dbfs(sq / cnt);
    CHECK(crest > 8.0 && crest < 16.0, "fixture crest %.1f dB is not speech-like", crest);
    VidLevel l; vid_level_init(&l, RATE, nch);
    Run r = run(&l, x, n, nch, 5.0);
    double lvl = dbfs(r.act_pw / r.act_blocks);
    CHECK(fabs(lvl + 20.0) <= 2.5, "%dch: active output level %.2f dBFS, want -20 +-2.5", nch, lvl);
    CHECK(r.maxabs <= CEIL_S, "%dch: output peak %d over ceiling %d", nch, r.maxabs, CEIL_S);
    CHECK(r.gmin >= 0.999f && r.gmax <= 7.95f, "%dch: gain %.3f..%.3f outside 0..+18 dB", nch, r.gmin, r.gmax);
    printf("  speech %dch: crest %.1f dB, active level %.2f dBFS, gain %.1f..%.1f dB, peak %d\n", nch, crest, lvl, 20 * log10(r.gmin), 20 * log10(r.gmax), r.maxabs);
    free(x);
}

static void test_burst(int nch, double burst_peak)
{
    int nq, nb; float *q = speech(14.0, nch, -38.0, &nq), *b = speech(3.0, nch, 20 * log10(burst_peak), &nb);
    VidLevel l; vid_level_init(&l, RATE, nch);
    Run rq = run(&l, q, nq, nch, 0.0);
    CHECK(rq.gend > 4.0f, "burst setup: gain only %.2f after quiet speech", rq.gend);
    Run rb = run(&l, b, nb, nch, 0.0);
    CHECK(rb.maxabs <= CEIL_S, "burst peak %.1f: output %d over ceiling %d", burst_peak, rb.maxabs, CEIL_S);
    printf("  burst peak %.1f (gain before %.1f dB): output peak %d\n", burst_peak, 20 * log10(rq.gend), rb.maxabs);
    free(q); free(b);
}

static void test_no_pumping(int digital)
{
    int ns, nq; float *s = speech(12.0, 1, -18.0, &ns);
    nq = 15 * RATE; float *q = calloc(nq, sizeof(float));
    if (!digital) for (int i = 0; i < nq; i++) q[i] = (float)(0.000316 * 1.732 * urand());   /* -70 dBFS rms */
    VidLevel l; vid_level_init(&l, RATE, 1);
    Run rs = run(&l, s, ns, 1, 0.0);
    Run rq = run(&l, q, nq, 1, 0.0);
    CHECK(rq.gmax <= rs.gend + 1e-6f, "%s: gain rose from %.3f to %.3f during silence", digital ? "digital" : "noise", rs.gend, rq.gmax);
    CHECK(digital ? rq.maxabs == 0 : rq.maxabs < 700, "%s: output peak %d after speech", digital ? "digital" : "noise", rq.maxabs);
    printf("  %s silence 15 s: gain %.2f -> max %.2f, output peak %d\n", digital ? "digital" : "-70 dBFS noise", rs.gend, rq.gmax, rq.maxabs);
    free(s); free(q);
}

static void test_fixed(void)
{
    int n = BLK * 40; float *x = malloc(sizeof(float) * n), *y = malloc(sizeof(float) * n);
    for (int i = 0; i < n; i++) { x[i] = (float)(0.1 * sin(2 * 3.14159265 * 440.0 * i / RATE)); y[i] = 4.0f * x[i] * 2.2f; }   /* y peaks 0.88 */
    VidLevel l; vid_level_init(&l, RATE, 1); vid_level_set(&l, 0, 6.0f);
    int16_t *o = malloc(sizeof(int16_t) * n);
    for (int f = 0; f + BLK <= n; f += BLK) vid_level_process(&l, x + f, BLK, o + f);
    float g = vid_level_gain(&l);
    CHECK(fabsf(g - 1.99526f) < 0.001f, "fixed: gain %.4f, want 1.9953", g);
    int m = 0; for (int i = BLK * 2; i < n; i++) if (abs(o[i]) > m) m = abs(o[i]);
    CHECK(abs(m - (int)(0.1 * 1.99526 * 32767)) <= 3, "fixed: peak %d, want %d", m, (int)(0.1 * 1.99526 * 32767));
    for (int f = 0; f + BLK <= n; f += BLK) vid_level_process(&l, y + f, BLK, o + f);   /* x2 -> over the ceiling at +6 dB */
    m = 0; for (int i = 0; i < n; i++) if (abs(o[i]) > m) m = abs(o[i]);
    CHECK(m <= CEIL_S, "fixed: limiter let %d through", m);
    printf("  fixed +6 dB: gain %.4f, quiet peak ok, loud peak %d\n", g, m);
    free(x); free(y); free(o);
}

static void test_gated_start(void)
{
    int n = BLK * 200; float *q = malloc(sizeof(float) * n); int16_t *o = malloc(sizeof(int16_t) * n);
    for (int i = 0; i < n; i++) q[i] = (float)(0.000316 * 1.732 * urand());
    VidLevel l; vid_level_init(&l, RATE, 1);
    for (int f = 0; f + BLK <= n; f += BLK) vid_level_process(&l, q + f, BLK, o + f);
    CHECK(vid_level_gain(&l) == 1.0f, "noise-only start moved the gain to %.3f", vid_level_gain(&l));
    printf("  noise-only start: gain stays %.2f\n", vid_level_gain(&l));
    free(q); free(o);
}

int main(void)
{
    test_speech(1);
    test_speech(2);
    test_burst(1, 1.0);
    test_burst(2, 3.0);
    test_no_pumping(0);
    test_no_pumping(1);
    test_fixed();
    test_gated_start();
    printf("vid_level_test: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
