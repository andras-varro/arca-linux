/* Host-side regression for the framebuffer's bits-per-pixel dispatch.
 *
 * Runs on the DEV MACHINE with native gcc, not on the device.  The drawing
 * primitives only touch fb->back_buffer / width / height / bytes_per_pixel /
 * double_buffering, so a synthetic Framebuffer over a malloc'd buffer exercises
 * the real code with no /dev/fb0 in the loop — the pattern gradient_test.c
 * established, and the only way to see this bug at all: it is a heap overflow,
 * so on the device it corrupts whatever malloc handed out next rather than
 * drawing anything visibly wrong.
 *
 *   cd native_apps && gcc -Wall -Wextra -Wno-unused-parameter -I common \
 *       -o build/framebuffer_bpp_test tests/framebuffer_bpp_test.c \
 *       common/framebuffer.c common/font.c common/hardware.c common/config.c \
 *       common/touch_input.c -lm && ./build/framebuffer_bpp_test
 *
 * What it asserts, and why: the back buffer is sized
 * width * height * bytes_per_pixel, but every primitive used to write a
 * uint32_t unconditionally.  At 16bpp — which is what ScummVM and vnc_client
 * set, and what a game launched over SSH after one of them INHERITS, because no
 * game asserted the bpp — that addresses twice the allocation.  Observed
 * on RW09 2026-08-02: a stale vnc_client left /dev/fb0 at 16bpp and
 * brick_breaker came up at "800x455 ... 16 bpp", running the full 2x overflow.
 *
 * The mechanism, so the assertions read as deliberate: a guard region is placed
 * immediately after the logical back buffer and filled with a sentinel.  Every
 * primitive is then driven over the whole surface INCLUDING its last pixel.  A
 * uint32_t write at the last 16bpp pixel index lands one whole buffer past the
 * end, so the guard catches it.  The pixel-value checks are the other half: a
 * primitive could stay in bounds and still write 32bpp words into a 16bpp
 * surface, which reads back as garbage colour.
 *
 * NOT part of build-and-deploy.sh: that script cross-compiles for ARM and this
 * is a host binary.  Run it by hand after touching a drawing primitive.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "framebuffer.h"

static int fails = 0;

static void expect_hex(const char *what, uint32_t got, uint32_t want) {
    if (got != want) { printf("  FAIL %-42s got %06X want %06X\n", what, got, want); fails++; }
    else             { printf("  ok   %-42s %06X\n", what, got); }
}

/* Small enough to reason about, large enough that the last pixel of a 16bpp
 * surface is well past the halfway point a 32bpp write would reach. */
#define W 40
#define H 24

#define GUARD_BYTE 0xA5
/* Room for the worst case: a full 32bpp-sized overwrite of a 16bpp surface,
 * plus slack for the scaled blit's rounding. */
#define GUARD_BYTES (W * H * 4)

static Framebuffer fb;
static uint8_t *buf;          /* logical surface followed by the guard region */
static size_t   logical_bytes;

/* The packing the 16bpp path must produce: RGB565, high bits of each channel. */
static uint16_t to565(uint32_t c) {
    return (uint16_t)((((c >> 16) & 0xF8) << 8) |
                      (((c >>  8) & 0xFC) << 3) |
                      (( c        & 0xF8) >> 3));
}

static void surface_init(int bytes_per_pixel) {
    fb.width  = W;
    fb.height = H;
    fb.bytes_per_pixel = (uint32_t)bytes_per_pixel;
    fb.double_buffering = true;
    fb.draw_offset_x = 0;
    fb.draw_offset_y = 0;
    fb.buffer = NULL;
    fb.screen_size = 0;

    logical_bytes = (size_t)W * H * bytes_per_pixel;
    free(buf);
    buf = (uint8_t *)malloc(logical_bytes + GUARD_BYTES);
    if (!buf) { printf("  FAIL out of memory\n"); exit(2); }
    memset(buf, 0, logical_bytes);
    memset(buf + logical_bytes, GUARD_BYTE, GUARD_BYTES);

    fb.back_buffer = (uint32_t *)buf;
    fb.back_buffer_size = logical_bytes;
}

/* Nothing may be written at or past logical_bytes. */
static bool guard_intact(void) {
    for (size_t i = 0; i < GUARD_BYTES; i++)
        if (buf[logical_bytes + i] != GUARD_BYTE) return false;
    return true;
}

static size_t guard_first_hit(void) {
    for (size_t i = 0; i < GUARD_BYTES; i++)
        if (buf[logical_bytes + i] != GUARD_BYTE) return i;
    return GUARD_BYTES;
}

static void check_guard(const char *what) {
    if (guard_intact()) {
        printf("  ok   %-42s no overflow\n", what);
    } else {
        printf("  FAIL %-42s clobbered the guard at end+%zu of a %zu-byte buffer\n",
               what, guard_first_hit(), logical_bytes);
        fails++;
    }
}

/* Read a pixel back in the surface's own format, returned as 24-bit RGB for
 * 32bpp and as the raw 16-bit word for 16bpp. */
static uint32_t peek(int x, int y) {
    size_t idx = (size_t)y * W + x;
    if (fb.bytes_per_pixel == 2) return ((uint16_t *)buf)[idx];
    return ((uint32_t *)buf)[idx] & 0x00FFFFFF;
}

/* ── The primitives, driven over the whole surface including its last pixel ── */

static const uint32_t C_A = RGB(255, 128, 8);
static const uint32_t C_B = RGB(0, 220, 255);

static void drive_all_primitives(void) {
    /* Every one of these reaches the last row and last column. */
    fb_draw_pixel(&fb, W - 1, H - 1, C_A);
    check_guard("fb_draw_pixel at last pixel");

    fb_fill_rect(&fb, 0, 0, W, H, C_B);
    check_guard("fb_fill_rect over whole surface");

    fb_draw_rect(&fb, 0, 0, W, H, C_A);
    check_guard("fb_draw_rect over whole surface");

    fb_clear(&fb, C_A);
    check_guard("fb_clear non-zero colour");

    fb_clear(&fb, 0);
    check_guard("fb_clear black (memset path)");

    fb_fill_rect_gradient(&fb, 0, 0, W, H, C_A, C_B);
    check_guard("fb_fill_rect_gradient");

    fb_fill_circle(&fb, W / 2, H / 2, W, C_B);
    check_guard("fb_fill_circle clipped at edges");

    fb_draw_circle(&fb, W / 2, H / 2, W / 2, C_A);
    check_guard("fb_draw_circle");

    fb_fill_rounded_rect(&fb, 0, 0, W, H, 6, C_B);
    check_guard("fb_fill_rounded_rect");

    fb_draw_rounded_rect(&fb, 0, 0, W, H, 6, C_A);
    check_guard("fb_draw_rounded_rect");

    fb_draw_line(&fb, 0, 0, W - 1, H - 1, C_A);
    check_guard("fb_draw_line");

    fb_draw_thick_line(&fb, 0, H - 1, W - 1, 0, 5, C_B);
    check_guard("fb_draw_thick_line");

    /* Text at the bottom-right: the glyph runs to the last row. */
    fb_draw_text(&fb, W - 6, H - 7, "X", C_A, 1);
    check_guard("fb_draw_text at last row");

    fb_fill_rect_alpha(&fb, 0, 0, W, H, C_A, 128);
    check_guard("fb_fill_rect_alpha");

    /* Sprite blits: source is always uint32_t ARGB regardless of surface bpp. */
    static uint32_t sprite[8 * 8];
    for (int i = 0; i < 8 * 8; i++) sprite[i] = C_B;
    fb_blit_sprite(&fb, sprite, 8, 0, 0, W - 8, H - 8, 8, 8, 0xFFFFFFFF);
    check_guard("fb_blit_sprite at bottom-right");

    fb_blit_sprite_flipped(&fb, sprite, 8, 0, 0, W - 8, H - 8, 8, 8, 0xFFFFFFFF);
    check_guard("fb_blit_sprite_flipped");

    fb_blit_sprite_scaled(&fb, sprite, 8, 0, 0, 8, 8, 0, 0, W, H, 0xFFFFFFFF);
    check_guard("fb_blit_sprite_scaled over surface");
}

/* ── Value checks: in-bounds is not enough, the format must be right ─────── */

static void check_16bpp_values(void) {
    printf("16bpp pixel format (RGB565)\n");

    fb_clear(&fb, 0);
    fb_draw_pixel(&fb, 3, 4, C_A);
    expect_hex("fb_draw_pixel packs RGB565", peek(3, 4), to565(C_A));
    expect_hex("neighbour untouched", peek(4, 4), 0);

    /* A 32bpp write at index (y*W+x) would land two pixels later and leave the
     * intended pixel at 0 — this is the assertion that fails pre-fix even where
     * the guard survives. */
    fb_clear(&fb, 0);
    fb_draw_pixel(&fb, 0, 0, C_B);
    expect_hex("first pixel is one 16-bit word", peek(0, 0), to565(C_B));
    expect_hex("pixel two words along untouched", peek(2, 0), 0);

    fb_clear(&fb, C_A);
    expect_hex("fb_clear fills every pixel", peek(W - 1, H - 1), to565(C_A));
    expect_hex("fb_clear fills the middle too", peek(W / 2, H / 2), to565(C_A));

    fb_clear(&fb, 0);
    fb_fill_rect(&fb, 2, 2, 4, 4, C_B);
    expect_hex("fb_fill_rect interior", peek(3, 3), to565(C_B));
    expect_hex("fb_fill_rect last cell", peek(5, 5), to565(C_B));
    expect_hex("outside the rect untouched", peek(6, 5), 0);

    /* Alpha blending has to unpack the destination, blend, and repack. The
     * endpoints are exact regardless of how the 5/6-bit expansion is done. */
    fb_clear(&fb, 0);
    fb_draw_pixel(&fb, 1, 1, C_A);
    fb_draw_pixel_alpha(&fb, 1, 1, C_B, 0);
    expect_hex("alpha 0 leaves the destination", peek(1, 1), to565(C_A));
    fb_draw_pixel_alpha(&fb, 1, 1, C_B, 255);
    expect_hex("alpha 255 takes the source", peek(1, 1), to565(C_B));

    /* Blits convert from the uint32_t source to the surface format. */
    static uint32_t sprite[4 * 4];
    for (int i = 0; i < 4 * 4; i++) sprite[i] = C_B;
    fb_clear(&fb, 0);
    fb_blit_sprite(&fb, sprite, 4, 0, 0, 10, 10, 4, 4, 0xFFFFFFFF);
    expect_hex("fb_blit_sprite packs RGB565", peek(11, 11), to565(C_B));
    expect_hex("beyond the sprite untouched", peek(14, 11), 0);

    /* The colour key is compared in the SOURCE's 32-bit space, not after
     * packing — two different 24-bit colours can share one RGB565 word. */
    for (int i = 0; i < 4 * 4; i++) sprite[i] = SPRITE_TRANSPARENT;
    fb_clear(&fb, 0);
    fb_blit_sprite(&fb, sprite, 4, 0, 0, 10, 10, 4, 4, SPRITE_TRANSPARENT);
    expect_hex("colour-keyed sprite writes nothing", peek(11, 11), 0);
}

static void check_32bpp_values(void) {
    printf("32bpp pixel format (unchanged behaviour)\n");

    fb_clear(&fb, 0);
    fb_draw_pixel(&fb, 3, 4, C_A);
    expect_hex("fb_draw_pixel stores 24-bit RGB", peek(3, 4), C_A & 0x00FFFFFF);
    expect_hex("neighbour untouched", peek(4, 4), 0);

    fb_clear(&fb, C_B);
    expect_hex("fb_clear fills every pixel", peek(W - 1, H - 1), C_B & 0x00FFFFFF);

    fb_clear(&fb, 0);
    fb_draw_pixel(&fb, 1, 1, C_A);
    fb_draw_pixel_alpha(&fb, 1, 1, C_B, 0);
    expect_hex("alpha 0 leaves the destination", peek(1, 1), C_A & 0x00FFFFFF);
    fb_draw_pixel_alpha(&fb, 1, 1, C_B, 255);
    expect_hex("alpha 255 takes the source", peek(1, 1), C_B & 0x00FFFFFF);

    static uint32_t sprite[4 * 4];
    for (int i = 0; i < 4 * 4; i++) sprite[i] = C_B;
    fb_clear(&fb, 0);
    fb_blit_sprite(&fb, sprite, 4, 0, 0, 10, 10, 4, 4, 0xFFFFFFFF);
    expect_hex("fb_blit_sprite copies the pixel", peek(11, 11), C_B & 0x00FFFFFF);
}

int main(void) {
    printf("16bpp surface: no primitive may write past width*height*2\n");
    surface_init(2);
    drive_all_primitives();

    printf("\n");
    check_16bpp_values();

    printf("\n32bpp surface: the same drive must still stay in bounds\n");
    surface_init(4);
    drive_all_primitives();

    printf("\n");
    check_32bpp_values();

    /* A bezel-resized surface is not a whole number of rows past the old one;
     * the primitives must use the CURRENT width, not a remembered stride. */
    printf("\n16bpp after a narrower re-init (stride must follow fb.width)\n");
    surface_init(2);
    fb.width = W - 7;
    logical_bytes = (size_t)fb.width * H * 2;
    memset(buf + logical_bytes, GUARD_BYTE, GUARD_BYTES);
    fb.back_buffer_size = logical_bytes;
    fb_clear(&fb, C_A);
    check_guard("fb_clear honours the new width");
    fb_fill_rect(&fb, 0, 0, (int)fb.width, H, C_B);
    check_guard("fb_fill_rect honours the new width");
    expect_hex("last pixel of the narrow surface",
               (uint32_t)((uint16_t *)buf)[(size_t)H * fb.width - 1], to565(C_B));

    free(buf);
    buf = NULL;
    printf("\n%s (%d failure%s)\n", fails ? "FAILED" : "PASSED",
           fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
