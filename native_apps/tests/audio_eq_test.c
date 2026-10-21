/* audio_eq_test: the system equaliser core (common/audio_eq.c).
 *
 * Build (from native_apps/):
 *   gcc -Wall -Wextra -Wno-unused-parameter -I common -o build/audio_eq_test \
 *       tests/audio_eq_test.c common/audio_eq.c -lm && ./build/audio_eq_test
 *
 * The module is new, so there is no pre-fix source to compile against; the
 * negative controls are in measure_audio_eq_sabotage.sh, which breaks a COPY
 * of audio_eq.c in single ways and counts the failures here. */
#include "audio_eq.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails = 0, checks = 0;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* Gain in dB of the EQ at `hz` for a sine of `amp`, channel 0 of `ch`, measured
 * as RMS over the last half of 1 s (after the transient). */
static double gain_at(int rate, const int g[EQ_BANDS], double hz, int amp, int ch)
{
    EqParams p; EqState s;
    eq_design(&p, rate, g);
    eq_state_reset(&s);
    long n = rate;
    int16_t *buf = malloc((size_t)n * ch * sizeof(int16_t));
    for (long i = 0; i < n; i++)
        for (int c = 0; c < ch; c++)
            buf[i * ch + c] = (int16_t)floor(amp * sin(2.0 * M_PI * hz * i / rate) + 0.5);
    int16_t *ref = malloc((size_t)n * ch * sizeof(int16_t));
    memcpy(ref, buf, (size_t)n * ch * sizeof(int16_t));
    /* two blocks, to exercise state carry across calls */
    long half = n / 2;
    eq_process(&s, &p, buf, half, ch);
    eq_process(&s, &p, buf + half * ch, n - half, ch);
    double si = 0, so = 0;
    for (long i = half; i < n; i++) {
        double a = ref[i * ch], b = buf[i * ch];
        si += a * a; so += b * b;
    }
    free(buf); free(ref);
    return 10.0 * log10(so / si);
}

int main(void)
{
    const int R = 44100;
    int flat[EQ_BANDS] = {0, 0, 0, 0, 0};

    /* 1. flat = bit-exact passthrough, mono and stereo */
    {
        EqParams p; EqState s; eq_design(&p, R, flat);
        CHECK(p.n == 0, "flat design has %d stages", p.n);
        int16_t a[4000], b[4000];
        for (int i = 0; i < 4000; i++) a[i] = b[i] = (int16_t)((i * 7919) % 65536 - 32768);
        eq_state_reset(&s);
        eq_process(&s, &p, a, 2000, 2);
        CHECK(memcmp(a, b, sizeof(a)) == 0, "flat stereo not bit exact");
        eq_process(&s, &p, a, 4000, 1);
        CHECK(memcmp(a, b, sizeof(a)) == 0, "flat mono not bit exact");
    }

    /* 2. a single band at +6 dB: +6 at the centre, ~0 two octaves away */
    {
        int g[EQ_BANDS] = {0, 0, 0, 0, 0};
        g[2] = 6;                                   /* 1 kHz peaking */
        double at = gain_at(R, g, 1000, 8000, 1);
        double lo = gain_at(R, g, 250, 8000, 1) ;
        double hi = gain_at(R, g, 4000, 8000, 1);
        /* the headroom pregain takes max-boost off the whole chain: -6 dB */
        CHECK(fabs(at - 0.0) < 0.5, "1k +6: centre net %.2f dB (expect 0 after -6 pregain)", at);
        CHECK(fabs((at - lo) - 6.0) < 0.6, "1k +6: centre-250 = %.2f", at - lo);
        CHECK(fabs((at - hi) - 6.0) < 0.6, "1k +6: centre-4k = %.2f", at - hi);
        CHECK(fabs(lo + 6.0) < 0.6, "250 Hz net %.2f dB, expect -6 (pregain only)", lo);
        /* a cut has no pregain: -6 at the centre, ~0 two octaves away */
        int c[EQ_BANDS] = {0, 0, -6, 0, 0};
        CHECK(fabs(gain_at(R, c, 1000, 8000, 1) + 6.0) < 0.5, "cut centre");
        CHECK(fabs(gain_at(R, c, 250, 8000, 1)) < 0.6, "cut 2 oct away");
    }

    /* 3. shelves behave as shelves */
    {
        int g[EQ_BANDS] = {6, 0, 0, 0, 0};          /* low shelf +6 */
        double dc = gain_at(R, g, 30, 8000, 1) + 6.0;       /* undo pregain */
        double mid = gain_at(R, g, 2000, 8000, 1) + 6.0;
        double top = gain_at(R, g, 10000, 8000, 1) + 6.0;
        CHECK(fabs(dc - 6.0) < 0.8, "low shelf at 30 Hz: %.2f", dc);
        CHECK(fabs(mid) < 0.4 && fabs(top) < 0.4, "low shelf leaks up: %.2f %.2f", mid, top);
        int h[EQ_BANDS] = {0, 0, 0, 0, 6};          /* high shelf +6 */
        double t = gain_at(R, h, 16000, 8000, 1) + 6.0;
        double m = gain_at(R, h, 500, 8000, 1) + 6.0;
        CHECK(fabs(t - 6.0) < 1.0, "high shelf at 16k: %.2f", t);
        CHECK(fabs(m) < 0.4, "high shelf leaks down: %.2f", m);
    }

    /* 4. headroom: full-scale sine, +12 dB bass, never wraps */
    {
        int g[EQ_BANDS] = {12, 12, 12, 12, 12};
        EqParams p; EqState s; eq_design(&p, R, g);
        eq_state_reset(&s);
        static int16_t buf[44100];
        for (int i = 0; i < 44100; i++)
            buf[i] = (int16_t)floor(32767.0 * sin(2.0 * M_PI * 130.0 * i / R) + 0.5);
        int16_t ref[44100]; memcpy(ref, buf, sizeof(buf));
        eq_process(&s, &p, buf, 44100, 1);
        int jumps = 0, sat = 0, mx0 = 0;
        for (int i = 4000; i < 44100; i++) {
            int d = buf[i] - buf[i - 1]; if (d < 0) d = -d;
            if (d > 40000) jumps++;
            if (buf[i] == 32767 || buf[i] == -32768) sat++;
            if (buf[i] > mx0) mx0 = buf[i];
        }
        CHECK(jumps == 0, "all +12 on a full-scale sine wrapped %d times", jumps);
        /* Sweep: overlapping boosts may reach the rail, and must saturate, not wrap. */
        jumps = 0; sat = 0;
        for (double f = 40.0; f < 18000.0; f *= 1.19) {
            for (int i = 0; i < 12000; i++)
                buf[i] = (int16_t)floor(32767.0 * sin(2.0 * M_PI * f * i / R) + 0.5);
            eq_state_reset(&s);
            eq_process(&s, &p, buf, 12000, 1);
            for (int i = 3000; i < 12000; i++) {
                int d = buf[i] - buf[i - 1]; if (d < 0) d = -d;
                if (d > 40000 && f < 1000.0) jumps++;   /* above 1 kHz a full-scale sine steps that much legitimately */
                if (buf[i] == 32767 || buf[i] == -32768) sat++;
            }
        }
        CHECK(jumps == 0, "sweep: %d wrap-sized jumps (sat samples %d)", jumps, sat);
        CHECK(mx0 > 10000, "all +12 at 130 Hz peak %d", mx0);
        /* single +12 bass: pregain makes a full-scale 20 Hz sine peak <= full scale without clipping */
        int b[EQ_BANDS] = {12, 0, 0, 0, 0};
        eq_design(&p, R, b); eq_state_reset(&s);
        for (int i = 0; i < 44100; i++)
            buf[i] = (int16_t)floor(32767.0 * sin(2.0 * M_PI * 20.0 * i / R) + 0.5);
        eq_process(&s, &p, buf, 44100, 1);
        int mx = 0, nsat = 0;
        for (int i = 8000; i < 44100; i++) {
            int v = buf[i] < 0 ? -buf[i] : buf[i];
            if (v > mx) mx = v;
            if (buf[i] == 32767 || buf[i] == -32768) nsat++;
        }
        CHECK(nsat < 50, "single +12 bass saturated %d samples", nsat);
        CHECK(mx > 24000, "single +12 bass peak only %d", mx);
    }

    /* 5. stereo: independent state (L only leaves R silent) */
    {
        int g[EQ_BANDS] = {6, 4, 0, 3, 5};
        EqParams p; EqState s; eq_design(&p, R, g); eq_state_reset(&s);
        static int16_t st[2 * 8000];
        for (int i = 0; i < 8000; i++) {
            st[2 * i] = (int16_t)floor(10000 * sin(2 * M_PI * 440.0 * i / R) + 0.5);
            st[2 * i + 1] = 0;
        }
        eq_process(&s, &p, st, 8000, 2);
        int rnz = 0, lnz = 0;
        for (int i = 0; i < 8000; i++) { if (st[2 * i + 1]) rnz++; if (st[2 * i]) lnz++; }
        CHECK(rnz == 0, "R not silent: %d nonzero", rnz);
        CHECK(lnz > 7000, "L went quiet");
        /* and mono of the same signal equals the stereo L channel */
        static int16_t mo[8000];
        for (int i = 0; i < 8000; i++) mo[i] = (int16_t)floor(10000 * sin(2 * M_PI * 440.0 * i / R) + 0.5);
        eq_state_reset(&s);
        eq_process(&s, &p, mo, 8000, 1);
        int diff = 0;
        for (int i = 0; i < 8000; i++) if (mo[i] != st[2 * i]) diff++;
        CHECK(diff == 0, "mono and stereo-L differ in %d samples", diff);
        /* Two different signals: each channel must equal its own mono result. */
        static int16_t two[2 * 8000], mr[8000];
        for (int i = 0; i < 8000; i++) {
            two[2 * i]     = (int16_t)floor(10000 * sin(2 * M_PI * 440.0 * i / R) + 0.5);
            two[2 * i + 1] = mr[i] = (int16_t)floor(9000 * sin(2 * M_PI * 3000.0 * i / R) + 0.5);
        }
        eq_state_reset(&s);
        eq_process(&s, &p, two, 3000, 2);                    /* two calls: the carry is per channel */
        eq_process(&s, &p, two + 2 * 3000, 5000, 2);
        static int16_t ml[8000];
        for (int i = 0; i < 8000; i++) ml[i] = (int16_t)floor(10000 * sin(2 * M_PI * 440.0 * i / R) + 0.5);
        eq_state_reset(&s); eq_process(&s, &p, ml, 8000, 1);
        eq_state_reset(&s); eq_process(&s, &p, mr, 8000, 1);
        int dl = 0, dr = 0;
        for (int i = 0; i < 8000; i++) { if (two[2 * i] != ml[i]) dl++; if (two[2 * i + 1] != mr[i]) dr++; }
        CHECK(dl == 0 && dr == 0, "two-signal stereo differs from per-channel mono (L %d, R %d)", dl, dr);
    }

    /* 6. rate change redesigns; a band at/above 0.45 fs is skipped */
    {
        int g[EQ_BANDS] = {0, 0, 0, 0, 6};          /* 12 kHz shelf */
        EqParams a, b, c;
        eq_design(&a, 44100, g); eq_design(&b, 48000, g); eq_design(&c, 22050, g);
        CHECK(a.n == 1 && b.n == 1, "12k shelf missing at 44.1/48k");
        CHECK(a.c[0].a1 != b.c[0].a1, "44.1k and 48k coefficients equal");
        CHECK(c.n == 0, "12k shelf not skipped at 22050 (n=%d)", c.n);
        EqParams d; eq_design(&d, 32000, g);
        CHECK(d.n == 1, "12k shelf skipped at 32000");
        /* the achieved gain follows the rate: +6 at 15 kHz vs a different net at 32k */
        double x = gain_at(44100, g, 18000, 8000, 1) + 6.0;
        CHECK(fabs(x - 6.0) < 1.0, "44.1k high shelf plateau %.2f", x);
    }

    /* 7. presets, parse, format, config key */
    {
        int g[EQ_BANDS];
        CHECK(eq_preset_count() == 5, "preset count");
        CHECK(eq_parse("bass", g) == 0 && eq_preset_match(g) == 1, "parse name");
        CHECK(eq_parse("1,-2,3,-4,5", g) == 0 && g[1] == -2 && g[4] == 5, "parse list");
        CHECK(eq_parse("30,-30,0,0,0", g) == 0 && g[0] == EQ_MAX_DB && g[1] == -EQ_MAX_DB, "clamp");
        CHECK(eq_parse("1,2,3", g) != 0, "short list accepted");
        char t[40]; int h[EQ_BANDS] = {1, -2, 3, -4, 5};
        eq_format(h, t, sizeof(t));
        CHECK(strcmp(t, "1,-2,3,-4,5") == 0, "format %s", t);
        FILE *f = fopen("audio_eq_test_cfg.tmp", "w");
        fprintf(f, "volume=5\neq=Loudness\neqx=1\n");
        fclose(f);
        CHECK(eq_config_read("audio_eq_test_cfg.tmp", g) == 0 && eq_preset_match(g) == 4, "config read");
        remove("audio_eq_test_cfg.tmp");
        CHECK(eq_config_read("audio_eq_test_missing.tmp", g) != 0 && eq_preset_match(g) == 0, "missing -> flat");
    }

    printf("audio_eq_test: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
