/* Host regression for common/icons.c: the span geometry of the media glyphs, and that icon_draw paints exactly it. */
#include <stdio.h>
#include <string.h>
#include "icons.h"

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("  FAIL " __VA_ARGS__); printf("\n"); fails++; } } while (0)

#define MAXS 48
static uint32_t px[MAXS * MAXS];
static Framebuffer fb;

static void bitmap(IconKind k, int s, unsigned char m[MAXS][MAXS])
{
    memset(m, 0, MAXS * MAXS);
    for (int r = 0; r < s; r++) {
        int xs[2 * ICON_MAX_SPANS];
        int n = icon_row_spans(k, s, r, xs);
        for (int i = 0; i < n; i++)
            for (int x = xs[2 * i]; x < xs[2 * i + 1]; x++) m[r][x] = 1;
    }
}

int main(void)
{
    fb.back_buffer = px; fb.buffer = NULL; fb.width = MAXS; fb.height = MAXS;
    fb.bytes_per_pixel = 4; fb.double_buffering = true;
    static unsigned char m[MAXS][MAXS], mn[MAXS][MAXS];

    for (int k = 0; k < ICON_COUNT; k++)
        for (int s = 8; s <= 40; s += 4) {
            int area = 0;
            for (int r = 0; r < s; r++) {
                int xs[2 * ICON_MAX_SPANS], n = icon_row_spans((IconKind)k, s, r, xs), prev = 0;
                CHECK(n >= 0 && n <= ICON_MAX_SPANS, "kind %d size %d row %d: span count %d", k, s, r, n);
                for (int i = 0; i < n; i++) {
                    CHECK(xs[2 * i] >= prev && xs[2 * i] < xs[2 * i + 1] && xs[2 * i + 1] <= s,
                          "kind %d size %d row %d: run %d [%d,%d) out of order or outside the box", k, s, r, i, xs[2 * i], xs[2 * i + 1]);
                    if (i) CHECK(xs[2 * i] > prev, "kind %d size %d row %d: runs touch", k, s, r);
                    prev = xs[2 * i + 1];
                    area += xs[2 * i + 1] - xs[2 * i];
                }
            }
            CHECK(area >= s * s / 5, "kind %d size %d: area %d too small", k, s, area);
            CHECK(area <= s * s, "kind %d size %d: area %d beyond the box", k, s, area);
        }
    {   int xs[4];
        CHECK(icon_row_spans(ICON_PLAY, 20, -1, xs) == 0 && icon_row_spans(ICON_PLAY, 20, 20, xs) == 0, "rows outside the box are blank");
        CHECK(icon_row_spans(ICON_PLAY, 1, 0, xs) == 0 && icon_row_spans((IconKind)99, 20, 3, xs) == 0, "tiny size / unknown kind is blank");
    }

    /* the exact PLAY triangle at size 8: widths 1 3 5 7 7 5 3 1 */
    {   static const int want[8] = { 1, 3, 5, 7, 7, 5, 3, 1 };
        for (int r = 0; r < 8; r++) {
            int xs[4], n = icon_row_spans(ICON_PLAY, 8, r, xs);
            CHECK(n == 1 && xs[0] == 0 && xs[1] == want[r], "PLAY 8 row %d: width %d, want %d", r, n ? xs[1] - xs[0] : -1, want[r]);
        }
    }
    for (int s = 8; s <= 40; s += 4) {
        bitmap(ICON_PLAY, s, m);
        for (int r = 0; r < s; r++) for (int x = 0; x < s; x++)
            CHECK(m[r][x] == m[s - 1 - r][x], "PLAY %d not symmetric top/bottom at (%d,%d)", s, x, r);
        int xs[4];
        CHECK(icon_row_spans(ICON_PLAY, s, 0, xs) == 1 && xs[1] - xs[0] == 1, "PLAY %d: apex row is one pixel wide", s);
        /* PREV is NEXT mirrored left to right */
        bitmap(ICON_NEXT, s, mn); bitmap(ICON_PREV, s, m);
        for (int r = 0; r < s; r++) for (int x = 0; x < s; x++)
            CHECK(mn[r][x] == m[r][s - 1 - x], "PREV %d is not NEXT mirrored at (%d,%d)", s, x, r);
        /* NEXT: a bar on the right edge full height, a triangle apart from it */
        bitmap(ICON_NEXT, s, mn);
        CHECK(mn[0][s - 1] && mn[s - 1][s - 1] && mn[s / 2][s - 1], "NEXT %d: the bar is not full height at the right edge", s);
        CHECK(mn[s / 2][0] && !mn[0][0], "NEXT %d: triangle apex/base wrong", s);
        /* PAUSE: two bars of equal width with a gap */
        int n = icon_row_spans(ICON_PAUSE, s, s / 2, xs ), n2;
        int ys[4]; n2 = icon_row_spans(ICON_PAUSE, s, 0, ys);
        CHECK(n == 2 && n2 == 2 && xs[1] - xs[0] == xs[3] - xs[2] && xs[2] > xs[1], "PAUSE %d: not two equal bars with a gap", s);
        /* STOP: filled square, left/right symmetric and top/bottom symmetric */
        bitmap(ICON_STOP, s, m);
        for (int r = 0; r < s; r++) for (int x = 0; x < s; x++)
            CHECK(m[r][x] == m[s - 1 - r][s - 1 - x], "STOP %d not point symmetric at (%d,%d)", s, x, r);
        /* DROPDOWN: left/right symmetric, narrowing downward, one row never wider than the one above */
        bitmap(ICON_DROPDOWN, s, m);
        int last_w = 1 << 20;
        for (int r = 0; r < s; r++) {
            int w = 0;
            for (int x = 0; x < s; x++) { w += m[r][x]; CHECK(m[r][x] == m[r][s - 1 - x], "DROPDOWN %d not symmetric at (%d,%d)", s, x, r); }
            if (w) { CHECK(w <= last_w, "DROPDOWN %d widens downward at row %d", s, r); last_w = w; }
        }
    }

    /* icon_draw paints exactly the spans, in the colour given, and nothing outside the box */
    for (int k = 0; k < ICON_COUNT; k++) {
        int s = 20;
        for (int i = 0; i < MAXS * MAXS; i++) px[i] = 0;
        icon_draw(&fb, (IconKind)k, 5, 7, s, 0x00123456u);
        bitmap((IconKind)k, s, m);
        int bad = 0;
        for (int y = 0; y < MAXS; y++) for (int x = 0; x < MAXS; x++) {
            bool in = x >= 5 && x < 5 + s && y >= 7 && y < 7 + s && m[y - 7][x - 5];
            if (in != ((px[y * MAXS + x] & 0xFFFFFF) == 0x123456u)) bad++;
        }
        CHECK(bad == 0, "icon_draw kind %d differs from its spans at %d pixels", k, bad);
    }

    printf("icons_test: %d failed\n", fails);
    return fails ? 1 : 0;
}
