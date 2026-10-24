/*
 * vid_clock_test.c — host regression for jfvideo/vid_clock.c.  Simulated fixtures,
 * no hardware: the output side consumes frames at rate*(1+drift) of the monotonic
 * clock, the pump renders in 2048-frame steps, and the clock is sampled every 1 ms
 * (more often than the real player does; samples on an unchanged step are ignored
 * by design, so that only makes the test stricter about it).
 *
 * Build (from native_apps):
 *   gcc -Wall -Wextra -O1 -fsanitize=address,undefined -I jfvideo tests/vid_clock_test.c jfvideo/vid_clock.c -lm
 */
#include "vid_clock.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define RATE 44100
#define STEP 2048
#define LEAD 4096

static int fails, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* Output side: D(m) frames consumed by mono time m; the pump fires when D crosses a
 * multiple of STEP (+ optional jitter), adding STEP rendered (and STEP padded when
 * the pad window is open). */
typedef struct {
    double drift;                /* audio runs (1+drift) x the monotonic clock */
    double jitter;               /* +- seconds on each pump's arrival          */
    double pad_a, pad_b;         /* pump times in [a,b) pad silence            */
    unsigned rng;
    long long rendered, padded;
    long k;                      /* pumps done                                  */
    double next_t;               /* mono time of the next pump                  */
    double t0;                   /* mono time at which D was zero               */
    double extra;                /* frames added to D by a counter jump          */
} Sim;

static double jit(Sim *s)
{
    s->rng = s->rng * 1664525u + 1013904223u;
    return s->jitter * (((s->rng >> 8) & 0xffff) / 32768.0 - 1.0);
}
static void sim_init(Sim *s, double drift, double jitter)
{
    *s = (Sim){ .drift = drift, .jitter = jitter, .pad_a = 1e18, .pad_b = 1e18, .rng = 12345 };
    s->next_t = (double)STEP / (RATE * (1.0 + drift)) + jit(s);
}
static double sim_D(const Sim *s, double m) { return (m - s->t0) * RATE * (1.0 + s->drift) + s->extra; }
/* Truth: the media position the listener hears at mono time m (media starts at base). */
static double sim_truth(const Sim *s, double base, double m)
{
    return base + (sim_D(s, m) - (double)s->padded - LEAD) / RATE;
}
/* Advance the pump to mono time m; returns 1 if a pump fired. */
static int sim_pump(Sim *s, double m)
{
    int fired = 0;
    while (m >= s->next_t) {
        int pad = s->next_t >= s->pad_a && s->next_t < s->pad_b;
        s->rendered += STEP;
        if (pad) s->padded += STEP;
        s->k++;
        s->next_t = s->t0 + (double)(s->k + 1) * STEP / (RATE * (1.0 + s->drift)) + jit(s);
        fired = 1;
    }
    return fired;
}

/* (a) staircase + jitter-free: accuracy and monotonicity */
static void test_staircase(void)
{
    VidClock c; Sim s; vid_clock_init(&c, RATE); vid_clock_start(&c, 10.0);
    sim_init(&s, 0.0, 0.0);
    double prev = -1, worst = 0, jump = 0;
    for (int i = 1; i <= 20000; i++) {
        double m = i * 0.001;
        sim_pump(&s, m);
        vid_clock_sample(&c, s.rendered, s.padded, LEAD, m);
        double v = vid_clock_now(&c, m);
        CHECK(v >= prev, "staircase: clock went back %.6f -> %.6f at %.3f", prev, v, m);
        if (prev >= 0 && v - prev > jump) jump = v - prev;
        prev = v;
        if (m > 1.0) { double e = fabs(v - sim_truth(&s, 10.0, m)); if (e > worst) worst = e; }
    }
    CHECK(worst < 0.060, "staircase: worst error %.1f ms", worst * 1e3);
    printf("  staircase: worst error %.1f ms, largest per-call advance %.2f ms\n", worst * 1e3, jump * 1e3);
}

/* (b) 0.5 s of padded silence mid-run: the clock stops, then tracks again */
static void test_underrun(void)
{
    VidClock c; Sim s; vid_clock_init(&c, RATE); vid_clock_start(&c, 0.0);
    sim_init(&s, 0.0, 0.0);
    s.pad_a = 5.0; s.pad_b = 5.5;
    double at_a = 0, at_b = 0, worst_after = 0, prev = -1;
    for (int i = 1; i <= 12000; i++) {
        double m = i * 0.001;
        sim_pump(&s, m);
        vid_clock_sample(&c, s.rendered, s.padded, LEAD, m);
        double v = vid_clock_now(&c, m);
        CHECK(v >= prev, "underrun: clock went back at %.3f", m);
        prev = v;
        if (i == 5000) at_a = v;
        if (i == 5500) at_b = v;
        if (m > 7.5) { double e = fabs(v - sim_truth(&s, 0.0, m)); if (e > worst_after) worst_after = e; }
        if (m > 3.0 && m < 4.9) { double e = fabs(v - sim_truth(&s, 0.0, m)); CHECK(e < 0.060, "underrun: pre-gap error %.1f ms", e * 1e3); }
    }
    CHECK(at_b - at_a <= 0.2, "underrun: clock advanced %.3f s over a 0.5 s gap", at_b - at_a);
    CHECK(worst_after < 0.060, "underrun: error %.1f ms 2 s after resume", worst_after * 1e3);
    printf("  underrun: advance over the 0.5 s gap %.3f s, error after resume %.1f ms\n", at_b - at_a, worst_after * 1e3);
}

/* (c) audio crystal 144 ppm fast, then slow, one simulated hour (5 ms calls) */
static void test_drift(void)
{
    for (int dir = 0; dir < 2; dir++) {
        double drift = dir ? -144e-6 : 144e-6;
        VidClock c; Sim s; vid_clock_init(&c, RATE); vid_clock_start(&c, 0.0);
        sim_init(&s, drift, 0.0);
        double worst = 0; long n = 3600L * 200;
        for (long i = 1; i <= n; i++) {
            double m = (double)i * 0.005;
            sim_pump(&s, m);
            vid_clock_sample(&c, s.rendered, s.padded, LEAD, m);
            double v = vid_clock_now(&c, m);
            if (m > 1.0) { double e = fabs(v - sim_truth(&s, 0.0, m)); if (e > worst) worst = e; }
        }
        CHECK(worst < 0.060, "drift %+.0f ppm: worst error %.1f ms over 1 h", drift * 1e6, worst * 1e3);
        printf("  drift %+4.0f ppm, 1 h: worst error %.1f ms\n", drift * 1e6, worst * 1e3);
    }
}

/* (f) pump arrival jitter must not move the clock by the jitter: slew limit */
static void test_jitter(void)
{
    VidClock c; Sim s; vid_clock_init(&c, RATE); vid_clock_start(&c, 0.0);
    sim_init(&s, 0.0, 0.010);
    double prev = -1, jump = 0, worst = 0;
    for (int i = 1; i <= 30000; i++) {
        double m = i * 0.001;
        sim_pump(&s, m);
        vid_clock_sample(&c, s.rendered, s.padded, LEAD, m);
        double v = vid_clock_now(&c, m);
        if (prev >= 0 && v - prev > jump) jump = v - prev;
        prev = v;
        if (m > 2.0) { double e = fabs(v - sim_truth(&s, 0.0, m)); if (e > worst) worst = e; }
    }
    CHECK(jump < 0.0035, "jitter: clock jumped %.2f ms in one call", jump * 1e3);
    CHECK(worst < 0.060, "jitter: worst error %.1f ms", worst * 1e3);
    printf("  jitter +-10 ms: largest per-call advance %.2f ms, worst error %.1f ms\n", jump * 1e3, worst * 1e3);
}

/* (d) decide() boundaries and offset sign */
static void test_decide(void)
{
    VidClock c; vid_clock_init(&c, RATE);
    double p = 1.0 / 24.0;
    CHECK(vid_clock_decide(&c, 10.0050, 10.0, p) == VID_CLK_WAIT, "5 ms early must wait");
    CHECK(vid_clock_decide(&c, 10.0039, 10.0, p) == VID_CLK_SHOW, "3.9 ms early must show");
    CHECK(vid_clock_decide(&c, 10.0, 10.0, p) == VID_CLK_SHOW, "on time must show");
    CHECK(vid_clock_decide(&c, 10.0 - p * 0.99, 10.0, p) == VID_CLK_SHOW, "just under a period late must show");
    CHECK(vid_clock_decide(&c, 10.0 - p * 1.01, 10.0, p) == VID_CLK_DROP, "just over a period late must drop");
    /* positive offset delays the picture: a frame that was due now becomes early */
    vid_clock_set_offset(&c, 0.1);
    CHECK(vid_clock_decide(&c, 10.0, 10.0, p) == VID_CLK_WAIT, "offset +0.1: on-time frame must now wait");
    CHECK(vid_clock_decide(&c, 9.9, 10.0, p) == VID_CLK_SHOW, "offset +0.1: frame 0.1 s old is now due");
    vid_clock_set_offset(&c, -0.1);
    CHECK(vid_clock_decide(&c, 10.0, 10.0, p) == VID_CLK_DROP, "offset -0.1: on-time frame is now late");
    CHECK(vid_clock_decide(&c, 10.1, 10.0, p) == VID_CLK_SHOW, "offset -0.1: frame 0.1 ahead is now due");
    printf("  decide: boundaries and offset sign checked\n");
}

/* (e) discontinuities: a new voice, and a jump inside one voice */
static void test_snap(void)
{
    VidClock c; Sim s; vid_clock_init(&c, RATE); vid_clock_start(&c, 0.0);
    sim_init(&s, 0.0, 0.0);
    double m = 0;
    for (int i = 1; i <= 3000; i++) { m = i * 0.001; sim_pump(&s, m); vid_clock_sample(&c, s.rendered, s.padded, LEAD, m); vid_clock_now(&c, m); }
    /* seek: new voice, counters restart, base 100 s */
    vid_clock_start(&c, 100.0);
    Sim s2; sim_init(&s2, 0.0, 0.0); s2.t0 = m; s2.next_t += m;
    s2.k = 0;
    double e = 0;
    for (int i = 1; i <= 400; i++) {
        double mm = m + i * 0.001;
        sim_pump(&s2, mm);
        vid_clock_sample(&c, s2.rendered, s2.padded, LEAD, mm);
        double v = vid_clock_now(&c, mm);
        if (i == 300) e = fabs(v - sim_truth(&s2, 100.0, mm));
    }
    CHECK(e < 0.060, "seek: error %.1f ms 0.3 s after start()", e * 1e3);
    /* inside one voice the counters jump 5 s ahead (resume after a long stall) */
    s2.rendered += 5 * RATE;
    s2.extra = 5.0 * RATE;                /* truth moves with it */
    double mm = m + 0.4;
    for (int i = 1; i <= 150; i++) {
        mm += 0.001; sim_pump(&s2, mm);
        vid_clock_sample(&c, s2.rendered, s2.padded, LEAD, mm);
        vid_clock_now(&c, mm);
    }
    double v = vid_clock_now(&c, mm), t = sim_truth(&s2, 100.0, mm);
    CHECK(fabs(v - t) < 0.060, "jump: error %.1f ms 0.15 s after a 5 s counter jump (clock %.3f truth %.3f)", fabs(v - t) * 1e3, v, t);
    printf("  snap: seek error %.1f ms, 5 s jump error %.1f ms\n", e * 1e3, fabs(v - t) * 1e3);
}

int main(void)
{
    test_staircase();
    test_underrun();
    test_drift();
    test_jitter();
    test_decide();
    test_snap();
    printf("vid_clock_test: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
