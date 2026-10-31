/*
 * seg7.h — seven-segment digits for the clock: a pure geometry half (digit -> segment mask, segment
 * polygons, anti-aliased coverage masks; all integer, no libm, host-tested) and a drawing half that
 * renders glyphs 0-9 and the colon once into pixel blocks and copies them into the back buffer.
 *
 * Coordinates are fixed point, SEG7_FP units per pixel, so no float and no divide runs per pixel
 * (the Cortex-A8 has no hardware divide and the build is soft-float).
 */
#ifndef SEG7_H
#define SEG7_H

#include <stdint.h>

#define SEG7_FP 16

/* segment bits, in the usual a..g naming: a top, b upper right, c lower right, d bottom,
 * e lower left, f upper left, g middle */
enum { SEG7_A = 1, SEG7_B = 2, SEG7_C = 4, SEG7_D = 8, SEG7_E = 16, SEG7_F = 32, SEG7_G = 64 };

typedef struct {
    int n;               /* vertex count: 6 for a segment, 4 for a colon dot */
    int x[6], y[6];      /* vertices in SEG7_FP units, relative to the glyph box's top-left */
} Seg7Poly;

/* lit segments of a digit 0-9; 0 for anything else */
int seg7_mask(int digit);

/* Hexagon of segment `seg` (0 = a ... 6 = g) in a digit box w x h px. t = thickness, gap = how far
 * each end is pulled back along its axis, so neighbouring 45-degree tips stay about 1.4*gap apart.
 * Returns 0 on success, -1 when the box cannot hold the parameters. */
int seg7_segment_poly(int seg, int w, int h, int t, int gap, Seg7Poly *out);

/* Colon dot i (0 upper, 1 lower): a rhombus of diagonal t centred in a glyph box w x h. */
int seg7_colon_poly(int i, int w, int h, int t, Seg7Poly *out);

/* Anti-aliased coverage of one polygon into mask[w*h] (0..255 per pixel, overwritten): exact
 * horizontal overlap on 4 sub-scanlines per pixel row. */
void seg7_fill_mask(const Seg7Poly *p, uint8_t *mask, int w, int h);

/* ---- drawing half ---- */

typedef struct {
    int w, h;            /* digit box */
    int cw;              /* colon glyph width */
    uint32_t *px[11];    /* 0-9 digits, 10 colon; each w*h (colon cw*h) XRGB8888, lit + ghost */
} Seg7Face;

/* Build the glyph blocks: lit colour `lit`, unlit segments at ghost_pct percent of it, over `bg`.
 * Returns 0, or -1 on bad geometry or out of memory. */
int seg7_face_build(Seg7Face *f, int w, int h, int t, int gap, uint32_t lit, uint32_t bg, int ghost_pct);
void seg7_face_free(Seg7Face *f);

/* Width of a text made of digits and ':' with `space` px between glyphs; other chars are skipped. */
int seg7_text_width(const Seg7Face *f, const char *s, int space);

/* Copy the text into a 32bpp surface dw x dh (stride dw) at (x, y), clipped to it. */
void seg7_draw_text(uint32_t *dst, int dw, int dh, const Seg7Face *f, const char *s, int x, int y, int space);

#endif
