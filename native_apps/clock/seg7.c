/* seg7.c — see seg7.h. */
#include <stdlib.h>
#include <string.h>
#include "seg7.h"

static const uint8_t k_mask[10] = {
    SEG7_A | SEG7_B | SEG7_C | SEG7_D | SEG7_E | SEG7_F,             /* 0 */
    SEG7_B | SEG7_C,                                                 /* 1 */
    SEG7_A | SEG7_B | SEG7_G | SEG7_E | SEG7_D,                      /* 2 */
    SEG7_A | SEG7_B | SEG7_G | SEG7_C | SEG7_D,                      /* 3 */
    SEG7_F | SEG7_G | SEG7_B | SEG7_C,                               /* 4 */
    SEG7_A | SEG7_F | SEG7_G | SEG7_C | SEG7_D,                      /* 5 */
    SEG7_A | SEG7_F | SEG7_G | SEG7_E | SEG7_C | SEG7_D,             /* 6 */
    SEG7_A | SEG7_B | SEG7_C,                                        /* 7 */
    SEG7_A | SEG7_B | SEG7_C | SEG7_D | SEG7_E | SEG7_F | SEG7_G,    /* 8 */
    SEG7_A | SEG7_B | SEG7_C | SEG7_D | SEG7_F | SEG7_G,              /* 9 */
};

int seg7_mask(int digit)
{
    return (digit >= 0 && digit <= 9) ? k_mask[digit] : 0;
}

int seg7_segment_poly(int seg, int w, int h, int t, int gap, Seg7Poly *o)
{
    if (seg < 0 || seg > 6 || t < 2 || gap < 0 || w < 2 * t + 2 || h < 3 * t) return -1;
    int hh = t * SEG7_FP / 2;                /* half thickness */
    int W = w * SEG7_FP, H = h * SEG7_FP, gd = gap * SEG7_FP;
    int xl = hh, xr = W - hh, yt = hh, yb = H - hh, ym = H / 2;
    o->n = 6;
    if (seg == 0 || seg == 3 || seg == 6) {          /* horizontal: a, d, g */
        int cy = seg == 0 ? yt : seg == 3 ? yb : ym;
        int a = xl + gd, b = xr - gd;
        if (b - a <= 2 * hh) return -1;
        int vx[6] = { a, a + hh, b - hh, b, b - hh, a + hh };
        int vy[6] = { cy, cy - hh, cy - hh, cy, cy + hh, cy + hh };
        memcpy(o->x, vx, sizeof vx); memcpy(o->y, vy, sizeof vy);
    } else {                                          /* vertical: b, c, e, f */
        int cx = (seg == 1 || seg == 2) ? xr : xl;
        int upper = (seg == 1 || seg == 5);
        int a = (upper ? yt : ym) + gd;
        int b = (upper ? ym : yb) - gd;
        if (b - a <= 2 * hh) return -1;
        int vx[6] = { cx, cx + hh, cx + hh, cx, cx - hh, cx - hh };
        int vy[6] = { a, a + hh, b - hh, b, b - hh, a + hh };
        memcpy(o->x, vx, sizeof vx); memcpy(o->y, vy, sizeof vy);
    }
    return 0;
}

int seg7_colon_poly(int i, int w, int h, int t, Seg7Poly *o)
{
    if (i < 0 || i > 1 || t < 2 || w < t || h < 4 * t) return -1;
    int r = t * SEG7_FP / 2;
    int cx = w * SEG7_FP / 2;
    int cy = i == 0 ? h * SEG7_FP * 3 / 10 : h * SEG7_FP * 7 / 10;
    o->n = 4;
    o->x[0] = cx;     o->y[0] = cy - r;
    o->x[1] = cx + r; o->y[1] = cy;
    o->x[2] = cx;     o->y[2] = cy + r;
    o->x[3] = cx - r; o->y[3] = cy;
    return 0;
}

/* x-extent of the convex polygon at height y (SEG7_FP units); 0 when y misses it. */
static int span_at(const Seg7Poly *p, int y, int *xl, int *xr)
{
    int lo = 1 << 30, hi = -(1 << 30);
    for (int i = 0; i < p->n; i++) {
        int j = i + 1 == p->n ? 0 : i + 1;
        int y0 = p->y[i], y1 = p->y[j], x0 = p->x[i], x1 = p->x[j];
        if (y0 == y1) continue;
        if (y0 > y1) { int s = y0; y0 = y1; y1 = s; s = x0; x0 = x1; x1 = s; }
        if (y < y0 || y >= y1) continue;                 /* half-open: a vertex counts once */
        int x = x0 + (x1 - x0) * (y - y0) / (y1 - y0);   /* start-up only: a divide here is fine */
        if (x < lo) lo = x;
        if (x > hi) hi = x;
    }
    if (hi < lo) return 0;
    *xl = lo; *xr = hi;
    return 1;
}

void seg7_fill_mask(const Seg7Poly *p, uint8_t *mask, int w, int h)
{
    memset(mask, 0, (size_t)w * h);
    for (int py = 0; py < h; py++) {
        int x0 = w, x1 = -1;
        int sl[4] = { 0 }, sr[4] = { 0 }, ok[4];
        for (int s = 0; s < 4; s++) {
            ok[s] = span_at(p, py * SEG7_FP + 2 + s * 4, &sl[s], &sr[s]);   /* sub-scanline centres */
            if (ok[s]) {
                int a = sl[s] >> 4, b = sr[s] >> 4;      /* SEG7_FP == 16 */
                if (a < x0) x0 = a;
                if (b > x1) x1 = b;
            }
        }
        if (x1 < 0) continue;
        if (x0 < 0) x0 = 0;
        if (x1 > w - 1) x1 = w - 1;
        for (int px = x0; px <= x1; px++) {
            int lo = px * SEG7_FP, hi = lo + SEG7_FP, sum = 0;      /* overlaps, at most 4*SEG7_FP */
            for (int s = 0; s < 4; s++) {
                if (!ok[s]) continue;
                int a = sl[s] > lo ? sl[s] : lo, b = sr[s] < hi ? sr[s] : hi;
                if (b > a) sum += b - a;
            }
            int c = sum * 4 - (sum >> 6);                /* 64 -> 255 */
            mask[py * w + px] = (uint8_t)(c > 255 ? 255 : c);
        }
    }
}

/* ---- drawing half ---- */

static uint32_t blend(uint32_t bg, uint32_t fg, int a)       /* a 0..255; start-up only, so / is fine */
{
    uint32_t r = 0;
    for (int sh = 0; sh <= 16; sh += 8) {
        int b = (bg >> sh) & 255, f = (fg >> sh) & 255;
        r |= (uint32_t)((b * (255 - a) + f * a + 127) / 255) << sh;
    }
    return r;
}

void seg7_face_free(Seg7Face *f)
{
    for (int i = 0; i < 11; i++) { free(f->px[i]); f->px[i] = NULL; }
}

int seg7_face_build(Seg7Face *f, int w, int h, int t, int gap, uint32_t lit, uint32_t bg, int ghost_pct)
{
    memset(f, 0, sizeof *f);
    f->w = w; f->h = h; f->cw = t + t / 4 + 6;
    uint8_t *seg[7] = { 0 };
    uint8_t *dot = NULL;
    int rc = -1;
    size_t n = (size_t)w * h;
    for (int s = 0; s < 7; s++) {
        Seg7Poly p;
        if (seg7_segment_poly(s, w, h, t, gap, &p) < 0) goto out;
        if (!(seg[s] = malloc(n))) goto out;
        seg7_fill_mask(&p, seg[s], w, h);
    }
    for (int d = 0; d < 10; d++) {
        if (!(f->px[d] = malloc(n * 4))) goto out;
        int m = seg7_mask(d);
        for (size_t i = 0; i < n; i++) {
            int a = 0;
            for (int s = 0; s < 7; s++) {
                int c = seg[s][i];
                if (!c) continue;
                a += (m >> s & 1) ? c : (c * ghost_pct + 50) / 100;   /* segments never overlap */
            }
            f->px[d][i] = blend(bg, lit, a > 255 ? 255 : a);
        }
    }
    size_t cn = (size_t)f->cw * h;
    if (!(dot = malloc(cn)) || !(f->px[10] = malloc(cn * 4))) goto out;
    for (size_t k = 0; k < cn; k++) f->px[10][k] = bg;
    for (int i = 0; i < 2; i++) {
        Seg7Poly p;
        if (seg7_colon_poly(i, f->cw, h, t + t / 4 + 1, &p) < 0) goto out;
        seg7_fill_mask(&p, dot, f->cw, h);
        for (size_t k = 0; k < cn; k++) if (dot[k]) f->px[10][k] = blend(bg, lit, dot[k]);
    }
    rc = 0;
out:
    for (int s = 0; s < 7; s++) free(seg[s]);
    free(dot);
    if (rc < 0) seg7_face_free(f);
    return rc;
}

static int glyph_of(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    return c == ':' ? 10 : -1;
}

int seg7_text_width(const Seg7Face *f, const char *s, int space)
{
    int w = 0, n = 0;
    for (; *s; s++) {
        int g = glyph_of(*s);
        if (g < 0) continue;
        w += g == 10 ? f->cw : f->w;
        n++;
    }
    return n ? w + (n - 1) * space : 0;
}

void seg7_draw_text(uint32_t *dst, int dw, int dh, const Seg7Face *f, const char *s, int x, int y, int space)
{
    for (; *s; s++) {
        int g = glyph_of(*s);
        if (g < 0) continue;
        int gw = g == 10 ? f->cw : f->w;
        int c0 = x < 0 ? -x : 0, c1 = x + gw > dw ? dw - x : gw;
        for (int r = 0; r < f->h && c1 > c0; r++) {
            int yy = y + r;
            if (yy < 0 || yy >= dh) continue;
            memcpy(dst + (size_t)yy * dw + x + c0, f->px[g] + (size_t)r * gw + c0, (size_t)(c1 - c0) * 4);
        }
        x += gw + space;
    }
}
