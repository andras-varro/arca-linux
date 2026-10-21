/* Host regression: the 6x9 font keeps every ASCII pixel except g j p q y.
 *
 * Runs on the DEV MACHINE with native gcc; the drawing code only touches the
 * Framebuffer fields set below, so a synthetic 32bpp surface over a malloc'd
 * buffer exercises the real fb_draw_text (the framebuffer_bpp_test pattern).
 *
 *   cd native_apps && gcc -Wall -Wextra -Wno-unused-parameter -I common \
 *       -o build/font_pixel_diff_test tests/font_pixel_diff_test.c \
 *       common/framebuffer.c common/font.c common/hardware.c common/config.c \
 *       common/touch_input.c -lm && ./build/font_pixel_diff_test
 *
 * The reference is a verbatim copy of the pre-change font_5x7 table and of its
 * 5-column, 7-row draw loop (below), so "byte-identical" is checked against the
 * old code itself, not against the new table.  Checks:
 *   - every ASCII glyph, scales 1-3: identical to the reference, except g j p q y;
 *   - g j p q y: ink in rows 7-8 (x scale) that the reference lacks, nothing
 *     below row 9, and the glyph is not blank;
 *   - a whole mixed string without g j p q y: identical (advance unchanged);
 *   - UTF-8: e-acute draws a real glyph, bad bytes and CJK draw the same box,
 *     controls advance a blank cell, ink never leaves the 6x9 cell;
 *   - negative control: one flipped pixel is counted as exactly one difference.
 *
 * Against the PRE-change framebuffer.c (no UTF-8, no descenders) the g/j/p/q/y
 * and UTF-8 checks fail; build that with the recipe in the commit message.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "framebuffer.h"

#define W 80
#define H 48
#define OX 4
#define OY 4

static int fails = 0, checks = 0;

#define CHECK(cond, ...) do { checks++; if (!(cond)) { fails++; printf("  FAIL " __VA_ARGS__); printf("\n"); } } while (0)

/* ---- verbatim copy of the pre-change table (framebuffer.c, 5 cols x 7 rows) ---- */
static const uint8_t old_font_5x7[][5] = {
    {0x00, 0x00, 0x00, 0x00, 0x00}, // Space
    {0x00, 0x00, 0x5F, 0x00, 0x00}, // !
    {0x00, 0x07, 0x00, 0x07, 0x00}, // "
    {0x14, 0x7F, 0x14, 0x7F, 0x14}, // #
    {0x24, 0x2A, 0x7F, 0x2A, 0x12}, // $
    {0x23, 0x13, 0x08, 0x64, 0x62}, // %
    {0x36, 0x49, 0x55, 0x22, 0x50}, // &
    {0x00, 0x05, 0x03, 0x00, 0x00}, // '
    {0x00, 0x1C, 0x22, 0x41, 0x00}, // (
    {0x00, 0x41, 0x22, 0x1C, 0x00}, // )
    {0x14, 0x08, 0x3E, 0x08, 0x14}, // *
    {0x08, 0x08, 0x3E, 0x08, 0x08}, // +
    {0x00, 0x50, 0x30, 0x00, 0x00}, // ,
    {0x08, 0x08, 0x08, 0x08, 0x08}, // -
    {0x00, 0x60, 0x60, 0x00, 0x00}, // .
    {0x20, 0x10, 0x08, 0x04, 0x02}, // /
    {0x3E, 0x51, 0x49, 0x45, 0x3E}, // 0
    {0x00, 0x42, 0x7F, 0x40, 0x00}, // 1
    {0x42, 0x61, 0x51, 0x49, 0x46}, // 2
    {0x21, 0x41, 0x45, 0x4B, 0x31}, // 3
    {0x18, 0x14, 0x12, 0x7F, 0x10}, // 4
    {0x27, 0x45, 0x45, 0x45, 0x39}, // 5
    {0x3C, 0x4A, 0x49, 0x49, 0x30}, // 6
    {0x01, 0x71, 0x09, 0x05, 0x03}, // 7
    {0x36, 0x49, 0x49, 0x49, 0x36}, // 8
    {0x06, 0x49, 0x49, 0x29, 0x1E}, // 9
    {0x00, 0x36, 0x36, 0x00, 0x00}, // :
    {0x00, 0x56, 0x36, 0x00, 0x00}, // ;
    {0x08, 0x14, 0x22, 0x41, 0x00}, // <
    {0x14, 0x14, 0x14, 0x14, 0x14}, // =
    {0x00, 0x41, 0x22, 0x14, 0x08}, // >
    {0x02, 0x01, 0x51, 0x09, 0x06}, // ?
    {0x32, 0x49, 0x79, 0x41, 0x3E}, // @
    {0x7E, 0x11, 0x11, 0x11, 0x7E}, // A
    {0x7F, 0x49, 0x49, 0x49, 0x36}, // B
    {0x3E, 0x41, 0x41, 0x41, 0x22}, // C
    {0x7F, 0x41, 0x41, 0x22, 0x1C}, // D
    {0x7F, 0x49, 0x49, 0x49, 0x41}, // E
    {0x7F, 0x09, 0x09, 0x09, 0x01}, // F
    {0x3E, 0x41, 0x49, 0x49, 0x7A}, // G
    {0x7F, 0x08, 0x08, 0x08, 0x7F}, // H
    {0x00, 0x41, 0x7F, 0x41, 0x00}, // I
    {0x20, 0x40, 0x41, 0x3F, 0x01}, // J
    {0x7F, 0x08, 0x14, 0x22, 0x41}, // K
    {0x7F, 0x40, 0x40, 0x40, 0x40}, // L
    {0x7F, 0x02, 0x0C, 0x02, 0x7F}, // M
    {0x7F, 0x04, 0x08, 0x10, 0x7F}, // N
    {0x3E, 0x41, 0x41, 0x41, 0x3E}, // O
    {0x7F, 0x09, 0x09, 0x09, 0x06}, // P
    {0x3E, 0x41, 0x51, 0x21, 0x5E}, // Q
    {0x7F, 0x09, 0x19, 0x29, 0x46}, // R
    {0x46, 0x49, 0x49, 0x49, 0x31}, // S
    {0x01, 0x01, 0x7F, 0x01, 0x01}, // T
    {0x3F, 0x40, 0x40, 0x40, 0x3F}, // U
    {0x1F, 0x20, 0x40, 0x20, 0x1F}, // V
    {0x3F, 0x40, 0x38, 0x40, 0x3F}, // W
    {0x63, 0x14, 0x08, 0x14, 0x63}, // X
    {0x07, 0x08, 0x70, 0x08, 0x07}, // Y
    {0x61, 0x51, 0x49, 0x45, 0x43}, // Z
    // ASCII 91–96: [ \ ] ^ _ `
    {0x00, 0x7F, 0x41, 0x41, 0x00}, // [
    {0x02, 0x04, 0x08, 0x10, 0x20}, // backslash
    {0x00, 0x41, 0x41, 0x7F, 0x00}, // ]
    {0x04, 0x02, 0x01, 0x02, 0x04}, // ^
    {0x40, 0x40, 0x40, 0x40, 0x40}, // _
    {0x00, 0x01, 0x02, 0x04, 0x00}, // `
    // ASCII 97–122: lowercase a–z
    {0x20, 0x54, 0x54, 0x54, 0x78}, // a
    {0x7F, 0x48, 0x44, 0x44, 0x38}, // b
    {0x38, 0x44, 0x44, 0x44, 0x20}, // c
    {0x38, 0x44, 0x44, 0x48, 0x7F}, // d
    {0x38, 0x54, 0x54, 0x54, 0x18}, // e
    {0x08, 0x7E, 0x09, 0x01, 0x02}, // f
    {0x0C, 0x52, 0x52, 0x52, 0x3E}, // g
    {0x7F, 0x08, 0x04, 0x04, 0x78}, // h
    {0x00, 0x44, 0x7D, 0x40, 0x00}, // i
    {0x20, 0x40, 0x44, 0x3D, 0x00}, // j
    {0x7F, 0x10, 0x28, 0x44, 0x00}, // k
    {0x00, 0x41, 0x7F, 0x40, 0x00}, // l
    {0x7C, 0x04, 0x18, 0x04, 0x78}, // m
    {0x7C, 0x08, 0x04, 0x04, 0x78}, // n
    {0x38, 0x44, 0x44, 0x44, 0x38}, // o
    {0x7C, 0x14, 0x14, 0x14, 0x08}, // p
    {0x08, 0x14, 0x14, 0x18, 0x7C}, // q
    {0x7C, 0x08, 0x04, 0x04, 0x08}, // r
    {0x48, 0x54, 0x54, 0x54, 0x20}, // s
    {0x04, 0x3F, 0x44, 0x40, 0x20}, // t
    {0x3C, 0x40, 0x40, 0x20, 0x7C}, // u
    {0x1C, 0x20, 0x40, 0x20, 0x1C}, // v
    {0x3C, 0x40, 0x30, 0x40, 0x3C}, // w
    {0x44, 0x28, 0x10, 0x28, 0x44}, // x
    {0x0C, 0x50, 0x50, 0x50, 0x3C}, // y
    {0x44, 0x64, 0x54, 0x4C, 0x44}, // z
    // ASCII 123–126: { | } ~
    {0x00, 0x08, 0x36, 0x41, 0x00}, // {
    {0x00, 0x00, 0x7F, 0x00, 0x00}, // |
    {0x00, 0x41, 0x36, 0x08, 0x00}, // }
    {0x10, 0x08, 0x08, 0x10, 0x08}, // ~
};

/* ---- the pre-change draw loop, writing straight into a 32bpp array ---- */
static void ref_text(uint32_t *buf, int x, int y, const char *text, uint32_t color, int scale) {
    int offset_x = 0;
    while (*text) {
        int idx = *text - ' ';
        if (idx < 0 || idx >= 95) { offset_x += 6 * scale; text++; continue; }
        for (int col = 0; col < 5; col++) {
            uint8_t column = old_font_5x7[idx][col];
            for (int row = 0; row < 7; row++) {
                if (column & (1 << row))
                    for (int sy = 0; sy < scale; sy++)
                        for (int sx = 0; sx < scale; sx++) {
                            int px = x + offset_x + col * scale + sx, py = y + row * scale + sy;
                            if (px >= 0 && px < W && py >= 0 && py < H) buf[py * W + px] = color;
                        }
            }
        }
        offset_x += 6 * scale;
        text++;
    }
}

static Framebuffer fb;
static uint32_t newbuf[W * H], refbuf[W * H];
#define INK 0x00FFFFFFu

static void new_text(const char *s, int scale) {
    memset(newbuf, 0, sizeof newbuf);
    fb.width = W; fb.height = H; fb.bytes_per_pixel = 4; fb.double_buffering = true;
    fb.draw_offset_x = 0; fb.draw_offset_y = 0; fb.buffer = NULL; fb.screen_size = 0;
    fb.back_buffer = newbuf; fb.back_buffer_size = sizeof newbuf;
    fb_draw_text(&fb, OX, OY, s, INK, scale);
}

static void old_text(const char *s, int scale) {
    memset(refbuf, 0, sizeof refbuf);
    ref_text(refbuf, OX, OY, s, INK, scale);
}

static int diff_count(const uint32_t *a, const uint32_t *b) {
    int n = 0;
    for (int i = 0; i < W * H; i++) if (a[i] != b[i]) n++;
    return n;
}

/* ink pixels of buf inside rows [r0, r1) (relative to OY) and columns [c0, c1) (relative to OX) */
static int ink_in(const uint32_t *buf, int r0, int r1, int c0, int c1) {
    int n = 0;
    for (int y = OY + r0; y < OY + r1 && y < H; y++)
        for (int x = OX + c0; x < OX + c1 && x < W; x++)
            if (y >= 0 && x >= 0 && buf[y * W + x]) n++;
    return n;
}

static int is_desc(char c) { return c == 'g' || c == 'j' || c == 'p' || c == 'q' || c == 'y'; }

int main(void) {
    /* negative control for the comparison itself */
    old_text("A", 1); new_text("A", 1);
    CHECK(diff_count(refbuf, newbuf) == 0, "control: A renders identically before the flip");
    newbuf[(OY + 3) * W + OX + 2] ^= 0x00FFFFFFu;
    CHECK(diff_count(refbuf, newbuf) == 1, "control: one flipped pixel counts as exactly one difference");

    int identical = 0, desc_ok = 0;
    for (int scale = 1; scale <= 3; scale++) {
        for (int c = 32; c <= 126; c++) {
            char s[2] = { (char)c, 0 };
            old_text(s, scale); new_text(s, scale);
            if (!is_desc((char)c)) {
                int d = diff_count(refbuf, newbuf);
                CHECK(d == 0, "scale %d '%c': %d pixels differ from the old font", scale, c, d);
                if (d == 0) identical++;
                CHECK(ink_in(newbuf, 7 * scale, H, 0, W) == 0, "scale %d '%c': ink below row 6", scale, c);
            } else {
                int below = ink_in(newbuf, 7 * scale, 9 * scale, 0, 6 * scale);
                CHECK(ink_in(refbuf, 7 * scale, H, 0, W) == 0, "scale %d '%c': reference has descender ink?", scale, c);
                CHECK(below > 0, "scale %d '%c': no ink in descender rows 7-8", scale, c);
                CHECK(ink_in(newbuf, 9 * scale, H, 0, W) == 0, "scale %d '%c': ink below row 8", scale, c);
                CHECK(ink_in(newbuf, 0, 7 * scale, 0, 6 * scale) > 0, "scale %d '%c': no ink above the baseline", scale, c);
                if (below > 0) desc_ok++;
            }
        }
    }
    printf("  ascii glyph renders identical: %d, descender glyphs ok: %d\n", identical, desc_ok);

    /* a mixed string without g j p q y: advance and every pixel identical */
    for (int scale = 1; scale <= 3; scale++) {
        const char *str = "Hello World! 0123 [x]~";
        old_text(str, scale); new_text(str, scale);
        CHECK(diff_count(refbuf, newbuf) == 0, "scale %d: whole string differs from the old font", scale);
    }

    /* UTF-8 */
    for (int scale = 1; scale <= 2; scale++) {
        int cell = 6 * scale;
        new_text("\xC3\xA9", scale);                       /* e-acute */
        int e_ink = ink_in(newbuf, 0, 9 * scale, 0, cell);
        CHECK(e_ink > 0, "scale %d: e-acute is blank", scale);
        uint32_t keep[W * H]; memcpy(keep, newbuf, sizeof keep);
        new_text("\xE6\x97\xA5", scale);                   /* CJK: unmapped -> box */
        int box_ink = ink_in(newbuf, 0, 9 * scale, 0, cell);
        CHECK(box_ink > 0, "scale %d: fallback box is blank", scale);
        CHECK(diff_count(keep, newbuf) > 0, "scale %d: e-acute drew the fallback box", scale);
        memcpy(keep, newbuf, sizeof keep);
        new_text("\xFF", scale);                           /* malformed byte */
        CHECK(diff_count(keep, newbuf) == 0, "scale %d: a malformed byte does not draw the same box", scale);
        new_text("\xC3", scale);                           /* truncated sequence */
        CHECK(diff_count(keep, newbuf) == 0, "scale %d: a truncated sequence does not draw the same box", scale);
        CHECK(ink_in(newbuf, 0, H, cell, W) == 0, "scale %d: the box spilled into the next cell", scale);
        new_text("\xE6\x97\xA5\xE6\x97\xA5", scale);       /* one box per code point */
        CHECK(ink_in(newbuf, 0, 9 * scale, cell, 2 * cell) == box_ink, "scale %d: second unmapped code point is not a box", scale);

        /* a code point advances exactly one cell; a control advances a blank cell */
        new_text("\xC3\xA9" "A", scale); memcpy(keep, newbuf, sizeof keep);
        new_text(" A", scale);
        CHECK(ink_in(keep, 0, H, cell, W) == ink_in(newbuf, 0, H, cell, W) && ink_in(keep, 0, H, cell, W) > 0,
              "scale %d: A after a 2-byte letter is not one cell further on", scale);
        new_text("\nA", scale);
        CHECK(ink_in(newbuf, 0, H, 0, cell) == 0, "scale %d: newline drew something", scale);
        CHECK(ink_in(newbuf, 0, H, cell, W) == ink_in(keep, 0, H, cell, W), "scale %d: newline did not advance one blank cell", scale);
    }

    /* every Latin-1 / Latin Ext-A letter stays inside its 6x9 cell */
    for (int cp = 0xA0; cp < 0x180; cp++) {
        char s[3];
        s[0] = (char)(0xC0 | (cp >> 6)); s[1] = (char)(0x80 | (cp & 0x3F)); s[2] = 0;
        new_text(s, 2);
        CHECK(ink_in(newbuf, 0, 18, 0, 12) == ink_in(newbuf, 0, H, 0, W), "U+%04X: ink outside the 6x9 cell", cp);
    }

    printf("%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
