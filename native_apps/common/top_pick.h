/*
 * top_pick — a drop-down on the top bar's title: the header names the current
 * choice, a tap on it (or Shift / the action button while it is focused) opens
 * a popup list under it, and the popup picks one entry.
 *
 * The popup is a list_view, so it SCROLLS when the entries do not fit under the
 * header.  While it is up it owns every input: Up / Down and a tap move the
 * cursor, ACTION / Enter / Space / a tap on the cursor row pick, Back, Start,
 * Shift and a tap outside close.  The caller owns the DATA (a count and a name
 * per entry) and the CHOICE (tp.sel, which the popup opens on); the widget owns
 * the open / close state and the cursor.
 *
 * The geometry (tp_head_rect, tp_popup_rect) and the input (tp_head_input,
 * tp_open, tp_input) are pure; the drawing is the last section.  Host test:
 * tests/top_pick_test.c.  Users: the Jellyfin root header, the Control Panel's
 * Log page (through the CpPage header hook).
 */
#ifndef TOP_PICK_H
#define TOP_PICK_H

#include <stdbool.h>
#include "list_view.h"

#define TP_ARROW  14    /* the drop-down glyph, a square box */
#define TP_GAP     8    /* between the title, the arrow and the edge of the header target */
#define TP_EXTRA  (TP_GAP + TP_ARROW + TP_GAP)   /* TopBar.title_extra: the arrow plus a gap each side, so it clears the info text */
#define TP_W     260    /* the popup's width (narrower when the screen is) */
#define TP_ROW_H  40

/* The pressable screen: the popup stays inside it, the header target is clamped to it. */
typedef struct { int safe_l, safe_t, safe_r, safe_b; } TpExt;

typedef struct {
    int         (*count)(void *ctx);
    const char *(*name)(void *ctx, int index);
    void         *ctx;
} TpSource;

typedef struct {
    TpSource src;
    ListView lv;
    bool     up;
    bool     head_press;   /* a press began on the header */
    int      sel;          /* the current choice: the cursor starts here when the popup opens */
    int      picked;       /* the entry a TP_PICKED chose until tp_take(); -1 none */
} TopPick;

typedef enum {
    TP_NONE,
    TP_CHANGED,   /* the popup moved or scrolled: redraw */
    TP_OPENED,
    TP_PICKED,    /* tp->picked holds the entry; the popup is closed */
    TP_CLOSED     /* closed without a pick */
} TpEvent;

void tp_init(TopPick *tp, TpSource src);
void tp_free(TopPick *tp);

/* The pressable header for a title drawn at title_x / title_w on a bar spanning top..bottom, with room for the
 * arrow when `arrow`; clamped to the pressable screen. */
UiRect tp_head_rect(int title_x, int title_w, int top, int bottom, const TpExt *e, bool arrow);

/* The popup under `head`: TP_W wide (or the screen's width), as many whole rows as the count and the room under the
 * header allow, at least one. */
UiRect tp_popup_rect(UiRect head, int count, const TpExt *e);

/* The closed header's input: a press and release on `head` opens, so does Shift (BTN_ID_RUN).  Returns TP_OPENED
 * or TP_NONE; `head` of zero width never opens by touch. */
TpEvent tp_head_input(TopPick *tp, UiRect head, const TpExt *e, const TouchState *ts, const InputState *in);

/* Open the popup under `head` with the cursor on tp->sel. */
void tp_open(TopPick *tp, UiRect head, const TpExt *e);
void tp_close(TopPick *tp);

/* One frame of input while the popup is up. */
TpEvent tp_input(TopPick *tp, const TouchState *ts, const InputState *in, uint32_t now_ms);

/* The picked entry, once: -1 when there is none. */
int tp_take(TopPick *tp);

/* ── Drawing ────────────────────────────────────────────────────────────── */

/* The drop-down arrow after a title of width title_w drawn at title_x on a bar top..top+h. */
void tp_draw_arrow(Framebuffer *fb, int title_x, int title_w, int top, int h);
/* The popup, over everything else; nothing when it is closed. */
void tp_draw(Framebuffer *fb, TopPick *tp);

#endif
