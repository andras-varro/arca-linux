/*
 * icon_grid — the paged grid of icon tiles, shared by app_launcher and control_panel.
 *
 * One implementation of the layout, the tile, the page dots/chevrons and the hit-test,
 * so the launcher and any settings home screen cannot drift apart. The caller owns
 * its item list and its selection; this module only knows how many items a page
 * holds and where tile N of a page sits.
 *
 * Paging, not scrolling: 3x2 tiles per page in landscape, 2x3 in portrait. A tap in
 * the 50 px band inside either SCREEN_SAFE_* edge flips the page.
 *
 * Layout must be (re)computed after fb_init() AND touch_init() — the tiles are
 * hit-tested, so they live in SCREEN_SAFE_*, which is only right once the touch
 * inset is published — and again after any fb_set_bezel() or bpp change.
 */
#ifndef ICON_GRID_H
#define ICON_GRID_H

#include "framebuffer.h"
#include "ui_focus.h"
#include <stdbool.h>
#include <stdint.h>

#define ICON_GRID_ICON_SIZE 96      /* icons are drawn at exactly this size */
#define ICON_GRID_RING_RADIUS 12    /* a tile's corner radius, and its focus ring's */

typedef struct {
    int cols, rows, per_page;
    int tile_w, tile_h;
    int content_w, content_h;       /* the tile block, gaps included */
    int left, top;                  /* its top-left corner */
} IconGrid;

/* Derive the grid from the screen's current orientation and safe rectangle.
 * `top_reserve` is the band under SCREEN_SAFE_TOP kept for the top bar
 * (top_bar.h: TOP_BAR_GRID_RESERVE); the page dots take a fixed band at the
 * bottom. Prints one receipt line prefixed with `tag` saying whether the last
 * row lands inside the touchable rectangle. */
void icon_grid_layout(IconGrid *g, const Framebuffer *fb, int top_reserve,
                      const char *tag);

/* Pages needed for `count` items (at least 1). */
int  icon_grid_pages(const IconGrid *g, int count);

/* Top-left of tile `idx_on_page`. */
void icon_grid_tile_xy(const IconGrid *g, int idx_on_page, int *x, int *y);

/* One tile: the 96x96 `icon` if non-NULL, otherwise a `letter_color` square
 * carrying the label's first letter; the label centred underneath. */
void icon_grid_draw_tile(Framebuffer *fb, const IconGrid *g, int tx, int ty,
                         const char *label, const uint32_t *icon,
                         uint32_t letter_color, bool highlight);

/* The keyboard/gamepad selection ring around a tile. */
void icon_grid_draw_selection(Framebuffer *fb, const IconGrid *g, int tx, int ty);
/* The same ring around any rect — the one selection look, for keyboard focus
 * on widgets that are not tiles (control_panel's pages). */
void icon_grid_draw_ring(Framebuffer *fb, int x, int y, int w, int h);
/* The same ring with the target's own corner radius (capped at half its
 * height), so a square-cornered button gets a square-cornered ring. */
void icon_grid_draw_ring_r(Framebuffer *fb, int x, int y, int w, int h, int radius);

/* Page dots (nothing when there is one page) and the edge chevrons. */
void icon_grid_draw_paging(Framebuffer *fb, const IconGrid *g, int page, int pages);

/* Index on the page of the tile under (x,y), or -1. `count_on_page` is how many
 * tiles the page actually shows. */
int  icon_grid_hit(const IconGrid *g, int count_on_page, int x, int y);

/* -1 = flip to the previous page, +1 = next, 0 = not a page-flip tap. */
int  icon_grid_page_hit(int x, int page, int pages);

/* Keyboard/pad movement over ABSOLUTE item indices, the launcher's model:
 * Left/Right step -1/+1 in reading order (off the end of a row onto the next,
 * off the end of a page onto the next page, wrapping last <-> first); Up/Down
 * step -/+cols and do nothing past an edge.  The page is the caller's to
 * follow (idx / per_page).  cur < 0 = nothing selected: returned unchanged —
 * where the first key lands is the caller's choice. */
int  icon_grid_nav(const IconGrid *g, int count, int cur, UiDir d);

/* As icon_grid_nav, plus the exit X (top_bar.h) as a stop above the
 * grid: Up from the top row of `page` goes to ICON_GRID_NAV_EXIT and records
 * the tile in *from; Down from the X returns to *from when it is on `page`,
 * else to the page's first tile; any other key on the X stays there.
 *
 * has_msg: the bar's message (top_bar.h) is showing, and is a second stop in
 * the bar's row, LEFT of the X: Left or Up from the X goes to
 * ICON_GRID_NAV_MSG; on it Right or Down return to the X and Left/Up stay.
 * It is a parameter, not a field, so a caller cannot leave it stale; with it
 * false everything is as before, and a selection still on the MSG falls to the X. */
#define ICON_GRID_NAV_EXIT (-2)
#define ICON_GRID_NAV_MSG  (-3)
int  icon_grid_nav_exit(const IconGrid *g, int count, int page, int cur,
                        int *from, UiDir d, bool has_msg);

/* ── The home-grid focus model ───────────────────────────────────────────────
 * The keyboard/pad selection on a paged grid, ONE model for the launcher and
 * the Control Panel home.  Pure: the caller polls, draws and dispatches; this
 * only decides.  Rules, once per frame, in icon_grid_focus_frame():
 *   - a real touch press hides the ring and keeps the selection;
 *   - the selection is always on the shown page: a page flipped by touch (or
 *     a list that shrank) re-anchors it there, same slot clamped, so Enter can
 *     never act on a tile that is not on screen;
 *   - with the ring hidden, the first arrow (or Enter) reveals it and does
 *     nothing else; later arrows move it with icon_grid_nav_exit(), the page
 *     following the selection;
 *   - one direction per frame — pick it with icon_grid_focus_dir().
 * Returning to home after a tile was used: icon_grid_focus_land(). */
typedef struct {
    int  sel;        /* absolute item index, ICON_GRID_NAV_EXIT/_MSG, or -1 = none yet */
    bool shown;      /* the ring is drawn */
    int  exit_from;  /* the tile Up left for the X (icon_grid_nav_exit) */
} IconGridFocus;

void icon_grid_focus_init(IconGridFocus *f);

/* The one direction this frame from four press edges, Up > Down > Left >
 * Right; -1 when none.  An int, not a UiDir, so "none" is representable. */
int  icon_grid_focus_dir(bool up, bool down, bool left, bool right);

/* One frame.  *page is the page shown (already flipped by this frame's touch,
 * if any) and is moved when the selection leaves it or the list shrank under
 * it.  touch_press is a REAL finger's press edge — never a synthetic tap.
 * dir is icon_grid_focus_dir()'s.  has_msg: the bar's message is showing this
 * frame (see icon_grid_nav_exit); a selection on a message that has gone falls
 * back to the X, ring state kept.  Returns what Enter activates — an absolute
 * index, ICON_GRID_NAV_EXIT or ICON_GRID_NAV_MSG — and -1 for nothing; Enter
 * acts only on a ring that was already showing at the start of the frame. */
int  icon_grid_focus_frame(const IconGrid *g, int count, int *page,
                           IconGridFocus *f, bool touch_press, int dir,
                           bool enter, bool has_msg);

/* Item idx was used (tapped or Entered): select it and show its page.  The
 * ring keeps its state — hidden after a tap, shown after Enter — so coming
 * back to home puts the ring, when it is up, on the tile that was used. */
void icon_grid_focus_land(const IconGrid *g, IconGridFocus *f, int *page,
                          int idx);

/* Load a PPM icon, scaled to ICON_GRID_ICON_SIZE. NULL if it is missing or
 * unreadable (the tile then falls back to the letter). Free with free(). */
uint32_t *icon_grid_load_icon(const char *path);

/* A stable colour for a letter-tile, derived from the label. */
uint32_t icon_grid_letter_color(const char *label);

#endif
