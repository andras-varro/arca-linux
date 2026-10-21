/* Host-side regression for common/list_view.c — the pure half of the list widget.
 *
 * Runs on the DEV MACHINE with native gcc; no framebuffer is opened (lv_draw is
 * linked but never called).  Build and run:
 *
 *   cd native_apps && gcc -Wall -Wextra -Wno-unused-parameter -I common \
 *       -o build/list_view_test tests/list_view_test.c common/list_view.c \
 *       <the object list in tests/run-all.sh> && ./build/list_view_test
 *
 * Groups: 1 page and clamp arithmetic, 2 cursor keys, 3 hit test, 4 scroll
 * thumb, 5 title fit, 6 drag gesture, 7 marks and re-count, 8 lv_input over
 * TouchState / InputState (tap, second tap, drag, keys, auto-repeat).
 *
 * What it cannot see: the drawing (colours, glyphs, the scroll bar on the
 * panel) and whether a real finger stays inside LV_DRAG_DEADZONE during a
 * tap.  Both are the panel's to judge.
 */
#include "list_view.h"
#include "utf8.h"
#include <stdio.h>
#include <string.h>

static int fails, checks;

#define CHECK(cond) do { checks++; if (!(cond)) { fails++; \
    printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

static int g_count;
static int src_count(void *ctx) { (void)ctx; return g_count; }
static void src_row(void *ctx, int i, LvRow *out) { (void)ctx; (void)i; out->title = "x"; }

static void make(ListView *lv, int count, int rect_h, int row_h) {
    g_count = count;
    LvSource s = { src_count, src_row, NULL };
    UiRect r = { 10, 20, 395, rect_h };
    lv_init(lv, s, r, row_h);
}

/* A TouchState for one frame. */
static TouchState T(int x, int y, bool pressed, bool held, bool released) {
    TouchState t; memset(&t, 0, sizeof(t));
    t.x = x; t.y = y; t.pressed = pressed; t.held = held; t.released = released;
    return t;
}

int main(void) {
    /* 1. arithmetic: 402 px rect, 40 px rows = (402-2)/40 = 10 rows */
    CHECK(lv_vis_rows(402, 40) == 10);
    CHECK(lv_vis_rows(401, 40) == 9);
    CHECK(lv_vis_rows(30, 40) == 0);
    CHECK(lv_vis_rows(100, 0) == 0);
    CHECK(lv_max_top(25, 10) == 15);
    CHECK(lv_max_top(5, 10) == 0);
    CHECK(lv_clamp_top(99, 25, 10) == 15);
    CHECK(lv_clamp_top(-4, 25, 10) == 0);
    CHECK(lv_clamp_top(3, 5, 10) == 0);
    CHECK(lv_clamp_cursor(5, 0) == -1);
    CHECK(lv_clamp_cursor(-3, 4) == 0);
    CHECK(lv_clamp_cursor(9, 4) == 3);
    CHECK(lv_top_for_cursor(0, 12, 25, 10) == 3);     /* shows row 12 at the bottom */
    CHECK(lv_top_for_cursor(8, 2, 25, 10) == 2);
    CHECK(lv_top_for_cursor(4, 6, 25, 10) == 4);      /* already visible: unchanged */
    CHECK(lv_top_for_cursor(4, 24, 25, 10) == 15);
    CHECK(lv_top_for_cursor(7, -1, 0, 10) == 0);

    /* 2. cursor keys: no wrap, a page is vis-1 */
    CHECK(lv_cursor_after(0, 25, 10, LV_KEY_UP) == 0);
    CHECK(lv_cursor_after(24, 25, 10, LV_KEY_DOWN) == 24);
    CHECK(lv_cursor_after(0, 25, 10, LV_KEY_PGDN) == 9);
    CHECK(lv_cursor_after(20, 25, 10, LV_KEY_PGDN) == 24);
    CHECK(lv_cursor_after(5, 25, 10, LV_KEY_PGUP) == 0);
    CHECK(lv_cursor_after(7, 25, 10, LV_KEY_HOME) == 0);
    CHECK(lv_cursor_after(7, 25, 10, LV_KEY_END) == 24);
    CHECK(lv_cursor_after(3, 0, 10, LV_KEY_DOWN) == -1);
    CHECK(lv_cursor_after(-1, 5, 10, LV_KEY_DOWN) == 1);
    CHECK(lv_cursor_after(0, 5, 1, LV_KEY_PGDN) == 1);   /* a page is never 0 */
    CHECK(lv_cursor_after(2, 5, 10, LV_KEY_MARK) == 2);

    /* 3. hit test */
    CHECK(lv_hit_row(0, 0, 40, 10, 25) == 0);
    CHECK(lv_hit_row(0, 39, 40, 10, 25) == 0);
    CHECK(lv_hit_row(0, 40, 40, 10, 25) == 1);
    CHECK(lv_hit_row(5, 85, 40, 10, 25) == 7);
    CHECK(lv_hit_row(0, 400, 40, 10, 25) == -1);      /* below the 10th row */
    CHECK(lv_hit_row(0, -1, 40, 10, 25) == -1);
    CHECK(lv_hit_row(0, 120, 40, 10, 3) == -1);       /* past the end of a short list */

    /* 4. thumb */
    int off, len;
    lv_thumb(10, 10, 0, 400, &off, &len);
    CHECK(len == 400 && off == 0);
    lv_thumb(20, 10, 0, 400, &off, &len);
    CHECK(len == 200 && off == 0);
    lv_thumb(20, 10, 10, 400, &off, &len);
    CHECK(len == 200 && off == 200);                  /* bottom: thumb ends on the track */
    lv_thumb(1000, 10, 0, 400, &off, &len);
    CHECK(len == 12);                                 /* never a sliver */
    lv_thumb(1000, 10, 990, 400, &off, &len);
    CHECK(off + len == 400);

    /* 5. fit: case kept, "..." when cut, always terminated */
    char b[32];
    lv_fit(b, sizeof(b), "Readme.txt", 200, 2);
    CHECK(strcmp(b, "Readme.txt") == 0);
    lv_fit(b, sizeof(b), "Readme.txt", 60, 2);        /* 5 chars of 12 px */
    CHECK(strcmp(b, "Re...") == 0);
    lv_fit(b, sizeof(b), "Readme.txt", 24, 2);        /* 2 chars: dots only */
    CHECK(strcmp(b, "..") == 0);
    lv_fit(b, sizeof(b), "abcdefgh", 100, 1);         /* 16 chars: fits */
    CHECK(strcmp(b, "abcdefgh") == 0);
    lv_fit(b, 6, "abcdefghij", 0, 1);                 /* no width limit, small buffer */
    CHECK(strcmp(b, "ab...") == 0);
    lv_fit(b, 1, "abc", 100, 1);
    CHECK(b[0] == '\0');

    /* 5b. fit with multibyte titles: code points counted, cut on a boundary, case kept */
    lv_fit(b, sizeof(b), "\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9", 30, 1);
    CHECK(strcmp(b, "\xC3\xA9\xC3\xA9...") == 0);     /* 5 of 8 fit: 2 letters + dots */
    lv_fit(b, sizeof(b), "\xC3\x9C" "n\xC3\xAF", 100, 1);
    CHECK(strcmp(b, "\xC3\x9C" "n\xC3\xAF") == 0);    /* fits whole, case untouched */
    lv_fit(b, 7, "\xD0\xB6\xD0\xB6\xD0\xB6\xD0\xB6\xD0\xB6\xD0\xB6\xD0\xB6\xD0\xB6", 0, 1);
    CHECK(strcmp(b, "\xD0\xB6...") == 0);             /* buffer cut after 1 whole letter */
    lv_fit(b, 5, "\xC3\xA9\xC3\xA9\xC3\xA9", 0, 1);   /* 6 bytes into dst[5] */
    CHECK(strcmp(b, "...") == 0);                     /* no half sequence */
    {
        const char *mix = "\xC5\x91\xC3\xA9\xD0\xB6\xCE\xA9\xC3\x9F" "abc\xE2\x82\xAC" "xyz";
        int bad = 0;
        for (int n = 1; n <= 24; n++)
            for (int w = 0; w <= 120; w += 6) {
                char g[40];
                memset(g, 0x5A, sizeof g);
                lv_fit(g, n, mix, w, 1);
                const char *p = g;
                while (*p) if (utf8_next(&p) == UTF8_REPLACEMENT) { bad++; break; }
                if (!memchr(g, '\0', (size_t)n)) bad++;
                for (int i = n; i < 40; i++) if (g[i] != 0x5A) { bad++; break; }
            }
        CHECK(bad == 0);                              /* always well-formed, terminated, in bounds */
    }

    /* 6. drag gesture */
    LvDrag d;
    lv_drag_begin(&d, 100);
    CHECK(lv_drag_move(&d, 100 + LV_DRAG_DEADZONE, 40) == 0 && !d.dragging);
    CHECK(lv_drag_move(&d, 100 + LV_DRAG_DEADZONE + 1, 40) == 0 && d.dragging);
    CHECK(lv_drag_move(&d, 100 + LV_DRAG_DEADZONE + 1 + 39, 40) == 0);
    CHECK(lv_drag_move(&d, 100 + LV_DRAG_DEADZONE + 1 + 40, 40) == -1);   /* finger down: earlier rows */
    CHECK(lv_drag_move(&d, 100 + LV_DRAG_DEADZONE + 1 + 40 - 85, 40) == 2); /* finger up 85 px */
    lv_drag_begin(&d, 100);
    lv_drag_move(&d, 90 - 1, 40);                      /* upward past the dead zone */
    CHECK(d.dragging);

    /* 7. marks and re-count */
    ListView lv;
    make(&lv, 25, 402, 40);
    CHECK(lv_visible_rows(&lv) == 10 && lv.scale == 2 && lv.cursor == 0);
    CHECK(lv_mark_count(&lv) == 0);
    CHECK(lv_key(&lv, LV_KEY_MARK) == LV_EV_CHANGED && lv_is_marked(&lv, 0));
    CHECK(lv_key(&lv, LV_KEY_MARK_NEXT) == LV_EV_CHANGED);   /* unmarks 0, cursor to 1 */
    CHECK(!lv_is_marked(&lv, 0) && lv.cursor == 1);
    CHECK(lv_key(&lv, LV_KEY_MARK_NEXT) == LV_EV_CHANGED && lv_is_marked(&lv, 1) && lv.cursor == 2);
    CHECK(lv_key(&lv, LV_KEY_MARK_ALL) == LV_EV_CHANGED && lv_mark_count(&lv) == 25);
    CHECK(lv_key(&lv, LV_KEY_MARK_CLEAR) == LV_EV_CHANGED && lv_mark_count(&lv) == 0);
    CHECK(lv_key(&lv, LV_KEY_MARK_CLEAR) == LV_EV_NONE);
    lv_key(&lv, LV_KEY_END);
    CHECK(lv.cursor == 24 && lv.top == 15);
    lv_key(&lv, LV_KEY_MARK);                                /* mark 24 */
    lv_move_to(&lv, 3); lv_key(&lv, LV_KEY_MARK);            /* mark 3 */
    CHECK(lv_marked_next(&lv, 0) == 3 && lv_marked_next(&lv, 4) == 24 && lv_marked_next(&lv, 25) == -1);
    g_count = 10; lv_sync(&lv);                              /* trim: 24 goes, 3 stays */
    CHECK(lv_is_marked(&lv, 3) && lv_mark_count(&lv) == 1 && lv.top == 0);
    g_count = 30; lv_sync(&lv);                              /* regrow: 24 must not return */
    CHECK(!lv_is_marked(&lv, 24) && lv_is_marked(&lv, 3) && lv_mark_count(&lv) == 1);
    lv_reset(&lv);
    CHECK(lv_mark_count(&lv) == 0 && lv.cursor == 0 && lv.top == 0 && lv.count == 30);
    g_count = 0; lv_sync(&lv);
    CHECK(lv.cursor == -1 && lv_key(&lv, LV_KEY_DOWN) == LV_EV_NONE && lv_key(&lv, LV_KEY_MARK) == LV_EV_NONE);
    lv_free(&lv);

    /* 8. lv_input.  rect y = 20, border 1, rows 40 px: row i spans y 21+40i .. */
    make(&lv, 25, 402, 40);
    lv.active = true;
    TouchState t = T(100, 21 + 40 * 3 + 5, true, true, false);   /* press on row 3 */
    CHECK(lv_input(&lv, &t, NULL, 0) == LV_EV_NONE);
    t = T(100, 21 + 40 * 3 + 7, false, false, true);             /* release: a tap, 2 px jitter */
    CHECK(lv_input(&lv, &t, NULL, 10) == LV_EV_TAP && lv.cursor == 3);
    t = T(100, 21 + 40 * 3 + 5, true, true, false); lv_input(&lv, &t, NULL, 20);
    t = T(100, 21 + 40 * 3 + 5, false, false, true);
    CHECK(lv_input(&lv, &t, NULL, 30) == LV_EV_ACTIVATE);        /* second tap on the cursor row */
    lv.active = false;
    t = T(100, 21 + 40 * 3 + 5, true, true, false); lv_input(&lv, &t, NULL, 40);
    t = T(100, 21 + 40 * 3 + 5, false, false, true);
    CHECK(lv_input(&lv, &t, NULL, 50) == LV_EV_TAP);             /* inactive list never activates by tap */
    lv.active = true;
    /* quick tap: press and release in one poll */
    t = T(100, 21 + 40 * 5 + 5, true, false, true);
    CHECK(lv_input(&lv, &t, NULL, 60) == LV_EV_TAP && lv.cursor == 5);
    /* a press outside is ignored */
    t = T(700, 100, true, true, false); lv_input(&lv, &t, NULL, 70);
    t = T(700, 100, false, false, true);
    CHECK(lv_input(&lv, &t, NULL, 80) == LV_EV_NONE && lv.cursor == 5);
    /* drag: finger moves up 100 px from y=300 (2 rows + dead zone) so the list scrolls down */
    t = T(100, 300, true, true, false); lv_input(&lv, &t, NULL, 90);
    t = T(100, 270, false, true, false);                          /* past the dead zone, anchor here */
    CHECK(lv_input(&lv, &t, NULL, 100) == LV_EV_NONE && lv.top == 0);
    t = T(100, 190, false, true, false);                          /* 80 px up = 2 rows */
    CHECK(lv_input(&lv, &t, NULL, 110) == LV_EV_CHANGED && lv.top == 2);
    t = T(100, 190, false, false, true);
    CHECK(lv_input(&lv, &t, NULL, 120) == LV_EV_NONE && lv.cursor == 5);   /* a drag is not a tap */
    CHECK(!lv.drag.active);
    /* never scrolls past the end */
    t = T(100, 400, true, true, false); lv_input(&lv, &t, NULL, 130);
    t = T(100, 380, false, true, false); lv_input(&lv, &t, NULL, 131);
    t = T(100, 20, false, true, false); lv_input(&lv, &t, NULL, 132);
    CHECK(lv.top == 11);
    lv_sync(&lv);                            /* every frame: must not snap back to the cursor */
    CHECK(lv.top == 11 && lv.cursor >= 11 && lv.cursor < 21);
    t = T(100, 20, false, false, true); lv_input(&lv, &t, NULL, 133);
    /* the cursor the drag carried along is not "the row already chosen" */
    t = T(100, 21 + 40 * (lv.cursor - lv.top) + 5, true, false, true);
    CHECK(lv_input(&lv, &t, NULL, 133) == LV_EV_TAP);
    t = T(100, 21 + 40 * (lv.cursor - lv.top) + 5, true, false, true);
    CHECK(lv_input(&lv, &t, NULL, 133) == LV_EV_ACTIVATE);
    t = T(100, 400, true, true, false); lv_input(&lv, &t, NULL, 134);
    t = T(100, 380, false, true, false); lv_input(&lv, &t, NULL, 135);
    t = T(100, 20, false, true, false); lv_input(&lv, &t, NULL, 136);
    CHECK(lv.top == 15);
    t = T(100, 20, false, false, true); lv_input(&lv, &t, NULL, 133);
    /* keys: only when active, edge then auto-repeat */
    lv_move_to(&lv, 0);
    InputState in; memset(&in, 0, sizeof(in));
    in.buttons[BTN_ID_DOWN].pressed = in.buttons[BTN_ID_DOWN].held = true;
    CHECK(lv_input(&lv, NULL, &in, 1000) == LV_EV_CHANGED && lv.cursor == 1);
    in.buttons[BTN_ID_DOWN].pressed = false;
    CHECK(lv_input(&lv, NULL, &in, 1200) == LV_EV_NONE && lv.cursor == 1);   /* inside the delay */
    CHECK(lv_input(&lv, NULL, &in, 1400) == LV_EV_CHANGED && lv.cursor == 2); /* repeat starts */
    CHECK(lv_input(&lv, NULL, &in, 1440) == LV_EV_NONE && lv.cursor == 2);    /* inside the interval */
    CHECK(lv_input(&lv, NULL, &in, 1480) == LV_EV_CHANGED && lv.cursor == 3);
    lv.active = false;
    in.buttons[BTN_ID_DOWN].pressed = true;
    CHECK(lv_input(&lv, NULL, &in, 2000) == LV_EV_NONE && lv.cursor == 3);   /* inactive: keys ignored */
    lv.active = true;
    memset(&in, 0, sizeof(in));
    in.buttons[BTN_ID_ACTION].pressed = true;
    CHECK(lv_input(&lv, NULL, &in, 2100) == LV_EV_ACTIVATE);
    CHECK(lv_repeat_due(0xFFFFFF00u, 0xFFFFFF00u, 0x00000400u));              /* clock wrap */
    lv_free(&lv);
    /* 9. keys: Page Up / Page Down / Home / End through lv_input, with repeat */
    make(&lv, 100, 402, 40);
    lv.active = true;
    memset(&in, 0, sizeof(in));
    in.buttons[BTN_ID_PGDN].pressed = in.buttons[BTN_ID_PGDN].held = true;
    CHECK(lv_input(&lv, NULL, &in, 5000) == LV_EV_CHANGED && lv.cursor >= 9);
    int after_one = lv.cursor;
    in.buttons[BTN_ID_PGDN].pressed = false;
    CHECK(lv_input(&lv, NULL, &in, 5200) == LV_EV_NONE && lv.cursor == after_one);
    CHECK(lv_input(&lv, NULL, &in, 5400) == LV_EV_CHANGED && lv.cursor > after_one);   /* repeat */
    memset(&in, 0, sizeof(in));
    in.buttons[BTN_ID_END].pressed = in.buttons[BTN_ID_END].held = true;
    CHECK(lv_input(&lv, NULL, &in, 6000) == LV_EV_CHANGED && lv.cursor == 99);
    memset(&in, 0, sizeof(in));
    in.buttons[BTN_ID_PGUP].pressed = in.buttons[BTN_ID_PGUP].held = true;
    CHECK(lv_input(&lv, NULL, &in, 6100) == LV_EV_CHANGED && lv.cursor < 99 && lv.cursor >= 80);
    memset(&in, 0, sizeof(in));
    in.buttons[BTN_ID_HOME].pressed = in.buttons[BTN_ID_HOME].held = true;
    CHECK(lv_input(&lv, NULL, &in, 6200) == LV_EV_CHANGED && lv.cursor == 0 && lv.top == 0);
    lv_free(&lv);

    /* 10. velocity arithmetic: 160 px up in 64 ms = 62 rows/s, capped */
    {
        LvSample s[5] = { {0,400}, {16,360}, {32,320}, {48,280}, {64,240} };
        CHECK(lv_fling_velocity(s, 5, 40) == LV_FLING_MAX);
        LvSample m[3] = { {0,400}, {50,380}, {100,360} };          /* 40 px/100 ms = 10 rows/s */
        CHECK(lv_fling_velocity(m, 3, 40) == 10000);
        LvSample dn[3] = { {0,300}, {50,320}, {100,340} };         /* finger down = earlier rows */
        CHECK(lv_fling_velocity(dn, 3, 40) == -10000);
        LvSample slow[3] = { {0,400}, {50,395}, {100,390} };       /* 1.25 rows/s */
        CHECK(lv_fling_velocity(slow, 3, 40) == 0);
        LvSample old[3] = { {0,400}, {300,100}, {400,99} };        /* the fast part is outside the window */
        CHECK(lv_fling_velocity(old, 3, 40) == 0);
        LvSample brief[2] = { {0,400}, {10,300} };                 /* span under LV_FLING_MIN_MS */
        CHECK(lv_fling_velocity(brief, 2, 40) == 0);
        CHECK(lv_fling_velocity(s, 1, 40) == 0);
        /* measured on .115 (file_manager, arca_tap): the release frame came 72 ms after
         * the finger stopped, so a window ending at the release saw only the still tail */
        LvSample flick[6] = { {0,274}, {34,230}, {104,208}, {138,186}, {172,130}, {244,130} };
        CHECK(lv_fling_velocity(flick, 6, 40) > LV_FLING_START);
        LvSample drag[8] = { {0,158}, {34,154}, {68,149}, {102,143}, {136,139}, {170,134}, {203,130}, {237,130} };
        CHECK(lv_fling_velocity(drag, 8, 40) == 0);                /* the same rig, a 1500 ms drag */
        LvSample held[5] = { {0,400}, {16,300}, {32,200}, {150,199}, {200,200} };  /* stop, hold, lift */
        CHECK(lv_fling_velocity(held, 5, 40) == 0);
    }

    /* 11. fling: a flick keeps scrolling after release, cursor rides along */
    make(&lv, 100, 402, 40);
    lv.active = true;
    t = T(100, 400, true, true, false);  lv_input(&lv, &t, NULL, 10000);
    t = T(100, 360, false, true, false); lv_input(&lv, &t, NULL, 10016);
    t = T(100, 320, false, true, false); lv_input(&lv, &t, NULL, 10032);
    t = T(100, 280, false, true, false); lv_input(&lv, &t, NULL, 10048);
    t = T(100, 240, false, false, true); lv_input(&lv, &t, NULL, 10064);
    CHECK(lv.fling_v > 0);
    {
        int top_release = lv.top, changed = 0, i;
        uint32_t now = 10064;
        for (i = 0; i < 400 && lv.fling_v != 0; i++) {
            now += 16;
            if (lv_input(&lv, NULL, NULL, now) == LV_EV_CHANGED) changed++;
            lv_sync(&lv);                                          /* must not snap back */
            CHECK(lv.cursor >= lv.top && lv.cursor < lv.top + 10);
        }
        CHECK(lv.fling_v == 0 && i < 400);                         /* it decays and stops */
        CHECK(changed > 5 && lv.top >= top_release + 10);
        CHECK(lv.top - top_release >= 36);                         /* about v x LV_FLING_DECAY_MS: 45 rows here, 20 at the old 350 ms */
        CHECK(lv_input(&lv, NULL, NULL, now + 16) == LV_EV_NONE);  /* and then reports nothing */
        CHECK(!lv_needs_frames(&lv));                              /* the loop may idle again */
    }
    lv_free(&lv);

    /* frame pacing: a finger down inside one row, and a running fling, both need frames */
    make(&lv, 100, 402, 40);
    lv.active = true;
    CHECK(!lv_needs_frames(&lv));
    t = T(100, 400, true, true, false);  lv_input(&lv, &t, NULL, 16000);
    CHECK(lv_needs_frames(&lv));                                   /* nothing redrew yet */
    t = T(100, 380, false, true, false); lv_input(&lv, &t, NULL, 16033);
    CHECK(lv_needs_frames(&lv) && lv.top == 0);                    /* dragging, still row 0 */
    t = T(100, 340, false, true, false); lv_input(&lv, &t, NULL, 16066);
    t = T(100, 300, false, false, true); lv_input(&lv, &t, NULL, 16099);
    CHECK(lv.fling_v != 0 && lv_needs_frames(&lv));                /* 33 ms frames do fling */
    lv_move_to(&lv, 0);
    CHECK(!lv_needs_frames(&lv));
    lv_free(&lv);

    /* a slow drag that ends still, and a plain tap, start no fling */
    make(&lv, 100, 402, 40);
    lv.active = true;
    t = T(100, 400, true, true, false);  lv_input(&lv, &t, NULL, 11000);
    t = T(100, 300, false, true, false); lv_input(&lv, &t, NULL, 11300);
    t = T(100, 300, false, true, false); lv_input(&lv, &t, NULL, 11400);
    t = T(100, 300, false, false, true); lv_input(&lv, &t, NULL, 11500);
    CHECK(lv.fling_v == 0);
    t = T(100, 100, true, true, false);  lv_input(&lv, &t, NULL, 12000);
    t = T(100, 100, false, false, true);
    CHECK(lv_input(&lv, &t, NULL, 12050) == LV_EV_TAP && lv.fling_v == 0);
    lv_free(&lv);

    /* a press during a fling stops it and its release is not a tap; a key stops it too */
    make(&lv, 100, 402, 40);
    lv.active = true;
    t = T(100, 400, true, true, false);  lv_input(&lv, &t, NULL, 13000);
    t = T(100, 360, false, true, false); lv_input(&lv, &t, NULL, 13016);
    t = T(100, 320, false, true, false); lv_input(&lv, &t, NULL, 13032);
    t = T(100, 280, false, true, false); lv_input(&lv, &t, NULL, 13048);
    t = T(100, 240, false, false, true); lv_input(&lv, &t, NULL, 13064);
    CHECK(lv.fling_v != 0);
    lv_input(&lv, NULL, NULL, 13080);
    int top_stop = lv.top;
    t = T(100, 200, true, true, false);  lv_input(&lv, &t, NULL, 13096);
    CHECK(lv.fling_v == 0 && lv.top == top_stop);
    t = T(100, 200, false, false, true);
    CHECK(lv_input(&lv, &t, NULL, 13112) == LV_EV_NONE);
    t = T(100, 200, true, true, false);  lv_input(&lv, &t, NULL, 13200);   /* a fresh press taps again */
    t = T(100, 200, false, false, true);
    CHECK(lv_input(&lv, &t, NULL, 13250) == LV_EV_TAP);
    t = T(100, 400, true, true, false);  lv_input(&lv, &t, NULL, 14000);
    t = T(100, 360, false, true, false); lv_input(&lv, &t, NULL, 14016);
    t = T(100, 320, false, true, false); lv_input(&lv, &t, NULL, 14032);
    t = T(100, 280, false, true, false); lv_input(&lv, &t, NULL, 14048);
    t = T(100, 240, false, false, true); lv_input(&lv, &t, NULL, 14064);
    CHECK(lv.fling_v != 0);
    memset(&in, 0, sizeof(in));
    in.buttons[BTN_ID_DOWN].pressed = in.buttons[BTN_ID_DOWN].held = true;
    lv_input(&lv, NULL, &in, 14080);
    CHECK(lv.fling_v == 0);
    lv_free(&lv);

    /* a fling stops at the list end instead of running on */
    make(&lv, 30, 402, 40);                                         /* max top 20 */
    lv.active = true;
    t = T(100, 400, true, true, false);  lv_input(&lv, &t, NULL, 15000);
    t = T(100, 360, false, true, false); lv_input(&lv, &t, NULL, 15016);
    t = T(100, 320, false, true, false); lv_input(&lv, &t, NULL, 15032);
    t = T(100, 280, false, true, false); lv_input(&lv, &t, NULL, 15048);
    t = T(100, 240, false, false, true); lv_input(&lv, &t, NULL, 15064);
    {
        uint32_t now = 15064;
        for (int i = 0; i < 400 && lv.fling_v != 0; i++) { now += 16; lv_input(&lv, NULL, NULL, now); lv_sync(&lv); }
        CHECK(lv.fling_v == 0 && lv.top == 20 && lv.cursor >= 20 && lv.cursor < 30);
    }
    lv_free(&lv);


    printf("%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
