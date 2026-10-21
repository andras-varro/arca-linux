#include "audio_enh.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define COEF_SHIFT 28
#define SIG_SHIFT  8
#define SIG_MAX    (32767 * (1 << SIG_SHIFT))
#define SIG_MIN    (-32768 * (1 << SIG_SHIFT))

/* Peak-guard gain on the added content, Q15. */
#define GUARD_ONE       32768
#define GUARD_REL_SHIFT 12      /* release time constant about 4096 samples */

/* Output ceilings of the three parts at amount 100 (fraction of full scale). */
#define HB_CEIL 0.35
#define DB_CEIL 0.70
#define HT_CEIL 0.25

static int16_t sine_tab[257];     /* sin(pi/2 * x), x in -1..1, Q15 */
static int     sine_ready = 0;

static int32_t q28(double v)
{
    double s = v * (double)(1 << COEF_SHIFT);
    if (s >  2147483647.0) s =  2147483647.0;
    if (s < -2147483648.0) s = -2147483648.0;
    return (int32_t)floor(s + 0.5);
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

void enh_clamp(AudioEnh *e)
{
    e->on     = e->on ? 1 : 0;
    e->hb_amt = clampi(e->hb_amt, 0, 100);  e->hb_rng = clampi(e->hb_rng, 0, 100);
    e->db_amt = clampi(e->db_amt, 0, 100);  e->db_rng = clampi(e->db_rng, 0, 100);
    e->ht_amt = clampi(e->ht_amt, 0, 100);  e->ht_rng = clampi(e->ht_rng, 0, 100);
}

/* Butterworth (Q = 1/sqrt 2) low- or high-pass at `fc`, clamped to 0.45 x rate. */
static void biquad(EnhCoef *c, int rate, double fc, int hp)
{
    if (fc > 0.45 * rate) fc = 0.45 * rate;
    double w0 = 2.0 * M_PI * fc / rate;
    double cs = cos(w0), al = sin(w0) / (2.0 * 0.70710678118654752);
    double b0, b1, b2;
    if (hp) { b0 = (1.0 + cs) / 2.0; b1 = -(1.0 + cs); b2 = b0; }
    else    { b0 = (1.0 - cs) / 2.0; b1 =   1.0 - cs;  b2 = b0; }
    double a0 = 1.0 + al, a1 = -2.0 * cs, a2 = 1.0 - al;
    c->b0 = q28(b0 / a0); c->b1 = q28(b1 / a0); c->b2 = q28(b2 / a0);
    c->a1 = q28(a1 / a0); c->a2 = q28(a2 / a0);
    c->hp = hp;
}

/* 0 at p = 0, 1 at p = 100, steep at the low end (a loudness-like taper). */
static int32_t gain_q15(int p, double ceil_)
{
    if (p <= 0) return 0;
    double t = 1.0 - log10(1.0 + 9.0 * (100 - p) / 100.0);
    return (int32_t)floor(t * ceil_ * 32768.0 + 0.5);
}

void enh_design(EnhParams *p, int rate, const AudioEnh *in)
{
    memset(p, 0, sizeof(*p));
    p->rate = rate;
    AudioEnh e = *in;
    enh_clamp(&e);
    p->cfg = e;
    if (!sine_ready) {
        for (int i = 0; i <= 256; i++)
            sine_tab[i] = (int16_t)floor(32767.0 * sin(M_PI / 2.0 * (i - 128) / 128.0) + 0.5);
        sine_ready = 1;
    }
    if (!e.on || rate <= 0) return;

    p->hb = e.hb_amt > 0;  p->db = e.db_amt > 0;  p->ht = e.ht_amt > 0;
    if (!p->hb && !p->db && !p->ht) return;
    p->active = 1;

    if (p->hb) {
        p->hb_g     = gain_q15(e.hb_amt, HB_CEIL);
        p->hb_drive = 256 + (768 * e.hb_rng) / 100;                 /* 1x .. 4x */
        biquad(&p->hb1, rate, 200.0 + 0.5 * e.hb_rng, 0);          /* 200..250 Hz */
        biquad(&p->hb2, rate, 160.0, 0);
    }
    if (p->db) {
        p->db_g = gain_q15(e.db_amt, DB_CEIL);
        biquad(&p->db_lo, rate,  50.0 + 1.0 * e.db_rng, 0);        /* 50..150 Hz  */
        biquad(&p->db_hi, rate, 200.0 + 4.0 * e.db_rng, 0);        /* 200..600 Hz */
    }
    if (p->ht) {
        p->ht_g     = gain_q15(e.ht_amt, HT_CEIL);
        p->ht_drive = 1024;                                         /* 4x */
        biquad(&p->ht_hp, rate, 5000.0 - 20.0 * e.ht_rng, 1);      /* 5 .. 3 kHz */
    }
}

void enh_state_reset(EnhState *s) { memset(s, 0, sizeof(*s)); }

static inline int32_t sat_sig(int64_t v)
{
    if (v > SIG_MAX) return SIG_MAX;
    if (v < SIG_MIN) return SIG_MIN;
    return (int32_t)v;
}

static inline int32_t bq(const EnhCoef *c, EnhHist *h, int32_t x)
{
    if (!h->primed) {
        h->x1 = h->x2 = x;
        h->y1 = h->y2 = c->hp ? 0 : x;
        h->primed = 1;
    }
    int64_t acc = (int64_t)c->b0 * x + (int64_t)c->b1 * h->x1 + (int64_t)c->b2 * h->x2
                - (int64_t)c->a1 * h->y1 - (int64_t)c->a2 * h->y2;
    int32_t y = sat_sig((acc + (1 << (COEF_SHIFT - 1))) >> COEF_SHIFT);
    h->x2 = h->x1; h->x1 = x;
    h->y2 = h->y1; h->y1 = y;
    return y;
}

/* Q8 in, Q8 out; f(x) = 1.5x - 0.5x^3 on |x| <= 1 (full scale), flat beyond. */
static inline int32_t shape_cubic(int32_t v, int32_t drive_q8)
{
    int64_t x = ((int64_t)v * drive_q8) >> SIG_SHIFT;          /* Q8 */
    int32_t x15 = (int32_t)(x >> SIG_SHIFT);                    /* Q15 */
    if (x15 >  32767) x15 =  32767;
    if (x15 < -32767) x15 = -32767;
    int32_t x2 = (x15 * x15) >> 15;
    int32_t x3 = (x2 * x15) >> 15;
    int32_t y15 = (3 * x15 - x3) >> 1;
    return y15 * (1 << SIG_SHIFT);
}

/* Q8 in, Q8 out; sin(pi/2 x), linear interpolation in a 256-segment table. */
static inline int32_t shape_sine(int32_t v, int32_t drive_q8)
{
    int64_t x = ((int64_t)v * drive_q8) >> SIG_SHIFT;
    int32_t x15 = (int32_t)(x >> SIG_SHIFT);
    if (x15 >  32767) x15 =  32767;
    if (x15 < -32767) x15 = -32767;
    int32_t u = x15 + 32768;                                    /* 1..65535 */
    int32_t i = u >> 8, f = u & 255;
    int32_t y15 = sine_tab[i] + (((sine_tab[i + 1] - sine_tab[i]) * f) >> 8);
    return y15 * (1 << SIG_SHIFT);
}

static inline int32_t mulq15(int32_t v, int32_t g)
{
    return (int32_t)(((int64_t)v * g + (1 << 14)) >> 15);
}

void enh_process(EnhState *s, const EnhParams *p, int16_t *buf, long frames, int channels)
{
    if (!s || !p || !buf || frames <= 0) return;
    if (channels < 1 || channels > ENH_MAX_CH) return;

    /* Forget a part that is off, so it is primed when it comes back. */
    if (!p->active || !p->hb) { s->hb1.primed = 0; s->hb2.primed = 0; }
    if (!p->active || !p->db) { s->db_lo.primed = 0; s->db_hi.primed = 0; }
    if (!p->active || !p->ht) for (int c = 0; c < ENH_MAX_CH; c++) s->ht[c].primed = 0;
    if (!p->active) { s->guard = 0; return; }
    if (s->guard <= 0) s->guard = GUARD_ONE;

    int32_t guard = s->guard;
    int16_t *q = buf;
    for (long i = 0; i < frames; i++, q += channels) {
        int32_t xl = q[0] * (1 << SIG_SHIFT);
        int32_t xr = channels > 1 ? q[1] * (1 << SIG_SHIFT) : xl;
        int32_t m  = (xl + xr) >> 1;
        int32_t add_m = 0, add_l, add_r;

        if (p->hb) {
            int32_t a = bq(&p->hb1, &s->hb1, m);
            int32_t z = shape_cubic(a, p->hb_drive);
            z = bq(&p->hb2, &s->hb2, z);
            add_m += mulq15(z, p->hb_g);
        }
        if (p->db) {
            int32_t lo = bq(&p->db_lo, &s->db_lo, m);
            int32_t hi = bq(&p->db_hi, &s->db_hi, m);
            add_m += mulq15(hi - lo, p->db_g);
        }
        add_l = add_r = add_m;
        if (p->ht) {
            int32_t hl = bq(&p->ht_hp, &s->ht[0], xl);
            add_l += mulq15(shape_sine(hl, p->ht_drive), p->ht_g);
            if (channels > 1) {
                int32_t hr = bq(&p->ht_hp, &s->ht[1], xr);
                add_r += mulq15(shape_sine(hr, p->ht_drive), p->ht_g);
            }
        }

        /* Peak guard: scale only the added content so dry + added stays inside
         * int16.  Fast attack in 25 % steps (bounded), slow release. */
        int dl = q[0], dr = channels > 1 ? q[1] : 0;
        int tries = 0;
        int32_t ol, orr;
        for (;;) {
            int64_t gl = ((int64_t)add_l * guard) >> 15;
            int64_t gr = ((int64_t)add_r * guard) >> 15;
            ol  = dl + (int32_t)((gl + (1 << (SIG_SHIFT - 1))) >> SIG_SHIFT);
            orr = dr + (int32_t)((gr + (1 << (SIG_SHIFT - 1))) >> SIG_SHIFT);
            int bad = ol > 32767 || ol < -32768 || (channels > 1 && (orr > 32767 || orr < -32768));
            if (!bad) break;
            if (++tries > 8) { guard = 0; ol = dl; orr = dr; break; }
            guard -= (guard >> 2) + 1;
            if (guard < 0) guard = 0;
        }
        if (guard < GUARD_ONE) {
            guard += ((GUARD_ONE - guard) >> GUARD_REL_SHIFT) + 1;
            if (guard > GUARD_ONE) guard = GUARD_ONE;
        }
        q[0] = (int16_t)ol;
        if (channels > 1) q[1] = (int16_t)orr;
    }
    s->guard = guard;
}

/* ── config ─────────────────────────────────────────────────────────────── */

static void enh_off(AudioEnh *e)
{
    memset(e, 0, sizeof(*e));
    e->hb_rng = e->db_rng = e->ht_rng = 50;
}

int audio_enh_parse(const char *value, AudioEnh *out)
{
    if (!value || !out) return -1;
    long v[7];
    const char *s = value;
    for (int k = 0; k < 7; k++) {
        while (*s == ' ' || *s == '\t') s++;
        char *end;
        v[k] = strtol(s, &end, 10);
        if (end == s) return -1;
        s = end;
        while (*s == ' ' || *s == '\t') s++;
        if (k < 6) { if (*s != ',') return -1; s++; }
    }
    for (int k = 0; k < 7; k++) {
        if (v[k] >  1000) v[k] =  1000;
        if (v[k] < -1000) v[k] = -1000;
    }
    AudioEnh e = { (int)v[0], (int)v[1], (int)v[2], (int)v[3], (int)v[4], (int)v[5], (int)v[6] };
    enh_clamp(&e);
    *out = e;
    return 0;
}

void audio_enh_format(const AudioEnh *e, char *buf, size_t n)
{
    if (!buf || n == 0) return;
    if (!e) { buf[0] = '\0'; return; }
    snprintf(buf, n, "%d,%d,%d,%d,%d,%d,%d",
             e->on, e->hb_amt, e->hb_rng, e->db_amt, e->db_rng, e->ht_amt, e->ht_rng);
}

int enh_config_read(const char *path, AudioEnh *e)
{
    enh_off(e);
    FILE *f = path ? fopen(path, "r") : NULL;
    if (!f) return -1;
    char line[160];
    int rc = -1;
    size_t kl = strlen(ENH_CONFIG_KEY);
    while (fgets(line, sizeof(line), f)) {
        char *t = line;
        while (*t == ' ' || *t == '\t') t++;
        if (strncmp(t, ENH_CONFIG_KEY, kl) != 0) continue;
        t += kl;
        while (*t == ' ' || *t == '\t') t++;
        if (*t != '=') continue;
        t++;
        AudioEnh g;
        if (audio_enh_parse(t, &g) == 0) { *e = g; rc = 0; }
    }
    fclose(f);
    return rc;
}
