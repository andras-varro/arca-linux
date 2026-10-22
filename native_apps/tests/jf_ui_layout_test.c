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
 *   R  the queue view of now playing: 12 buttons in one row (wrapping when narrower than 738 px), the transport sends now playing's events, CLR, EQ, the title returns (queue_row, queue_widths).
 *   Q  the EQ door: the NOW PLAYING button and Shift open the panel, Back cancels, DONE saves (eq_door).
  *   M  MIX tick rows carry no bracket text or checkbox glyph; ticks draw as the ListView mark (mix_ticks).
 *   S  SETTINGS: the root row is SEARCH, NOW PLAYING, SETTINGS; settings and library rows hit-test and pad-navigate to their events (settings_ui).
 *
 * Build (from native_apps/, one line; run under setarch -R):
 *   gcc -Wall -Wextra -Wno-unused-parameter -fsanitize=undefined -I. -I common -I jellyfin -o build/jf_ui_layout_test tests/jf_ui_layout_test.c jellyfin/jf_ui.c common/eq_panel.c jellyfin/jf_art.c common/icons.c common/jf_img.c common/logger.c common/ui_focus.c jellyfin/jf_core.c jellyfin/jf_url.c jellyfin/jf_model.c jellyfin/jf_login.c common/json.c common/http.c common/list_view.c common/top_bar.c common/icon_grid.c common/ppm.c common/pointer.c common/status_line.c control_panel/cpu_load.c common/common.c common/framebuffer.c common/font.c common/touch_input.c common/hardware.c common/config.c common/highscore.c common/keyboard.c common/audio.c common/audio_gen.c common/audio_out.c common/audio_eq.c common/audio_enh.c common/audio_wav.c common/start_menu.c common/gamepad.c common/input_slots.c common/input_scan.c -lssl -lcrypto -lm
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
    for (int i = 0; i < JF_UI_FIELDS; i++) press[n++] = L.field[i];
    press[n++] = L.signin; press[n++] = L.signexit;
    for (int i = 0; i < JF_UI_ABAR; i++) press[n++] = L.abar[i];
    for (int i = 0; i < JF_UI_NP; i++) press[n++] = L.np[i];
    for (int i = 0; i < JF_UI_MINI; i++) press[n++] = L.mini_btn[i];
    press[n++] = L.mini_title; press[n++] = L.seek;
    for (int i = 0; i < JF_UI_MIXBTN; i++) press[n++] = L.mixb[i];
    press[n++] = L.list_mini;
    for (int i = 0; i < JF_UI_QROW; i++) press[n++] = L.qrow[i];

    for (int i = 0; i < n; i++)
        CHECK(inside(press[i], e.safe_l, e.safe_t, e.safe_r, e.safe_b),
              "A: inset %d/%d/%d/%d: button %d (%d,%d %dx%d) outside SAFE", inset_t, inset_b, inset_l, inset_r,
              i, press[i].x, press[i].y, press[i].w, press[i].h);
    CHECK(inside(L.list, e.vis_l, e.vis_t, e.vis_r, e.vis_b), "B: list outside VISIBLE (inset %d)", inset_t);
    CHECK(inside(L.list, e.safe_l, e.safe_t, e.safe_r, e.safe_b), "B: list outside SAFE (inset %d)", inset_t);
    CHECK(L.list.y > L.bar_bottom, "B: list starts under the bar rule");
    CHECK(L.list.h >= 2 * 40, "B: list shows at least two rows (inset %d): h %d", inset_t, L.list.h);
    /* E  the settings view has no button row: its list runs through the row's place, inside SAFE, under the banner */
    CHECK(inside(L.list_set, e.safe_l, e.safe_t, e.safe_r, e.safe_b) && L.list_set.y == L.list.y && L.list_set.y + L.list_set.h >= L.row3[0].y + L.row3[0].h, "E: list_set does not cover the list and the button row (inset %d)", inset_t);
    for (int i = 0; i < JF_UI_ROW3; i++) {
        CHECK(disjoint(L.row3[i], L.list), "C: row3[%d] overlaps the list", i);
        for (int j = i + 1; j < JF_UI_ROW3; j++) CHECK(disjoint(L.row3[i], L.row3[j]), "C: row3 %d/%d overlap", i, j);
    }
    /* F  the action bar and the queue's SAVE take the button row, under the list; the bar's buttons are disjoint */
    for (int i = 0; i < JF_UI_ABAR; i++) {
        CHECK(disjoint(L.abar[i], L.list), "F: abar[%d] overlaps a list (inset %d)", i, inset_t);
        CHECK(L.abar[i].y == L.row3[0].y, "F: abar[%d] is not in the button row", i);
        for (int j = i + 1; j < JF_UI_ABAR; j++) CHECK(disjoint(L.abar[i], L.abar[j]), "F: abar %d/%d overlap", i, j);
    }
    for (int i = 0; i < JF_UI_QROW; i++) {
        CHECK(disjoint(L.qrow[i], L.list_q), "F: queue row button %d overlaps the queue list (inset %d)", i, inset_t);
        CHECK(L.qrow[i].w >= 56 && L.qrow[i].h >= JF_UI_BTN_H, "F: queue row button %d narrower than 56 px (%d, inset %d)", i, L.qrow[i].w, inset_t);
        for (int j = i + 1; j < JF_UI_QROW; j++) CHECK(disjoint(L.qrow[i], L.qrow[j]), "F: queue row buttons %d/%d overlap (inset %d)", i, j, inset_t);
    }
    CHECK(L.list_q.h >= 2 * 40 && L.list_q.y + L.list_q.h <= L.qrow[0].y, "F: queue list too short or not above its row (h %d, inset %d)", L.list_q.h, inset_t);
    bool q_one = (e.safe_r - e.safe_l) >= JF_UI_QROW * 56 + (JF_UI_QROW - 1) * 6;
    for (int i = 0; i < JF_UI_QROW; i++) CHECK(!q_one || L.qrow[i].y == L.qrow[0].y, "F: queue row button %d left the single landscape row (inset %d)", i, inset_t);
    CHECK(q_one == (L.qrow_rows == 1), "F: queue row is in %d rows, one-row prediction %d (inset %d)", L.qrow_rows, q_one, inset_t);
    CHECK(L.list.y + L.list.h <= L.abar[0].y, "F: the list does not end above the action bar (inset %d)", inset_t);
    /* one row when the safe width holds 10 buttons at the 72 px minimum plus gaps, else the fewest equal rows (the largest inset) */
    bool one_row = (e.safe_r - e.safe_l) >= JF_UI_NP * 72 + (JF_UI_NP - 1) * 6;
    for (int i = 0; i < JF_UI_NP; i++) {
        CHECK(!one_row || L.np[i].y == L.np[0].y, "F: now-playing button %d left the single landscape row", i);
        CHECK(L.np[i].w >= 72, "F: np[%d] narrower than the minimum (inset %d)", i, inset_t);
        for (int j = i + 1; j < JF_UI_NP; j++) CHECK(disjoint(L.np[i], L.np[j]), "F: np %d/%d overlap", i, j);
        CHECK((!one_row || disjoint(L.np[i], L.list)) && disjoint(L.np[i], L.seek), "F: np[%d] overlaps the list or the seek area (inset %d)", i, inset_t);
    }
    CHECK(L.progress.y + L.progress.h + 30 <= L.np[0].y, "F: progress and its label leave the button row clear (inset %d)", inset_t);
    CHECK(inside(L.progress, e.vis_l, e.vis_t, e.vis_r, e.vis_b), "D: progress outside VISIBLE");
    CHECK(inside(L.info, e.vis_l, e.vis_t, e.vis_r, e.vis_b), "D: info outside VISIBLE");
    CHECK(L.progress.y + L.progress.h + 30 <= L.np[0].y, "D: progress and its label leave the button row clear");
    /* G  mini player: the strip sits between the shortened lists and the button row, clear of SEARCH / the category
     * bar / every row; its buttons are disjoint; the lists keep two rows */
    CHECK(disjoint(L.mini, L.list_mini), "G: mini overlaps a shortened list (inset %d)", inset_t);
    CHECK(L.list_mini.h >= 2 * 40, "G: shortened list under two rows (inset %d)", inset_t);
    CHECK(L.list_mini.y == L.list.y, "G: shortened list moved its top");
    for (int i = 0; i < JF_UI_ROW3; i++) CHECK(disjoint(L.mini, L.row3[i]), "G: mini overlaps row3[%d] (inset %d)", i, inset_t);
    for (int i = 0; i < JF_UI_ABAR; i++) CHECK(disjoint(L.mini, L.abar[i]), "G: mini overlaps abar[%d]", i);
    CHECK(disjoint(L.mini, L.banner), "G: mini overlaps save / category / banner");
    CHECK(disjoint(L.mini_title, L.mini_btn[0]), "G: mini title overlaps PLAY");
    for (int i = 0; i < JF_UI_MINI; i++) {
        CHECK(L.mini_btn[i].h >= 40 && L.mini_btn[i].w >= 60, "G: mini button %d too small", i);
        for (int j = i + 1; j < JF_UI_MINI; j++) CHECK(disjoint(L.mini_btn[i], L.mini_btn[j]), "G: mini buttons %d/%d overlap", i, j);
    }
    CHECK(L.mini_title.w >= 200, "G: mini title too narrow: %d", L.mini_title.w);
    /* H  now playing: two button rows, the seek area (taller than the 14 px bar) clear of both and of each other */
    CHECK(L.seek.h >= 30 && L.seek.y <= L.progress.y && L.seek.y + L.seek.h >= L.progress.y + L.progress.h, "H: seek area does not cover the bar");
    CHECK(L.np[0].h >= JF_UI_BTN_H && L.np[0].w >= 72, "H: now-playing buttons shrank");
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

/* A frame whose only input is one pressed pad / keyboard button. */
static int feed_btn(JfUi *ui, int id, uint32_t now, JfEvent *out, int max)
{
    TouchState ts = { 0, 0, false, false, false };
    InputState in;
    Pointer ptr;
    memset(&in, 0, sizeof in);
    memset(&ptr, 0, sizeof ptr);
    in.buttons[id].pressed = true;
    return jf_ui_input(ui, &ts, &in, &ptr, now, out, max);
}

/* The root: the header opens the category picker (touch and key), a row picks, outside closes; the bottom row names the
 * neighbouring categories; below the root the row has no category buttons. */
static void root_picker(void)
{
    static Framebuffer fb;
    static TouchInput touch;
    JfApp app;
    JfUi ui;
    JfEvent ev[MINI_MAX];
    memset(&app, 0, sizeof app);
    app.screen = JF_SCR_ARTISTS;
    app.cat = JF_CAT_ARTISTS;
    jf_ui_init(&ui, &fb, &touch, NULL, &app);
    feed(&ui, 0, 0, false, false, false, 1000, ev, MINI_MAX);
    CHECK(!ui.pick_up, "R: picker up before anything was tapped");

    ui.bar.title = "JELLYFIN";   /* head_rect() sets the real title itself */
    UiRect h = { ui.bar.L.title_x, ui.bar.L.top, ui.bar.L.title_w, ui.bar.L.h };
    int n = tap_at(&ui, h.x + h.w / 2, h.y + h.h / 2, 2000, ev);
    CHECK(ui.pick_up && n == 0, "R: a tap on the header did not open the picker (up %d, %d events)", ui.pick_up, n);
    UiRect pr = ui.pick.rect;
    CHECK(pr.x >= SCREEN_SAFE_LEFT && pr.x + pr.w <= SCREEN_SAFE_RIGHT && pr.y + pr.h <= SCREEN_SAFE_BOTTOM,
          "R: the picker is not inside SAFE");
    n = tap_at(&ui, pr.x + pr.w / 2, pr.y + LV_BORDER + 40 * 3 + 20, 3000, ev);
    int k = -1;
    for (int i = 0; i < n; i++) if (ev[i].type == JF_EV_CATEGORY_SET) k = ev[i].i;
    CHECK(k == JF_CAT_GENRES && !ui.pick_up, "R: tapping the 4th row sent category %d (want %d), picker up %d", k, JF_CAT_GENRES, ui.pick_up);

    /* the key: opens at the current category, Down moves, ENTER picks */
    feed_btn(&ui, BTN_ID_RUN, 4000, ev, MINI_MAX);
    CHECK(ui.pick_up && ui.pick.cursor == (int)app.cat, "R: Shift did not open the picker on the current category");
    feed_btn(&ui, BTN_ID_DOWN, 4100, ev, MINI_MAX);
    n = feed_btn(&ui, BTN_ID_ACTION, 4200, ev, MINI_MAX);
    CHECK(n == 1 && ev[0].type == JF_EV_CATEGORY_SET && ev[0].i == JF_CAT_PLAYLISTS && !ui.pick_up,
          "R: Down + ENTER picked %d (want PLAYLISTS), %d events", n ? ev[0].i : -1, n);

    /* close: Back, and a tap outside, send nothing */
    feed_btn(&ui, BTN_ID_RUN, 5000, ev, MINI_MAX);
    n = feed_btn(&ui, BTN_ID_BACK, 5100, ev, MINI_MAX);
    CHECK(!ui.pick_up && n == 0, "R: Back did not close the picker quietly");
    feed_btn(&ui, BTN_ID_RUN, 6000, ev, MINI_MAX);
    n = feed(&ui, 5, 300, true, true, false, 6100, ev, MINI_MAX);
    CHECK(!ui.pick_up && n == 0, "R: a tap outside did not close the picker quietly (%d events)", n);
    feed(&ui, 5, 300, false, false, true, 6200, ev, MINI_MAX);   /* its release, so the next tap is a fresh one */
    feed(&ui, 0, 0, false, false, false, 7000, ev, MINI_MAX);

    /* the bottom row: SEARCH, NOW PLAYING (needs a queue), SETTINGS; the categories step with Left / Right (the row had category buttons) */
    app.cat = JF_CAT_ALBUMS;
    ui.last_cat = app.cat;
    n = feed_btn(&ui, BTN_ID_LEFT, 8000, ev, MINI_MAX);
    CHECK(n == 1 && ev[0].type == JF_EV_CATEGORY && ev[0].i == -1, "R: Left did not step back (%d events)", n);
    n = feed_btn(&ui, BTN_ID_RIGHT, 9000, ev, MINI_MAX);
    CHECK(n == 1 && ev[0].type == JF_EV_CATEGORY && ev[0].i == 1, "R: Right did not step forward (%d events)", n);
    n = tap_at(&ui, ui.L.row3[1].x + 20, ui.L.row3[1].y + 20, 10000, ev);
    CHECK(!has_event(ev, n, JF_EV_OPEN_NOWPLAYING), "R: NOW PLAYING answered with no queue");
    app.queue_n = 1;
    n = tap_at(&ui, ui.L.row3[1].x + 20, ui.L.row3[1].y + 20, 11000, ev);
    CHECK(has_event(ev, n, JF_EV_OPEN_NOWPLAYING), "R: NOW PLAYING did not open with a queue");
    jf_ui_free(&ui);

    /* below the root: no category button where the root has one (the left slot is SEARCH) */
    memset(&app, 0, sizeof app);
    app.screen = JF_SCR_ALBUMS;
    jf_ui_init(&ui, &fb, &touch, NULL, &app);
    feed(&ui, 0, 0, false, false, false, 1000, ev, MINI_MAX);
    n = tap_at(&ui, ui.L.row3[2].x + 20, ui.L.row3[2].y + 20, 2000, ev);
    CHECK(!has_event(ev, n, JF_EV_CATEGORY) && !has_event(ev, n, JF_EV_CATEGORY_SET), "R: a category button below the root");
    jf_ui_free(&ui);
}

/* SETTINGS: the root's bottom row is SEARCH, NOW PLAYING, SETTINGS (no category buttons); the settings rows hit-test to
 * their events; the library page's ALL row sends -1 and a library row its index; pad and keyboard reach every row. */
static void settings_ui(void)
{
    static Framebuffer fb;
    static TouchInput touch;
    JfApp app;
    JfUi ui;
    JfEvent ev[MINI_MAX];
    jf_app_init(&app);
    app.screen = JF_SCR_ARTISTS;
    app.queue_n = 1;
    snprintf(app.login.token, sizeof app.login.token, "t");
    jf_ui_init(&ui, &fb, &touch, NULL, &app);
    feed(&ui, 0, 0, false, false, false, 1000, ev, MINI_MAX);
    int n;
    for (int i = 1; i < JF_UI_ROW3; i++) {   /* slot 0 is SEARCH: it opens the blocking keyboard */
        n = tap_at(&ui, ui.L.row3[i].x + ui.L.row3[i].w / 2, ui.L.row3[i].y + 20, 2000u + 1000u * (uint32_t)i, ev);
        CHECK(!has_event(ev, n, JF_EV_CATEGORY), "S: root row button %d sent a category step", i);
        CHECK((i == 2) == has_event(ev, n, JF_EV_SETTINGS_OPEN) && (i == 1) == has_event(ev, n, JF_EV_OPEN_NOWPLAYING), "S: root row button %d: wrong events (%d)", i, n);
    }
    jf_ui_free(&ui);

    /* the settings screen: two libraries, so LIBRARY then SIGN OUT */
    jf_app_init(&app);
    app.screen = JF_SCR_SETTINGS;
    app.nav_n = 1;
    app.lib_n = 2;
    app.lists[JF_LIST_LIBS].n = 2;
    snprintf(app.lists[JF_LIST_LIBS].items[0].id, sizeof app.lists[0].items[0].id, "a1");
    snprintf(app.lists[JF_LIST_LIBS].items[0].name, sizeof app.lists[0].items[0].name, "ROCK");
    snprintf(app.lists[JF_LIST_LIBS].items[1].id, sizeof app.lists[0].items[0].id, "b2");
    snprintf(app.lists[JF_LIST_LIBS].items[1].name, sizeof app.lists[0].items[1].name, "JAZZ");
    int ids[JF_SET_COUNT];
    int cnt = jf_settings_rows(&app, ids);
    CHECK(cnt == 2 && ids[0] == JF_SET_LIBRARY && ids[1] == JF_SET_SIGNOUT, "S: settings rows %d", cnt);
    jf_ui_init(&ui, &fb, &touch, NULL, &app);
    feed(&ui, 0, 0, false, false, false, 1000, ev, MINI_MAX);
    CHECK(ui.lv.count == 2, "S: settings list has %d rows, want 2", ui.lv.count);
    for (int r = 0; r < cnt; r++) {
        int y = ui.lv.rect.y + LV_BORDER + 40 * r + 20;
        n = tap_at(&ui, ui.lv.rect.x + ui.lv.rect.w / 2, y, 2000u + 1000u * (uint32_t)r, ev);
        CHECK(n >= 1 && ev[0].type == JF_EV_SETTING_ACTIVATE && ev[0].i == ids[r], "S: tapping settings row %d sent %d/%d (%d events)",
              r, n ? (int)ev[0].type : -1, n ? ev[0].i : -1, n);
    }
    /* the pad: Down walks, ENTER acts; every row is reachable */
    jf_ui_free(&ui);
    jf_ui_init(&ui, &fb, &touch, NULL, &app);
    feed(&ui, 0, 0, false, false, false, 10000, ev, MINI_MAX);
    for (int r = 0; r < cnt; r++) {
        n = feed_btn(&ui, BTN_ID_ACTION, 11000u + 1000u * (uint32_t)r, ev, MINI_MAX);
        CHECK(n >= 1 && ev[0].type == JF_EV_SETTING_ACTIVATE && ev[0].i == ids[r], "S: ENTER on settings row %d sent %d/%d", r, n ? (int)ev[0].type : -1, n ? ev[0].i : -1);
        feed_btn(&ui, BTN_ID_DOWN, 11500u + 1000u * (uint32_t)r, ev, MINI_MAX);
    }
    n = feed_btn(&ui, BTN_ID_BACK, 15000, ev, MINI_MAX);
    CHECK(has_event(ev, n, JF_EV_BACK), "S: Back left no BACK event on settings");
    jf_ui_free(&ui);

    /* the library page: ALL LIBRARIES, then each library; the chosen one is the marked row */
    app.settings_page = JF_SETPAGE_LIBRARY;
    snprintf(app.login.library, sizeof app.login.library, "b2");
    jf_ui_init(&ui, &fb, &touch, NULL, &app);
    feed(&ui, 0, 0, false, false, false, 20000, ev, MINI_MAX);
    CHECK(ui.lv.count == 3, "S: library page has %d rows, want 3", ui.lv.count);
    CHECK(ui.lv.cursor == 2, "S: library page cursor is on row %d, want the chosen library (2)", ui.lv.cursor);
    CHECK(lv_is_marked(&ui.lv, 2) && !lv_is_marked(&ui.lv, 0) && !lv_is_marked(&ui.lv, 1), "S: the chosen library is not the one marked row");
    for (int r = 0; r < 3; r++) {
        int y = ui.lv.rect.y + LV_BORDER + 40 * r + 20;
        n = tap_at(&ui, ui.lv.rect.x + ui.lv.rect.w / 2, y, 21000u + 1000u * (uint32_t)r, ev);
        CHECK(n >= 1 && ev[0].type == JF_EV_LIBRARY_SET && ev[0].i == r - 1, "S: tapping library row %d sent %d/%d", r, n ? (int)ev[0].type : -1, n ? ev[0].i : -99);
    }
    jf_ui_free(&ui);
    app.login.library[0] = '\0';
    jf_ui_init(&ui, &fb, &touch, NULL, &app);
    feed(&ui, 0, 0, false, false, false, 30000, ev, MINI_MAX);
    CHECK(lv_is_marked(&ui.lv, 0) && !lv_is_marked(&ui.lv, 1), "S: ALL LIBRARIES is not marked when no library is chosen");
    n = feed_btn(&ui, BTN_ID_ACTION, 31000, ev, MINI_MAX);
    CHECK(n >= 1 && ev[0].type == JF_EV_LIBRARY_SET && ev[0].i == -1, "S: ENTER on the ALL row sent %d/%d", n ? (int)ev[0].type : -1, n ? ev[0].i : -99);
    jf_ui_free(&ui);

    /* a single library: LIBRARY is absent, not greyed */
    app.lib_n = 1;
    app.settings_page = JF_SETPAGE_MAIN;
    cnt = jf_settings_rows(&app, ids);
    jf_ui_init(&ui, &fb, &touch, NULL, &app);
    feed(&ui, 0, 0, false, false, false, 40000, ev, MINI_MAX);
    CHECK(cnt == 1 && ids[0] == JF_SET_SIGNOUT && ui.lv.count == 1, "S: with one library the list has %d rows (want only SIGN OUT)", ui.lv.count);
    jf_ui_free(&ui);
}

/* SHUFFLE on the queue puts the cursor on the first row, where the playing track now is */
static void queue_shuffle_cursor(void)
{
    static Framebuffer fb;
    static TouchInput touch;
    JfApp app;
    JfUi ui;
    JfEvent ev[MINI_MAX];
    memset(&app, 0, sizeof app);
    app.screen = JF_SCR_QUEUE;
    app.queue_n = 20;
    app.queue_cur = 12;
    jf_ui_init(&ui, &fb, &touch, NULL, &app);
    feed(&ui, 0, 0, false, false, false, 1000, ev, MINI_MAX);
    lv_move_to(&ui.lv, 15);
    CHECK(ui.lv.cursor == 15 && ui.lv.top > 0, "S: setup: cursor %d top %d", ui.lv.cursor, ui.lv.top);
    int n = tap_at(&ui, ui.L.qrow[6].x + 20, ui.L.qrow[6].y + 20, 2000, ev);
    CHECK(has_event(ev, n, JF_EV_SHUFFLE), "S: SHUFFLE sent no event");
    CHECK(ui.lv.cursor == 0 && ui.lv.top == 0, "S: after SHUFFLE the cursor is %d and the top %d (want 0, 0)", ui.lv.cursor, ui.lv.top);
    jf_ui_free(&ui);
}

/* R  the queue view of now playing: its transport buttons send the events now playing's do, CLR asks to clear, the title
 * returns to now playing, EQ opens the panel, and the pad walks all twelve. */
static JfEventType first_event(JfUi *ui, int x, int y, uint32_t now)
{
    JfEvent ev[MINI_MAX];
    int n = tap_at(ui, x, y, now, ev);
    return n > 0 ? ev[0].type : JF_EV_NONE;
}

static void queue_row(void)
{
    static Framebuffer fb;
    static TouchInput touch;
    JfApp app, nowp;
    JfUi ui, np;
    JfEvent ev[MINI_MAX];
    memset(&app, 0, sizeof app);
    app.screen = JF_SCR_QUEUE;
    app.queue_n = 5;
    nowp = app;
    nowp.screen = JF_SCR_NOWPLAYING;
    jf_ui_init(&ui, &fb, &touch, NULL, &app);
    jf_ui_init(&np, &fb, &touch, NULL, &nowp);
    feed(&ui, 0, 0, false, false, false, 1000, ev, MINI_MAX);
    feed(&np, 0, 0, false, false, false, 1000, ev, MINI_MAX);
    uint32_t t = 3000;
    for (int i = 0; i < 4; i++, t += 1000) {
        JfEventType q = first_event(&ui, ui.L.qrow[i].x + 20, ui.L.qrow[i].y + 20, t);
        JfEventType p = first_event(&np, np.L.np[i].x + 20, np.L.np[i].y + 20, t);
        CHECK(q != JF_EV_NONE && q == p, "R: queue transport button %d sent event %d, now playing's sends %d", i, q, p);
        static const JfEventType want[4] = { JF_EV_PREV, JF_EV_PAUSE, JF_EV_STOP, JF_EV_NEXT };
        CHECK(q == want[i], "R: queue transport button %d sent event %d, want %d", i, q, want[i]);
    }
    /* EQ opens the shared panel, quietly */
    int n = tap_at(&ui, ui.L.qrow[JF_UI_QROW_EQ].x + 20, ui.L.qrow[JF_UI_QROW_EQ].y + 20, t, ev);
    CHECK(ui.eq_up && n == 0, "R: EQ did not open the panel quietly (up %d, %d events)", ui.eq_up, n);
    feed_btn(&ui, BTN_ID_BACK, t + 3000, ev, MINI_MAX);
    CHECK(!ui.eq_up, "R: Back did not close the panel");
    t += 8000;
    CHECK(first_event(&ui, ui.L.qrow[10].x + 20, ui.L.qrow[10].y + 20, t) == JF_EV_VOL_UP, "R: VOL+ sent the wrong event"); t += 1000;
    CHECK(first_event(&ui, ui.L.qrow[11].x + 20, ui.L.qrow[11].y + 20, t) == JF_EV_VOL_DOWN, "R: VOL- sent the wrong event"); t += 1000;
    CHECK(first_event(&ui, ui.L.qrow[5].x + 20, ui.L.qrow[5].y + 20, t) == JF_EV_QUEUE_CLEAR, "R: CLR did not send QUEUE_CLEAR"); t += 1000;
    CHECK(first_event(&ui, ui.L.qrow[6].x + 20, ui.L.qrow[6].y + 20, t) == JF_EV_SHUFFLE, "R: RND did not send SHUFFLE"); t += 1000;
    /* the title: a tap on it goes back; a tap elsewhere in the bar's middle row of the list does not */
    TopBarLayout *TL = &ui.bar.L;
    CHECK(first_event(&ui, TL->title_x + TL->title_w / 2, TL->top + TL->h / 2, t) == JF_EV_BACK, "R: a tap on the QUEUE title did not send BACK");
    t += 1000;
    /* the pad: Right walks the twelve and wraps; ACTION on the last is VOL- */
    feed(&ui, 0, 0, false, false, false, t, ev, MINI_MAX);
    ui.zone = JF_ZONE_ROW;
    ui.focus = 0;
    for (int i = 0; i < JF_UI_QROW - 1; i++) feed_btn(&ui, BTN_ID_RIGHT, t += 1000, ev, MINI_MAX);
    CHECK(ui.focus == JF_UI_QROW - 1, "R: the pad reached button %d after %d Rights, want %d", ui.focus, JF_UI_QROW - 1, JF_UI_QROW - 1);
    int k = feed_btn(&ui, BTN_ID_ACTION, t += 1000, ev, MINI_MAX);
    CHECK(has_event(ev, k, JF_EV_VOL_DOWN), "R: ACTION on the last button did not send VOL-");
    feed_btn(&ui, BTN_ID_RIGHT, t += 1000, ev, MINI_MAX);
    CHECK(ui.focus == 0, "R: the pad did not wrap to the first button (%d)", ui.focus);
    jf_ui_free(&ui);
    jf_ui_free(&np);
}

/* the numbers the operator asked for, at the measured unit and a narrower one */
static void queue_widths(void)
{
    JfExtents u = { 0, 0, 800, 480, 17, 17, 797, 463 };      /* the measured unit: 780 px of safe width */
    JfExtents z = { 0, 0, 800, 480, 0, 0, 800, 480 };
    JfExtents nar = { 0, 0, 800, 480, 30, 17, 770, 463 };    /* 740 px: still one row at 56 */
    JfExtents tight = { 0, 0, 800, 480, 31, 17, 769, 463 };  /* 738 px: the last width of one row */
    JfExtents wrap = { 0, 0, 800, 480, 32, 17, 768, 463 };   /* 736 px: wraps */
    const JfExtents *es[5] = { &u, &z, &nar, &tight, &wrap };
    const int want_rows[5] = { 1, 1, 1, 1, 2 };
    for (int k = 0; k < 5; k++) {
        JfLayout L;
        jf_ui_layout(&L, es[k]);
        CHECK(L.qrow_rows == want_rows[k], "R: width %d gives %d rows, want %d", es[k]->safe_r - es[k]->safe_l, L.qrow_rows, want_rows[k]);
        for (int i = 0; i < JF_UI_QROW; i++) CHECK(L.qrow[i].w >= 56, "R: width %d button %d is %d px", es[k]->safe_r - es[k]->safe_l, i, L.qrow[i].w);
    }
    JfLayout L;
    jf_ui_layout(&L, &u);
    printf("queue row at 780 px: %d buttons of %d px, gap 6, %d row(s)\n", JF_UI_QROW, L.qrow[0].w, L.qrow_rows);
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
    UiRect full = ui.L.list;
    UiRect mini = ui.L.list_mini;
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

/* N  portrait (480 x 800): the now-playing buttons wrap into two rows, none overlapping, all inside the safe area */
static void portrait(void)
{
    JfExtents e = { 0, 0, 480, 800, 17, 17, 480 - 17, 800 - 17 };
    JfLayout L;
    jf_ui_layout(&L, &e);
    int rows = 0, last_y = -1;
    for (int i = 0; i < JF_UI_NP; i++) {
        CHECK(inside(L.np[i], e.safe_l, e.safe_t, e.safe_r, e.safe_b), "N: np[%d] outside SAFE in portrait", i);
        CHECK(L.np[i].w >= 72, "N: np[%d] narrower than the minimum", i);
        for (int j = i + 1; j < JF_UI_NP; j++) CHECK(disjoint(L.np[i], L.np[j]), "N: portrait np %d/%d overlap", i, j);
        if (L.np[i].y != last_y) { rows++; last_y = L.np[i].y; }
    }
    CHECK(rows == 2, "N: portrait now-playing buttons in %d rows, want 2", rows);
}

/* Q  the equaliser door on NOW PLAYING: the EQ button and Shift open the panel (no events, the screen is the panel's), a
 * tap on a neighbouring button while it is up does nothing, CANCEL (Back) hands the opening gains back unsaved, DONE
 * hands the edited gains back to save; the gains the panel opens with come from the app's callback. */
static const int Q_OPEN[EQ_BANDS] = { 3, 0, -2, 0, 5 };
static void q_gains(int g[EQ_BANDS]) { memcpy(g, Q_OPEN, sizeof Q_OPEN); }

static void eq_door(void)
{
    static Framebuffer fb;
    static TouchInput touch;
    JfApp app;
    JfUi ui;
    JfEvent ev[MINI_MAX];
    memset(&app, 0, sizeof app);
    app.screen = JF_SCR_NOWPLAYING;
    jf_ui_init(&ui, &fb, &touch, NULL, &app);
    ui.eq_gains_fn = q_gains;
    feed(&ui, 0, 0, false, false, false, 1000, ev, MINI_MAX);
    int g[EQ_BANDS];
    bool save = true;
    CHECK(!ui.eq_up && !jf_ui_eq_take(&ui, g, &save), "Q: panel up or a gain set pending before anything was pressed");

    UiRect b = ui.L.np[JF_UI_NP_EQ];
    int n = tap_at(&ui, b.x + b.w / 2, b.y + b.h / 2, 3000, ev);
    CHECK(ui.eq_up && n == 0, "Q: a tap on EQ did not open the panel quietly (up %d, %d events)", ui.eq_up, n);
    CHECK(memcmp(eq_panel_gains(&ui.eq), Q_OPEN, sizeof Q_OPEN) == 0, "Q: the panel did not open with the callback's gains");
    UiRect v = ui.eq.L.title;   /* an inert spot of the panel: no event reaches the screen under it */
    n = tap_at(&ui, v.x + v.w / 2, v.y + v.h / 2, 6000, ev);
    CHECK(n == 0 && ui.eq_up, "Q: a tap on the panel title closed it or reached the screen under it (%d events)", n);
    n = feed_btn(&ui, BTN_ID_BACK, 9000, ev, MINI_MAX);
    CHECK(n == 0 && !ui.eq_up, "Q: Back did not close the panel quietly (%d events, up %d)", n, ui.eq_up);
    CHECK(jf_ui_eq_take(&ui, g, &save) && !save && memcmp(g, Q_OPEN, sizeof Q_OPEN) == 0, "Q: cancel did not hand back the opening gains unsaved");
    CHECK(!jf_ui_eq_take(&ui, g, &save), "Q: a taken gain set was handed out twice");

    feed(&ui, 0, 0, false, false, false, 12000, ev, MINI_MAX);
    n = feed_btn(&ui, BTN_ID_RUN, 13000, ev, MINI_MAX);
    CHECK(ui.eq_up && n == 0, "Q: Shift did not open the panel (up %d)", ui.eq_up);
    /* edit: a tap on the DONE button after the guards are quiet */
    UiRect d = ui.eq.L.done;
    n = tap_at(&ui, d.x + d.w / 2, d.y + d.h / 2, 16000, ev);
    CHECK(!ui.eq_up && n == 0, "Q: DONE did not close the panel (up %d, %d events)", ui.eq_up, n);
    CHECK(jf_ui_eq_take(&ui, g, &save) && save, "Q: DONE did not hand the gains back to be saved");
    jf_ui_free(&ui);
}

int main(void)
{
    run(0, 0, 0, 0);
    run(17, 17, 3, 3);       /* the measured unit */
    run(30, 30, 10, 10);
    run(48, 48, 48, 48);     /* the largest inset the library allows */
    portrait();
    eq_door();
    mini_taps();
    mini_flip(JF_SCR_ALBUMS, "albums");
    mini_flip(JF_SCR_ARTISTS, "artists");
    mix_ticks();
    root_picker();
    settings_ui();
    queue_shuffle_cursor();
    queue_row();
    queue_widths();
    if (fails) { printf("jf_ui_layout_test: %d FAILED\n", fails); return 1; }
    printf("jf_ui_layout_test: all passed\n");
    return 0;
}
