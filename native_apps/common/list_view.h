/*
 * list_view — a scrolling list with a cursor, marks and an active/inactive look.
 *
 * The caller owns the DATA and supplies it through LvSource (a count and a
 * per-row fill); the widget owns the VIEW: scroll position, cursor, multi-select
 * marks, drag state.  Used by a two-pane file manager (two lists side by side,
 * 40 px rows with a 32 px icon) and fits a 64 px album-art row as well.
 *
 * Rows scroll by WHOLE rows (no pixel offset), so nothing is ever cut by the
 * rect and no clip is needed; the up-to-one-row slack sits at the bottom.
 *
 * Marks are kept by INDEX in a bitmap the widget mallocs (lv_free releases it).
 * A re-count keeps the marks below min(old, new) count (an append or a trim
 * leaves them valid); after ANY other change to the source (re-sort, a new
 * directory, a new search) call lv_reset(), which clears marks, cursor and
 * scroll.  lv_sync() is the cheap per-frame re-count that does the former.
 *
 * Input: lv_input() takes the repo's TouchState and InputState; lv_key() is the
 * pure entry for keys InputState has no button for (PgUp/PgDn/Home/End, mark).
 * Everything above the "Drawing" line is pure (no framebuffer), so a host test
 * drives it; tests/list_view_test.c.
 */
#ifndef LIST_VIEW_H
#define LIST_VIEW_H

#include <stdbool.h>
#include <stdint.h>
#include "framebuffer.h"
#include "touch_input.h"
#include "gamepad.h"
#include "ui_focus.h"   /* UiRect */

/* ── Data source ────────────────────────────────────────────────────────── */

/* NONE draws a placeholder box; BLANK draws nothing (the icon column stays empty). */
typedef enum { LV_GLYPH_NONE, LV_GLYPH_FILE, LV_GLYPH_FOLDER, LV_GLYPH_UP, LV_GLYPH_BLANK } LvGlyph;

/* A caller-owned XRGB8888 tile (a thumbnail, album art); drawn scaled to fit
 * the icon square.  Must outlive the draw call. */
typedef struct { const uint32_t *pixels; int w, h; } LvIcon;

#define LV_ROW_DIM 0x1   /* greyed text (an unreadable file, a disabled entry) */

typedef struct {
    const char   *title;    /* left; cut with "..." to fit; case kept */
    const char   *detail;   /* right-aligned, may be NULL or "" */
    const LvIcon *icon;     /* NULL: the glyph, or a placeholder box if that is NONE too */
    LvGlyph       glyph;
    unsigned      flags;    /* LV_ROW_* */
} LvRow;

typedef struct {
    int  (*count)(void *ctx);
    void (*row)(void *ctx, int index, LvRow *out);  /* out is zeroed first */
    void  *ctx;
} LvSource;

/* ── Pure arithmetic ────────────────────────────────────────────────────── */

#define LV_DRAG_DEADZONE 10   /* px a finger must travel before a press is a drag */
#define LV_BORDER         1
#define LV_BAR_W          6   /* scroll indicator column at the right edge */
#define LV_PICK_MS      400   /* a reorder-mode press held this long inside the dead zone picks the row up */

typedef enum {
    LV_KEY_UP, LV_KEY_DOWN, LV_KEY_PGUP, LV_KEY_PGDN, LV_KEY_HOME, LV_KEY_END,
    LV_KEY_MARK,        /* toggle the cursor row's mark */
    LV_KEY_MARK_NEXT,   /* toggle, then step down (the Commander Insert) */
    LV_KEY_MARK_ALL,
    LV_KEY_MARK_CLEAR
} LvKey;

typedef enum {
    LV_EV_NONE,
    LV_EV_CHANGED,    /* cursor, scroll or marks moved: redraw */
    LV_EV_TAP,        /* a tap selected a row (also fired on an inactive list) */
    LV_EV_ACTIVATE,   /* Enter / action button, or a second tap on the cursor row */
    LV_EV_REORDER     /* a held-and-dragged row was dropped (reorder mode only): lv->pick_from moves to lv->pick_to */
} LvEvent;

/* Whole rows that fit in a rect of height h (borders excluded). */
int lv_vis_rows(int rect_h, int row_h);
/* Largest valid top for count rows with vis visible. */
int lv_max_top(int count, int vis);
int lv_clamp_top(int top, int count, int vis);
/* -1 when count <= 0, else cur clamped into 0..count-1 (cur < 0 becomes 0). */
int lv_clamp_cursor(int cur, int count);
/* Smallest scroll that shows `cursor`, from `top`. */
int lv_top_for_cursor(int top, int cursor, int count, int vis);
/* The cursor after a key (movement keys only; a mark key returns cur). */
int lv_cursor_after(int cur, int count, int vis, LvKey key);
/* Row index under local y (y measured from the top of the first row), or -1
 * past the visible rows or the end of the list. */
int lv_hit_row(int top, int y, int row_h, int vis, int count);
/* Scroll-indicator thumb inside a track of track_h px: offset and length.
 * length == track_h when everything fits (the caller then skips the bar). */
void lv_thumb(int count, int vis, int top, int track_h, int *off, int *len);

/* Reorder: the press is due to pick its row up (reorder on, still inside the dead zone, held LV_PICK_MS; wrap-safe). */
bool lv_pick_due(bool reorder, bool dragging, uint32_t down_ms, uint32_t now_ms);
/* The drop row for a picked row: the row under local y (from the top of the first row), clamped into the visible rows and the list. */
int  lv_drop_index(int top, int y, int row_h, int vis, int count);
/* Key auto-repeat: true once `since` is 400 ms old and `last` 80 ms (wrap-safe). */
bool lv_repeat_due(uint32_t since, uint32_t last, uint32_t now);
/* Cut src with "..." to max_w px at `scale`, case kept (text_truncate upcases,
 * which would rewrite file names).  Always NUL-terminated; size 0 writes nothing. */
void lv_fit(char *dest, int size, const char *src, int max_w, int scale);

/* Touch gesture: a press is a tap until the finger leaves LV_DRAG_DEADZONE. */
typedef struct {
    bool active;     /* a press began inside the list */
    bool dragging;   /* it has left the dead zone */
    int  down_y;     /* where it began */
    int  anchor_y;   /* the y the last whole-row step was taken from */
} LvDrag;

void lv_drag_begin(LvDrag *d, int y);
/* Feed the finger's y; returns the rows to ADD to top (dragging down shows
 * earlier rows, so it is negative then).  0 inside the dead zone. */
int  lv_drag_move(LvDrag *d, int y, int row_h);

/* Fling: momentum after a fast swipe.  Velocity is in milli-rows per second
 * (positive scrolls toward later rows), measured over the last
 * LV_FLING_WINDOW_MS of finger movement. */
#define LV_FLING_WINDOW_MS  100
#define LV_FLING_MIN_MS      20    /* a shorter span is too noisy to read */
#define LV_FLING_START   8000      /* below this a release is just a release */
#define LV_FLING_STOP    3000      /* the fling ends once it slows to this */
#define LV_FLING_MAX    60000
#define LV_FLING_DECAY_MS  800     /* v loses dt/LV_FLING_DECAY_MS of itself per step; travel is about v x this */
#define LV_FLING_HOLD_MS   100     /* a finger still this long before lifting flings nothing */
#define LV_FLING_STILL_PX    2     /* touch jitter that still counts as "not moving" */
#define LV_SAMPLES 8

typedef struct { uint32_t ms; int y; } LvSample;

/* Velocity from samples[0..n-1] (oldest first) over the window before the
 * finger stopped (a still tail up to LV_FLING_HOLD_MS is the release lag, not
 * a hold): 0 when below LV_FLING_START, the span is under LV_FLING_MIN_MS or
 * the finger held still longer; clamped to +-LV_FLING_MAX.  Finger moving up
 * (y falling) scrolls toward later rows. */
int lv_fling_velocity(const LvSample *samples, int n, int row_h);

/* ── The widget ─────────────────────────────────────────────────────────── */

typedef struct {
    LvSource src;
    UiRect   rect;       /* outer rect, border included */
    int      row_h;
    int      scale;      /* text scale; lv_init derives it from row_h, a caller may set it after */
    bool     active;     /* keys act only on the active list; touch always does */
    bool     ring_hidden;  /* the cursor row is drawn without the yellow pad edge even when active: the last input was a touch */
    bool     tap2_any;     /* a second tap on the cursor row is LV_EV_ACTIVATE even on an inactive list */
    int      top, cursor;
    bool     cursor_dragged;  /* a drag carried the cursor into view: the next tap on it is not a second tap */
    LvSample samples[LV_SAMPLES];  /* finger positions of the current press, oldest first */
    int      nsamples;
    int      fling_v;       /* milli-rows/s, 0 = no fling running */
    int      fling_acc;     /* milli-rows carried between frames */
    uint32_t fling_last;    /* when the fling was last advanced */
    bool     swallow_tap;   /* this press stopped a fling: its release is not a tap */
    int      count;      /* as of the last lv_sync() */
    LvDrag   drag;
    uint8_t *marks;      /* bitmap, malloc'd, owned */
    int      marks_cap;  /* bits */
    int      held_key;   /* auto-repeat: LvKey held, or -1 */
    uint32_t held_since, held_last;
    bool     reorder;    /* opt-in (default off): a press held LV_PICK_MS without leaving the dead zone picks a row up; its drop reports LV_EV_REORDER */
    bool     picked;     /* a row is held by the finger */
    int      pick_from, pick_to;  /* valid while picked, and for the LV_EV_REORDER it reports */
    uint32_t down_ms;    /* when the current press began */
} ListView;

/* scale 2 (14 px text) from 28 px rows up, else 1.  rows are whole; a rect
 * shorter than one row shows none. */
void lv_init(ListView *lv, LvSource src, UiRect rect, int row_h);
void lv_free(ListView *lv);
/* Clear marks, cursor 0, scroll 0; re-reads the count. */
void lv_reset(ListView *lv);
/* Re-read the count (call each frame, or after the source changed by append /
 * trim): keeps marks below the smaller count, re-clamps cursor and scroll. */
void lv_sync(ListView *lv);
/* Does row `index` get the yellow cursor edge: it is the cursor row of an active list and ring_hidden is clear. */
bool lv_row_edge_lit(const ListView *lv, int index);

int  lv_visible_rows(const ListView *lv);
void lv_move_to(ListView *lv, int index);   /* cursor to index, scrolled into view */

/* Marks.  lv_marked_next(from) is the first marked index >= from, or -1: loop
 * `for (i = lv_marked_next(lv,0); i >= 0; i = lv_marked_next(lv,i+1))`. */
bool lv_is_marked(const ListView *lv, int index);
int  lv_mark_count(const ListView *lv);
int  lv_marked_next(const ListView *lv, int from);

/* Returns LV_EV_CHANGED when something moved.  Mark keys work on any list. */
LvEvent lv_key(ListView *lv, LvKey key);

/* One frame of input.  Touch: press inside starts a gesture, a drag scrolls, a
 * release without a drag taps the row under the press (LV_EV_TAP; on a row
 * that already holds the cursor of an ACTIVE list, LV_EV_ACTIVATE).  Keys (only
 * when lv->active): UP / DOWN / PGUP / PGDN / HOME / END with auto-repeat,
 * ACTION = activate.  A drag released above LV_FLING_START keeps scrolling,
 * reporting LV_EV_CHANGED each step, until it decays, hits an end, or a press,
 * key or lv_move_to() stops it.  `in` may be
 * NULL.  now_ms is get_time_ms(). */
LvEvent lv_input(ListView *lv, const TouchState *ts, const InputState *in,
                 uint32_t now_ms);
/* True while a finger is down on the list or a fling runs: the caller's loop
 * must then run at its ACTIVE frame rate.  The fling velocity is read from the
 * frames lv_input saw, so a loop that idles at 100 ms while a finger moves
 * inside one row records too few samples and no swipe ever flings. */
bool lv_needs_frames(const ListView *lv);

/* ── Drawing ────────────────────────────────────────────────────────────── */

/* Border (bright when active), rows, scroll bar, or `empty_msg` (may be NULL)
 * centred when the list is empty.  Call lv_sync() first. */
void lv_draw(Framebuffer *fb, ListView *lv, const char *empty_msg);

#endif /* LIST_VIEW_H */
