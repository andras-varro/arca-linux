#include "audio_eq.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define COEF_SHIFT 28
#define SIG_SHIFT  8
#define SIG_MAX    (32767 * (1 << SIG_SHIFT))
#define SIG_MIN    (-32768 * (1 << SIG_SHIFT))

static const int BAND_HZ[EQ_BANDS] = { 60, 250, 1000, 4000, 12000 };

int eq_band_hz(int b) { return (b >= 0 && b < EQ_BANDS) ? BAND_HZ[b] : 0; }

static int32_t q28(double v)
{
    double s = v * (double)(1 << COEF_SHIFT);
    if (s >  2147483647.0) s =  2147483647.0;
    if (s < -2147483648.0) s = -2147483648.0;
    return (int32_t)floor(s + 0.5);
}

/* Normalised RBJ coefficients of band `b` at `gain_db`. */
static void rbj(int b, int rate, int gain_db, double out[5])
{
    double A  = pow(10.0, gain_db / 40.0);
    double w0 = 2.0 * M_PI * BAND_HZ[b] / rate;
    double cs = cos(w0), sn = sin(w0);
    double b0, b1, b2, a0, a1, a2;

    if (b == 1 || b == 2 || b == 3) {                 /* peaking, Q = 1 */
        double al = sn / 2.0;
        b0 = 1.0 + al * A;  b1 = -2.0 * cs;  b2 = 1.0 - al * A;
        a0 = 1.0 + al / A;  a1 = -2.0 * cs;  a2 = 1.0 - al / A;
    } else {                                          /* shelf, S = 1 */
        double al = sn / 2.0 * sqrt(2.0);
        double t  = 2.0 * sqrt(A) * al;
        if (b == 0) {
            b0 =  A * ((A + 1) - (A - 1) * cs + t);
            b1 =  2 * A * ((A - 1) - (A + 1) * cs);
            b2 =  A * ((A + 1) - (A - 1) * cs - t);
            a0 =  (A + 1) + (A - 1) * cs + t;
            a1 = -2 * ((A - 1) + (A + 1) * cs);
            a2 =  (A + 1) + (A - 1) * cs - t;
        } else {
            b0 =  A * ((A + 1) + (A - 1) * cs + t);
            b1 = -2 * A * ((A - 1) + (A + 1) * cs);
            b2 =  A * ((A + 1) + (A - 1) * cs - t);
            a0 =  (A + 1) - (A - 1) * cs + t;
            a1 =  2 * ((A - 1) - (A + 1) * cs);
            a2 =  (A + 1) - (A - 1) * cs - t;
        }
    }
    out[0] = b0 / a0; out[1] = b1 / a0; out[2] = b2 / a0;
    out[3] = a1 / a0; out[4] = a2 / a0;
}

void eq_design(EqParams *p, int rate, const int gains_db[EQ_BANDS])
{
    memset(p, 0, sizeof(*p));
    p->rate = rate;
    int maxb = 0;
    double first[3] = {0, 0, 0};
    for (int b = 0; b < EQ_BANDS; b++) {
        int g = gains_db ? gains_db[b] : 0;
        if (g >  EQ_MAX_DB) g =  EQ_MAX_DB;
        if (g < -EQ_MAX_DB) g = -EQ_MAX_DB;
        p->gains_db[b] = g;
        /* A band at or above 0.45 x rate is not realisable; skip it. */
        if (g == 0 || rate <= 0 || BAND_HZ[b] * 20 >= rate * 9) continue;
        double c[5];
        rbj(b, rate, g, c);
        if (p->n == 0) { first[0] = c[0]; first[1] = c[1]; first[2] = c[2]; }
        EqCoef *e = &p->c[p->n];
        e->b0 = q28(c[0]); e->b1 = q28(c[1]); e->b2 = q28(c[2]);
        e->a1 = q28(c[3]); e->a2 = q28(c[4]);
        p->band[p->n] = (uint8_t)b;
        p->n++;
        if (g > maxb) maxb = g;
    }
    if (p->n > 0 && maxb > 0) {
        double pre = pow(10.0, -maxb / 20.0);
        EqCoef *e = &p->c[0];
        e->b0 = q28(first[0] * pre);
        e->b1 = q28(first[1] * pre);
        e->b2 = q28(first[2] * pre);
    }
}

void eq_state_reset(EqState *s) { memset(s, 0, sizeof(*s)); }

static inline int32_t sat_sig(int64_t v)
{
    if (v > SIG_MAX) return SIG_MAX;
    if (v < SIG_MIN) return SIG_MIN;
    return (int32_t)v;
}

void eq_process(EqState *s, const EqParams *p, int16_t *buf, long frames, int channels)
{
    if (!s || !p || !buf || frames <= 0) return;
    if (channels < 1 || channels > EQ_MAX_CH) return;

    /* Forget a band that is off, so it is primed when it comes back. */
    int on[EQ_BANDS] = {0};
    for (int k = 0; k < p->n; k++) on[p->band[k]] = 1;
    for (int ch = 0; ch < channels; ch++)
        for (int b = 0; b < EQ_BANDS; b++)
            if (!on[b]) s->primed[ch][b] = 0;
    if (p->n == 0) return;

    for (int ch = 0; ch < channels; ch++) {
        int16_t *q = buf + ch;
        for (long i = 0; i < frames; i++, q += channels) {
            int32_t x = (int32_t)(*q) * (1 << SIG_SHIFT);
            for (int k = 0; k < p->n; k++) {
                const EqCoef *c = &p->c[k];
                int bd = p->band[k];
                EqHist *h = &s->h[ch][bd];
                if (!s->primed[ch][bd]) {
                    h->x1 = h->x2 = h->y1 = h->y2 = x;
                    s->primed[ch][bd] = 1;
                }
                int64_t acc = (int64_t)c->b0 * x + (int64_t)c->b1 * h->x1
                            + (int64_t)c->b2 * h->x2
                            - (int64_t)c->a1 * h->y1 - (int64_t)c->a2 * h->y2;
                int32_t y = sat_sig((acc + (1 << (COEF_SHIFT - 1))) >> COEF_SHIFT);
                h->x2 = h->x1; h->x1 = x;
                h->y2 = h->y1; h->y1 = y;
                x = y;
            }
            int32_t o = (x + (1 << (SIG_SHIFT - 1))) >> SIG_SHIFT;
            if (o >  32767) o =  32767;
            if (o < -32768) o = -32768;
            *q = (int16_t)o;
        }
    }
}

/* ── presets, text, config ──────────────────────────────────────────────── */

static const struct { const char *name; int g[EQ_BANDS]; } PRESETS[] = {
    { "Flat",     {  0,  0,  0,  0,  0 } },
    { "Bass",     {  6,  4,  0,  0,  0 } },
    { "Treble",   {  0,  0,  0,  4,  6 } },
    { "Vocal",    { -3, -1,  3,  4,  0 } },
    { "Loudness", {  6,  2,  0,  2,  5 } },
};
#define NPRESETS ((int)(sizeof(PRESETS) / sizeof(PRESETS[0])))

int         eq_preset_count(void) { return NPRESETS; }
const char *eq_preset_name(int i) { return (i >= 0 && i < NPRESETS) ? PRESETS[i].name : NULL; }
const int  *eq_preset_gains(int i) { return (i >= 0 && i < NPRESETS) ? PRESETS[i].g : NULL; }

int eq_preset_match(const int gains[EQ_BANDS])
{
    for (int i = 0; i < NPRESETS; i++)
        if (memcmp(PRESETS[i].g, gains, sizeof(PRESETS[i].g)) == 0) return i;
    return -1;
}

int eq_parse(const char *text, int gains[EQ_BANDS])
{
    if (!text || !gains) return -1;
    while (*text == ' ' || *text == '\t') text++;
    for (int i = 0; i < NPRESETS; i++) {
        size_t n = strlen(PRESETS[i].name);
        if (strncasecmp(text, PRESETS[i].name, n) == 0 &&
            (text[n] == '\0' || strchr(" \t\r\n", text[n]))) {
            memcpy(gains, PRESETS[i].g, sizeof(PRESETS[i].g));
            return 0;
        }
    }
    int g[EQ_BANDS];
    const char *s = text;
    for (int b = 0; b < EQ_BANDS; b++) {
        char *end;
        long v = strtol(s, &end, 10);
        if (end == s) return -1;
        if (v >  EQ_MAX_DB) v =  EQ_MAX_DB;
        if (v < -EQ_MAX_DB) v = -EQ_MAX_DB;
        g[b] = (int)v;
        s = end;
        while (*s == ' ' || *s == '\t') s++;
        if (b < EQ_BANDS - 1) {
            if (*s != ',') return -1;
            s++;
        }
    }
    memcpy(gains, g, sizeof(g));
    return 0;
}

int eq_format(const int gains[EQ_BANDS], char *buf, int cap)
{
    return snprintf(buf, (size_t)cap, "%d,%d,%d,%d,%d",
                    gains[0], gains[1], gains[2], gains[3], gains[4]);
}

int eq_config_read(const char *path, int gains[EQ_BANDS])
{
    memset(gains, 0, sizeof(int) * EQ_BANDS);
    FILE *f = path ? fopen(path, "r") : NULL;
    if (!f) return -1;
    char line[160];
    int rc = -1;
    size_t kl = strlen(EQ_CONFIG_KEY);
    while (fgets(line, sizeof(line), f)) {
        char *t = line;
        while (*t == ' ' || *t == '\t') t++;
        if (strncmp(t, EQ_CONFIG_KEY, kl) != 0) continue;
        t += kl;
        while (*t == ' ' || *t == '\t') t++;
        if (*t != '=') continue;
        t++;
        int g[EQ_BANDS];
        if (eq_parse(t, g) == 0) { memcpy(gains, g, sizeof(g)); rc = 0; }
    }
    fclose(f);
    return rc;
}
