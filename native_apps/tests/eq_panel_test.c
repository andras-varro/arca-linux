/* eq_panel_test — the equaliser panel's pure rules.  eq_panel.c is NEW: there is no pre-fix source to fail against;
 * tests/measure_eq_panel_sabotage.sh breaks a copy in single ways and counts the failures.
 * Build (from native_apps/): see the eq_panel_test row of tests/run-all.sh. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include "eq_panel.h"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static EqPanel P;
static void open_flat(void) { int g[5] = { 0, 0, 0, 0, 0 }; eq_panel_open(&P, g, 0, 0, 800, 480, 1000); }

int main(void)
{
    /* layout: everything inside the rectangle, no overlap between rows, tracks tall enough */
    EqPanelLayout L;
    eq_panel_layout(&L, 10, 20, 790, 470);
    for (int b = 0; b < 5; b++) {
        CHECK(L.col[b].x >= 10 && L.col[b].x + L.col[b].w <= 790, "col %d x", b);
        CHECK(L.track[b].x >= L.col[b].x && L.track[b].x + L.track[b].w <= L.col[b].x + L.col[b].w, "track %d in col", b);
        CHECK(L.track[b].h >= 49, "track %d height %d", b, L.track[b].h);
        CHECK(L.track[b].y + L.track[b].h <= L.preset[0].y, "track %d above presets", b);
    }
    CHECK(L.cancel.y + L.cancel.h <= 470 && L.done.y + L.done.h <= 470, "buttons inside bottom");
    CHECK(L.preset[0].y + L.preset[0].h <= L.cancel.y, "presets above buttons");
    CHECK(L.preset[4].x + L.preset[4].w <= 790, "last preset inside right");
    EqPanelLayout Lp;
    eq_panel_layout(&Lp, 0, 0, 480, 700);   /* portrait width */
    CHECK(Lp.track[0].h >= 49 && Lp.preset[4].x + Lp.preset[4].w <= 480, "portrait layout");

    /* height -> dB: ends, middle, clamp, monotone, round trip */
    const UiRect *t = &L.track[2];
    CHECK(eq_panel_db_at(t, t->y) == 12, "top is +12");
    CHECK(eq_panel_db_at(t, t->y + t->h - 1) == -12, "bottom is -12");
    CHECK(eq_panel_db_at(t, t->y - 50) == 12 && eq_panel_db_at(t, t->y + t->h + 50) == -12, "clamped");
    CHECK(eq_panel_db_at(t, t->y + (t->h - 1) / 2) == 0, "middle is 0, got %d", eq_panel_db_at(t, t->y + (t->h - 1) / 2));
    int prev = 99, mono = 1;
    for (int y = t->y; y < t->y + t->h; y++) { int d = eq_panel_db_at(t, y); if (d > prev) mono = 0; prev = d; }
    CHECK(mono, "db never rises going down");
    for (int d = -12; d <= 12; d++) CHECK(eq_panel_db_at(t, eq_panel_y_of_db(t, d)) == d, "round trip %d", d);

    /* x -> band */
    for (int b = 0; b < 5; b++) CHECK(eq_panel_band_at(&L, L.col[b].x + L.col[b].w / 2, L.col[b].y + 5) == b, "band centre %d", b);
    CHECK(eq_panel_band_at(&L, 0, L.col[0].y + 5) == 0 && eq_panel_band_at(&L, 799, L.col[0].y + 5) == 4, "outer x clamps to end bands");
    CHECK(eq_panel_band_at(&L, 400, L.preset[0].y + 5) == -1, "y below the sliders is no band");

    /* keys: sliders */
    open_flat();
    CHECK(eq_panel_key(&P, EQK_UP) == EQP_CHANGED && P.gains[0] == 1, "up +1");
    eq_panel_key(&P, EQK_RIGHT); eq_panel_key(&P, EQK_RIGHT);
    CHECK(P.band == 2, "right twice");
    CHECK(eq_panel_key(&P, EQK_DOWN) == EQP_CHANGED && P.gains[2] == -1 && P.gains[0] == 1, "down hits the selected band only");
    for (int i = 0; i < 40; i++) eq_panel_key(&P, EQK_UP);
    CHECK(P.gains[2] == 12, "clamped at +12, got %d", P.gains[2]);
    CHECK(eq_panel_key(&P, EQK_UP) == EQP_NONE, "no change at the rail is no event");
    for (int i = 0; i < 9; i++) eq_panel_key(&P, EQK_LEFT);
    CHECK(P.band == 0, "left clamps at band 0");
    /* presets */
    CHECK(eq_panel_preset(&P) == -1, "custom has no preset");
    eq_panel_key(&P, EQK_ACT);
    CHECK(P.zone == EQP_ZONE_PRESETS, "act on a band goes to presets");
    eq_panel_key(&P, EQK_RIGHT);                    /* focus BASS (index 1) */
    CHECK(eq_panel_key(&P, EQK_ACT) == EQP_CHANGED, "apply preset");
    CHECK(eq_panel_preset(&P) == 1 && memcmp(P.gains, eq_preset_gains(1), 5 * sizeof(int)) == 0, "BASS matches");
    CHECK(eq_panel_key(&P, EQK_ACT) == EQP_NONE, "applying the same preset again is no change");
    eq_panel_key(&P, EQK_UP);
    CHECK(P.zone == EQP_ZONE_SLIDERS, "up leaves presets");
    eq_panel_key(&P, EQK_UP);
    CHECK(eq_panel_preset(&P) == -1, "editing a band leaves the preset (custom)");
    /* zones and buttons */
    eq_panel_key(&P, EQK_NEXT); eq_panel_key(&P, EQK_NEXT);
    CHECK(P.zone == EQP_ZONE_BUTTONS, "next twice reaches buttons");
    eq_panel_key(&P, EQK_NEXT);
    CHECK(P.zone == EQP_ZONE_SLIDERS, "next wraps");
    eq_panel_key(&P, EQK_PREV);
    CHECK(P.zone == EQP_ZONE_BUTTONS, "prev wraps back");
    /* cancel restores what the panel opened with */
    int g0[5] = { 3, -2, 0, 5, 1 };
    eq_panel_open(&P, g0, 0, 0, 800, 480, 1000);
    eq_panel_key(&P, EQK_UP); eq_panel_key(&P, EQK_RIGHT); eq_panel_key(&P, EQK_DOWN);
    CHECK(memcmp(P.gains, g0, sizeof g0) != 0, "edited");
    CHECK(eq_panel_key(&P, EQK_BACK) == EQP_CANCEL && memcmp(P.gains, g0, sizeof g0) == 0, "BACK restores");
    eq_panel_key(&P, EQK_UP);
    P.zone = EQP_ZONE_BUTTONS; P.btn_focus = 0;
    CHECK(eq_panel_key(&P, EQK_ACT) == EQP_CANCEL && memcmp(P.gains, g0, sizeof g0) == 0, "CANCEL button restores");
    open_flat();
    eq_panel_key(&P, EQK_UP);
    P.zone = EQP_ZONE_BUTTONS; P.btn_focus = EQP_BTN_DONE;
    int before[5]; memcpy(before, P.gains, sizeof before);
    CHECK(eq_panel_key(&P, EQK_ACT) == EQP_DONE && memcmp(P.gains, before, sizeof before) == 0 && P.gains[0] == 1, "DONE keeps the edit");
    /* touch: a drag sets the dragged band from y */
    open_flat();
    eq_panel_set_from_y(&P, 3, P.L.track[3].y);
    CHECK(P.gains[3] == 12 && P.gains[2] == 0, "set_from_y moves only its band");
    CHECK(!eq_panel_set_from_y(&P, 3, P.L.track[3].y), "same value is no change");
    CHECK(!eq_panel_set_from_y(&P, 7, 0), "bad band refused");
    /* open clamps a wild input */
    int wild[5] = { 99, -99, 0, 0, 0 };
    eq_panel_open(&P, wild, 0, 0, 800, 480, 1000);
    CHECK(P.gains[0] == 12 && P.gains[1] == -12, "open clamps");
    /* text */
    char b[8];
    eq_panel_db_text(b, sizeof b, 3);  CHECK(!strcmp(b, "+3"), "+3 got %s", b);
    eq_panel_db_text(b, sizeof b, 0);  CHECK(!strcmp(b, "0"), "0 got %s", b);
    eq_panel_db_text(b, sizeof b, -12); CHECK(!strcmp(b, "-12"), "-12 got %s", b);
    /* config value + round trip through the file, other keys kept */
    char v[64];
    eq_panel_config_value(eq_preset_gains(3), v, sizeof v);
    CHECK(!strcasecmp(v, eq_preset_name(3)), "preset writes its name, got %s", v);
    eq_panel_config_value(g0, v, sizeof v);
    CHECK(!strcmp(v, "3,-2,0,5,1"), "custom writes ints, got %s", v);
    char path[] = "/tmp/eq_panel_test_XXXXXX";
    int fd = mkstemp(path);
    CHECK(fd >= 0, "mkstemp");
    if (fd >= 0) {
        const char *seed = "audio_enabled=1\nled_brightness=40\neq=0,0,0,0,0\n";
        CHECK(write(fd, seed, strlen(seed)) == (ssize_t)strlen(seed), "seed");
        close(fd);
        CHECK(eq_panel_store_file(path, g0) == 0, "store custom");
        int back[5];
        CHECK(eq_config_read(path, back) == 0 && memcmp(back, g0, sizeof g0) == 0, "custom round-trips");
        Config c; config_init_path(&c, path); config_load(&c);
        CHECK(config_get_int(&c, "led_brightness", -1) == 40 && config_get_int(&c, "audio_enabled", -1) == 1, "other keys kept");
        CHECK(eq_panel_store_file(path, eq_preset_gains(2)) == 0, "store preset");
        CHECK(eq_config_read(path, back) == 0 && memcmp(back, eq_preset_gains(2), sizeof back) == 0, "preset round-trips");
        config_init_path(&c, path); config_load(&c);
        CHECK(!strcasecmp(config_get(&c, "eq", ""), eq_preset_name(2)), "stored as the name");
        unlink(path);
    }

    /* ── the ENHANCER page ── */
    {
        /* layout inside the safe rectangle at a bare panel and at a 48 px inset (the cap), every pressable thing inside it */
        static const int rects[2][4] = { { 0, 0, 800, 480 }, { 12, 48, 788, 432 } };
        for (int r = 0; r < 2; r++) {
            EqPanelLayout E;
            int sl = rects[r][0], st = rects[r][1], sr = rects[r][2], sb = rects[r][3];
            eq_panel_layout(&E, sl, st, sr, sb);
            for (int i = 0; i < EQP_ENH_SLIDERS; i++) {
                CHECK(E.ecol[i].x >= sl && E.ecol[i].x + E.ecol[i].w <= sr, "r%d ecol %d inside x", r, i);
                CHECK(E.ecol[i].y >= st && E.ecol[i].y + E.ecol[i].h <= sb, "r%d ecol %d inside y", r, i);
                CHECK(E.ecol[i].w >= 60 && E.etrack[i].w >= 40 && E.etrack[i].h >= 120, "r%d slider %d hit box %dx%d track %dx%d", r, i, E.ecol[i].w, E.ecol[i].h, E.etrack[i].w, E.etrack[i].h);
                CHECK(E.etrack[i].x >= E.ecol[i].x && E.etrack[i].x + E.etrack[i].w <= E.ecol[i].x + E.ecol[i].w, "r%d etrack %d in column", r, i);
                CHECK(E.etrack[i].y + E.etrack[i].h <= E.toggle.y, "r%d etrack %d above the toggle", r, i);
                if (i) CHECK(E.ecol[i].x >= E.ecol[i - 1].x + E.ecol[i - 1].w, "r%d ecol %d overlaps its neighbour", r, i);
            }
            CHECK(E.toggle.x >= sl && E.toggle.x + E.toggle.w <= sr && E.toggle.y + E.toggle.h <= E.cancel.y && E.toggle.h >= 40, "r%d toggle", r);
            CHECK(E.cancel.y + E.cancel.h <= sb && E.done.x + E.done.w <= sr && E.page.x >= E.cancel.x + E.cancel.w && E.done.x >= E.page.x + E.page.w, "r%d buttons in order inside", r);
            CHECK(E.page.w >= 80 && E.page.h >= 40, "r%d page button size", r);
            for (int g = 0; g < EQP_ENH_PARTS; g++)
                CHECK(E.group[g].x == E.ecol[2 * g].x && E.group[g].x + E.group[g].w == E.ecol[2 * g + 1].x + E.ecol[2 * g + 1].w
                      && E.group[g].y + E.group[g].h <= E.ecol[2 * g].y, "r%d group %d spans its two sliders", r, g);
        }
        /* open: all off; clamps wild input; the page starts on EQ */
        open_flat();
        CHECK(P.page == EQP_PAGE_EQ && !eq_panel_enh(&P)->on, "opens on the EQ page, enhancer off");
        AudioEnh w = { 7, 150, -5, 50, 100, 0, 33 };
        int g5[5] = { 0, 0, 0, 0, 0 };
        eq_panel_open_enh(&P, g5, &w, 0, 0, 800, 480, 1000);
        const AudioEnh *e = eq_panel_enh(&P);
        CHECK(e->on == 1 && e->hb_amt == 100 && e->hb_rng == 0 && e->db_amt == 50 && e->db_rng == 100 && e->ht_amt == 0 && e->ht_rng == 33, "open clamps the enhancer");
        /* getters / setters: the six slots in field order, clamp, no-change */
        AudioEnh t = { 0 };
        for (int i = 0; i < 6; i++) CHECK(eq_panel_enh_set(&t, i, 10 + i) && eq_panel_enh_get(&t, i) == 10 + i, "slot %d set/get", i);
        CHECK(t.hb_amt == 10 && t.hb_rng == 11 && t.db_amt == 12 && t.db_rng == 13 && t.ht_amt == 14 && t.ht_rng == 15, "slot order is the struct order");
        CHECK(eq_panel_enh_set(&t, 3, 999) && t.db_rng == 100 && !eq_panel_enh_set(&t, 3, 100), "clamp high, same value is no change");
        CHECK(eq_panel_enh_set(&t, 3, -4) && t.db_rng == 0, "clamp low");
        CHECK(!eq_panel_enh_set(&t, 6, 1) && !eq_panel_enh_set(&t, -1, 1), "bad slot refused");
        /* height -> value */
        EqPanelLayout E;
        eq_panel_layout(&E, 0, 0, 800, 480);
        const UiRect *et = &E.etrack[1];
        CHECK(eq_panel_enh_value_at(et, et->y) == 100 && eq_panel_enh_value_at(et, et->y + et->h - 1) == 0, "track ends are 100 and 0");
        CHECK(eq_panel_enh_value_at(et, et->y - 40) == 100 && eq_panel_enh_value_at(et, et->y + et->h + 40) == 0, "track clamps");
        for (int v = 0; v <= 100; v++) CHECK(eq_panel_enh_value_at(et, eq_panel_enh_y_of_value(et, v)) == v, "round trip %d", v);
        int pv = 101, mono = 1;
        for (int y = et->y; y < et->y + et->h; y++) { int v = eq_panel_enh_value_at(et, y); if (v > pv) mono = 0; pv = v; }
        CHECK(mono, "value never rises going down");
        for (int i = 0; i < 6; i++) CHECK(eq_panel_enh_at(&E, E.ecol[i].x + E.ecol[i].w / 2, E.ecol[i].y + 5) == i, "enh column %d hit", i);
        CHECK(eq_panel_enh_at(&E, 400, E.toggle.y + 5) == -1, "below the sliders is no column");

        /* page switch by pad: Next x2 to the buttons, Left to the page button, Act; then back */
        open_flat();
        CHECK(P.btn_focus == EQP_BTN_DONE, "DONE is the default button");
        eq_panel_key(&P, EQK_NEXT); eq_panel_key(&P, EQK_NEXT);
        CHECK(P.zone == EQP_ZONE_BUTTONS, "on the buttons");
        eq_panel_key(&P, EQK_LEFT);
        CHECK(P.btn_focus == EQP_BTN_PAGE && eq_panel_key(&P, EQK_ACT) == EQP_NONE && P.page == EQP_PAGE_ENH, "ENH button opens the page");
        eq_panel_key(&P, EQK_LEFT); eq_panel_key(&P, EQK_LEFT);
        CHECK(P.btn_focus == EQP_BTN_CANCEL, "left clamps at CANCEL");
        eq_panel_key(&P, EQK_RIGHT);
        CHECK(eq_panel_key(&P, EQK_ACT) == EQP_NONE && P.page == EQP_PAGE_EQ, "the same button goes back");
        /* the sliders of the enhancer page: each reachable, +-5, clamped, right field */
        AudioEnh zero = { 0 };
        eq_panel_open_enh(&P, g5, &zero, 0, 0, 800, 480, 1000);
        eq_panel_key(&P, EQK_NEXT); eq_panel_key(&P, EQK_NEXT); eq_panel_key(&P, EQK_LEFT);
        eq_panel_key(&P, EQK_ACT);
        CHECK(P.page == EQP_PAGE_ENH && P.zone == EQP_ZONE_BUTTONS, "enhancer page up");
        eq_panel_key(&P, EQK_NEXT);
        CHECK(P.zone == EQP_ZONE_SLIDERS, "next wraps to the sliders");
        for (int i = 0; i < 6; i++) {
            CHECK(P.enh_i == i, "slider %d focused, got %d", i, P.enh_i);
            CHECK(eq_panel_key(&P, EQK_UP) == EQP_CHANGED && eq_panel_enh_get(&P.enh, i) == EQP_ENH_STEP, "slider %d up", i);
            for (int j = 0; j < 6; j++) if (j != i) CHECK(eq_panel_enh_get(&P.enh, j) == (j < i ? EQP_ENH_STEP : 0), "slider %d changed slider %d", i, j);
            eq_panel_key(&P, EQK_RIGHT);
        }
        CHECK(P.enh_i == 5, "right clamps at the last slider");
        for (int i = 0; i < 40; i++) eq_panel_key(&P, EQK_UP);
        CHECK(eq_panel_enh_get(&P.enh, 5) == 100 && eq_panel_key(&P, EQK_UP) == EQP_NONE, "pad clamps at 100, no event at the rail");
        for (int i = 0; i < 40; i++) eq_panel_key(&P, EQK_DOWN);
        CHECK(eq_panel_enh_get(&P.enh, 5) == 0 && eq_panel_key(&P, EQK_DOWN) == EQP_NONE, "pad clamps at 0");
        CHECK(P.gains[0] == 0 && P.gains[4] == 0, "enhancer keys leave the EQ gains alone");
        /* toggle: the row below the sliders, Act flips, Up goes back */
        CHECK(!P.enh.on, "off");
        eq_panel_key(&P, EQK_NEXT);
        CHECK(P.zone == EQP_ZONE_PRESETS && eq_panel_key(&P, EQK_ACT) == EQP_CHANGED && P.enh.on == 1, "toggle on");
        CHECK(eq_panel_key(&P, EQK_ACT) == EQP_CHANGED && P.enh.on == 0, "toggle off");
        eq_panel_key(&P, EQK_RIGHT);
        CHECK(P.preset_focus == 0 && P.zone == EQP_ZONE_PRESETS, "left/right do nothing on the toggle");
        eq_panel_key(&P, EQK_UP);
        CHECK(P.zone == EQP_ZONE_SLIDERS, "up from the toggle");

        /* CANCEL restores both, by BACK and by the CANCEL button */
        AudioEnh o = { 1, 10, 20, 30, 40, 50, 60 };
        int go[5] = { 3, -2, 0, 5, 1 };
        eq_panel_open_enh(&P, go, &o, 0, 0, 800, 480, 1000);
        eq_panel_key(&P, EQK_UP);                                         /* EQ edit */
        P.page = EQP_PAGE_ENH; P.zone = EQP_ZONE_SLIDERS; P.enh_i = 2;
        eq_panel_key(&P, EQK_UP);                                         /* enhancer edit */
        P.zone = EQP_ZONE_PRESETS; eq_panel_key(&P, EQK_ACT);             /* toggle */
        CHECK(memcmp(P.gains, go, sizeof go) != 0 && memcmp(&P.enh, &o, sizeof o) != 0, "both edited");
        CHECK(eq_panel_key(&P, EQK_BACK) == EQP_CANCEL && memcmp(P.gains, go, sizeof go) == 0 && memcmp(eq_panel_enh(&P), &o, sizeof o) == 0, "BACK restores EQ and enhancer");
        P.gains[0]++; P.enh.hb_rng = 99; P.enh.on = 0;
        P.zone = EQP_ZONE_BUTTONS; P.btn_focus = EQP_BTN_CANCEL;
        CHECK(eq_panel_key(&P, EQK_ACT) == EQP_CANCEL && memcmp(P.gains, go, sizeof go) == 0 && memcmp(eq_panel_enh(&P), &o, sizeof o) == 0, "CANCEL button restores EQ and enhancer");
        P.gains[0]++; P.enh.hb_rng = 99;
        P.btn_focus = EQP_BTN_DONE;
        CHECK(eq_panel_key(&P, EQK_ACT) == EQP_DONE && P.enh.hb_rng == 99 && P.gains[0] == 4, "DONE keeps both edits");

        /* touch: tap the middle button, drag a slider, tap the toggle, tap CANCEL */
        eq_panel_open_enh(&P, go, &o, 0, 0, 800, 480, 1000);
        TouchState ts;
        uint32_t now = 5000;
        memset(&ts, 0, sizeof ts);
        eq_panel_input(&P, &ts, NULL, now);
#define TAP(rc) do { \
            memset(&ts, 0, sizeof ts); ts.pressed = true; ts.held = true; ts.x = (rc).x + (rc).w / 2; ts.y = (rc).y + (rc).h / 2; \
            now += 100; eq_panel_input(&P, &ts, NULL, now); \
            ts.pressed = false; now += 100; eq_panel_input(&P, &ts, NULL, now); \
            ts.held = false; ts.released = true; now += 100; eq_panel_input(&P, &ts, NULL, now); \
            memset(&ts, 0, sizeof ts); now += 300; eq_panel_input(&P, &ts, NULL, now); } while (0)
        TAP(P.L.page);
        CHECK(P.page == EQP_PAGE_ENH, "touch on ENH opens the enhancer page");
        EqPanelEvent ev = EQP_NONE;
        memset(&ts, 0, sizeof ts); ts.pressed = true; ts.held = true;
        ts.x = P.L.ecol[3].x + P.L.ecol[3].w / 2; ts.y = P.L.etrack[3].y;
        now += 100; ev = eq_panel_input(&P, &ts, NULL, now);
        CHECK(ev == EQP_CHANGED && P.enh.db_rng == 100 && P.enh_i == 3 && P.enh.db_amt == 30, "press on a track sets that slider only, got %d", P.enh.db_rng);
        ts.pressed = false; ts.y = P.L.etrack[3].y + P.L.etrack[3].h - 1;
        now += 100; ev = eq_panel_input(&P, &ts, NULL, now);
        CHECK(ev == EQP_CHANGED && P.enh.db_rng == 0, "dragging follows the finger");
        memset(&ts, 0, sizeof ts); ts.released = true; now += 100; eq_panel_input(&P, &ts, NULL, now);
        CHECK(P.enh_drag == -1, "release ends the drag");
        memset(&ts, 0, sizeof ts); now += 300; eq_panel_input(&P, &ts, NULL, now);
        int on0 = P.enh.on;
        TAP(P.L.toggle);
        CHECK(P.enh.on == !on0, "touch on the toggle flips it");
        TAP(P.L.page);
        CHECK(P.page == EQP_PAGE_EQ, "touch on EQ goes back");
        TAP(P.L.cancel);
        CHECK(memcmp(P.gains, go, sizeof go) == 0 && memcmp(&P.enh, &o, sizeof o) == 0, "touch CANCEL restores both");

        /* store: both keys, every other key kept, and the file reads back */
        char path2[] = "/tmp/eq_panel_enh_test_XXXXXX";
        int fd2 = mkstemp(path2);
        CHECK(fd2 >= 0, "mkstemp enh");
        if (fd2 >= 0) {
            const char *seed = "audio_enabled=1\nled_brightness=40\neq=0,0,0,0,0\nenh=0,0,0,0,0,0,0\nzzz=keep\n";
            CHECK(write(fd2, seed, strlen(seed)) == (ssize_t)strlen(seed), "seed enh");
            close(fd2);
            CHECK(eq_panel_store_file_both(path2, go, &o) == 0, "store both");
            int back[5]; AudioEnh eb;
            CHECK(eq_config_read(path2, back) == 0 && memcmp(back, go, sizeof go) == 0, "eq key written");
            CHECK(enh_config_read(path2, &eb) == 0 && memcmp(&eb, &o, sizeof o) == 0, "enh key written and read back");
            Config c2; config_init_path(&c2, path2); config_load(&c2);
            CHECK(config_get_int(&c2, "led_brightness", -1) == 40 && config_get_int(&c2, "audio_enabled", -1) == 1 && !strcmp(config_get(&c2, "zzz", ""), "keep"), "other keys kept by the double store");
            CHECK(!strcmp(config_get(&c2, "enh", ""), "1,10,20,30,40,50,60"), "enh value text");
            CHECK(eq_panel_store_file_both(path2, eq_preset_gains(1), &zero) == 0 && enh_config_read(path2, &eb) == 0 && memcmp(&eb, &zero, sizeof zero) == 0, "a second store replaces enh");
            unlink(path2);
        }
    }
    printf("eq_panel_test: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
