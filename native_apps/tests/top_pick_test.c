/* Host-side regression for common/top_pick.c: the top bar drop-down.  Header target, popup geometry, open / close,
 * cursor, pick, and scrolling when the entries outnumber the rows under the header.
 *
 * Runs on the DEV MACHINE with native gcc; no framebuffer is opened (the drawing is linked but never called).  Build
 * and run with the object list tests/run-all.sh gives top_pick_test:
 *
 *   cd native_apps && gcc -Wall -Wextra -Wno-unused-parameter -I common -o build/top_pick_test \
 *       tests/top_pick_test.c common/top_pick.c common/icons.c <list_view and its objects> && ./build/top_pick_test
 *
 * What it cannot see: that a user puts the header rect where it draws the title (Jellyfin is covered at its end by
 * jf_ui_layout_test, the Control Panel is judged on the panel) and the drawn popup.
 */
#include "top_pick.h"
#include <stdio.h>
#include <string.h>

static int fails, checks;

#define CHECK(cond) do { checks++; if (!(cond)) { fails++; \
    printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

static const char *names[40];
static char nm[40][8];
static int n_entries;
static int src_n(void *ctx) { (void)ctx; return n_entries; }
static const char *src_name(void *ctx, int i) { (void)ctx; return names[i]; }

/* 800 x 480 screen, the bar's header around x 292..468 */
static const TpExt E = { 0, 0, 800, 480 };
static const UiRect HEAD = { 292, 0, 176, 44 };

static TouchState touch(int x, int y, bool pressed, bool held, bool released)
{
    TouchState t;
    memset(&t, 0, sizeof t);
    t.x = x; t.y = y; t.pressed = pressed; t.held = held; t.released = released;
    return t;
}

static InputState key(int btn)
{
    InputState in;
    memset(&in, 0, sizeof in);
    if (btn >= 0) in.buttons[btn].pressed = true;
    return in;
}

static TouchState idle(void) { return touch(0, 0, false, false, false); }

static uint32_t clk = 1000;
static TpEvent press_key(TopPick *tp, int btn)
{
    InputState in = key(btn);
    TouchState t = idle();
    clk += 700;   /* clear of the auto-repeat window */
    return tp_input(tp, &t, &in, clk);
}

static TpEvent tap(TopPick *tp, int x, int y)
{
    InputState in = key(-1);
    TouchState t = touch(x, y, true, true, false);
    clk += 700;
    TpEvent first = tp_input(tp, &t, &in, clk);
    if (first == TP_CLOSED) return first;
    t = touch(x, y, false, false, true);
    clk += 50;
    TpEvent e = tp_input(tp, &t, &in, clk);
    return e != TP_NONE ? e : first;
}

static void fresh(TopPick *tp, int n)
{
    n_entries = n;
    TpSource s = { src_n, src_name, NULL };
    tp_init(tp, s);
}

static void test_geometry(void)
{
    UiRect h = tp_head_rect(300, 160, 0, 44, &E, false);
    CHECK(h.x == 292 && h.w == 176 && h.y == 0 && h.h == 44);
    UiRect ha = tp_head_rect(300, 160, 0, 44, &E, true);
    CHECK(ha.w == 176 + TP_ARROW + TP_GAP);
    TpExt in = { 20, 12, 780, 470 };
    UiRect hc = tp_head_rect(10, 50, 0, 44, &in, true);
    CHECK(hc.x == 20 && hc.y == 12 && hc.h == 32);
    UiRect hr = tp_head_rect(700, 200, 0, 44, &in, true);
    CHECK(hr.x + hr.w == 780);

    UiRect p = tp_popup_rect(HEAD, 5, &E);
    CHECK(p.x == 292 && p.y == 48 && p.w == TP_W && p.h == 5 * TP_ROW_H + 2 * LV_BORDER);
    UiRect big = tp_popup_rect(HEAD, 30, &E);
    CHECK(big.y + big.h <= 480 && big.h < 30 * TP_ROW_H);
    CHECK((big.h - 2 * LV_BORDER) % TP_ROW_H == 0 && big.h > TP_ROW_H);
    UiRect none = tp_popup_rect(HEAD, 0, &E);
    CHECK(none.h == TP_ROW_H + 2 * LV_BORDER);   /* never zero rows */
    UiRect hd2 = { 700, 0, 90, 44 };
    UiRect right = tp_popup_rect(hd2, 5, &E);
    CHECK(right.x + right.w <= 800);
    TpExt narrow = { 0, 0, 200, 480 };
    UiRect hd3 = { 40, 0, 100, 44 };
    UiRect nw = tp_popup_rect(hd3, 5, &narrow);
    CHECK(nw.x >= 0 && nw.x + nw.w <= 200 && nw.w == 200);
}

static void test_open_close(void)
{
    TopPick tp;
    fresh(&tp, 6);
    tp.sel = 4;
    InputState none = key(-1);
    TouchState t;

    /* a release without its press, and a press elsewhere, do not open */
    t = touch(380, 20, false, false, true);
    CHECK(tp_head_input(&tp, HEAD, &E, &t, &none) == TP_NONE && !tp.up);
    t = touch(10, 20, true, true, false);
    tp_head_input(&tp, HEAD, &E, &t, &none);
    t = touch(380, 20, false, false, true);
    CHECK(tp_head_input(&tp, HEAD, &E, &t, &none) == TP_NONE && !tp.up);

    /* press and release on the header opens, on the current choice */
    t = touch(380, 20, true, true, false);
    CHECK(tp_head_input(&tp, HEAD, &E, &t, &none) == TP_NONE && !tp.up);
    t = touch(380, 20, false, false, true);
    CHECK(tp_head_input(&tp, HEAD, &E, &t, &none) == TP_OPENED && tp.up);
    CHECK(tp.lv.cursor == 4);
    CHECK(tp_head_input(&tp, HEAD, &E, &t, &none) == TP_NONE);   /* already up: the header does nothing */

    /* Back closes without a pick */
    CHECK(press_key(&tp, BTN_ID_BACK) == TP_CLOSED && !tp.up && tp_take(&tp) == -1);

    /* Shift opens; a header of zero width does not open by touch or Shift */
    InputState run = key(BTN_ID_RUN);
    t = idle();
    CHECK(tp_head_input(&tp, HEAD, &E, &t, &run) == TP_OPENED && tp.up);
    tp_close(&tp);
    UiRect nohead = { 0, 0, 0, 0 };
    CHECK(tp_head_input(&tp, nohead, &E, &t, &run) == TP_NONE && !tp.up);
    t = touch(0, 0, true, true, false);
    tp_head_input(&tp, nohead, &E, &t, &none);
    t = touch(0, 0, false, false, true);
    CHECK(tp_head_input(&tp, nohead, &E, &t, &none) == TP_NONE && !tp.up);
    tp_free(&tp);
}

static void test_pick(void)
{
    TopPick tp;
    fresh(&tp, 6);
    tp.sel = 1;
    tp_open(&tp, HEAD, &E);
    CHECK(tp.up && tp.lv.cursor == 1);

    CHECK(press_key(&tp, BTN_ID_DOWN) == TP_CHANGED && tp.lv.cursor == 2);
    CHECK(press_key(&tp, BTN_ID_UP) == TP_CHANGED && tp.lv.cursor == 1);
    CHECK(press_key(&tp, BTN_ID_DOWN) == TP_CHANGED);
    CHECK(press_key(&tp, BTN_ID_ACTION) == TP_PICKED && !tp.up && tp.picked == 2);
    CHECK(tp_take(&tp) == 2 && tp_take(&tp) == -1);

    /* the key that picks is not only ACTION */
    tp_open(&tp, HEAD, &E);
    CHECK(press_key(&tp, BTN_ID_JUMP) == TP_PICKED && tp_take(&tp) == 1);

    /* a tap on a row picks that row (rows start at the popup's top + border) */
    tp_open(&tp, HEAD, &E);
    UiRect r = tp.lv.rect;
    CHECK(tap(&tp, r.x + 40, r.y + LV_BORDER + TP_ROW_H * 3 + 20) == TP_PICKED && tp_take(&tp) == 3 && !tp.up);

    /* a tap outside closes with no pick, on the press */
    tp_open(&tp, HEAD, &E);
    r = tp.lv.rect;
    CHECK(tap(&tp, r.x + r.w + 30, r.y + 10) == TP_CLOSED && !tp.up && tp_take(&tp) == -1);

    /* Start (pause) and Shift close too */
    tp_open(&tp, HEAD, &E);
    CHECK(press_key(&tp, BTN_ID_PAUSE) == TP_CLOSED);
    tp_open(&tp, HEAD, &E);
    CHECK(press_key(&tp, BTN_ID_RUN) == TP_CLOSED);

    /* closed, the popup takes nothing */
    CHECK(press_key(&tp, BTN_ID_ACTION) == TP_NONE && tp_take(&tp) == -1);
    tp_free(&tp);
}

static void test_scroll(void)
{
    TopPick tp;
    fresh(&tp, 20);
    TpExt small = { 0, 0, 800, 300 };   /* room for 6 rows under a 44 px header */
    tp.sel = 0;
    tp_open(&tp, HEAD, &small);
    int vis = lv_visible_rows(&tp.lv);
    CHECK(vis >= 3 && vis < 20);
    CHECK(tp.lv.rect.y + tp.lv.rect.h <= 300);

    /* walk past the last visible row: the list scrolls and the cursor stays shown */
    for (int i = 0; i < vis + 2; i++) press_key(&tp, BTN_ID_DOWN);
    CHECK(tp.lv.cursor == vis + 2);
    CHECK(tp.lv.top > 0 && tp.lv.cursor >= tp.lv.top && tp.lv.cursor < tp.lv.top + vis);
    CHECK(press_key(&tp, BTN_ID_ACTION) == TP_PICKED && tp_take(&tp) == vis + 2);

    /* a tap on the first visible row after a scroll picks top + 0, not 0 */
    tp.sel = 15;
    tp_open(&tp, HEAD, &small);
    int top = tp.lv.top;
    CHECK(top > 0);
    UiRect r = tp.lv.rect;
    CHECK(tap(&tp, r.x + 40, r.y + LV_BORDER + 20) == TP_PICKED && tp_take(&tp) == top);

    /* with room for all of them nothing needs to scroll */
    fresh(&tp, 10);
    tp_open(&tp, HEAD, &E);
    CHECK(tp.lv.rect.h == 10 * TP_ROW_H + 2 * LV_BORDER && tp.lv.top == 0);
    tp_close(&tp);
    tp_free(&tp);
}

int main(void)
{
    for (int i = 0; i < 40; i++) { snprintf(nm[i], sizeof nm[i], "E%d", i); names[i] = nm[i]; }
    test_geometry();
    test_open_close();
    test_pick();
    test_scroll();
    printf("top_pick_test: %d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
