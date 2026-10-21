/* audio_enh_test: the system enhancer core (common/audio_enh.c).
 *
 * Build (from native_apps/):
 *   gcc -Wall -Wextra -Wno-unused-parameter -I common -o build/audio_enh_test \
 *       tests/audio_enh_test.c common/audio_enh.c -lm && ./build/audio_enh_test
 *
 * The module is new, so there is no pre-fix source to compile against; the
 * negative controls are in measure_audio_enh_sabotage.sh, which breaks a COPY
 * of audio_enh.c in single ways and counts the failures here. */
#include "audio_enh.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails = 0, checks = 0;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static const double PI = 3.14159265358979323846;

/* Render `secs` of a sine (channel 0; channel 1 = sign * same) through the
 * enhancer.  Returns the ADDED content (out - in) as doubles of channel 0 in
 * `added` (n samples) and the output in `out` (interleaved, n*ch). */
typedef struct { int16_t *in, *out; long n; int ch; } Run;

static Run render(const AudioEnh *e, int rate, int ch, double hz, int amp, int sign_r, double secs)
{
    Run r;
    r.n = (long)(rate * secs);
    r.ch = ch;
    r.in  = malloc((size_t)r.n * ch * sizeof(int16_t));
    r.out = malloc((size_t)r.n * ch * sizeof(int16_t));
    for (long i = 0; i < r.n; i++)
        for (int c = 0; c < ch; c++) {
            double v = amp * sin(2.0 * PI * hz * i / rate);
            if (c == 1) v *= sign_r;
            r.in[i * ch + c] = (int16_t)floor(v + 0.5);
        }
    memcpy(r.out, r.in, (size_t)r.n * ch * sizeof(int16_t));
    EnhParams p; EnhState s;
    enh_design(&p, rate, e);
    enh_state_reset(&s);
    /* in blocks, as the stream does */
    for (long i = 0; i < r.n; i += 1000) {
        long f = r.n - i < 1000 ? r.n - i : 1000;
        enh_process(&s, &p, r.out + i * ch, f, ch);
    }
    return r;
}
static void run_free(Run *r) { free(r->in); free(r->out); }

/* Amplitude of the added content (out - in, channel 0) at `hz`, last half. */
static double added_amp(const Run *r, int rate, double hz)
{
    double sr = 0, si = 0;
    long n0 = r->n / 2, m = 0;
    for (long i = n0; i < r->n; i++, m++) {
        double d = (double)r->out[i * r->ch] - (double)r->in[i * r->ch];
        double w = 0.5 - 0.5 * cos(2.0 * PI * (double)(i - n0) / (double)(r->n - n0)); /* Hann */
        sr += w * d * cos(2.0 * PI * hz * i / rate);
        si += w * d * sin(2.0 * PI * hz * i / rate);
    }
    return 2.0 * sqrt(sr * sr + si * si) / (0.5 * (double)m);
}

static AudioEnh mk(int on, int hb, int hbr, int db, int dbr, int ht, int htr)
{
    AudioEnh e = { on, hb, hbr, db, dbr, ht, htr };
    return e;
}

static int identical(const Run *r) { return memcmp(r->in, r->out, (size_t)r->n * r->ch * sizeof(int16_t)) == 0; }

int main(void)
{
    const int R = 48000;

    /* 1. Bypass is bit-exact: off, or all amounts 0, mono and stereo, noise. */
    {
        AudioEnh cases[3] = { mk(0, 100, 50, 100, 50, 100, 50), mk(1, 0, 50, 0, 50, 0, 50), mk(1, 0, 100, 0, 0, 0, 100) };
        for (int k = 0; k < 3; k++)
            for (int ch = 1; ch <= 2; ch++) {
                Run r = render(&cases[k], R, ch, 440.0, 20000, 1, 0.2);
                uint32_t sd = 7;
                for (long i = 0; i < r.n * ch; i++) {
                    sd = sd * 1664525u + 1013904223u;
                    r.in[i] = (int16_t)((int)(sd >> 16) - 32768);
                }
                memcpy(r.out, r.in, (size_t)r.n * ch * sizeof(int16_t));
                EnhParams p; EnhState s;
                enh_design(&p, R, &cases[k]);
                enh_state_reset(&s);
                enh_process(&s, &p, r.out, r.n, ch);
                CHECK(identical(&r), "bypass case %d ch %d not bit-exact", k, ch);
                CHECK(!p.active, "bypass case %d reports active", k);
                run_free(&r);
            }
    }

    /* 2. Harmonic bass: 60 Hz in -> 180 Hz out (odd harmonic), little at 5 kHz. */
    {
        AudioEnh e = mk(1, 100, 50, 0, 50, 0, 50);
        Run r = render(&e, R, 2, 60.0, 8000, 1, 1.0);
        double a60 = added_amp(&r, R, 60.0), a180 = added_amp(&r, R, 180.0), a5k = added_amp(&r, R, 5000.0);
        CHECK(a180 > 100.0, "hbass 180 Hz harmonic too small: %.1f", a180);
        CHECK(a60 > 100.0, "hbass fundamental not added: %.1f", a60);
        CHECK(a5k < a180 / 20.0, "hbass leaks at 5 kHz: %.2f vs %.1f", a5k, a180);
        run_free(&r);
        /* an 8 kHz tone is outside the band: nothing added */
        Run h = render(&e, R, 2, 8000.0, 8000, 1, 0.5);
        CHECK(added_amp(&h, R, 8000.0) < 5.0, "hbass touches 8 kHz: %.2f", added_amp(&h, R, 8000.0));
        run_free(&h);
        /* anti-phase stereo has a zero mono sum: the bass parts add nothing */
        AudioEnh b = mk(1, 100, 50, 100, 50, 0, 50);
        Run a = render(&b, R, 2, 60.0, 8000, -1, 0.5);
        CHECK(identical(&a) || added_amp(&a, R, 60.0) < 3.0, "bass parts ignore the mono sum: %.2f", added_amp(&a, R, 60.0));
        run_free(&a);
    }

    /* 3. Drum bass responds in its band (100..400 Hz at range 50), not outside. */
    {
        AudioEnh e = mk(1, 0, 50, 100, 50, 0, 50);
        Run in_band = render(&e, R, 2, 180.0, 8000, 1, 1.0);
        Run lo = render(&e, R, 2, 20.0, 8000, 1, 1.0);
        Run hi = render(&e, R, 2, 8000.0, 8000, 1, 0.5);
        double ab = added_amp(&in_band, R, 180.0), al = added_amp(&lo, R, 20.0), ah = added_amp(&hi, R, 8000.0);
        CHECK(ab > 400.0, "drum in-band response too small: %.1f", ab);
        CHECK(al < ab / 3.0, "drum responds below its band: %.1f vs %.1f", al, ab);
        CHECK(ah < ab / 50.0, "drum responds above its band: %.1f vs %.1f", ah, ab);
        run_free(&in_band); run_free(&lo); run_free(&hi);
    }

    /* 4. Harmonic treble: 5 kHz in -> 15 kHz (3rd) out; no bass added. */
    {
        AudioEnh e = mk(1, 0, 50, 0, 50, 100, 50);
        Run r = render(&e, R, 2, 5000.0, 6000, 1, 0.5);
        double a15 = added_amp(&r, R, 15000.0), a5 = added_amp(&r, R, 5000.0);
        CHECK(a15 > 30.0, "treble 3rd harmonic too small: %.1f", a15);
        CHECK(a5 > 100.0, "treble fundamental not added: %.1f", a5);
        run_free(&r);
        Run l = render(&e, R, 2, 100.0, 8000, 1, 0.5);
        CHECK(added_amp(&l, R, 100.0) < 8.0, "treble touches 100 Hz: %.2f", added_amp(&l, R, 100.0));
        run_free(&l);
    }

    /* 5. Peak guard: full scale, everything at 100, never wraps and stays in int16. */
    {
        AudioEnh e = mk(1, 100, 100, 100, 100, 100, 100);
        double fr[4] = { 60.0, 180.0, 1000.0, 5000.0 };
        int wraps = 0, beyond = 0;
        for (int k = 0; k < 4; k++) {
            Run r = render(&e, R, 2, fr[k], 32767, 1, 0.5);
            for (long i = 0; i < r.n * 2; i++) {
                int a = r.in[i], b = r.out[i];
                if (abs(a) > 20000 && ((a > 0) != (b > 0))) wraps++;
                if (abs(b - a) > 32767 + 32768) beyond++;
            }
            run_free(&r);
        }
        CHECK(wraps == 0, "guard: %d sign flips on full-scale sines", wraps);
        CHECK(beyond == 0, "guard: %d out-of-range samples", beyond);
        /* full-scale square, and noise */
        Run q = render(&e, R, 2, 100.0, 32767, 1, 0.5);
        for (long i = 0; i < q.n * 2; i++) q.in[i] = ((i / 2) / 240) & 1 ? 32767 : -32768;
        memcpy(q.out, q.in, (size_t)q.n * 4);
        EnhParams p; EnhState s; enh_design(&p, R, &e); enh_state_reset(&s);
        enh_process(&s, &p, q.out, q.n, 2);
        int sq = 0;
        for (long i = 0; i < q.n * 2; i++) { int a = q.in[i], b = q.out[i]; if ((a > 0) != (b > 0) && b != 0) sq++; }
        CHECK(sq == 0, "guard: %d sign flips on a full-scale square", sq);
        run_free(&q);
        /* the guard is not a mute: at -6 dB the enhancer still adds content */
        Run h = render(&e, R, 2, 60.0, 16000, 1, 1.0);
        CHECK(added_amp(&h, R, 60.0) > 200.0, "guard mutes the added content: %.1f", added_amp(&h, R, 60.0));
        run_free(&h);
    }

    /* 6. Parse, format, clamp, config. */
    {
        AudioEnh e; char t[64];
        CHECK(audio_enh_parse("1,50,50,30,50,40,50", &e) == 0 && e.on == 1 && e.hb_amt == 50 && e.db_amt == 30 && e.ht_amt == 40 && e.ht_rng == 50, "parse");
        audio_enh_format(&e, t, sizeof(t));
        CHECK(strcmp(t, "1,50,50,30,50,40,50") == 0, "format round trip: %s", t);
        CHECK(audio_enh_parse(" 1 , 200 , -5 , 3 , 4 , 5 , 6 ", &e) == 0 && e.hb_amt == 100 && e.hb_rng == 0 && e.ht_rng == 6, "clamp/space");
        CHECK(audio_enh_parse("7,1,2,3,4,5,6", &e) == 0 && e.on == 1, "on clamps to 1");
        CHECK(audio_enh_parse("1,2,3", &e) != 0, "short value refused");
        CHECK(audio_enh_parse("abc", &e) != 0, "garbled refused");
        CHECK(audio_enh_parse("1,2,3,4,5,6;7", &e) != 0, "bad separator refused");
        FILE *f = fopen("audio_enh_test_cfg.tmp", "w");
        fprintf(f, "volume=5\nenh=1,60,50,40,50,40,50\nenhx=0\n");
        fclose(f);
        CHECK(enh_config_read("audio_enh_test_cfg.tmp", &e) == 0 && e.on == 1 && e.hb_amt == 60, "config read");
        f = fopen("audio_enh_test_cfg.tmp", "w");
        fprintf(f, "enh=garbage\n");
        fclose(f);
        CHECK(enh_config_read("audio_enh_test_cfg.tmp", &e) != 0 && e.on == 0 && e.hb_rng == 50, "garbled key -> off, range 50");
        remove("audio_enh_test_cfg.tmp");
        CHECK(enh_config_read("audio_enh_test_missing.tmp", &e) != 0 && e.on == 0 && e.ht_rng == 50, "missing -> off");
    }

    /* 7. Ranges change behaviour. */
    {
        AudioEnh a = mk(1, 100, 0, 0, 50, 0, 50), b = mk(1, 100, 100, 0, 50, 0, 50);
        Run ra = render(&a, R, 2, 60.0, 4000, 1, 1.0), rb = render(&b, R, 2, 60.0, 4000, 1, 1.0);
        double h0 = added_amp(&ra, R, 180.0), h1 = added_amp(&rb, R, 180.0);
        CHECK(h1 > 1.5 * h0, "hbass range has no effect: %.1f vs %.1f", h0, h1);
        run_free(&ra); run_free(&rb);
        AudioEnh c = mk(1, 0, 50, 100, 0, 0, 50), d = mk(1, 0, 50, 100, 100, 0, 50);
        Run rc = render(&c, R, 2, 500.0, 8000, 1, 1.0), rd = render(&d, R, 2, 500.0, 8000, 1, 1.0);
        double d0 = added_amp(&rc, R, 500.0), d1 = added_amp(&rd, R, 500.0);
        CHECK(d1 > 2.0 * d0, "drum range has no effect: %.1f vs %.1f", d0, d1);
        run_free(&rc); run_free(&rd);
        AudioEnh g = mk(1, 0, 50, 0, 50, 100, 0), h = mk(1, 0, 50, 0, 50, 100, 100);
        Run rg = render(&g, R, 2, 3500.0, 4000, 1, 0.5), rh = render(&h, R, 2, 3500.0, 4000, 1, 0.5);
        double t0 = added_amp(&rg, R, 3500.0), t1 = added_amp(&rh, R, 3500.0);
        CHECK(t1 > 1.5 * t0, "treble range has no effect: %.1f vs %.1f", t0, t1);
        run_free(&rg); run_free(&rh);
    }

    /* 8. Silence in is silence out; rates; mono. */
    {
        AudioEnh e = mk(1, 100, 100, 100, 100, 100, 100);
        Run z = render(&e, R, 2, 100.0, 0, 1, 0.3);
        int nz = 0;
        for (long i = 0; i < z.n * 2; i++) if (z.out[i] != 0) nz++;
        CHECK(nz == 0, "silence produced %d non-zero samples", nz);
        run_free(&z);
        AudioEnh b = mk(1, 80, 50, 0, 50, 0, 50);
        int rates[3] = { 22050, 44100, 48000 };
        double amp[3];
        for (int k = 0; k < 3; k++) {
            Run r = render(&b, rates[k], 1, 60.0, 8000, 1, 1.0);
            amp[k] = added_amp(&r, rates[k], 180.0);
            CHECK(amp[k] > 100.0, "mono %d Hz: no 180 Hz harmonic (%.1f)", rates[k], amp[k]);
            run_free(&r);
        }
        CHECK(fabs(amp[1] - amp[2]) < 0.25 * amp[2], "rate-dependent harmonic level: %.1f vs %.1f", amp[1], amp[2]);
    }

    printf("audio_enh_test: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
