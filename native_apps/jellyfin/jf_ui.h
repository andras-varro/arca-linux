/*
 * jf_ui — the Jellyfin client's screens: draw from JfApp, map touch / keyboard /
 * pad to user-intent events.  No policy: every decision is jf_core's.
 *
 * jf_ui_layout() is the one place the rectangles are computed; it is pure (the
 * screen extents are an argument), so tests/jf_ui_layout_test.c calls the very
 * function the app draws and hit-tests with.
 */
#ifndef ARCA_JF_UI_H
#define ARCA_JF_UI_H

#include <stdbool.h>
#include <stdint.h>
#include "framebuffer.h"
#include "common.h"
#include "touch_input.h"
#include "gamepad.h"
#include "list_view.h"
#include "top_bar.h"
#include "ui_focus.h"
#include "pointer.h"
#include "jf_core.h"

#define JF_UI_ROW3   3     /* browse screens: SEARCH, NOW PLAYING, PLAY ALL */
#define JF_UI_ROW5   5     /* now playing: NEXT, VOL-, VOL+, QUEUE, BACK */
#define JF_UI_FIELDS 3     /* sign-in: SERVER, USER, PASSWORD */
#define JF_UI_BTN_H  44
#define JF_UI_CAT_H  40     /* root category strip: arrows and name */
#define JF_UI_CAT_W  72     /* one arrow */

/* The screen the layout is taken against (SCREEN_* macros in the app). */
typedef struct {
    int vis_l, vis_t, vis_r, vis_b;
    int safe_l, safe_t, safe_r, safe_b;
} JfExtents;

typedef struct {
    int    bar_bottom;              /* top_bar_bottom(): the rule is drawn on this row */
    UiRect banner;                  /* notice / busy line under the bar */
    UiRect list;                    /* browse list (above the row) */
    UiRect list_full;               /* the queue list: no button row under it */
    UiRect list_root;               /* the browse list on the root screen: under the category strip */
    UiRect cat_prev, cat_next;      /* root category arrows, pressable */
    UiRect cat_label;               /* root category name and count, drawn only */
    UiRect row3[JF_UI_ROW3];        /* pressable */
    UiRect row5[JF_UI_ROW5];        /* pressable */
    UiRect field[JF_UI_FIELDS];     /* sign-in rows, pressable */
    UiRect signin, signexit;        /* sign-in buttons, pressable */
    UiRect progress;                /* now playing bar, drawn only */
    UiRect info;                    /* now playing text block, drawn only */
} JfLayout;

void jf_ui_layout(JfLayout *L, const JfExtents *e);
void jf_ui_extents_live(JfExtents *e);

typedef enum { JF_ZONE_LIST = 0, JF_ZONE_ROW, JF_ZONE_BAR } JfZone;

typedef struct {
    Framebuffer *fb;
    TouchInput *touch;
    GamepadManager *gm;
    const JfApp *app;
    JfLayout L;
    TopBar bar;
    ListView lv;
    Button row3[JF_UI_ROW3], row5[JF_UI_ROW5];
    Button field[JF_UI_FIELDS], signin, signexit;
    ModalDialog dlg;
    bool dlg_up;
    unsigned dlg_buttons;
    JfScreen last_screen;
    JfCat last_cat;
    Button cat_btn[2];
    bool lv_ready;
    char fields[JF_UI_FIELDS][64];       /* server, user, password (wiped after the POST) */
    int  focus;                          /* index in the zone (sign-in: 0..4) */
    JfZone zone;
    bool show_ring;                      /* a key / pad press shows it, a touch hides it */
    int  bar_focus;                      /* 0 back, 1 exit */
    int  played_ms;                      /* the loop's wall-clock count for the track */
    bool exit_press, back_press;
    char rowtxt[32][24];                 /* detail strings of the rows being drawn */
    int  rowtxt_i;
    bool dirty;
    unsigned seen_banner;
} JfUi;

void jf_ui_init(JfUi *ui, Framebuffer *fb, TouchInput *touch, GamepadManager *gm, const JfApp *app);
void jf_ui_free(JfUi *ui);

/** One frame of input.  Writes up to `max` intents to `out` and returns how many;
 *  may block in keyboard_enter() (then drains keys and pointer itself). */
int  jf_ui_input(JfUi *ui, const TouchState *ts, const InputState *in, Pointer *ptr,
                 uint32_t now, JfEvent *out, int max);

void jf_ui_draw(JfUi *ui);
void jf_ui_set_played(JfUi *ui, int played_ms);
void jf_ui_mark_dirty(JfUi *ui);
/** True once when something wants a repaint (state change, button, bar tick). */
bool jf_ui_take_dirty(JfUi *ui);

#endif /* ARCA_JF_UI_H */
