/* Host-side regression for fb_fill_rect_gradient().
 *
 * Runs on the DEV MACHINE with native gcc, not on the device.  fb_draw_pixel()
 * only touches fb->back_buffer / width / height / double_buffering, so a
 * synthetic Framebuffer with a malloc'd back buffer exercises the real
 * primitive with no /dev/fb0 in the loop — which matters here, because the bug
 * this covers is pure integer arithmetic and needs no hardware to reproduce,
 * while SEEING it on the panel needs a human to tap into a game (there is no
 * /dev/uinput on this device, see ../CLAUDE.md -> Input).
 *
 *   cd native_apps && gcc -Wall -Wextra -Wno-unused-parameter -I common \
 *       -o build/gradient_test tests/gradient_test.c \
 *       common/framebuffer.c common/font.c common/hardware.c common/config.c \
 *       common/touch_input.c -lm && ./build/gradient_test
 *
 * What it asserts, and why: the channel deltas used to be
 * computed in uint32_t, so a DESCENDING gradient — bottom channel darker than
 * top, which is what platformer's sky RGB(135,206,235) -> RGB(100,180,220) is on
 * all three channels — wrapped (br - tr) to ~4.29e9.  The following division
 * destroys the modular-arithmetic equivalence that would otherwise have made the
 * wrap harmless, so every row got a garbage colour instead of a ramp.  Ascending
 * gradients were unaffected, which is why this survived: most call sites ascend.
 *
 * NOT part of build-and-deploy.sh: that script cross-compiles for ARM and this
 * is a host binary.  Run it by hand after touching fb_fill_rect_gradient().
 */
#include <stdio.h>
#include <stdlib.h>
#include "framebuffer.h"

static int fails = 0;

static void expect(const char *what, int got, int want) {
    if (got != want) { printf("  FAIL %-38s got %d want %d\n", what, got, want); fails++; }
    else             { printf("  ok   %-38s %d\n", what, got); }
}

static void expect_true(const char *what, bool cond) {
    if (!cond) { printf("  FAIL %-38s\n", what); fails++; }
    else       { printf("  ok   %-38s\n", what); }
}

#define W 8
#define H 16

static Framebuffer fb;
static uint32_t pixels[W * H];

static uint32_t row_color(int j) { return pixels[j * W] & 0x00FFFFFF; }
static int chan(uint32_t c, int shift) { return (int)((c >> shift) & 0xFF); }

/* Assert a single channel ramps monotonically from `from` to `to` across the
 * rect, hitting both endpoints exactly. Monotonicity is the property the wrap
 * destroyed: a wrapped delta produces values that jump around instead. */
static void check_channel(const char *name, int shift, int from, int to) {
    char buf[80];
    snprintf(buf, sizeof buf, "%s first row", name);
    expect(buf, chan(row_color(0), shift), from);
    snprintf(buf, sizeof buf, "%s last row", name);
    expect(buf, chan(row_color(H - 1), shift), to);

    bool monotone = true, in_range = true;
    int lo = (from < to) ? from : to, hi = (from < to) ? to : from;
    for (int j = 1; j < H; j++) {
        int prev = chan(row_color(j - 1), shift), cur = chan(row_color(j), shift);
        if (from <= to) { if (cur < prev) monotone = false; }
        else            { if (cur > prev) monotone = false; }
        if (cur < lo || cur > hi) in_range = false;
    }
    snprintf(buf, sizeof buf, "%s monotone", name);
    expect_true(buf, monotone);
    snprintf(buf, sizeof buf, "%s within endpoints", name);
    expect_true(buf, in_range);
}

static void run(const char *label, uint32_t top, uint32_t bottom) {
    printf("%s: %06X -> %06X\n", label, top & 0x00FFFFFF, bottom & 0x00FFFFFF);
    for (int i = 0; i < W * H; i++) pixels[i] = 0xDEADBEEF;
    fb_fill_rect_gradient(&fb, 0, 0, W, H, top, bottom);

    check_channel("R", 16, chan(top, 16), chan(bottom, 16));
    check_channel("G",  8, chan(top,  8), chan(bottom,  8));
    check_channel("B",  0, chan(top,  0), chan(bottom,  0));

    /* Every pixel in a row must share that row's colour. */
    bool uniform = true;
    for (int j = 0; j < H; j++)
        for (int i = 1; i < W; i++)
            if ((pixels[j * W + i] & 0x00FFFFFF) != row_color(j)) uniform = false;
    expect_true("rows are horizontally uniform", uniform);
}

int main(void) {
    /* Synthetic framebuffer: no device, no mmap. */
    fb.back_buffer = pixels;
    fb.buffer = NULL;
    fb.width = W;
    fb.height = H;
    fb.bytes_per_pixel = 4;
    fb.double_buffering = true;
    fb.draw_offset_x = 0;
    fb.draw_offset_y = 0;

    /* Ascending — always worked, kept so a "fix" cannot regress it. */
    run("ascending", RGB(10, 20, 30), RGB(200, 210, 220));

    /* Descending on all three channels: platformer's sky, the wrap reproducer. */
    printf("\n");
    run("descending (platformer sky)", RGB(135, 206, 235), RGB(100, 180, 220));

    /* Mixed directions per channel — R down, G flat, B up. */
    printf("\n");
    run("mixed per-channel directions", RGB(200, 128, 10), RGB(50, 128, 240));

    /* Degenerate heights must not divide by zero. h==1 collapses to the top
     * colour; the whole rect is one row, so there is nothing to ramp over. */
    printf("\ndegenerate heights\n");
    for (int i = 0; i < W * H; i++) pixels[i] = 0xDEADBEEF;
    fb_fill_rect_gradient(&fb, 0, 0, W, 1, RGB(135, 206, 235), RGB(100, 180, 220));
    expect("h==1 takes the top colour", (int)row_color(0), (int)(RGB(135, 206, 235) & 0x00FFFFFF));
    fb_fill_rect_gradient(&fb, 0, 0, W, 0, RGB(1, 2, 3), RGB(4, 5, 6));
    expect_true("h==0 draws nothing", row_color(0) == (RGB(135, 206, 235) & 0x00FFFFFF));

    printf("\n%s (%d failure%s)\n", fails ? "FAILED" : "PASSED",
           fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
