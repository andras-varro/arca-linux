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
    int drate = rate / ENH_DEC;                    /* the bass parts run decimated */
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
        biquad(&p->hb1, drate, 200.0 + 0.5 * e.hb_rng, 0);          /* 200..250 Hz */
        biquad(&p->hb2, drate, 160.0, 0);
    }
    if (p->db) {
        p->db_g = gain_q15(e.db_amt, DB_CEIL);
        biquad(&p->db_lo, drate,  50.0 + 1.0 * e.db_rng, 0);        /* 50..150 Hz  */
        biquad(&p->db_hi, drate, 200.0 + 4.0 * e.db_rng, 0);        /* 200..600 Hz */
    }
    if (p->ht) {
        p->ht_g     = gain_q15(e.ht_amt, HT_CEIL);
        p->ht_drive = 1024;                                         /* 4x */
        biquad(&p->ht_hp, rate, 5000.0 - 20.0 * e.ht_rng, 1);      /* 5 .. 3 kHz */
    }
}

void enh_state_reset(EnhState *s) { memset(s, 0, sizeof(*s)); }

static inline int32_t bq(const EnhCoef *c, EnhHist *h, int32_t x)
{
    if (!h->primed) {
        h->x1 = h->x2 = x;
        h->y1 = h->y2 = c->hp ? 0 : x;
        h->primed = 1;
    }
    /* No saturation: a stable Butterworth section has an L1 impulse norm below
     * 2, so a +-2^23 (Q8 int16) input cannot leave int32. */
    int64_t acc = (int64_t)c->b0 * x + (int64_t)c->b1 * h->x1 + (int64_t)c->b2 * h->x2
                - (int64_t)c->a1 * h->y1 - (int64_t)c->a2 * h->y2;
    int32_t y = (int32_t)((acc + (1 << (COEF_SHIFT - 1))) >> COEF_SHIFT);
    h->x2 = h->x1; h->x1 = x;
    h->y2 = h->y1; h->y1 = y;
    return y;
}

/* Q8 in; f(x) = 1.5x - 0.5x^3 on |x| <= 1 (full scale), flat beyond.  Returns
 * the result in Q8 (int16 units x 256).  32-bit only: v>>4 is below 2^19 and the
 * drive at most 1024. */
static inline int32_t shape_cubic(int32_t v, int32_t drive_q8)
{
    int32_t x15 = ((v >> 4) * drive_q8) >> 12;                  /* Q15 */
    if (x15 >  32767) x15 =  32767;
    if (x15 < -32767) x15 = -32767;
    int32_t x2 = (x15 * x15) >> 15;
    int32_t x3 = (x2 * x15) >> 15;
    return ((3 * x15 - x3) >> 1) * (1 << SIG_SHIFT);
}

/* Q8 in; sin(pi/2 x), linear interpolation in a 256-segment table.  Returns Q15. */
static inline int32_t shape_sine(int32_t v, int32_t drive_q8)
{
    int32_t x15 = ((v >> 4) * drive_q8) >> 12;
    if (x15 >  32767) x15 =  32767;
    if (x15 < -32767) x15 = -32767;
    int32_t u = x15 + 32768;                                    /* 1..65535 */
    int32_t i = u >> 8, f = u & 255;
    return sine_tab[i] + (((sine_tab[i + 1] - sine_tab[i]) * f) >> 8);
}

/* Q8 add of a Q15 shaper output scaled by the Q15 part gain. */
static inline int32_t scale_q15(int32_t y15, int32_t g)
{
    return (y15 * g + 64) >> 7;
}

static inline int32_t mulhi(int32_t a, int32_t b) { return (int32_t)(((int64_t)a * b) >> 32); }

static inline int32_t mulq15(int32_t v, int32_t g)
{
    return (int32_t)(((int64_t)v * g + (1 << 14)) >> 15);
}

/* Slow path of the peak guard: the guard gain is below unity, or the unscaled
 * sum overshoots.  Scales only the ADDED content; fast attack in 25 % steps
 * (bounded), slow release.  Returns the new guard gain. */
static __attribute__((noinline)) int32_t guard_slow(int32_t guard, int32_t add_l, int32_t add_r,
                                                    int dl, int dr, int channels,
                                                    int32_t *pol, int32_t *porr)
{
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
    *pol = ol; *porr = orr;
    return guard;
}

#define ENH_CHUNK 256

/* High-pass section + sine shaper for one channel of `n` frames, accumulating the
 * Q8 added content into add[].  Registers only; three 32x32 high-word multiplies
 * (b0 = b2 and b1 = -2 b0 for a high-pass, so the feed-forward is one product).
 * Signals are Q8; the multiplier operands are scaled by 16 so that the high word
 * of (Q28 coefficient x Q12 value) is Q8 again. */
static void ht_pass(const EnhCoef *c, EnhHist *h, const int16_t *src, int stride,
                    int32_t *add, int n, int32_t drive, int32_t g)
{
    int32_t x1 = h->x1, x2 = h->x2, y1 = h->y1, y2 = h->y2;
    const int32_t b0 = c->b0, a1 = c->a1, a2 = c->a2;
    for (int i = 0; i < n; i++) {
        int32_t x = src[i * stride] * (1 << SIG_SHIFT);
        int32_t y = mulhi(b0, (x - 2 * x1 + x2) * 16) - mulhi(a1, y1 * 16) - mulhi(a2, y2 * 16);
        x2 = x1; x1 = x; y2 = y1; y1 = y;
        add[i] += scale_q15(shape_sine(y, drive), g);
    }
    h->x1 = x1; h->x2 = x2; h->y1 = y1; h->y2 = y2;
}

void enh_process(EnhState *s, const EnhParams *p, int16_t *buf, long frames, int channels)
{
    if (!s || !p || !buf || frames <= 0) return;
    if (channels < 1 || channels > ENH_MAX_CH) return;

    /* Forget a part that is off, so it is primed when it comes back. */
    if (!p->active || !p->hb) { s->hb1.primed = 0; s->hb2.primed = 0; }
    if (!p->active || !p->db) { s->db_lo.primed = 0; s->db_hi.primed = 0; }
    if (!p->active || !p->ht) for (int c = 0; c < ENH_MAX_CH; c++) s->ht[c].primed = 0;
    if (!p->active || (!p->hb && !p->db)) s->dsum = s->dph = s->bv = s->bt = s->bstep = 0;
    if (!p->active) { s->guard = 0; return; }
    if (s->guard <= 0) s->guard = GUARD_ONE;

    /* Prime the treble high-pass sections from the first frame. */
    if (p->ht)
        for (int c = 0; c < channels; c++) {
            EnhHist *h = &s->ht[c];
            if (!h->primed) {
                int32_t x = buf[c] * (1 << SIG_SHIFT);
                h->x1 = h->x2 = x; h->y1 = h->y2 = 0; h->primed = 1;
            }
        }

    int32_t guard = s->guard;
    int32_t addl[ENH_CHUNK], addr[ENH_CHUNK];
    int16_t *q = buf;
    for (long done = 0; done < frames; ) {
        int n = frames - done < ENH_CHUNK ? (int)(frames - done) : ENH_CHUNK;

        /* 1. Bass parts.  They only carry content below about 600 Hz, so they
         * run on the mono sum decimated by ENH_DEC (a box average), and their
         * output is linearly interpolated back up. */
        if (p->hb || p->db) {
            int32_t dsum = s->dsum, dph = s->dph, bv = s->bv, bt = s->bt, bstep = s->bstep;
            for (int i = 0; i < n; i++) {
                int32_t xl = q[i * channels] * (1 << SIG_SHIFT);
                int32_t xr = channels > 1 ? q[i * channels + 1] * (1 << SIG_SHIFT) : xl;
                int32_t m  = (xl + xr) >> 1;
                dsum += m;
                if (++dph == ENH_DEC) {
                    dph = 0;
                    int32_t md = dsum >> ENH_DEC_SHIFT;
                    dsum = 0;
                    int32_t t = 0;
                    if (p->hb) {
                        int32_t a = bq(&p->hb1, &s->hb1, md);
                        int32_t z = shape_cubic(a, p->hb_drive);
                        z = bq(&p->hb2, &s->hb2, z);
                        t += mulq15(z, p->hb_g);
                    }
                    if (p->db) {
                        int32_t lo = bq(&p->db_lo, &s->db_lo, md);
                        int32_t hi = bq(&p->db_hi, &s->db_hi, md);
                        t += mulq15(hi - lo, p->db_g);
                    }
                    bv = bt;
                    bt = t;
                    bstep = (t - bv) >> ENH_DEC_SHIFT;
                }
                bv += bstep;
                addl[i] = bv;
            }
            s->dsum = dsum; s->dph = dph; s->bv = bv; s->bt = bt; s->bstep = bstep;
        } else {
            memset(addl, 0, (size_t)n * sizeof(addl[0]));
        }
        if (channels > 1) memcpy(addr, addl, (size_t)n * sizeof(addl[0]));

        /* 2. Treble, one channel per pass. */
        if (p->ht) {
            ht_pass(&p->ht_hp, &s->ht[0], q, channels, addl, n, p->ht_drive, p->ht_g);
            if (channels > 1)
                ht_pass(&p->ht_hp, &s->ht[1], q + 1, channels, addr, n, p->ht_drive, p->ht_g);
        }

        /* 3. Peak guard.  Fast path: gain at unity and the sum inside int16. */
        for (int i = 0; i < n; i++, q += channels) {
            int dl = q[0], dr = channels > 1 ? q[1] : 0;
            int32_t ol  = dl + ((addl[i] + (1 << (SIG_SHIFT - 1))) >> SIG_SHIFT);
            int32_t orr = dr + ((addr[i] + (1 << (SIG_SHIFT - 1))) >> SIG_SHIFT);
            if (channels < 2) orr = 0;
            if (guard != GUARD_ONE || (uint32_t)((ol + 32768) | (orr + 32768)) > 65535u)
                guard = guard_slow(guard, addl[i], channels > 1 ? addr[i] : 0, dl, dr, channels, &ol, &orr);
            q[0] = (int16_t)ol;
            if (channels > 1) q[1] = (int16_t)orr;
        }
        done += n;
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
