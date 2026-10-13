/* Host-side regression for the home-grid focus model (icon_grid_focus_*) —
 * the ring, the selection and what Enter activates on a paged tile grid,
 * shared by app_launcher and the Control Panel home — and for the
 * ModalDialog button focus both of them use in their dialogs.
 *
 * Runs on the DEV MACHINE with native gcc.  Build and run:
 *
 *   cd native_apps && gcc -Wall -Wextra -Wno-unused-parameter -I common \
 *       -o build/icon_grid_focus_test tests/icon_grid_focus_test.c \
 *       common/icon_grid.c common/ppm.c common/common.c common/framebuffer.c \
 *       common/touch_input.c common/hardware.c common/config.c \
 *       common/highscore.c common/keyboard.c common/audio.c common/audio_gen.c \
 *       common/audio_out.c common/audio_wav.c -lm && ./build/icon_grid_focus_test
 *
 * Group 1 is the ring: hidden by a real press, the selection kept, the first
 * key revealing without moving.  Group 2 is the page invariant: whatever
 * flipped the page, the selection is re-anchored onto it, so Enter can never
 * act on a tile that is not shown.  Group 3 is Enter, group 4 the return to
 * home, group 5 the one-direction-per-frame pick, group 6 the bar's message
 * as a stop beside the exit X, group 7 the dialog focus.
 *
 * ⚠️ What it cannot see: whether each app feeds these functions the REAL
 * press (not the synthetic tap) and the page its touch handler just flipped,
 * nor anything drawn.  Movement itself is icon_grid_nav_test.c's.
 */
#include "icon_grid.h"
#include "common.h"
#include <stdio.h>

static int fails;

static void expect(const char *what, int got, int want) {
    if (got != want) {
        printf("  FAIL %-56s got %d want %d\n", what, got, want);
        fails++;
    }
}

static IconGrid grid_3x2(void) {
    IconGrid g = { 0 };
    g.cols = 3; g.rows = 2; g.per_page = 6;
    return g;
}

#define NONE (-1)

int main(void) {
    IconGrid g = grid_3x2();
    IconGridFocus f;
    int page, act;

    printf("group 1  the ring: hidden by touch, revealed by the first key\n");
    icon_grid_focus_init(&f);
    expect("init: nothing selected", f.sel, -1);
    expect("init: ring hidden", f.shown, 0);
    page = 0;
    act = icon_grid_focus_frame(&g, 9, &page, &f, false, NONE, false, false);
    expect("a quiet frame activates nothing", act, -1);
    expect("  and shows nothing", f.shown, 0);
    act = icon_grid_focus_frame(&g, 9, &page, &f, false, UI_DIR_RIGHT, false, false);
    expect("first key reveals the ring", f.shown, 1);
    expect("  on the shown page's first tile, not moved", f.sel, 0);
    icon_grid_focus_frame(&g, 9, &page, &f, false, UI_DIR_RIGHT, false, false);
    expect("second key moves: 0 -> 1", f.sel, 1);
    icon_grid_focus_frame(&g, 9, &page, &f, true, NONE, false, false);
    expect("a real press hides the ring", f.shown, 0);
    expect("  and keeps the selection", f.sel, 1);
    icon_grid_focus_frame(&g, 9, &page, &f, false, UI_DIR_DOWN, false, false);
    expect("next key reveals again", f.shown, 1);
    expect("  without moving", f.sel, 1);
    icon_grid_focus_init(&f);
    page = 1;
    icon_grid_focus_frame(&g, 9, &page, &f, false, UI_DIR_LEFT, false, false);
    expect("first key on page 2 lands on its first tile", f.sel, 6);

    printf("group 2  the selection follows the shown page\n");
    icon_grid_focus_init(&f);
    page = 0;
    icon_grid_focus_frame(&g, 9, &page, &f, false, UI_DIR_RIGHT, false, false);
    icon_grid_focus_frame(&g, 9, &page, &f, false, UI_DIR_RIGHT, false, false);
    expect("set-up: tile 1 selected", f.sel, 1);
    page = 1;                                  /* the touch handler flipped it */
    icon_grid_focus_frame(&g, 9, &page, &f, true, NONE, false, false);
    expect("flip re-anchors onto page 2, same slot", f.sel, 7);
    expect("  the page stays where touch put it", page, 1);
    icon_grid_focus_init(&f);
    page = 0;
    icon_grid_focus_frame(&g, 9, &page, &f, false, UI_DIR_RIGHT, false, false);
    for (int i = 0; i < 5; i++)
        icon_grid_focus_frame(&g, 9, &page, &f, false, UI_DIR_RIGHT, false, false);
    expect("set-up: tile 5 selected", f.sel, 5);
    page = 1;
    act = icon_grid_focus_frame(&g, 9, &page, &f, false, NONE, true, false);
    expect("flip clamps slot 5 to page 2's last tile", f.sel, 8);
    expect("  and Enter acts on THAT, the shown tile", act, 8);
    icon_grid_focus_frame(&g, 9, &page, &f, false, UI_DIR_RIGHT, false, false);
    expect("Right off the last tile wraps to 0", f.sel, 0);
    expect("  and the page follows the selection", page, 0);
    icon_grid_focus_frame(&g, 7, &page, &f, false, UI_DIR_LEFT, false, false);
    expect("Left from 0 wraps to the last of 7", f.sel, 6);
    expect("  page follows", page, 1);
    icon_grid_focus_frame(&g, 4, &page, &f, false, NONE, false, false);
    expect("list shrank under the page: page clamps to the last", page, 0);
    expect("  and the selection to the last item", f.sel, 3);

    printf("group 3  Enter\n");
    icon_grid_focus_init(&f);
    page = 0;
    act = icon_grid_focus_frame(&g, 9, &page, &f, false, NONE, true, false);
    expect("Enter with the ring hidden activates nothing", act, -1);
    expect("  but reveals it", f.shown, 1);
    expect("  on the first tile", f.sel, 0);
    act = icon_grid_focus_frame(&g, 9, &page, &f, false, NONE, true, false);
    expect("Enter with the ring shown activates its tile", act, 0);
    icon_grid_focus_frame(&g, 9, &page, &f, true, NONE, false, false);
    act = icon_grid_focus_frame(&g, 9, &page, &f, false, NONE, true, false);
    expect("Enter after a touch hid the ring activates nothing", act, -1);
    icon_grid_focus_init(&f);
    act = icon_grid_focus_frame(&g, 9, &page, &f, false, UI_DIR_UP, true, false);
    expect("key + Enter in the revealing frame activates nothing", act, -1);
    icon_grid_focus_frame(&g, 9, &page, &f, false, UI_DIR_UP, false, false);
    expect("Up from the top row reaches the X", f.sel, ICON_GRID_NAV_EXIT);
    act = icon_grid_focus_frame(&g, 9, &page, &f, false, NONE, true, false);
    expect("Enter on the X reports the X", act, ICON_GRID_NAV_EXIT);
    icon_grid_focus_frame(&g, 9, &page, &f, false, UI_DIR_DOWN, false, false);
    expect("Down from the X returns to the tile", f.sel, 0);
    icon_grid_focus_init(&f);
    page = 0;
    icon_grid_focus_frame(&g, 0, &page, &f, false, UI_DIR_DOWN, false, false);
    expect("no items: the first key lands on the X", f.sel, ICON_GRID_NAV_EXIT);
    icon_grid_focus_frame(&g, 0, &page, &f, false, UI_DIR_DOWN, false, false);
    expect("  and Down cannot leave it for a tile that is not there",
           f.sel, ICON_GRID_NAV_EXIT);

    printf("group 4  returning home lands on the tile that was used\n");
    icon_grid_focus_init(&f);
    page = 0;
    icon_grid_focus_land(&g, &f, &page, 7);    /* tapped tile 7 */
    expect("land: selection is the used tile", f.sel, 7);
    expect("  its page is shown", page, 1);
    expect("  a tap leaves the ring hidden", f.shown, 0);
    icon_grid_focus_frame(&g, 9, &page, &f, false, UI_DIR_RIGHT, false, false);
    expect("first key reveals ON the used tile", f.sel, 7);
    icon_grid_focus_land(&g, &f, &page, 2);    /* opened by Enter */
    expect("land after Enter: ring still shown", f.shown, 1);
    expect("  on the used tile", f.sel, 2);
    expect("  its page is shown", page, 0);

    printf("group 5  one direction per frame, Up > Down > Left > Right\n");
    expect("all four -> Up",   icon_grid_focus_dir(true, true, true, true), UI_DIR_UP);
    expect("D L R -> Down",    icon_grid_focus_dir(false, true, true, true), UI_DIR_DOWN);
    expect("L R -> Left",      icon_grid_focus_dir(false, false, true, true), UI_DIR_LEFT);
    expect("R -> Right",       icon_grid_focus_dir(false, false, false, true), UI_DIR_RIGHT);
    expect("none -> -1",       icon_grid_focus_dir(false, false, false, false), -1);

    printf("group 6  the bar's message is a stop beside the X\n");
    {
        icon_grid_focus_init(&f);
        page = 0;
        icon_grid_focus_frame(&g, 9, &page, &f, false, UI_DIR_RIGHT, false, true);  /* reveal */
        icon_grid_focus_frame(&g, 9, &page, &f, false, UI_DIR_UP, false, true);
        expect("Up from the top row reaches the X (message present)", f.sel, ICON_GRID_NAV_EXIT);
        icon_grid_focus_frame(&g, 9, &page, &f, false, UI_DIR_LEFT, false, true);
        expect("Left from the X reaches the message", f.sel, ICON_GRID_NAV_MSG);
        act = icon_grid_focus_frame(&g, 9, &page, &f, false, NONE, true, true);
        expect("Enter on the message reports it", act, ICON_GRID_NAV_MSG);
        icon_grid_focus_frame(&g, 9, &page, &f, false, UI_DIR_LEFT, false, true);
        expect("Left on the message stays", f.sel, ICON_GRID_NAV_MSG);
        icon_grid_focus_frame(&g, 9, &page, &f, false, UI_DIR_UP, false, true);
        expect("Up on the message stays", f.sel, ICON_GRID_NAV_MSG);
        icon_grid_focus_frame(&g, 9, &page, &f, false, UI_DIR_RIGHT, false, true);
        expect("Right on the message returns to the X", f.sel, ICON_GRID_NAV_EXIT);
        icon_grid_focus_frame(&g, 9, &page, &f, false, UI_DIR_UP, false, true);
        expect("Up from the X reaches the message too", f.sel, ICON_GRID_NAV_MSG);
        icon_grid_focus_frame(&g, 9, &page, &f, false, UI_DIR_DOWN, false, true);
        expect("Down on the message returns to the X", f.sel, ICON_GRID_NAV_EXIT);
        icon_grid_focus_frame(&g, 9, &page, &f, false, UI_DIR_DOWN, false, true);
        expect("  and Down from the X returns to the tile", f.sel, 0);

        icon_grid_focus_frame(&g, 9, &page, &f, false, UI_DIR_UP, false, true);
        icon_grid_focus_frame(&g, 9, &page, &f, false, UI_DIR_LEFT, false, true);
        expect("(back on the message)", f.sel, ICON_GRID_NAV_MSG);
        act = icon_grid_focus_frame(&g, 9, &page, &f, false, NONE, false, false);
        expect("message gone while selected: falls back to the X", f.sel, ICON_GRID_NAV_EXIT);
        expect("  ring state kept", f.shown, 1);
        expect("  and nothing activated", act, -1);

        icon_grid_focus_init(&f);
        page = 0;
        icon_grid_focus_frame(&g, 9, &page, &f, false, UI_DIR_RIGHT, false, false);
        icon_grid_focus_frame(&g, 9, &page, &f, false, UI_DIR_UP, false, false);
        icon_grid_focus_frame(&g, 9, &page, &f, false, UI_DIR_LEFT, false, false);
        expect("no message: Left on the X stays", f.sel, ICON_GRID_NAV_EXIT);
        icon_grid_focus_frame(&g, 9, &page, &f, false, UI_DIR_UP, false, false);
        expect("no message: Up on the X stays", f.sel, ICON_GRID_NAV_EXIT);
    }

    printf("group 7  ModalDialog button focus\n");
    {
        ModalDialog d;
        modal_dialog_init(&d, "T", NULL, 3);
        expect("init: no focus (games' dialogs unchanged)", d.focus, -1);
        modal_dialog_focus_step(&d, UI_DIR_DOWN);
        expect("a step with no focus does nothing", d.focus, -1);
        modal_dialog_set_focus(&d, 2);
        expect("set_focus", d.focus, 2);
        modal_dialog_focus_step(&d, UI_DIR_DOWN);
        expect("Down from the last wraps to 0", d.focus, 0);
        modal_dialog_focus_step(&d, UI_DIR_UP);
        expect("Up from 0 wraps to the last", d.focus, 2);
        modal_dialog_focus_step(&d, UI_DIR_LEFT);
        expect("Left = previous", d.focus, 1);
        modal_dialog_focus_step(&d, UI_DIR_RIGHT);
        expect("Right = next", d.focus, 2);
        modal_dialog_set_focus(&d, 7);
        expect("set_focus out of range is refused", d.focus, 2);
        modal_dialog_init_confirm(&d, "T", "M", "OK", 0, "CANCEL", 0);
        modal_dialog_set_focus(&d, 1);
        modal_dialog_focus_step(&d, UI_DIR_RIGHT);
        expect("2 buttons: Right from 1 wraps to 0", d.focus, 0);
        modal_dialog_focus_step(&d, UI_DIR_UP);
        expect("2 buttons: Up moves too", d.focus, 1);
    }

    printf("\n%s (%d failure%s)\n", fails ? "REGRESSION" : "ALL PASS",
           fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
