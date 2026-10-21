/* Host-side regression for icon_grid_nav() / icon_grid_nav_exit() — the
 * keyboard/pad movement shared by app_launcher and the Control Panel home.
 *
 * Runs on the DEV MACHINE with native gcc.  Build and run:
 *
 *   cd native_apps && gcc -Wall -Wextra -Wno-unused-parameter -I common \
 *       -o build/icon_grid_nav_test tests/icon_grid_nav_test.c \
 *       common/icon_grid.c common/ppm.c common/common.c common/framebuffer.c common/font.c \
 *       common/touch_input.c common/hardware.c common/config.c \
 *       common/highscore.c common/keyboard.c common/audio.c common/audio_gen.c \
 *       common/audio_out.c common/audio_wav.c -lm && ./build/icon_grid_nav_test
 *
 * Group 1 pins the launcher's semantics: launcher_ref() below is the former
 * inline navigation with Left/Right made to wrap (last tile -> first, first ->
 * last), each branch otherwise guarded as it was, and the shared
 * function must agree with it for every count, cur, direction and column
 * count.  Group 2 spells the important cases out so a failure names them —
 * notably the Left/Right wrap at both ends.  Group 3 is the
 * Control Panel home's exit X.
 *
 * ⚠️ What it cannot see: the page following the selection, the ring and
 * the one-direction-per-frame pick — those are icon_grid_focus_frame()'s,
 * tested by icon_grid_focus_test.c — nor anything drawn.
 */
#include "icon_grid.h"
#include <stdio.h>

static int fails;

static void expect(const char *what, int got, int want) {
    if (got != want) {
        printf("  FAIL %-56s got %d want %d\n", what, got, want);
        fails++;
    }
}

/* The launcher's navigation as it moved into icon_grid.c, plus the wrap. */
static int launcher_ref(int cols, int count, int sel, UiDir d) {
    if (d == UI_DIR_RIGHT) { sel = (sel + 1 < count) ? sel + 1 : 0; }
    if (d == UI_DIR_LEFT)  { sel = (sel > 0) ? sel - 1 : count - 1; }
    if (d == UI_DIR_DOWN)  { int t = sel + cols; if (t < count) sel = t; }
    if (d == UI_DIR_UP)    { int t = sel - cols; if (t >= 0) sel = t; }
    return sel;
}

static IconGrid grid_of(int cols, int rows) {
    IconGrid g = { 0 };
    g.cols = cols; g.rows = rows; g.per_page = cols * rows;
    return g;
}

int main(void) {
    printf("group 1  agrees with the launcher's former inline navigation\n");
    {
        int bad = 0;
        static const int shapes[][2] = { { 3, 2 }, { 2, 3 }, { 1, 1 }, { 4, 2 } };
        for (unsigned s = 0; s < sizeof(shapes) / sizeof(shapes[0]); s++) {
            IconGrid g = grid_of(shapes[s][0], shapes[s][1]);
            for (int count = 1; count <= 20; count++)
                for (int cur = 0; cur < count; cur++)
                    for (int d = UI_DIR_UP; d <= UI_DIR_RIGHT; d++) {
                        int got = icon_grid_nav(&g, count, cur, (UiDir)d);
                        int want = launcher_ref(g.cols, count, cur, (UiDir)d);
                        if (got != want) {
                            if (bad < 5)
                                printf("  FAIL cols=%d count=%d cur=%d dir=%d got %d want %d\n",
                                       g.cols, count, cur, d, got, want);
                            bad++;
                        }
                    }
        }
        if (bad) { printf("  (%d disagreements)\n", bad); fails += bad; }
    }

    printf("group 2  the cases by name (3x2 per page, 9 tiles: the Control Panel home)\n");
    {
        IconGrid g = grid_of(3, 2);
        expect("Right along the top row: 0 -> 1",          icon_grid_nav(&g, 9, 0, UI_DIR_RIGHT), 1);
        expect("Right off the top row: 2 -> 3 (bottom)",   icon_grid_nav(&g, 9, 2, UI_DIR_RIGHT), 3);
        expect("Right off page 1: 5 -> 6 (page 2)",        icon_grid_nav(&g, 9, 5, UI_DIR_RIGHT), 6);
        expect("Right on the last tile wraps: 8 -> 0",     icon_grid_nav(&g, 9, 8, UI_DIR_RIGHT), 0);
        expect("Left on the first tile wraps: 0 -> 8",     icon_grid_nav(&g, 9, 0, UI_DIR_LEFT), 8);
        expect("Left back onto page 1: 6 -> 5",            icon_grid_nav(&g, 9, 6, UI_DIR_LEFT), 5);
        expect("Down: 1 -> 4",                             icon_grid_nav(&g, 9, 1, UI_DIR_DOWN), 4);
        expect("Down off page 1's bottom row: 4 -> 7",     icon_grid_nav(&g, 9, 4, UI_DIR_DOWN), 7);
        expect("Down with no tile below stays: 7",         icon_grid_nav(&g, 9, 7, UI_DIR_DOWN), 7);
        expect("Down past count stays: 5 (8 is last)",     icon_grid_nav(&g, 9, 5, UI_DIR_DOWN), 8);
        expect("Up on the first row stays: 2",             icon_grid_nav(&g, 9, 2, UI_DIR_UP), 2);
        expect("Up onto page 1: 6 -> 3",                   icon_grid_nav(&g, 9, 6, UI_DIR_UP), 3);
        expect("cur -1 is returned unchanged",             icon_grid_nav(&g, 9, -1, UI_DIR_RIGHT), -1);
    }

    printf("group 3  the exit X above the grid (Control Panel home)\n");
    {
        IconGrid g = grid_of(3, 2);
        int from = -1;
        expect("Up from page 1 top row -> X",
               icon_grid_nav_exit(&g, 9, 0, 1, &from, UI_DIR_UP, false), ICON_GRID_NAV_EXIT);
        expect("  and remembers the tile", from, 1);
        expect("Down from the X -> that tile",
               icon_grid_nav_exit(&g, 9, 0, ICON_GRID_NAV_EXIT, &from, UI_DIR_DOWN, false), 1);
        expect("Left on the X stays",
               icon_grid_nav_exit(&g, 9, 0, ICON_GRID_NAV_EXIT, &from, UI_DIR_LEFT, false), ICON_GRID_NAV_EXIT);
        expect("Right on the X stays",
               icon_grid_nav_exit(&g, 9, 0, ICON_GRID_NAV_EXIT, &from, UI_DIR_RIGHT, false), ICON_GRID_NAV_EXIT);
        expect("Up on the X stays",
               icon_grid_nav_exit(&g, 9, 0, ICON_GRID_NAV_EXIT, &from, UI_DIR_UP, false), ICON_GRID_NAV_EXIT);
        expect("Up from page 2 top row (6) -> X, not page 1",
               icon_grid_nav_exit(&g, 9, 1, 6, &from, UI_DIR_UP, false), ICON_GRID_NAV_EXIT);
        expect("  remembers 6", from, 6);
        expect("Down from the X, page shown is 2 -> 6",
               icon_grid_nav_exit(&g, 9, 1, ICON_GRID_NAV_EXIT, &from, UI_DIR_DOWN, false), 6);
        expect("Down from the X, remembered tile on another page -> page start",
               icon_grid_nav_exit(&g, 9, 0, ICON_GRID_NAV_EXIT, &from, UI_DIR_DOWN, false), 0);
        from = -1;
        expect("Down from the X with nothing remembered -> page start",
               icon_grid_nav_exit(&g, 9, 1, ICON_GRID_NAV_EXIT, &from, UI_DIR_DOWN, false), 6);
        expect("Up from the bottom row is ordinary: 4 -> 1",
               icon_grid_nav_exit(&g, 9, 0, 4, &from, UI_DIR_UP, false), 1);
        expect("Right is the launcher's: 5 -> 6",
               icon_grid_nav_exit(&g, 9, 0, 5, &from, UI_DIR_RIGHT, false), 6);
    }

    printf("group 4  the bar's message beside the X (launcher)\n");
    {
        IconGrid g = grid_of(3, 2);
        int from = -1;
        expect("Left from the X -> message",
               icon_grid_nav_exit(&g, 9, 0, ICON_GRID_NAV_EXIT, &from, UI_DIR_LEFT, true), ICON_GRID_NAV_MSG);
        expect("Up from the X -> message",
               icon_grid_nav_exit(&g, 9, 0, ICON_GRID_NAV_EXIT, &from, UI_DIR_UP, true), ICON_GRID_NAV_MSG);
        expect("Right on the X stays (message present)",
               icon_grid_nav_exit(&g, 9, 0, ICON_GRID_NAV_EXIT, &from, UI_DIR_RIGHT, true), ICON_GRID_NAV_EXIT);
        expect("Down from the X still goes to the tile",
               icon_grid_nav_exit(&g, 9, 0, ICON_GRID_NAV_EXIT, &from, UI_DIR_DOWN, true), 0);
        expect("Right on the message -> X",
               icon_grid_nav_exit(&g, 9, 0, ICON_GRID_NAV_MSG, &from, UI_DIR_RIGHT, true), ICON_GRID_NAV_EXIT);
        expect("Down on the message -> X",
               icon_grid_nav_exit(&g, 9, 0, ICON_GRID_NAV_MSG, &from, UI_DIR_DOWN, true), ICON_GRID_NAV_EXIT);
        expect("Left on the message stays",
               icon_grid_nav_exit(&g, 9, 0, ICON_GRID_NAV_MSG, &from, UI_DIR_LEFT, true), ICON_GRID_NAV_MSG);
        expect("Up on the message stays",
               icon_grid_nav_exit(&g, 9, 0, ICON_GRID_NAV_MSG, &from, UI_DIR_UP, true), ICON_GRID_NAV_MSG);
        expect("Left on the X with no message stays",
               icon_grid_nav_exit(&g, 9, 0, ICON_GRID_NAV_EXIT, &from, UI_DIR_LEFT, false), ICON_GRID_NAV_EXIT);
        expect("Up on the X with no message stays",
               icon_grid_nav_exit(&g, 9, 0, ICON_GRID_NAV_EXIT, &from, UI_DIR_UP, false), ICON_GRID_NAV_EXIT);
        expect("a stale message selection with none showing acts as the X: Down -> tile",
               icon_grid_nav_exit(&g, 9, 0, ICON_GRID_NAV_MSG, &from, UI_DIR_DOWN, false), 0);
    }

    printf("\n%s (%d failure%s)\n", fails ? "REGRESSION" : "ALL PASS",
           fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
