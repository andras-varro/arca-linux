/*
 * seg7_test.c — host regression for clock/seg7.c: the lit segments of each digit, hexagon geometry
 * (inside the box, mirror symmetry, a gap between neighbours), anti-aliased coverage at known edges,
 * and the cached glyph blocks (lit, ghost, background, clipping).
 *
 * Build: gcc -I clock tests/seg7_test.c clock/seg7.c -o seg7_test
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "seg7.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static int mask_of(const char *letters)
{
    int m = 0;
    for (; *letters; letters++) m |= 1 << (*letters - 'a');
    return m;
}

/* every vertex of a, reflected in x (mx >= 0: x -> mx - x) or y (my >= 0), is a vertex of b */
static int mirrored(const Seg7Poly *a, const Seg7Poly *b, int mx, int my)
{
    for (int i = 0; i < a->n; i++) {
        int x = mx >= 0 ? mx - a->x[i] : a->x[i], y = my >= 0 ? my - a->y[i] : a->y[i], found = 0;
        for (int j = 0; j < b->n; j++) found |= b->x[j] == x && b->y[j] == y;
        if (!found) return 0;
    }
    return 1;
}

int main(void)
{
    /* ---- which segments light ---- */
    static const char *want[10] = { "abcdef", "bc", "abdeg", "abcdg", "bcfg", "acdfg", "acdefg", "abc", "abcdefg", "abcdfg" };
    for (int d = 0; d < 10; d++) CHECK(seg7_mask(d) == mask_of(want[d]));
    CHECK(seg7_mask(-1) == 0 && seg7_mask(10) == 0);

    /* ---- geometry: box 60 x 100, thickness 12, gap 2 ---- */
    const int W = 60, H = 100, T = 12, G = 2;
    Seg7Poly p[7];
    for (int s = 0; s < 7; s++) CHECK(seg7_segment_poly(s, W, H, T, G, &p[s]) == 0 && p[s].n == 6);
    CHECK(seg7_segment_poly(7, W, H, T, G, &p[0]) < 0);
    CHECK(seg7_segment_poly(0, 20, H, T, G, &p[0]) < 0);            /* box too narrow for the thickness */
    seg7_segment_poly(0, W, H, T, G, &p[0]);
    for (int s = 0; s < 7; s++) {
        for (int i = 0; i < 6; i++) {
            CHECK(p[s].x[i] >= 0 && p[s].x[i] <= W * SEG7_FP);
            CHECK(p[s].y[i] >= 0 && p[s].y[i] <= H * SEG7_FP);
        }
    }
    /* mirror symmetry: b<->f, c<->e left-right; a<->d top-bottom; g about the middle row */
    CHECK(mirrored(&p[1], &p[5], W * SEG7_FP, -1) && mirrored(&p[2], &p[4], W * SEG7_FP, -1));
    CHECK(mirrored(&p[0], &p[3], -1, H * SEG7_FP));
    CHECK(mirrored(&p[6], &p[6], W * SEG7_FP, -1) && mirrored(&p[6], &p[6], -1, H * SEG7_FP));
    int ymin = 1 << 30, ymax = 0;
    for (int i = 0; i < 6; i++) { if (p[6].y[i] < ymin) ymin = p[6].y[i]; if (p[6].y[i] > ymax) ymax = p[6].y[i]; }
    CHECK(ymin + ymax == H * SEG7_FP);                              /* g is centred vertically */

    /* neighbours share no pixel: a-f, a-b, g-b, g-c, g-e, g-f, d-c, d-e */
    size_t n = (size_t)W * H;
    uint8_t *m[7];
    for (int s = 0; s < 7; s++) { m[s] = malloc(n); seg7_fill_mask(&p[s], m[s], W, H); }
    static const int adj[8][2] = { {0,5}, {0,1}, {6,1}, {6,2}, {6,4}, {6,5}, {3,2}, {3,4} };
    for (int k = 0; k < 8; k++) {
        int shared = 0;
        for (size_t i = 0; i < n; i++) shared += m[adj[k][0]][i] && m[adj[k][1]][i];
        CHECK(shared == 0);
    }
    /* each segment has a solid core and an area near thickness * length */
    CHECK(m[0][(T / 2) * W + W / 2] == 255);
    CHECK(m[1][(H / 4) * W + W - T / 2] == 255);
    CHECK(m[0][(H / 2) * W + W / 2] == 0 && m[0][0] == 0);
    for (int s = 0; s < 7; s++) free(m[s]);

    /* ---- coverage at a known edge: rectangle 0..8.5 px wide, 0..4 px tall ---- */
    Seg7Poly r = { 4, { 0, 136, 136, 0 }, { 0, 0, 64, 64 } };
    uint8_t rm[10 * 6];
    seg7_fill_mask(&r, rm, 10, 6);
    CHECK(rm[0] == 255 && rm[1 * 10 + 7] == 255);
    CHECK(rm[2 * 10 + 8] >= 125 && rm[2 * 10 + 8] <= 129);            /* half-covered column */
    CHECK(rm[2 * 10 + 9] == 0 && rm[4 * 10 + 2] == 0);                /* outside */
    Seg7Poly h = { 4, { 0, 160, 160, 0 }, { 0, 0, 24, 24 } };         /* 1.5 px tall: row 1 half covered */
    uint8_t hm[10 * 4];
    seg7_fill_mask(&h, hm, 10, 4);
    CHECK(hm[0 * 10 + 3] == 255 && hm[1 * 10 + 3] >= 125 && hm[1 * 10 + 3] <= 129 && hm[2 * 10 + 3] == 0);

    /* ---- colon dots: a rhombus of diagonal 12 has area 72 px ---- */
    Seg7Poly c0, c1;
    CHECK(seg7_colon_poly(0, 20, 100, 12, &c0) == 0 && seg7_colon_poly(1, 20, 100, 12, &c1) == 0 && c0.n == 4);
    CHECK(seg7_colon_poly(2, 20, 100, 12, &c0) < 0);
    uint8_t *cm = malloc(20 * 100);
    seg7_fill_mask(&c0, cm, 20, 100);
    int sum = 0;
    for (int i = 0; i < 20 * 100; i++) sum += cm[i];
    CHECK(sum / 255 >= 70 && sum / 255 <= 74);
    CHECK(cm[(30) * 20 + 10] == 255);
    free(cm);

    /* ---- cached glyphs: lit 0xFFFFFF, ghost 10 %, background 0x000000 ---- */
    Seg7Face f;
    CHECK(seg7_face_build(&f, W, H, T, G, 0xFFFFFF, 0x000000, 10) == 0);
    CHECK(f.px[1][(T / 2) * W + W / 2] == 0x1A1A1A);                  /* segment a unlit in "1": ghost */
    CHECK(f.px[8][(T / 2) * W + W / 2] == 0xFFFFFF);                  /* lit in "8" */
    CHECK(f.px[1][(H / 4) * W + W - T / 2] == 0xFFFFFF);              /* b lit in "1" */
    CHECK(f.px[1][(H / 4) * W + W / 2] == 0x000000);                  /* between segments: background */
    seg7_face_free(&f);
    CHECK(seg7_face_build(&f, 20, H, T, G, 0xFFFFFF, 0, 10) < 0 && f.px[0] == NULL);
    seg7_face_free(&f);
    CHECK(seg7_face_build(&f, W, H, T, G, 0xFFFFFF, 0x000000, 10) == 0);

    /* text width and clipped drawing into a 200 x 120 surface */
    CHECK(seg7_text_width(&f, "12:34", 5) == 4 * W + f.cw + 4 * 5);
    CHECK(seg7_text_width(&f, "", 5) == 0 && seg7_text_width(&f, "ab", 5) == 0);
    uint32_t *surf = malloc(200 * 120 * 4 + 64);
    for (int i = 0; i < 200 * 120 + 16; i++) surf[i] = 0x777777;
    seg7_draw_text(surf, 200, 120, &f, "8", 10, 10, 0);
    CHECK(surf[(10 + T / 2) * 200 + 10 + W / 2] == 0xFFFFFF);
    CHECK(surf[9 * 200 + 10] == 0x777777 && surf[(10 + H) * 200 + 10] == 0x777777);
    seg7_draw_text(surf, 200, 120, &f, "8:8", -30, -20, 3);             /* partly off every edge: no crash */
    seg7_draw_text(surf, 200, 120, &f, "8", 180, 50, 0);
    CHECK(surf[200 * 120] == 0x777777);                                /* nothing past the end */
    free(surf);
    seg7_face_free(&f);

    printf("%s (%d failures)\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
