/* Host regression for the Jellyfin screens' layout (jellyfin/jf_ui.c, jf_ui_layout):
 * the function the app draws and hit-tests with, run against the panel geometry.
 *
 *   A  every pressable rect (button rows, sign-in rows, sign-in buttons) lies inside
 *      SCREEN_SAFE_* at several touch insets;
 *   B  the lists lie inside SCREEN_VISIBLE_* (and, being tapped, inside SAFE_* too);
 *   C  the button rows do not overlap the list, and buttons within a row do not overlap;
 *   D  the drawn-only now-playing rects lie inside SCREEN_VISIBLE_*.
 *   F  the action bar, SAVE AS PLAYLIST and the now-playing MIX pair (all pressable, so also under A) do not
 *      overlap the list or each other, and the list ends above the bar.
 *   I  mini player taps send their events; a queue that starts on an open list shortens it (mini_taps, mini_flip).
 *   M  MIX tick rows carry no bracket text or checkbox glyph; ticks draw as the ListView mark (mix_ticks).
 */
#include <stdio.h>
#include <string.h>
#include "jf_ui.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static bool inside(UiRect r, int l, int t, int rt, int b)
{
    return r.w > 0 && r.h > 0 && r.x >= l && r.y >= t && r.x + r.w <= rt && r.y + r.h <= b;
}

static bool disjoint(UiRect a, UiRect b)
{
    return a.x + a.w <= b.x || b.x + b.w <= a.x || a.y + a.h <= b.y || b.y + b.h <= a.y;
}

static void run(int inset_t, int inset_b, int inset_l, int inset_r)
{
    JfExtents e = { 0, 0, 800, 480, inset_l, inset_t, 800 - inset_r, 480 - inset_b };
    JfLayout L;
    jf_ui_layout(&L, &e);
    UiRect press[48];
    int n = 0;
    for (int i = 0; i < JF_UI_ROW3; i++) press[n++] = L.row3[i];
    for (int i = 0; i < JF_UI_ROW5; i++) press[n++] = L.row5[i];
    for (int i = 0; i < JF_UI_FIELDS; i++) press[n++] = L.field[i];
    press[n++] = L.signin; press[n++] = L.signexit;
    for (int i = 0; i < JF_UI_ABAR; i++) press[n++] = L.abar[i];
    for (int i = 0; i < JF_UI_NPMIX; i++) press[n++] = L.np_mix[i];
    for (int i = 0; i < JF_UI_MINI; i++) press[n++] = L.mini_btn[i];
    press[n++] = L.mini_title; press[n++] = L.seek;
    for (int i = 0; i < JF_UI_MIXBTN; i++) press[n++] = L.mixb[i];
    press[n++] = L.list_mini; press[n++] = L.list_root_mini;
    press[n++] = L.save;
    press[n++] = L.cat_prev; press[n++] = L.cat_next;
    for (int i = 0; i < n; i++)
        CHECK(inside(press[i], e.safe_l, e.safe_t, e.safe_r, e.safe_b),
              "A: inset %d/%d/%d/%d: button %d (%d,%d %dx%d) outside SAFE", inset_t, inset_b, inset_l, inset_r,
              i, press[i].x, press[i].y, press[i].w, press[i].h);
    CHECK(inside(L.list, e.vis_l, e.vis_t, e.vis_r, e.vis_b), "B: list outside VISIBLE (inset %d)", inset_t);
    CHECK(inside(L.list, e.safe_l, e.safe_t, e.safe_r, e.safe_b), "B: list outside SAFE (inset %d)", inset_t);
    CHECK(L.list.y > L.bar_bottom, "B: list starts under the bar rule");
    CHECK(L.list.h >= 2 * 40, "B: list shows at least two rows (inset %d): h %d", inset_t, L.list.h);
    CHECK(inside(L.list_root, e.safe_l, e.safe_t, e.safe_r, e.safe_b), "E: root list outside SAFE (inset %d)", inset_t);
    CHECK(L.list_root.h >= 2 * 40, "E: root list shows at least two rows (inset %d): h %d", inset_t, L.list_root.h);
    CHECK(disjoint(L.cat_prev, L.list_root) && disjoint(L.cat_next, L.list_root) && disjoint(L.cat_label, L.list_root), "E: category strip overlaps the root list");
    CHECK(disjoint(L.cat_prev, L.cat_label) && disjoint(L.cat_next, L.cat_label) && disjoint(L.cat_prev, L.cat_next), "E: category strip parts overlap");
    CHECK(disjoint(L.cat_prev, L.banner) && disjoint(L.cat_label, L.banner), "E: category strip overlaps the banner");
    for (int i = 0; i < JF_UI_ROW3; i++) CHECK(disjoint(L.row3[i], L.list_root), "E: row3[%d] overlaps the root list", i);
    for (int i = 0; i < JF_UI_ROW3; i++) {
        CHECK(disjoint(L.row3[i], L.list), "C: row3[%d] overlaps the list", i);
        for (int j = i + 1; j < JF_UI_ROW3; j++) CHECK(disjoint(L.row3[i], L.row3[j]), "C: row3 %d/%d overlap", i, j);
    }
    for (int i = 0; i < JF_UI_ROW5; i++)
        for (int j = i + 1; j < JF_UI_ROW5; j++) CHECK(disjoint(L.row5[i], L.row5[j]), "C: row5 %d/%d overlap", i, j);
    /* F  the action bar and the queue's SAVE take the button row, under the list; the bar's buttons are disjoint */
    for (int i = 0; i < JF_UI_ABAR; i++) {
        CHECK(disjoint(L.abar[i], L.list) && disjoint(L.abar[i], L.list_root), "F: abar[%d] overlaps a list (inset %d)", i, inset_t);
        CHECK(L.abar[i].y == L.row3[0].y, "F: abar[%d] is not in the button row", i);
        for (int j = i + 1; j < JF_UI_ABAR; j++) CHECK(disjoint(L.abar[i], L.abar[j]), "F: abar %d/%d overlap", i, j);
    }
    CHECK(disjoint(L.save, L.list), "F: SAVE overlaps the queue list (inset %d)", inset_t);
    CHECK(L.list.y + L.list.h <= L.abar[0].y, "F: the list does not end above the action bar (inset %d)", inset_t);
    CHECK(disjoint(L.np_mix[0], L.np_mix[1]), "F: now-playing MIX buttons overlap");
    for (int i = 0; i < JF_UI_NPMIX; i++)
        for (int j = 0; j < JF_UI_ROW5; j++) CHECK(disjoint(L.np_mix[i], L.row5[j]), "F: np_mix[%d] overlaps row5[%d] (inset %d)", i, j, inset_t);
    CHECK(L.progress.y + L.progress.h + 30 <= L.np_mix[0].y, "F: progress and its label leave the MIX row clear (inset %d)", inset_t);
    CHECK(inside(L.progress, e.vis_l, e.vis_t, e.vis_r, e.vis_b), "D: progress outside VISIBLE");
    CHECK(inside(L.info, e.vis_l, e.vis_t, e.vis_r, e.vis_b), "D: info outside VISIBLE");
    CHECK(L.progress.y + L.progress.h + 30 <= L.row5[0].y, "D: progress and its label leave the button row clear");
    /* G  mini player: the strip sits between the shortened lists and the button row, clear of SEARCH / the category
     * bar / every row; its buttons are disjoint; the lists keep two rows */
    CHECK(disjoint(L.mini, L.list_mini) && disjoint(L.mini, L.list_root_mini), "G: mini overlaps a shortened list (inset %d)", inset_t);
    CHECK(L.list_mini.h >= 2 * 40 && L.list_root_mini.h >= 2 * 40, "G: shortened list under two rows (inset %d)", inset_t);
    CHECK(L.list_mini.y == L.list.y && L.list_root_mini.y == L.list_root.y, "G: shortened list moved its top");
    for (int i = 0; i < JF_UI_ROW3; i++) CHECK(disjoint(L.mini, L.row3[i]), "G: mini overlaps row3[%d] (inset %d)", i, inset_t);
    for (int i = 0; i < JF_UI_ABAR; i++) CHECK(disjoint(L.mini, L.abar[i]), "G: mini overlaps abar[%d]", i);
    CHECK(disjoint(L.mini, L.save) && disjoint(L.mini, L.cat_prev) && disjoint(L.mini, L.cat_next) && disjoint(L.mini, L.banner), "G: mini overlaps save / category / banner");
    CHECK(disjoint(L.mini_title, L.mini_btn[0]), "G: mini title overlaps PLAY");
    for (int i = 0; i < JF_UI_MINI; i++) {
        CHECK(L.mini_btn[i].h >= 40 && L.mini_btn[i].w >= 60, "G: mini button %d too small", i);
        for (int j = i + 1; j < JF_UI_MINI; j++) CHECK(disjoint(L.mini_btn[i], L.mini_btn[j]), "G: mini buttons %d/%d overlap", i, j);
    }
    CHECK(L.mini_title.w >= 200, "G: mini title too narrow: %d", L.mini_title.w);
    /* H  now playing: two button rows, the seek area (taller than the 14 px bar) clear of both and of each other */
    CHECK(L.seek.h >= 30 && L.seek.y <= L.progress.y && L.seek.y + L.seek.h >= L.progress.y + L.progress.h, "H: seek area does not cover the bar");
    for (int i = 0; i < JF_UI_NPMIX; i++) {
        CHECK(disjoint(L.seek, L.np_mix[i]), "H: seek overlaps np_mix[%d] (inset %d)", i, inset_t);
        for (int j = i + 1; j < JF_UI_NPMIX; j++) CHECK(disjoint(L.np_mix[i], L.np_mix[j]), "H: np_mix %d/%d overlap", i, j);
    }
    for (int i = 0; i < JF_UI_ROW5; i++) CHECK(disjoint(L.seek, L.row5[i]), "H: seek overlaps row5[%d]", i);
    CHECK(L.np_mix[0].h >= JF_UI_BTN_H && L.row5[0].h >= JF_UI_BTN_H, "H: now-playing buttons shrank");
    for (int i = 0; i < JF_UI_MIXBTN; i++) CHECK(disjoint(L.mixb[i], L.list), "H: MIX screen button %d overlaps the list", i);
    CHECK(disjoint(L.mixb[0], L.mixb[1]), "H: MIX START / CANCEL overlap");
}

/* I  mini player: a tap on each strip button sends its event (the press frame fires button_check_tap, the
 *    release frame clears its latch), a release inside the title opens NOW PLAYING, a tap on the list above the
 *    strip sends none of those, and a queue that starts on an open list shortens the list to make room */
#define MINI_MAX 16

static bool has_event(const JfEvent *ev, int n, JfEventType t)
{
    for (int i = 0; i < n; i++) if (ev[i].type == t) return true;
    return false;
}

static int feed(JfUi *ui, int x, int y, bool pressed, bool held, bool released, uint32_t now, JfEvent *out, int max)
{
    TouchState ts = { x, y, pressed, released, held };
    InputState in;
    Pointer ptr;
    memset(&in, 0, sizeof in);
    memset(&ptr, 0, sizeof ptr);
    return jf_ui_input(ui, &ts, &in, &ptr, now, out, max);
}

/* one tap: a press frame, then a release frame 100 ms later; returns the events of both */
static int tap_at(JfUi *ui, int x, int y, uint32_t now, JfEvent *out)
{
    int n = feed(ui, x, y, true, true, false, now, out, MINI_MAX);
    n += feed(ui, x, y, false, false, true, now + 100, out + n, MINI_MAX - n);
    return n;
}

static void mini_taps(void)
{
    static Framebuffer fb;
    static TouchInput touch;
    JfApp app;
    JfUi ui;
    JfEvent ev[MINI_MAX];
    static const JfEventType want[JF_UI_MINI + 1] = { JF_EV_PAUSE, JF_EV_VOL_DOWN, JF_EV_VOL_UP, JF_EV_OPEN_NOWPLAYING };
    static const char *name[JF_UI_MINI + 1] = { "PAUSE", "VOL -", "VOL +", "title" };

    memset(&app, 0, sizeof app);
    app.screen = JF_SCR_ALBUMS;
    app.queue_n = 1;
    jf_ui_init(&ui, &fb, &touch, NULL, &app);
    feed(&ui, 0, 0, false, false, false, 1000, ev, MINI_MAX);   /* settle: begin_screen, strip up */
    CHECK(ui.mini_on, "I: strip is not up on a list screen with a queue");
    for (int k = 0; k <= JF_UI_MINI; k++) {
        UiRect r = k < JF_UI_MINI ? ui.L.mini_btn[k] : ui.L.mini_title;
        int n = tap_at(&ui, r.x + r.w / 2, r.y + r.h / 2, 2000 + 1000u * (uint32_t)k, ev);
        CHECK(has_event(ev, n, want[k]), "I: tap on %s sent no event of type %d (%d events)", name[k], (int)want[k], n);
    }
    /* negative control: the centre of the shortened list is a list tap, never a strip event */
    UiRect lr = ui.lv.rect;
    int n = tap_at(&ui, lr.x + lr.w / 2, lr.y + lr.h / 2, 9000, ev);
    CHECK(!has_event(ev, n, JF_EV_PAUSE) && !has_event(ev, n, JF_EV_VOL_DOWN) && !has_event(ev, n, JF_EV_VOL_UP) &&
          !has_event(ev, n, JF_EV_OPEN_NOWPLAYING), "I: a tap on the list sent a strip event (%d events)", n);
    jf_ui_free(&ui);
}

/* the list (or root list) takes the strip's room the frame the queue starts, and gives it back when it empties */
static void mini_flip(JfScreen s, const char *tag)
{
    static Framebuffer fb;
    static TouchInput touch;
    JfApp app;
    JfUi ui;
    JfEvent ev[MINI_MAX];

    memset(&app, 0, sizeof app);
    app.screen = s;
    app.queue_n = 0;
    jf_ui_init(&ui, &fb, &touch, NULL, &app);
    feed(&ui, 0, 0, false, false, false, 1000, ev, MINI_MAX);
    UiRect full = s == JF_SCR_ARTISTS ? ui.L.list_root : ui.L.list;
    UiRect mini = s == JF_SCR_ARTISTS ? ui.L.list_root_mini : ui.L.list_mini;
    CHECK(ui.lv.rect.y == full.y && ui.lv.rect.h == full.h, "I: %s: empty queue does not give the full list (h %d, want %d)", tag, ui.lv.rect.h, full.h);
    app.queue_n = 1;
    feed(&ui, 0, 0, false, false, false, 2000, ev, MINI_MAX);
    CHECK(ui.lv.rect.y == mini.y && ui.lv.rect.h == mini.h, "I: %s: queue start did not shorten the list (y %d h %d, want y %d h %d)",
          tag, ui.lv.rect.y, ui.lv.rect.h, mini.y, mini.h);
    jf_ui_free(&ui);
}

/* M  the MIX tick rows: no "[X]"/"[ ]" text and no checkbox glyph in the row; a ticked row is drawn with the
 *    ListView mark (lv_is_marked) and an unticked one is not, and un-ticking clears it (mix_ticks). */
static void mix_ticks(void)
{
    static Framebuffer fb;
    static TouchInput touch;
    static const char *label[3] = { "Rock", "Jazz", "1990s" };
    JfApp app;
    JfUi ui;
    JfEvent ev[MINI_MAX];
    LvRow r;

    memset(&app, 0, sizeof app);
    app.screen = JF_SCR_MIX;
    app.mix.phase = JF_MIX_TICKS;
    app.mix.opt_n = 3;
    for (int i = 0; i < 3; i++) { strcpy(app.mix.opt[i].label, label[i]); app.mix.opt[i].on = (i == 1); }
    jf_ui_init(&ui, &fb, &touch, NULL, &app);
    feed(&ui, 0, 0, false, false, false, 1000, ev, MINI_MAX);   /* settle: begin_screen, lv_sync */

    for (int i = 0; i < 3; i++) {
        memset(&r, 0, sizeof r);
        ui.lv.src.row(ui.lv.src.ctx, i, &r);
        CHECK(r.title && strcmp(r.title, label[i]) == 0, "M: row %d title \"%s\" is not the bare label", i, r.title ? r.title : "(null)");
        CHECK(r.title && !strchr(r.title, '['), "M: row %d title still carries a bracket tick", i);
        CHECK(r.glyph == LV_GLYPH_BLANK, "M: row %d glyph %d is not LV_GLYPH_BLANK (checkbox placeholder)", i, (int)r.glyph);
    }
    CHECK(lv_is_marked(&ui.lv, 1), "M: ticked row 1 is not drawn with the ListView mark");
    CHECK(!lv_is_marked(&ui.lv, 0) && !lv_is_marked(&ui.lv, 2), "M: unticked rows carry the ListView mark");
    CHECK(lv_mark_count(&ui.lv) == 1, "M: mark count %d, expected 1", lv_mark_count(&ui.lv));

    app.mix.opt[1].on = false; app.mix.opt[0].on = true;
    feed(&ui, 0, 0, false, false, false, 1100, ev, MINI_MAX);
    CHECK(lv_is_marked(&ui.lv, 0) && !lv_is_marked(&ui.lv, 1), "M: after re-tick the marks did not follow app state");
    CHECK(lv_mark_count(&ui.lv) == 1, "M: after re-tick mark count %d, expected 1", lv_mark_count(&ui.lv));
    jf_ui_free(&ui);
}

int main(void)
{
    run(0, 0, 0, 0);
    run(17, 17, 3, 3);       /* the measured unit */
    run(30, 30, 10, 10);
    run(48, 48, 48, 48);     /* the largest inset the library allows */
    mini_taps();
    mini_flip(JF_SCR_ALBUMS, "albums");
    mini_flip(JF_SCR_ARTISTS, "artists");
    mix_ticks();
    if (fails) { printf("jf_ui_layout_test: %d FAILED\n", fails); return 1; }
    printf("jf_ui_layout_test: all passed\n");
    return 0;
}
