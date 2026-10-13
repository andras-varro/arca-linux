/*
 * top_bar — the one bar across the top of the launcher and the Control Panel.
 *
 * Five sections, left to right, each optional but the info:
 *   [back]  [message]  [      title      ]  [CPU % / clock]  [exit X]
 * The message is a tap target (the launcher's NOT CALIBRATED, which opens the
 * page that fixes it); the right-hand info is display only.  Games do not
 * carry it: they are immersive.
 *
 * ⚠️ The bar starts at SCREEN_VISIBLE_TOP and DELIBERATELY reaches into the band
 * the digitizer cannot press.  Its buttons are full-bar-height hit targets, so
 * a finger on the touchable part of the bar counts, and text is centred in the
 * WHOLE bar so it never looks pushed down.  What keeps that honest on any unit
 * is top_bar_bottom(): the bar is tall enough that at least
 * TOP_BAR_MIN_TOUCH_H rows of it are pressable however large the per-unit
 * touch inset is.  Everything here is derived from SCREEN_VISIBLE_* and
 * SCREEN_SAFE_*, so it is right only after fb_init() AND touch_init(), and a
 * layout taken from it must be re-run after any fb_set_bezel().
 *
 * The geometry (top_bar_bottom, top_bar_compute, top_bar_hit) is pure and has a
 * host test; the drawing and the /proc reads are the callers' loop.
 */
#ifndef TOP_BAR_H
#define TOP_BAR_H

#include "framebuffer.h"
#include "common.h"
#include "control_panel/cpu_load.h"
#include <stdbool.h>
#include <stdint.h>

/* The bar is at least this tall, and at least this much of it is below
 * SCREEN_SAFE_TOP.  40 is the height of the buttons it replaces (BACK and the
 * exit X were 40) and the repo's minimum comfortable touch target; 44 is the
 * old page bar.  Together they mean the Control Panel's content never starts
 * lower than it did under that bar, at any inset. */
#define TOP_BAR_MIN_H        44
#define TOP_BAR_MIN_TOUCH_H  40
#define TOP_BAR_BTN_W        55     /* back and exit X: the width they always had */
#define TOP_BAR_PAD          8      /* inside a text target, and between sections */
#define TOP_BAR_RING_INSET   3      /* focus ring sits inside the target, clear of the screen edge */
#define TOP_BAR_RING_RADIUS  4      /* the bar's targets are near-square-cornered; the tile radius makes an oval */
/* back and exit X are DRAWN this far inside their (full-bar-height, bezel-reaching)
 * hit target on every side, so they sit in the bar with a margin instead of
 * filling it edge to edge.  The hit target and the Button's geometry are unchanged;
 * larger than TOP_BAR_RING_INSET so the focus ring still encloses the drawn button. */
#define TOP_BAR_BTN_INSET    6

/* The row the bar ends on (exclusive: the 1 px rule is drawn ON it).  The larger
 * of "44 px tall" and "TOP_BAR_MIN_TOUCH_H rows below the touchable edge". */
int top_bar_bottom(int visible_top, int safe_top);

#define TOP_BAR_BOTTOM       top_bar_bottom(SCREEN_VISIBLE_TOP, SCREEN_SAFE_TOP)
/* What an icon grid reserves under SCREEN_SAFE_TOP (icon_grid_layout's
 * top_reserve): the bar's touchable part, its rule, and a gap to the tiles. */
#define TOP_BAR_GRID_GAP     8
#define TOP_BAR_GRID_RESERVE (TOP_BAR_BOTTOM - SCREEN_SAFE_TOP + TOP_BAR_GRID_GAP)

typedef struct { int x, w; } TbSpan;            /* w == 0: not there */

/* Where each section sits.  Rows are common: top .. bottom for every target. */
typedef struct {
    int    left, right;       /* what the bar fills: the visible width */
    int    top, bottom, h;    /* h = bottom - top; the rule is row `bottom` */
    TbSpan back, exit;        /* hit targets, flush with the touchable edges */
    TbSpan msg;               /* hit target; its text starts at x + TOP_BAR_PAD */
    int    msg_chars;         /* characters of the message drawn; -1 = all */
    TbSpan info;              /* the right-hand text; display only */
    int    title_x, title_w;  /* the drawn text; title_scale 0 = not drawn */
    int    title_scale;
    int    title_chars;       /* characters kept before "..", -1 = all */
} TopBarLayout;

/* The screen the layout is taken against (SCREEN_* macros in production). */
typedef struct {
    int vis_left, vis_right;      /* the bar's fill */
    int safe_left, safe_right;    /* the pressable horizontal extent */
    int top, bottom;
} TbExtents;

typedef struct {
    /* content: the caller sets these whenever they change */
    const char *title;            /* centred; NULL = none */
    uint32_t    title_color;
    int         title_scale;      /* largest scale tried (3, 2 or 1); 0 = 3 */
    bool        title_cut;        /* may be cut with ".." (a status line); else dropped */
    const char *message;          /* tap target, left of the title; NULL = none */
    uint32_t    message_color;
    const char *right;            /* display only; set by top_bar_info_sample */
    bool        has_back, has_exit;

    /* derived */
    TopBarLayout L;
    Button       back, exit;      /* geometry follows L; drive with button_update */
    char         right_buf[24];
    CpuSnap      cpu_prev;
    uint32_t     info_next_ms;
} TopBar;

/* Zero the bar; back/exit as asked.  Buttons get their look here. */
void top_bar_init(TopBar *t, bool has_back, bool has_exit);

/* Pure: lay the sections out against `e`.  A title that cannot fit at its
 * largest scale tries smaller ones, then is cut with ".." and, with no room for
 * even three characters, is not drawn. */
void top_bar_compute(TopBarLayout *L, const TopBar *t, const TbExtents *e);

/* top_bar_compute() against the live screen; also moves back/exit. */
void top_bar_layout(TopBar *t);

/* What a point lands on.  Any row top..bottom-1, including the part above
 * SCREEN_SAFE_TOP that no finger can reach. */
typedef enum { TB_NONE = 0, TB_BACK, TB_MSG, TB_EXIT } TbHit;
TbHit top_bar_hit(const TopBarLayout *L, int x, int y);

/* Paint the bar (lays out first).  The focus ring is top_bar_draw_focus(). */
void top_bar_draw(Framebuffer *fb, TopBar *t);

/* The ring rect for a target, inset so the ring stays inside the screen. */
UiRect top_bar_ring_rect(const TopBarLayout *L, TbHit which);

/* The keyboard-focus ring on one target (TB_BACK, TB_MSG or TB_EXIT) in the
 * bar's own corner radius; nothing for TB_NONE or a target the bar lacks.
 * The one drawer: callers pick the target, never the look. */
void top_bar_draw_focus(Framebuffer *fb, const TopBarLayout *L, TbHit which);

/* The rectangle a back/exit button is drawn in: its hit target inset by
 * TOP_BAR_BTN_INSET on every side (pure; `which` is TB_BACK or TB_EXIT, else
 * all zero).  top_bar_hit() still answers for the whole target. */
UiRect top_bar_button_draw_rect(const TopBarLayout *L, TbHit which);

/* Print one receipt line saying how much of the bar is pressable. */
void top_bar_receipt(const TopBar *t, const char *tag);

/* The right-hand text, "CPU 7%  14:05".  top_bar_info_due() is true once a
 * second (and on the first call); top_bar_info_sample() then reads /proc and
 * the clock, and says whether the text changed — redraw only on true. */
bool top_bar_info_due(TopBar *t, uint32_t now_ms);
bool top_bar_info_sample(TopBar *t);
/* Make the next top_bar_info_due() true (after a child process returned). */
void top_bar_info_invalidate(TopBar *t);

#endif
