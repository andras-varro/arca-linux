/*
 * ui_zone — pad / keyboard focus over a vertical stack of zones: lists and rows of buttons.
 *
 * A screen that is a list over a row of buttons (and under a top bar) used to read the frame's Up/Down in several
 * independent blocks, one per zone, so one press could leave the list and be taken again by the button row in the
 * same call.  Here one uz_input() call reads the direction ONCE and makes at most one focus move.
 *
 * The caller rebuilds the UiZone array every frame from what is PRESENT: an absent button is not listed, so it can
 * be neither focused nor counted.  Focus is held by the caller's button id, not by position, so a button that
 * appears or vanishes under the focus re-picks the nearest present one (ui_focus_nearest).
 *
 * The ListView keeps owning its cursor and marks; this file only reads ListView.count for the list's edges.  The
 * caller keeps the actions (id -> event) and the screen-specific keys (a Left/Right category switch, a volume key).
 * Pure: no framebuffer, so tests/ui_zone_test.c drives it.
 */
#ifndef UI_ZONE_H
#define UI_ZONE_H

#include <stdbool.h>
#include "ui_focus.h"    /* UiRect, UiDir */
#include "gamepad.h"     /* InputState, BTN_ID_* */
#include "list_view.h"   /* ListView */

#define UZ_ZONES    5
#define UZ_BUTTONS 12

#define UZ_TRAP 0x1      /* Up/Down that finds no neighbour inside this zone is reported (out.left), not moved: the action bar = dismiss */
#define UZ_SLIDER 0x2    /* cycle mode: Left/Right on this pane are reported (out.left_right = -1/+1), not moved: a seek bar */

/* One zone of the screen, top to bottom.  A zone with lv == NULL and n == 0 is absent and skipped. */
typedef struct {
    ListView *lv;               /* a list zone, or NULL for a button zone */
    int       n;                /* present buttons */
    UiRect    r[UZ_BUTTONS];    /* their rects (any layout: one row, wrapped rows; movement inside is spatial) */
    int       id[UZ_BUTTONS];   /* the caller's id for each, stable across frames (e.g. the slot index) */
    unsigned  flags;            /* UZ_* */
} UiZone;

typedef struct {
    int    zone;       /* index of the focused zone */
    int    id;         /* focused button's id in a button zone, -1 in a list zone or until first resolved */
    UiRect last;       /* its rect, to re-pick with ui_focus_nearest when the id vanished (w == 0: none yet) */
    bool   shown;      /* pad/keyboard used since the last touch: the ring and the list's yellow edge are drawn */
    bool   cycle;      /* cycle model (see uz_input): Tab/LB-RB change pane, arrows stay inside it.  Off after uz_begin; set it after. */
    int    mem[UZ_ZONES]; /* cycle mode: the id focused last time in each button zone, -1 none (set by uz_begin) */
} UiZoneFocus;

typedef struct {
    bool              moved;     /* zone, button or shown changed: redraw */
    int               activate;  /* id of the button ACTION/JUMP fired on, else -1 */
    int               left;      /* index of a UZ_TRAP zone just left by Up/Down, else -1 */
    const InputState *list_in;   /* hand to lv_input(): `in` when the focused list takes this frame's keys, else NULL */
    bool              back;      /* cycle mode: Esc / pad B / Backspace / Select pressed - the caller treats it as the top bar BACK */
    int               activate_row; /* cycle mode: cursor row of the focused list when ACTION/JUMP fired (the caller fires ACTIVATE), else -1 */
    int               left_right;/* cycle mode, UZ_SLIDER pane: -1 Left / +1 Right pressed, else 0 */
    InputState        list_buf;  /* cycle mode: the copy list_in points at (ACTION/JUMP/NEXT/PREV/CANCEL stripped); valid until the next call */
} UzOut;

/* Screen opened: focus `zone` (its first button, or the list); ring hidden until a key is used. */
void uz_begin(UiZoneFocus *f, int zone);
/* Same, in cycle mode (f->cycle = true): the model below.  An old-mode caller keeps uz_begin and its behaviour. */
void uz_begin_cycle(UiZoneFocus *f, int zone);
/* Put focus on (zone, id) - the action bar's rising edge.  Leaves `shown` alone. */
void uz_focus(UiZoneFocus *f, const UiZone *z, int nz, int zone, int id);
/* One frame.  At most ONE focus move per frame.  Rules:
 *  - a pressed direction or ACTION/JUMP sets f->shown; ts_pressed clears it and changes nothing else;
 *  - list zone: Up at cursor <= 0 goes to the nearest present zone above; Down at cursor >= count-1 (or count 0)
 *    to the one below; otherwise list_in = in (Left/Right are NOT consumed: the caller may use them);
 *  - button zone: Left/Right = ui_focus_move inside the zone (wraps); Up/Down = ui_focus_step inside the zone
 *    (wrapped rows), else the adjacent zone (a UZ_TRAP zone reports out.left instead); ACTION/JUMP = activate;
 *  - the focused id gone this frame: ui_focus_nearest(last); the zone empty: the nearest list zone, else the nearest.
 *
 * CYCLE MODE (f->cycle, via uz_begin_cycle): "like Tab on WinForms" - the focused pane decides what the arrows mean.
 *  - NEXT (Tab / pad RB) and PREV (Shift+Tab = NEXT with RUN held / pad LB) move to the next / previous PRESENT pane,
 *    wrapping, landing on the id last focused there (f->mem) or its first; one pane only: nothing happens;
 *  - arrows never leave the pane: list zone = list_in (Up/Down at the edge stay on the cursor); button zone = Left/Right
 *    ui_focus_move (WRAPS inside the row), Up/Down ui_focus_step between wrapped rows (no neighbour: stays; UZ_TRAP is
 *    an ordinary pane here); UZ_SLIDER pane = Left/Right reported as out.left_right, nothing moves;
 *  - ACTION or JUMP (Enter, Space, pad A or X): button pane -> out.activate = id; list pane -> out.activate_row = cursor
 *    (list_in then carries no ACTION/JUMP, so the list does not also fire; the caller acts on activate_row);
 *  - out.back = Esc / Backspace (PAUSE, BACK) or pad B (CANCEL) pressed, in any pane.  Touch behaves as in the old mode. */
void uz_input(UiZoneFocus *f, const UiZone *z, int nz, const InputState *in, bool ts_pressed, UzOut *out);
/* The rect to ring this frame: false in a list zone, when !shown, or when nothing is focused. */
bool uz_ring(const UiZoneFocus *f, const UiZone *z, int nz, UiRect *out);

#endif /* UI_ZONE_H */
