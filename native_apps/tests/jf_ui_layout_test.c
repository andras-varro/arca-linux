/* Host regression for the Jellyfin screens' layout (jellyfin/jf_ui.c, jf_ui_layout):
 * the function the app draws and hit-tests with, run against the panel geometry.
 *
 *   A  every pressable rect (button rows, sign-in rows, sign-in buttons) lies inside
 *      SCREEN_SAFE_* at several touch insets;
 *   B  the lists lie inside SCREEN_VISIBLE_* (and, being tapped, inside SAFE_* too);
 *   C  the button rows do not overlap the list, and buttons within a row do not overlap;
 *   D  the drawn-only now-playing rects lie inside SCREEN_VISIBLE_*.
 */
#include <stdio.h>
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
    UiRect press[18];
    int n = 0;
    for (int i = 0; i < JF_UI_ROW3; i++) press[n++] = L.row3[i];
    for (int i = 0; i < JF_UI_ROW5; i++) press[n++] = L.row5[i];
    for (int i = 0; i < JF_UI_FIELDS; i++) press[n++] = L.field[i];
    press[n++] = L.signin; press[n++] = L.signexit;
    press[n++] = L.cat_prev; press[n++] = L.cat_next;
    for (int i = 0; i < n; i++)
        CHECK(inside(press[i], e.safe_l, e.safe_t, e.safe_r, e.safe_b),
              "A: inset %d/%d/%d/%d: button %d (%d,%d %dx%d) outside SAFE", inset_t, inset_b, inset_l, inset_r,
              i, press[i].x, press[i].y, press[i].w, press[i].h);
    CHECK(inside(L.list, e.vis_l, e.vis_t, e.vis_r, e.vis_b), "B: list outside VISIBLE (inset %d)", inset_t);
    CHECK(inside(L.list_full, e.vis_l, e.vis_t, e.vis_r, e.vis_b), "B: queue list outside VISIBLE (inset %d)", inset_t);
    CHECK(inside(L.list, e.safe_l, e.safe_t, e.safe_r, e.safe_b), "B: list outside SAFE (inset %d)", inset_t);
    CHECK(inside(L.list_full, e.safe_l, e.safe_t, e.safe_r, e.safe_b), "B: queue list outside SAFE (inset %d)", inset_t);
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
    CHECK(inside(L.progress, e.vis_l, e.vis_t, e.vis_r, e.vis_b), "D: progress outside VISIBLE");
    CHECK(inside(L.info, e.vis_l, e.vis_t, e.vis_r, e.vis_b), "D: info outside VISIBLE");
    CHECK(L.progress.y + L.progress.h + 30 <= L.row5[0].y, "D: progress and its label leave the button row clear");
}

int main(void)
{
    run(0, 0, 0, 0);
    run(17, 17, 3, 3);       /* the measured unit */
    run(30, 30, 10, 10);
    run(48, 48, 48, 48);     /* the largest inset the library allows */
    if (fails) { printf("jf_ui_layout_test: %d FAILED\n", fails); return 1; }
    printf("jf_ui_layout_test: all passed\n");
    return 0;
}
