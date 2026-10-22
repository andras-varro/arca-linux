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
 *   R  the queue view of now playing: 11 buttons in one row (wrapping when narrower than 676 px), the transport sends now playing's events, CLR, EQ, the title returns (queue_row, queue_widths).
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
    UiRect press[64];
    int n = 0;
    for (int i = 0; i < JF_UI_ROW3; i++) press[n++] = L.row3[i];
    for (int i = 0; i < JF_UI_ROW4; i++) {   /* the TRACKS screens' four-button row: pressable, clear of the list and of each other */
        press[n++] = L.row4[i];
        CHECK(disjoint(L.row4[i], L.list) && L.row4[i].y == L.row3[0].y, "C: row4[%d] overlaps the list or left the button row", i);
        for (int j = i + 1; j < JF_UI_ROW4; j++) CHECK(disjoint(L.row4[i], L.row4[j]), "C: row4 %d/%d overlap", i, j);
    }
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
 * returns to now playing, EQ opens the panel, and the pad walks all eleven. */
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
    CHECK(first_event(&ui, ui.L.qrow[9].x + 20, ui.L.qrow[9].y + 20, t) == JF_EV_VOL_UP, "R: VOL+ sent the wrong event"); t += 1000;
    CHECK(first_event(&ui, ui.L.qrow[10].x + 20, ui.L.qrow[10].y + 20, t) == JF_EV_VOL_DOWN, "R: VOL- sent the wrong event"); t += 1000;
    CHECK(first_event(&ui, ui.L.qrow[5].x + 20, ui.L.qrow[5].y + 20, t) == JF_EV_QUEUE_CLEAR, "R: CLR did not send QUEUE_CLEAR"); t += 1000;
    CHECK(first_event(&ui, ui.L.qrow[6].x + 20, ui.L.qrow[6].y + 20, t) == JF_EV_SHUFFLE, "R: RND did not send SHUFFLE"); t += 1000;
    /* the title: a tap on it goes back; a tap elsewhere in the bar's middle row of the list does not */
    TopBarLayout *TL = &ui.bar.L;
    CHECK(first_event(&ui, TL->title_x + TL->title_w / 2, TL->top + TL->h / 2, t) == JF_EV_BACK, "R: a tap on the QUEUE title did not send BACK");
    t += 1000;
    /* the pad: Right walks the eleven and wraps; ACTION on the last is VOL- */
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

/* Q  the queue row has REMOVE where UP and DN were: eleven buttons, EQ at 8, the labels in order */
static void qrow_shape(void)
{
    static Framebuffer fb;
    static TouchInput touch;
    JfApp app;
    JfUi ui;
    JfEvent ev[MINI_MAX];
    memset(&app, 0, sizeof app);
    app.screen = JF_SCR_QUEUE;
    app.queue_n = 5;
    jf_ui_init(&ui, &fb, &touch, NULL, &app);
    feed(&ui, 0, 0, false, false, false, 1000, ev, MINI_MAX);
    CHECK(JF_UI_QROW == 11, "Q: the queue row has %d buttons, want 11 (UP and DN are gone)", JF_UI_QROW);
    CHECK(JF_UI_QROW_EQ == 8, "Q: EQ is at index %d, want 8", JF_UI_QROW_EQ);
    static const char *want[11] = { "", "", "", "", "SAVE", "CLR", "RND", "REMOVE", "EQ", "VOL+", "VOL-" };
    int m = JF_UI_QROW < 11 ? JF_UI_QROW : 11;
    for (int i = 0; i < m; i++)
        CHECK(strcmp(ui.qrow[i].text, want[i]) == 0, "Q: queue button %d reads '%s', want '%s'", i, ui.qrow[i].text, want[i]);
    jf_ui_free(&ui);
}

/* Q  touch on queue rows: the first tap only moves the selection, a tap on the selected row plays it, REMOVE sends the
 *    cursor row and pulls the cursor off a vanished last row, and a held-and-dragged row reorders without playing */
static int row_y(const JfUi *ui, int r) { return ui->L.list.y + LV_BORDER + 40 * r + 20; }

static void queue_touch(void)
{
    static Framebuffer fb;
    static TouchInput touch;
    JfApp app;
    JfUi ui;
    JfEvent ev[MINI_MAX];
    memset(&app, 0, sizeof app);
    app.screen = JF_SCR_QUEUE;
    app.queue_n = 6;
    jf_ui_init(&ui, &fb, &touch, NULL, &app);
    feed(&ui, 0, 0, false, false, false, 1000, ev, MINI_MAX);
    int x = ui.L.list.x + 30;
    uint32_t t = 3000;
    int n = tap_at(&ui, x, row_y(&ui, 2), t, ev);
    CHECK(ui.lv.cursor == 2, "Q: first tap left the cursor on row %d, want 2", ui.lv.cursor);
    CHECK(!has_event(ev, n, JF_EV_QUEUE_JUMP), "Q: the first tap on an unselected row played it");
    n = tap_at(&ui, x, row_y(&ui, 2), t += 1000, ev);
    CHECK(has_event(ev, n, JF_EV_QUEUE_JUMP) && ev[0].i == 2, "Q: a tap on the selected row did not play row 2");
    n = tap_at(&ui, x, row_y(&ui, 4), t += 1000, ev);
    CHECK(ui.lv.cursor == 4 && !has_event(ev, n, JF_EV_QUEUE_JUMP), "Q: tapping another row played it or missed it (cursor %d)", ui.lv.cursor);
    /* REMOVE: the cursor row goes, and a cursor on the last row steps up */
    UiRect rm = ui.L.qrow[7];
    n = tap_at(&ui, rm.x + 20, rm.y + 20, t += 1000, ev);
    CHECK(n == 1 && ev[0].type == JF_EV_QUEUE_REMOVE && ev[0].i == 4, "Q: REMOVE sent %d events, first type %d i %d (want one QUEUE_REMOVE of row 4)", n, n ? (int)ev[0].type : -1, n ? ev[0].i : -1);
    lv_move_to(&ui.lv, 5);
    n = tap_at(&ui, rm.x + 20, rm.y + 20, t += 1000, ev);
    CHECK(has_event(ev, n, JF_EV_QUEUE_REMOVE) && ev[0].i == 5 && ui.lv.cursor == 4, "Q: REMOVE of the last row left the cursor on %d (want 4)", ui.lv.cursor);
    lv_move_to(&ui.lv, 1);
    n = tap_at(&ui, rm.x + 20, rm.y + 20, t += 1000, ev);
    CHECK(has_event(ev, n, JF_EV_QUEUE_REMOVE) && ev[0].i == 1 && ui.lv.cursor == 1, "Q: REMOVE of a middle row moved the cursor to %d (want 1)", ui.lv.cursor);
    /* a drag from row 1 to row 3 reorders and is not a tap on the selected row */
    lv_move_to(&ui.lv, 1);
    t += 3000;
    feed(&ui, x, row_y(&ui, 1), true, true, false, t, ev, MINI_MAX);
    feed(&ui, x, row_y(&ui, 1), false, true, false, t + LV_PICK_MS + 50, ev, MINI_MAX);
    feed(&ui, x, row_y(&ui, 3), false, true, false, t + LV_PICK_MS + 150, ev, MINI_MAX);
    n = feed(&ui, x, row_y(&ui, 3), false, false, true, t + LV_PICK_MS + 250, ev, MINI_MAX);
    CHECK(has_event(ev, n, JF_EV_QUEUE_MOVE), "Q: the drag sent no QUEUE_MOVE");
    CHECK(!has_event(ev, n, JF_EV_QUEUE_JUMP), "Q: the drag counted as a tap on the selected row");
    jf_ui_free(&ui);
}

/* the numbers the operator asked for, at the measured unit and a narrower one */
static void queue_widths(void)
{
    JfExtents u = { 0, 0, 800, 480, 17, 17, 797, 463 };      /* the measured unit: 780 px of safe width */
    JfExtents z = { 0, 0, 800, 480, 0, 0, 800, 480 };
    JfExtents nar = { 0, 0, 800, 480, 30, 17, 770, 463 };    /* 740 px: still one row at 56 */
    JfExtents tight = { 0, 0, 800, 480, 62, 17, 738, 463 };  /* 676 px: the last width of one row */
    JfExtents wrap = { 0, 0, 800, 480, 63, 17, 738, 463 };   /* 675 px: wraps */
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

/* P  the playlist action menu: it takes the mini player's strip (its rects, its list shortening), shows OPEN / DELETE /
 *    CANCEL or OPEN / CANCEL when the server's policy gives no delete right, hides the mini player's own buttons while
 *    up, hands the list its rectangle back, and sends OPEN / DELETE / CANCEL with the row; a track row keeps the
 *    five-button bar */
static void pm_setup(JfApp *app, int queue_n, bool can_delete, JfItemType type)
{
    jf_app_init(app);
    app->screen = JF_SCR_ARTISTS;
    app->cat = JF_CAT_PLAYLISTS;
    app->list = &app->lists[JF_LIST_ARTISTS];
    app->list->n = 2;
    app->list->total = 2;
    for (int i = 0; i < 2; i++) {
        app->list->items[i].type = type;
        snprintf(app->list->items[i].name, sizeof app->list->items[i].name, "LIST%d", i);
        app->list->items[i].count = 12;
    }
    app->queue_n = queue_n;
    app->policy_known = can_delete;
    app->policy.is_admin = can_delete;
    app->action_bar = true;
    app->action_row = 1;
}

static void playlist_menu(void)
{
    static Framebuffer fb;
    static TouchInput touch;
    static JfApp app;
    JfUi ui;
    JfEvent ev[MINI_MAX];
    UiRect r;
    int n;

    /* with delete and no mini player: the strip is used, three buttons on the mini_btn rects */
    pm_setup(&app, 0, true, JF_ITEM_PLAYLIST);
    jf_ui_init(&ui, &fb, &touch, NULL, &app);
    feed(&ui, 0, 0, false, false, false, 1000, ev, MINI_MAX);
    CHECK(ui.mini_on && ui.lv.rect.h == ui.L.list_mini.h, "P: playlist_menu_uses_mini_strip_rects: list not shortened (mini_on %d, h %d)", ui.mini_on, ui.lv.rect.h);
    static const JfEventType want[3] = { JF_EV_ACTIVATE, JF_EV_PLAYLIST_DELETE, JF_EV_DISMISS };
    for (int k = 0; k < 3; k++) {
        r = ui.L.mini_btn[k];
        n = tap_at(&ui, r.x + r.w / 2, r.y + r.h / 2, 2000u + 1000u * (uint32_t)k, ev);
        CHECK(has_event(ev, n, want[k]), "P: playlist_menu_emits_open_delete_dismiss: mini_btn[%d] sent no event %d (%d events)", k, (int)want[k], n);
        for (int j = 0; j < n; j++) if (ev[j].type == want[k] && want[k] != JF_EV_DISMISS)
            CHECK(ev[j].i == 1, "P: playlist_menu_emits_open_delete_dismiss: button %d sent row %d, want action_row 1", k, ev[j].i);
    }
    /* the pad: A on OPEN acts, Right then A is DELETE, Left from OPEN wraps to CANCEL */
    n = feed_btn(&ui, BTN_ID_ACTION, 6000, ev, MINI_MAX);
    CHECK(has_event(ev, n, JF_EV_ACTIVATE) && ev[0].i == 1, "P: playlist_menu_emits_open_delete_dismiss: Enter on the first button is not OPEN");
    feed_btn(&ui, BTN_ID_RIGHT, 6100, ev, MINI_MAX);
    n = feed_btn(&ui, BTN_ID_ACTION, 6200, ev, MINI_MAX);
    CHECK(has_event(ev, n, JF_EV_PLAYLIST_DELETE), "P: playlist_menu_emits_open_delete_dismiss: Right, Enter is not DELETE (%d events)", n);
    feed_btn(&ui, BTN_ID_RIGHT, 6300, ev, MINI_MAX);
    n = feed_btn(&ui, BTN_ID_ACTION, 6400, ev, MINI_MAX);
    CHECK(has_event(ev, n, JF_EV_DISMISS), "P: playlist_menu_emits_open_delete_dismiss: third button is not CANCEL (%d events)", n);
    n = feed_btn(&ui, BTN_ID_DOWN, 6500, ev, MINI_MAX);
    CHECK(has_event(ev, n, JF_EV_DISMISS), "P: playlist_menu_emits_open_delete_dismiss: Down did not dismiss");
    /* the menu goes: the list gets its rectangle back, there is no mini player to return */
    app.action_bar = false;
    feed(&ui, 0, 0, false, false, false, 7000, ev, MINI_MAX);
    CHECK(!ui.mini_on && ui.lv.rect.h == ui.L.list.h, "P: playlist_menu_restores_list_rect_after_cancel: list is %d high, want %d (mini_on %d)", ui.lv.rect.h, ui.L.list.h, ui.mini_on);
    jf_ui_free(&ui);

    /* without delete right: two buttons, OPEN and CANCEL; the queue's mini player returns afterwards */
    pm_setup(&app, 1, false, JF_ITEM_PLAYLIST);
    jf_ui_init(&ui, &fb, &touch, NULL, &app);
    feed(&ui, 0, 0, false, false, false, 1000, ev, MINI_MAX);
    static const JfEventType want2[2] = { JF_EV_ACTIVATE, JF_EV_DISMISS };
    for (int k = 0; k < 2; k++) {
        r = ui.L.mini_btn[k + 1];
        n = tap_at(&ui, r.x + r.w / 2, r.y + r.h / 2, 2000u + 1000u * (uint32_t)k, ev);
        CHECK(has_event(ev, n, want2[k]) && !has_event(ev, n, JF_EV_PLAYLIST_DELETE), "P: playlist_menu_two_buttons_when_no_delete: button %d sent no %d (%d events)", k, (int)want2[k], n);
    }
    /* the mini player's own buttons are not hit-tested while the menu is up (PAUSE sits under the vacant first slot) */
    r = ui.L.mini_btn[0];
    n = tap_at(&ui, r.x + r.w / 2, r.y + r.h / 2, 5000, ev);
    CHECK(!has_event(ev, n, JF_EV_PAUSE) && !has_event(ev, n, JF_EV_VOL_DOWN) && !has_event(ev, n, JF_EV_VOL_UP),
          "P: playlist_menu_hides_mini_buttons: a mini player button answered under the menu (%d events)", n);
    app.action_bar = false;
    feed(&ui, 0, 0, false, false, false, 7000, ev, MINI_MAX);
    CHECK(ui.mini_on && ui.lv.rect.h == ui.L.list_mini.h, "P: playlist_menu_restores_list_rect_after_cancel: mini player did not return (mini_on %d)", ui.mini_on);
    r = ui.L.mini_btn[0];
    n = tap_at(&ui, r.x + r.w / 2, r.y + r.h / 2, 9000, ev);
    CHECK(has_event(ev, n, JF_EV_PAUSE), "P: playlist_menu_hides_mini_buttons: PAUSE dead after the menu closed (%d events)", n);
    jf_ui_free(&ui);

    /* a track row keeps the five-button bar in the button row, and the strip stays out */
    pm_setup(&app, 0, true, JF_ITEM_TRACK);
    jf_ui_init(&ui, &fb, &touch, NULL, &app);
    feed(&ui, 0, 0, false, false, false, 1000, ev, MINI_MAX);
    CHECK(!ui.mini_on, "P: track_bar_unchanged_for_track_rows: the strip came up for a track row");
    r = ui.L.abar[4];
    n = tap_at(&ui, r.x + r.w / 2, r.y + r.h / 2, 2000, ev);
    CHECK(has_event(ev, n, JF_EV_INSTANT_MIX) && ev[0].i == 1, "P: track_bar_unchanged_for_track_rows: abar[4] is not INSTANT MIX on the row (%d events)", n);
    for (int k = 0; k < 4; k++) feed_btn(&ui, BTN_ID_RIGHT, 3000u + 100u * (uint32_t)k, ev, MINI_MAX);
    n = feed_btn(&ui, BTN_ID_ACTION, 4000, ev, MINI_MAX);
    CHECK(has_event(ev, n, JF_EV_INSTANT_MIX), "P: track_bar_unchanged_for_track_rows: four Rights then Enter is not INSTANT MIX (%d events)", n);
    jf_ui_free(&ui);
}

/* Q  the queue's REMOVE label keeps the margin the other labels have: its ink (6 px a glyph times the scale) is inside the
 *    button less 12 px */
static void remove_label_fits(void)
{
    static Framebuffer fb;
    static TouchInput touch;
    JfApp app;
    JfUi ui;
    memset(&app, 0, sizeof app);
    jf_ui_init(&ui, &fb, &touch, NULL, &app);
    for (int i = 4; i < JF_UI_QROW; i++) {
        int ink = 6 * (int)strlen(ui.qrow[i].text) * ui.qrow[i].text_scale;
        CHECK(ink <= ui.L.qrow[i].w - 12, "Q: remove_label_fits: queue button %d \"%s\" is %d px of ink in a %d px button", i, ui.qrow[i].text, ink, ui.L.qrow[i].w);
    }
    CHECK(strcmp(ui.qrow[7].text, "REMOVE") == 0, "Q: remove_label_fits: the label was renamed");
    jf_ui_free(&ui);
}

/* T  ADD TO PLAYLIST, the UI half: the track action bar's sixth button and the TRACKS screens' fourth bottom button open the
 *    picker; the picker is "+ NEW PLAYLIST" over the playlists, selects without opening, shows ADD only with a selection and
 *    closes on Back; the duplicate question names the playlist and the counts */
static void tk_setup(JfApp *app, JfScreen sc, bool bar)
{
    jf_app_init(app);
    app->screen = sc;
    app->list = &app->lists[sc == JF_SCR_ALBUMS ? JF_LIST_ARTISTS : JF_LIST_TRACKS];
    app->list->n = 3;
    app->list->total = 3;
    for (int i = 0; i < 3; i++) {
        app->list->items[i].type = sc == JF_SCR_ALBUMS ? JF_ITEM_ALBUM : JF_ITEM_TRACK;
        snprintf(app->list->items[i].name, sizeof app->list->items[i].name, "ITEM%d", i);
    }
    app->action_bar = bar;
    app->action_row = 2;
}

static void pp_setup(JfApp *app, int sel)
{
    jf_app_init(app);
    app->screen = JF_SCR_PLPICK;
    app->list = &app->lists[JF_LIST_PLPICK];
    app->lists[JF_LIST_PLPICK].n = 3;
    app->lists[JF_LIST_PLPICK].total = 3;
    for (int i = 0; i < 3; i++) {
        app->lists[JF_LIST_PLPICK].items[i].type = JF_ITEM_PLAYLIST;
        snprintf(app->lists[JF_LIST_PLPICK].items[i].name, sizeof app->lists[JF_LIST_PLPICK].items[i].name, "PL%d", i);
    }
    app->pick_n = 5;
    app->pick_sel = sel;
}

static void add_to_playlist(void)
{
    static Framebuffer fb;
    static TouchInput touch;
    static JfApp app;
    JfUi ui;
    JfEvent ev[MINI_MAX];
    UiRect r;
    int n;

    /* abar_has_6_and_fits_800: six buttons, the new one named, INSTANT MIX shortened */
    CHECK(JF_UI_ABAR == 6, "T: abar_has_6_and_fits_800: JF_UI_ABAR is %d", JF_UI_ABAR);
    tk_setup(&app, JF_SCR_TRACKS, true);
    jf_ui_init(&ui, &fb, &touch, NULL, &app);
    CHECK(strcmp(ui.abar[5].text, "+ PLAYLIST") == 0 && strcmp(ui.abar[4].text, "I-MIX") == 0,
          "T: abar_has_6_and_fits_800: labels are \"%s\" / \"%s\"", ui.abar[5].text, ui.abar[4].text);
    for (int i = 0; i < JF_UI_ABAR; i++) {
        int ink = 6 * (int)strlen(ui.abar[i].text) * ui.abar[i].text_scale;
        CHECK(ui.L.abar[i].x + ui.L.abar[i].w <= 800 && ink <= ui.L.abar[i].w - 12,
              "T: abar_has_6_and_fits_800: button %d \"%s\" is %d px of ink in a %d px button at x %d", i, ui.abar[i].text, ink, ui.L.abar[i].w, ui.L.abar[i].x);
    }
    feed(&ui, 0, 0, false, false, false, 1000, ev, MINI_MAX);

    /* abar_plus_playlist_emits_pick_open */
    r = ui.L.abar[5];
    n = tap_at(&ui, r.x + r.w / 2, r.y + r.h / 2, 2000, ev);
    CHECK(has_event(ev, n, JF_EV_PICK_OPEN) && ev[0].i == 2, "T: abar_plus_playlist_emits_pick_open: no PICK_OPEN on row 2 (%d events)", n);
    r = ui.L.abar[4];
    n = tap_at(&ui, r.x + r.w / 2, r.y + r.h / 2, 3000, ev);
    CHECK(has_event(ev, n, JF_EV_INSTANT_MIX) && !has_event(ev, n, JF_EV_PICK_OPEN), "T: abar_plus_playlist_emits_pick_open: I-MIX button changed its event");
    for (int k = 0; k < 5; k++) feed_btn(&ui, BTN_ID_RIGHT, 4000u + 100u * (uint32_t)k, ev, MINI_MAX);
    n = feed_btn(&ui, BTN_ID_ACTION, 5000, ev, MINI_MAX);
    CHECK(has_event(ev, n, JF_EV_PICK_OPEN), "T: abar_plus_playlist_emits_pick_open: five Rights then Enter is not + PLAYLIST (%d events)", n);
    jf_ui_free(&ui);

    /* row3_plus_playlist_on_album_tracks_only: an album's or playlist's tracks only, not an artist's albums, search or a genre's tracks */
    static const struct { JfScreen sc; bool genre; bool want; const char *name; } rc[4] = {
        { JF_SCR_TRACKS, false, true, "tracks" }, { JF_SCR_TRACKS, true, false, "genre tracks" },
        { JF_SCR_ALBUMS, false, false, "albums" }, { JF_SCR_SEARCH, false, false, "search" } };
    for (int k = 0; k < 4; k++) {
        tk_setup(&app, rc[k].sc, false);
        if (rc[k].genre) app.ctx[JF_LIST_TRACKS].sub = JF_SUB_GENRE;
        jf_ui_init(&ui, &fb, &touch, NULL, &app);
        feed(&ui, 0, 0, false, false, false, 1000, ev, MINI_MAX);
        r = ui.L.row4[3];
        n = tap_at(&ui, r.x + r.w / 2, r.y + r.h / 2, 2000, ev);
        bool got = has_event(ev, n, JF_EV_PICK_OPEN);
        CHECK(got == rc[k].want && (!got || ev[0].i == -1), "T: row3_plus_playlist_on_album_tracks_only: %s: PICK_OPEN %d, want %d", rc[k].name, got, rc[k].want);
        if (k == 0) CHECK(strcmp(ui.row4[3].text, "+ PLAYLIST") == 0, "T: row3_plus_playlist_on_album_tracks_only: label \"%s\"", ui.row4[3].text);
        jf_ui_free(&ui);
    }

    /* the picker's rows: row 0 is NEW PLAYLIST, row k is playlist k-1 */
    pp_setup(&app, -1);
    jf_ui_init(&ui, &fb, &touch, NULL, &app);
    feed(&ui, 0, 0, false, false, false, 1000, ev, MINI_MAX);
    LvRow row;
    memset(&row, 0, sizeof row);
    CHECK(ui.lv.src.count(ui.lv.src.ctx) == 4, "T: plpick_row0_is_new_playlist: %d rows, want 4", ui.lv.src.count(ui.lv.src.ctx));
    ui.lv.src.row(ui.lv.src.ctx, 0, &row);
    CHECK(row.title && strcmp(row.title, "+ NEW PLAYLIST") == 0, "T: plpick_row0_is_new_playlist: row 0 is \"%s\"", row.title ? row.title : "(null)");
    for (int k = 1; k <= 3; k++) {
        memset(&row, 0, sizeof row);
        ui.lv.src.row(ui.lv.src.ctx, k, &row);
        char want[8];
        snprintf(want, sizeof want, "PL%d", k - 1);
        CHECK(row.title && strstr(row.title, want), "T: plpick_row_k_maps_to_list_k_minus_1: row %d is \"%s\", want %s", k, row.title ? row.title : "(null)", want);
    }

    /* plpick_select_emits_pick_select_not_activate: a tap on list row 2 selects playlist 1 and opens nothing */
    r = ui.lv.rect;
    n = tap_at(&ui, r.x + r.w / 2, r.y + 2 * 40 + 20, 2000, ev);
    CHECK(has_event(ev, n, JF_EV_PICK_SELECT) && ev[0].i == 1, "T: plpick_select_emits_pick_select_not_activate: no PICK_SELECT 1 (%d events)", n);
    CHECK(!has_event(ev, n, JF_EV_SELECT) && !has_event(ev, n, JF_EV_ACTIVATE), "T: plpick_select_emits_pick_select_not_activate: a tap opened something");

    /* plpick_esc_closes */
    n = feed_btn(&ui, BTN_ID_BACK, 3000, ev, MINI_MAX);
    CHECK(has_event(ev, n, JF_EV_PICK_CLOSE), "T: plpick_esc_closes: Back sent no PICK_CLOSE (%d events)", n);
    r = ui.L.mixb[1];
    n = tap_at(&ui, r.x + r.w / 2, r.y + r.h / 2, 4000, ev);
    CHECK(has_event(ev, n, JF_EV_PICK_CLOSE) && strcmp(ui.mixb[1].text, "CLOSE") == 0, "T: plpick_buttons_reuse_mixb: CLOSE sent no PICK_CLOSE (%d events)", n);

    /* plpick_add_absent_without_selection: no selection, no ADD (not greyed, and a tap on its place does nothing) */
    CHECK(!jf_ui_plpick_add_shown(&app), "T: plpick_add_absent_without_selection: ADD shown with pick_sel -1");
    r = ui.L.mixb[0];
    n = tap_at(&ui, r.x + r.w / 2, r.y + r.h / 2, 5000, ev);
    CHECK(!has_event(ev, n, JF_EV_PICK_ADD), "T: plpick_add_absent_without_selection: a tap on the empty ADD place sent PICK_ADD");
    app.pick_sel = 1;
    CHECK(jf_ui_plpick_add_shown(&app), "T: plpick_add_absent_without_selection: ADD missing with a selection");
    feed(&ui, 0, 0, false, false, false, 5500, ev, MINI_MAX);
    n = tap_at(&ui, r.x + r.w / 2, r.y + r.h / 2, 6000, ev);
    CHECK(has_event(ev, n, JF_EV_PICK_ADD) && strcmp(ui.mixb[0].text, "ADD") == 0, "T: plpick_buttons_reuse_mixb: ADD sent no PICK_ADD (%d events)", n);
    app.dup_scan = true;
    CHECK(!jf_ui_plpick_add_shown(&app), "T: plpick_add_absent_without_selection: ADD shown during the duplicate scan");
    app.dup_scan = false;

    /* plpick_select_marks_one_row: pick_sel 1 is list row 2, and only that row carries the yellow bar */
    feed(&ui, 0, 0, false, false, false, 7000, ev, MINI_MAX);
    int marked = 0;
    for (int i = 0; i < 4; i++) if (lv_is_marked(&ui.lv, i)) marked++;
    CHECK(marked == 1 && lv_is_marked(&ui.lv, 2), "T: plpick_select_marks_one_row: %d rows marked, row 2 %d", marked, lv_is_marked(&ui.lv, 2));
    app.pick_sel = -1;
    feed(&ui, 0, 0, false, false, false, 8000, ev, MINI_MAX);
    CHECK(!lv_is_marked(&ui.lv, 2) && !lv_is_marked(&ui.lv, 0), "T: plpick_select_marks_one_row: a mark stayed with no selection");
    jf_ui_free(&ui);

    /* dup_confirm_titles */
    char t[96];
    snprintf(app.add_name, sizeof app.add_name, "MIXTAPE");
    app.pick_n = 1; app.dup_hits = 1; app.dup_unknown = false;
    jf_ui_dup_title(&app, t, sizeof t);
    CHECK(strcmp(t, "ALREADY IN MIXTAPE") == 0, "T: dup_confirm_titles: one of one is \"%s\"", t);
    app.pick_n = 12; app.dup_hits = 3;
    jf_ui_dup_title(&app, t, sizeof t);
    CHECK(strcmp(t, "3 OF 12 ALREADY IN MIXTAPE") == 0, "T: dup_confirm_titles: some is \"%s\"", t);
    app.dup_unknown = true;
    jf_ui_dup_title(&app, t, sizeof t);
    CHECK(strcmp(t, "COULD NOT CHECK DUPLICATES - ADD?") == 0, "T: dup_confirm_titles: unknown is \"%s\"", t);
}

/* The artist folder (ALBUMS / PLAIN), the picker and the search: header counts, the PLAY ALL button, the delete question. */
static void folder_ui(void)
{
    static Framebuffer fb;
    static TouchInput touch;
    static JfApp app;
    JfUi ui;
    JfEvent ev[MINI_MAX];
    char t[96];

    jf_app_init(&app);
    app.screen = JF_SCR_ALBUMS;
    app.ctx[JF_LIST_ALBUMS].sub = JF_SUB_PLAIN;
    app.list = &app.lists[JF_LIST_ALBUMS];
    app.list->n = 7;
    app.list->total = 13;      /* the album phase's total, not the rows */
    for (int i = 0; i < 7; i++) app.list->items[i].type = i < 6 ? JF_ITEM_ALBUM : JF_ITEM_TRACK;
    CHECK(jf_ui_head_count(&app, t, sizeof t) && strcmp(t, "6 ALBUMS, 1 TRACK") == 0, "U: folder_header_counts_rows_by_type: \"%s\"", t);
    app.list->n = 1; app.list->items[0].type = JF_ITEM_ALBUM;
    CHECK(jf_ui_head_count(&app, t, sizeof t) && strcmp(t, "1 ALBUM") == 0, "U: folder_header_counts_rows_by_type: one album is \"%s\"", t);
    app.list->items[0].type = JF_ITEM_TRACK;
    CHECK(jf_ui_head_count(&app, t, sizeof t) && strcmp(t, "1 TRACK") == 0, "U: folder_header_counts_rows_by_type: one track is \"%s\"", t);
    app.list->n = 3; app.list->items[1].type = JF_ITEM_TRACK; app.list->items[2].type = JF_ITEM_TRACK;
    CHECK(jf_ui_head_count(&app, t, sizeof t) && strcmp(t, "3 TRACKS") == 0, "U: folder_header_counts_rows_by_type: tracks only is \"%s\"", t);
    app.list->n = 0;
    CHECK(!jf_ui_head_count(&app, t, sizeof t), "U: folder_header_counts_rows_by_type: an empty folder says \"%s\"", t);

    /* a genre's albums keep the server count */
    app.ctx[JF_LIST_ALBUMS].sub = JF_SUB_GENRE;
    CHECK(jf_ui_head_count(&app, t, sizeof t) && strcmp(t, "13 ALBUMS") == 0, "U: genre_header_keeps_total: \"%s\"", t);

    /* the picker names playlists */
    app.screen = JF_SCR_PLPICK;
    app.list = &app.lists[JF_LIST_PLPICK];
    app.list->n = 7; app.list->total = 7;
    CHECK(jf_ui_head_count(&app, t, sizeof t) && strcmp(t, "7 PLAYLISTS") == 0, "U: picker_header_says_playlists: \"%s\"", t);
    app.list->n = 1; app.list->total = 1;
    CHECK(jf_ui_head_count(&app, t, sizeof t) && strcmp(t, "1 PLAYLIST") == 0, "U: picker_header_says_playlists: one is \"%s\"", t);

    /* search: the rows once an inline artist made them exceed the server's total, the total while it is the larger */
    app.screen = JF_SCR_SEARCH;
    app.list = &app.lists[JF_LIST_SEARCH];
    app.list->n = 9; app.list->total = 2;
    CHECK(jf_ui_head_count(&app, t, sizeof t) && strcmp(t, "9 RESULTS") == 0, "U: search_header_follows_inline_rows: \"%s\"", t);
    app.list->n = 2; app.list->total = 40;
    CHECK(jf_ui_head_count(&app, t, sizeof t) && strcmp(t, "40 RESULTS") == 0, "U: search_header_follows_inline_rows: paging total is \"%s\"", t);

    /* PLAY ALL on the artist folder's bottom row, and not on a genre's albums */
    app.screen = JF_SCR_ALBUMS;
    app.list = &app.lists[JF_LIST_ALBUMS];
    app.ctx[JF_LIST_ALBUMS].sub = JF_SUB_PLAIN;
    app.list->n = 2; app.list->total = 2;
    app.list->items[0].type = JF_ITEM_ALBUM; app.list->items[1].type = JF_ITEM_ALBUM;
    jf_ui_init(&ui, &fb, &touch, NULL, &app);
    feed(&ui, 0, 0, false, false, false, 1000, ev, MINI_MAX);
    UiRect r = ui.L.row3[2];
    int n = tap_at(&ui, r.x + r.w / 2, r.y + r.h / 2, 2000, ev);
    CHECK(has_event(ev, n, JF_EV_PLAY_ALL), "U: folder_has_play_all: a tap on the third button sent no PLAY_ALL (%d events)", n);
    app.ctx[JF_LIST_ALBUMS].sub = JF_SUB_GENRE;
    feed(&ui, 0, 0, false, false, false, 3000, ev, MINI_MAX);
    n = tap_at(&ui, r.x + r.w / 2, r.y + r.h / 2, 4000, ev);
    CHECK(!has_event(ev, n, JF_EV_PLAY_ALL), "U: folder_has_play_all: a genre's albums sent PLAY_ALL");
    jf_ui_free(&ui);

    /* the delete question shows a 20-glyph name whole, and a longer one cut inside the dialog's 400 px */
    snprintf(app.confirm_seed.name, sizeof app.confirm_seed.name, "arca-test-playlist-20");
    jf_ui_delete_msg(&app, t, sizeof t);
    CHECK(strcmp(t, "DELETE arca-test-playlist-20?") == 0 || strcmp(t, "DELETE ARCA-TEST-PLAYLIST-20?") == 0, "U: delete_question_shows_long_name: \"%s\"", t);
    snprintf(app.confirm_seed.name, sizeof app.confirm_seed.name, "a very long playlist name that cannot fit the dialog at all");
    jf_ui_delete_msg(&app, t, sizeof t);
    CHECK(strlen(t) <= 33 && t[strlen(t) - 1] == '?', "U: delete_question_shows_long_name: long name gives %zu glyphs \"%s\"", strlen(t), t);
}

int main(void)
{
    add_to_playlist();
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
    qrow_shape();
    queue_touch();
    queue_widths();
    playlist_menu();
    remove_label_fits();
    folder_ui();
    if (fails) { printf("jf_ui_layout_test: %d FAILED\n", fails); return 1; }
    printf("jf_ui_layout_test: all passed\n");
    return 0;
}
