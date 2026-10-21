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
    P.zone = EQP_ZONE_BUTTONS; P.btn_focus = 1;
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
    printf("eq_panel_test: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
