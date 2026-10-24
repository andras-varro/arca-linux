/* Host regression for the video client's screen layout (jfvideo/jv_ui_layout.c, jv_ui_layout):
 * the function the app draws and hit-tests with, run against several extents.
 *
 *   A  every pressable rect of a screen (sign-in rows and buttons, home tiles, grid cells and scroll buttons,
 *      list, search rows, details text, track rows, action row, player BACK) lies inside SAFE;
 *   B  within one screen no two rects overlap;
 *   C  the drawn-only rects (banner, details poster, player info) lie inside VISIBLE; the poster does not
 *      overlap the details text;
 *   D  no rect of any screen has zero or negative size.
 *
 * Build (from native_apps/, one line; run under setarch -R):
 *   gcc -Wall -Wextra -Wno-unused-parameter -fsanitize=undefined,address -I. -I common -I jfvideo -I jf_shared -o build/jv_ui_layout_test tests/jv_ui_layout_test.c jfvideo/jv_ui_layout.c common/top_bar.c common/eq_panel.c common/icons.c common/jf_img.c common/logger.c common/ui_focus.c common/json.c common/http.c common/list_view.c common/top_pick.c common/ui_zone.c common/icon_grid.c common/ppm.c common/pointer.c common/status_line.c common/common.c common/framebuffer.c common/font.c common/touch_input.c common/hardware.c common/config.c common/highscore.c common/keyboard.c common/audio.c common/audio_gen.c common/audio_out.c common/audio_eq.c common/audio_enh.c common/audio_wav.c common/start_menu.c common/gamepad.c common/input_slots.c common/input_scan.c control_panel/cpu_load.c -lm -lssl -lcrypto
 *   (the link needs the whole common/ set: jv_ui_layout.c reads the SCREEN_* globals and top_bar.c calls button_init_full)
 */
#include <stdio.h>
#include <string.h>
#include "jv_ui.h"

static int checks, fails;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static bool inside(UiRect r, int l, int t, int rt, int b)
{
    return r.x >= l && r.y >= t && r.x + r.w <= rt && r.y + r.h <= b;
}

static bool disjoint(UiRect a, UiRect b)
{
    return a.x + a.w <= b.x || b.x + b.w <= a.x || a.y + a.h <= b.y || b.y + b.h <= a.y;
}

static const char *const N_FIELD[JV_UI_FIELDS] = { "field0", "field1", "field2" };
static const char *const N_TILE[JV_UI_TILES] = { "tile0", "tile1", "tile2", "tile3", "tile4", "tile5" };
static const char *const N_CELL[JV_UI_CELLS] = { "cell0", "cell1", "cell2", "cell3", "cell4",
                                                 "cell5", "cell6", "cell7", "cell8", "cell9" };
static const char *const N_TRK[JV_UI_TRK] = { "trk0", "trk1" };
static const char *const N_ACT[JV_UI_ACT] = { "act0", "act1", "act2", "act3" };

/* One screen's rects: `hit` ones are pressable (checked against SAFE), the others are drawn only (checked against VISIBLE). */
typedef struct { const char *name; UiRect r; } Item;
typedef struct {
    const char *screen;
    bool hit;
    Item it[32];
    int n;
} Group;

static void add(Group *g, const char *name, UiRect r)
{
    if (g->n >= (int)(sizeof g->it / sizeof g->it[0])) { fails++; printf("FAIL: group %s has too many rects\n", g->screen); return; }
    g->it[g->n].name = name;
    g->it[g->n].r = r;
    g->n++;
}

static void group_flush(const Group *g, const JvExtents *e, const char *tag)
{
    for (int i = 0; i < g->n; i++) {
        const Item *a = &g->it[i];
        CHECK(a->r.w > 0 && a->r.h > 0, "D: %s %s has no size (%dx%d) [%s]", g->screen, a->name, a->r.w, a->r.h, tag);
        if (g->hit)
            CHECK(inside(a->r, e->safe_l, e->safe_t, e->safe_r, e->safe_b),
                  "A: %s %s (%d,%d %dx%d) is outside the safe rect (%d,%d %dx%d) [%s]", g->screen, a->name,
                  a->r.x, a->r.y, a->r.w, a->r.h, e->safe_l, e->safe_t, e->safe_r - e->safe_l, e->safe_b - e->safe_t, tag);
        else
            CHECK(inside(a->r, e->vis_l, e->vis_t, e->vis_r, e->vis_b),
                  "C: %s %s (%d,%d %dx%d) is outside the visible rect (%d,%d %dx%d) [%s]", g->screen, a->name,
                  a->r.x, a->r.y, a->r.w, a->r.h, e->vis_l, e->vis_t, e->vis_r - e->vis_l, e->vis_b - e->vis_t, tag);
        for (int j = i + 1; j < g->n; j++) {
            const Item *b = &g->it[j];
            CHECK(disjoint(a->r, b->r),
                  "B: %s %s (%d,%d %dx%d) overlaps %s (%d,%d %dx%d) [%s]", g->screen, a->name,
                  a->r.x, a->r.y, a->r.w, a->r.h, b->name, b->r.x, b->r.y, b->r.w, b->r.h, tag);
        }
    }
}

static void run(const char *tag, const JvExtents *e)
{
    JvLayout L;
    jv_ui_layout(&L, e);
    Group g;

    memset(&g, 0, sizeof g); g.screen = "sign-in"; g.hit = true;
    for (int i = 0; i < JV_UI_FIELDS; i++) add(&g, N_FIELD[i], L.field[i]);
    add(&g, "signin", L.signin);
    add(&g, "reuse", L.reuse);
    add(&g, "signexit", L.signexit);
    group_flush(&g, e, tag);

    memset(&g, 0, sizeof g); g.screen = "home"; g.hit = true;
    for (int i = 0; i < JV_UI_TILES; i++) add(&g, N_TILE[i], L.tile[i]);
    group_flush(&g, e, tag);

    memset(&g, 0, sizeof g); g.screen = "grid"; g.hit = true;
    for (int i = 0; i < JV_UI_CELLS; i++) add(&g, N_CELL[i], L.cell[i]);
    add(&g, "gup", L.gup);
    add(&g, "gdn", L.gdn);
    group_flush(&g, e, tag);

    memset(&g, 0, sizeof g); g.screen = "list"; g.hit = true;
    add(&g, "list", L.list);
    group_flush(&g, e, tag);

    memset(&g, 0, sizeof g); g.screen = "search"; g.hit = true;
    add(&g, "list_s", L.list_s);
    add(&g, "sbtn", L.sbtn);
    group_flush(&g, e, tag);

    memset(&g, 0, sizeof g); g.screen = "details"; g.hit = true;
    add(&g, "text", L.text);
    for (int i = 0; i < JV_UI_TRK; i++) add(&g, N_TRK[i], L.trk[i]);
    for (int i = 0; i < JV_UI_ACT; i++) add(&g, N_ACT[i], L.act[i]);
    group_flush(&g, e, tag);

    memset(&g, 0, sizeof g); g.screen = "player"; g.hit = true;
    add(&g, "pback", L.pback);
    group_flush(&g, e, tag);

    /* drawn only: the bar's banner, the details poster (beside the text, not on it), the player's request text */
    memset(&g, 0, sizeof g); g.screen = "banner"; g.hit = false;
    add(&g, "banner", L.banner);
    group_flush(&g, e, tag);

    memset(&g, 0, sizeof g); g.screen = "details-drawn"; g.hit = false;
    add(&g, "poster", L.poster);
    group_flush(&g, e, tag);
    CHECK(disjoint(L.poster, L.text),
          "C: details poster (%d,%d %dx%d) overlaps the details text (%d,%d %dx%d) [%s]",
          L.poster.x, L.poster.y, L.poster.w, L.poster.h, L.text.x, L.text.y, L.text.w, L.text.h, tag);

    memset(&g, 0, sizeof g); g.screen = "player-drawn"; g.hit = false;
    add(&g, "pinfo", L.pinfo);
    group_flush(&g, e, tag);
}

int main(void)
{
    static const struct { const char *tag; JvExtents e; } ext[] = {
        { "no inset",          { 0, 0, 800, 480,  0,  0, 800, 480 } },
        { "inset 17/17/3/3",   { 0, 0, 800, 480,  3, 17, 797, 463 } },   /* the measured unit: t/b 17, l/r 3 */
        { "inset 30/30/10/10", { 0, 0, 800, 480, 10, 30, 790, 450 } },
        { "inset 48/48/48/48", { 0, 0, 800, 480, 48, 48, 752, 432 } },   /* the largest inset the library allows */
    };
    for (unsigned i = 0; i < sizeof ext / sizeof ext[0]; i++)
        run(ext[i].tag, &ext[i].e);

    if (fails) { printf("jv_ui_layout_test: %d of %d checks FAILED\n", fails, checks); return 1; }
    printf("jv_ui_layout_test: %d checks passed\n", checks);
    return 0;
}
