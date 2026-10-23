/* Host-side regression for fb_fill_rect_alpha().
 *
 * Runs on the dev machine with native gcc. fb_fill_rect_alpha() must be
 * bit-identical to a loop of fb_draw_pixel_alpha() over the same rect, for
 * 32bpp and 16bpp surfaces, double-buffered and not. The reference is the old
 * per-pixel loop, run on a second copy of the same random buffer; every stored
 * word is compared.
 *
 *   cd native_apps && gcc -Wall -Wextra -Wno-unused-parameter -I common \
 *       -o <bin> tests/fb_alpha_rect_test.c \
 *       common/framebuffer.c common/font.c common/hardware.c common/config.c \
 *       common/touch_input.c -lm && <bin>
 *
 * Not part of build-and-deploy.sh (host binary, not ARM). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "framebuffer.h"

#define SW 800
#define SH 480

static int checks = 0, fails = 0, mismatched_pixels = 0;

/* xorshift32: deterministic, no libc rand() dependence. */
static uint32_t rng_state = 0x12345678u;
static uint32_t rnd(void) {
    uint32_t x = rng_state;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return rng_state = x;
}

static Framebuffer fb;
static uint32_t *ref_store, *fast_store;   /* raw storage, both 32bpp & 16bpp */

/* Make fb address `store` at its bpp; double-buffered or not. */
static void attach(uint32_t *store, uint32_t bpp, bool dbl) {
    memset(&fb, 0, sizeof fb);
    fb.width = SW;
    fb.height = SH;
    fb.bytes_per_pixel = bpp;
    fb.line_length = SW * bpp;
    fb.double_buffering = dbl;
    fb.buffer = store;
    fb.back_buffer = store;   /* fb_target() reads back_buffer when dbl */
}

static void fill_random(uint32_t *store, uint32_t bpp) {
    size_t words = (size_t)SW * SH * bpp / 4;
    for (size_t i = 0; i < words; i++) store[i] = rnd();
}

static void reference_rect(Framebuffer *f, int x, int y, int w, int h,
                           uint32_t color, uint8_t alpha) {
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++)
            fb_draw_pixel_alpha(f, x + i, y + j, color, alpha);
}

/* One case: same random start, reference vs fast path, compare every word. */
static void check_case(uint32_t bpp, bool dbl, int ox, int oy,
                       int x, int y, int w, int h, uint32_t color, uint8_t alpha) {
    size_t bytes = (size_t)SW * SH * bpp;
    uint32_t saved_seed = rng_state;
    fill_random(ref_store, bpp);
    rng_state = saved_seed;               /* identical start */
    fill_random(fast_store, bpp);

    attach(ref_store, bpp, dbl);
    fb.draw_offset_x = ox; fb.draw_offset_y = oy;
    reference_rect(&fb, x, y, w, h, color, alpha);

    attach(fast_store, bpp, dbl);
    fb.draw_offset_x = ox; fb.draw_offset_y = oy;
    fb_fill_rect_alpha(&fb, x, y, w, h, color, alpha);

    size_t words = bytes / 4;
    int bad = 0;
    for (size_t i = 0; i < words; i++)
        if (ref_store[i] != fast_store[i]) bad++;
    /* Count per 32-bit word; a 16bpp pixel is half a word, so the figure is
     * an upper bound on pixels and is used only as a mismatch indicator. */
    checks++;
    if (bad) { fails++; mismatched_pixels += bad; }
}

static const int RECTS[][4] = {
    /* x, y, w, h */
    {0, 0, SW, SH},                 /* full screen */
    {10, 10, 100, 50},              /* interior */
    {-30, 10, 100, 50},             /* off left */
    {750, 10, 100, 50},             /* off right */
    {10, -20, 50, 60},              /* off top */
    {10, 450, 50, 100},             /* off bottom */
    {-5, -5, 20, 20},               /* top-left corner */
    {790, 470, 30, 30},             /* bottom-right corner */
    {-100, -100, 1000, 1000},       /* bigger than screen both ways */
    {900, 0, 10, 10},               /* fully off right */
    {-50, -50, 10, 10},             /* fully off top-left */
    {0, 480, 10, 10},               /* fully off bottom */
    {10, 10, 0, 5},                 /* zero width */
    {10, 10, 5, 0},                 /* zero height */
    {10, 10, -5, 5},                /* negative width */
    {10, 10, 5, -5},                /* negative height */
};
static const uint8_t ALPHAS[] = {0, 1, 128, 180, 254, 255};

int main(void) {
    size_t cap = (size_t)SW * SH * 4 / 4 + 1;
    ref_store = malloc(cap * 4);
    fast_store = malloc(cap * 4);
    if (!ref_store || !fast_store) { printf("alloc failed\n"); return 2; }

    const uint32_t bpps[2] = {4, 2};
    for (int bi = 0; bi < 2; bi++) {
        uint32_t bpp = bpps[bi];
        for (int dbl = 0; dbl < 2; dbl++) {
            for (size_t ri = 0; ri < sizeof RECTS / sizeof RECTS[0]; ri++) {
                for (size_t ai = 0; ai < sizeof ALPHAS; ai++) {
                    for (int c = 0; c < 3; c++) {
                        uint32_t color = rnd() & 0x00FFFFFF;
                        if (c == 0) color = 0x00FFFFFF;
                        if (c == 1) color = 0x00000000;
                        check_case(bpp, dbl, 0, 0,
                                   RECTS[ri][0], RECTS[ri][1],
                                   RECTS[ri][2], RECTS[ri][3], color, ALPHAS[ai]);
                    }
                }
            }
            /* nonzero draw offsets, including ones that move a rect off screen */
            static const int OFFS[][2] = {{30, -20}, {-400, 300}, {-790, -470}};
            for (size_t oi = 0; oi < sizeof OFFS / sizeof OFFS[0]; oi++)
                for (size_t ri = 0; ri < sizeof RECTS / sizeof RECTS[0]; ri++)
                    for (size_t ai = 0; ai < sizeof ALPHAS; ai++)
                        check_case(bpp, dbl, OFFS[oi][0], OFFS[oi][1],
                                   RECTS[ri][0], RECTS[ri][1],
                                   RECTS[ri][2], RECTS[ri][3],
                                   rnd() & 0x00FFFFFF, ALPHAS[ai]);
            /* random rects over random colours and alphas */
            for (int k = 0; k < 60; k++) {
                int x = (int)(rnd() % (SW + 200)) - 100;
                int y = (int)(rnd() % (SH + 200)) - 100;
                int w = (int)(rnd() % 300) - 20;
                int h = (int)(rnd() % 300) - 20;
                check_case(bpp, dbl, (int)(rnd() % 60) - 30, (int)(rnd() % 60) - 30,
                           x, y, w, h, rnd(), (uint8_t)rnd());
            }
        }
    }

    printf("cases %d, failed %d (mismatched 32-bit words %d)\n",
           checks, fails, mismatched_pixels);
    printf(fails ? "RESULT FAIL\n" : "RESULT PASS\n");
    free(ref_store);
    free(fast_store);
    return fails ? 1 : 0;
}
