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

    printf("%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
