/*
 * jf_ui — the Jellyfin client's screens: draw from JfApp, map touch / keyboard /
 * pad to user-intent events.  No policy: every decision is jf_core's.
 * Screens: sign-in, root / browse lists, queue, now playing, and the MIX tick screen (JF_SCR_MIX).
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
#include "eq_panel.h"

#define JF_UI_ROW3   3     /* browse screens: SEARCH, NOW PLAYING, PLAY ALL */
#define JF_UI_ROW4   4     /* the root screen: previous category, SEARCH, NOW PLAYING, next category */
#define JF_UI_ART   120    /* now playing: the cover square, the height of the info block */
#define JF_UI_NP     10    /* now playing, one row (wraps in portrait): PREV, PLAY/PAUSE, STOP, NEXT, QUEUE, MIX, I-MIX, EQ, VOL +, VOL - */
#define JF_UI_NP_EQ  7     /* the EQ button's index in that row */
#define JF_UI_QROW  12     /* the queue view of now playing: PREV, PLAY/PAUSE, STOP, NEXT, SAVE, CLR, RND, UP, DN, EQ, VOL +, VOL - */
#define JF_UI_QROW_EQ 9    /* the EQ button's index in that row */
#define JF_UI_FIELDS 3     /* sign-in: SERVER, USER, PASSWORD */
#define JF_UI_ABAR   5     /* action bar: PLAY, PLAY NEXT, ADD QUEUE, MIX, INSTANT MIX */
#define JF_UI_MINI   3     /* mini player: PLAY/PAUSE, VOL -, VOL + */
#define JF_UI_MINI_H 40    /* the strip above the button row */
#define JF_UI_MIXBTN 2     /* MIX screen: START, CANCEL */
#define JF_UI_BTN_H  44

/* The screen the layout is taken against (SCREEN_* macros in the app). */
typedef struct {
    int vis_l, vis_t, vis_r, vis_b;
    int safe_l, safe_t, safe_r, safe_b;
} JfExtents;

typedef struct {
    int    bar_bottom;              /* top_bar_bottom(): the rule is drawn on this row */
    UiRect banner;                  /* notice / busy line under the bar */
    UiRect list;                    /* browse list and queue list (above the button row) */
    UiRect abar[JF_UI_ABAR];        /* the action bar: in the button row's place while it is up; pressable */
    UiRect qrow[JF_UI_QROW];        /* the queue screen's button row (one row, or the fewest equal rows stacked upward when narrow); pressable */
    UiRect list_q;                  /* the queue list: above that row, whatever its row count */
    int    qrow_rows;               /* how many rows the queue button row wrapped into */
    UiRect np[JF_UI_NP];            /* now playing buttons, in a row at the bottom (two rows when the width is too small); pressable */
    UiRect list_mini;               /* the list while the mini player strip is up (shorter, same top) */
    UiRect mini;                    /* mini player strip, drawn only (its parts are pressable) */
    UiRect mini_title;              /* tap: open NOW PLAYING; pressable */
    UiRect mini_btn[JF_UI_MINI];    /* PLAY/PAUSE, VOL -, VOL +; pressable */
    UiRect seek;                    /* now playing: the progress bar's touch area (taller than the bar); pressable */
    UiRect mixb[JF_UI_MIXBTN];      /* MIX screen: START, CANCEL in the button row; pressable (the tick rows are the list) */
    UiRect row3[JF_UI_ROW3];        /* pressable */
    UiRect row4[JF_UI_ROW4];        /* the root screen's button row; pressable */
    UiRect field[JF_UI_FIELDS];     /* sign-in rows, pressable */
    UiRect signin, signexit;        /* sign-in buttons, pressable */
    UiRect progress;                /* now playing bar, drawn only */
    UiRect info;                    /* now playing text block, drawn only */
} JfLayout;

void jf_ui_layout(JfLayout *L, const JfExtents *e);
void jf_ui_extents_live(JfExtents *e);

/* ABAR: the action bar owns focus while the core says it is up (Left / Right move, ENTER acts, Up / Down dismiss it). */
typedef enum { JF_ZONE_LIST = 0, JF_ZONE_ROW, JF_ZONE_BAR, JF_ZONE_ABAR } JfZone;

typedef struct {
    Framebuffer *fb;
    TouchInput *touch;
    GamepadManager *gm;
    const JfApp *app;
    JfLayout L;
    TopBar bar;
    ListView lv;
    Button row3[JF_UI_ROW3], row4[JF_UI_ROW4], np[JF_UI_NP], abar[JF_UI_ABAR], qrow[JF_UI_QROW], mini_b[JF_UI_MINI];
    ModalDialog cdlg;                    /* the INSTANT MIX confirmation */
    bool cdlg_up;
    bool mini_on;                        /* the strip is up (and the list rect is the shorter one) */
    bool seeking;                        /* a finger is on the progress bar */
    int  seek_ms;                        /* its target */
    Button mixb[JF_UI_MIXBTN];
    bool abar_up;                        /* the core's action_bar as of the last frame: a rising edge moves focus onto it */
    Button field[JF_UI_FIELDS], signin, signexit;
    ModalDialog dlg;
    bool dlg_up;
    unsigned dlg_buttons;
    JfScreen last_screen;
    JfCat last_cat;
    ListView pick;                       /* the root header's category picker */
    bool pick_up, head_press;
    char head_title[48];                 /* "JELLYFIN . CATEGORY", the root's bar title */
    bool lv_ready;
    char fields[JF_UI_FIELDS][64];       /* server, user, password (wiped after the POST) */
    int  focus;                          /* index in the zone (sign-in: 0..4) */
    JfZone zone;
    bool show_ring;                      /* a key / pad press shows it, a touch hides it */
    int  bar_focus;                      /* 0 back, 1 exit */
    int  played_ms;                      /* the loop's wall-clock count for the track */
    bool exit_press, back_press;
    char rowtxt[32][48];                 /* detail strings of the rows being drawn */
    int  rowtxt_i;
    bool dirty;
    unsigned seen_banner;
    unsigned seen_restore;               /* the core's restore_seq as of the last frame */
    bool restore_pend;                   /* a BACK came: put the list view back once the rows are loaded */
    EqPanel eq;                          /* the equaliser panel (common/eq_panel.h), full screen while eq_up */
    bool eq_up;
    bool eq_apply, eq_save;              /* a gain set for jf_ui_eq_take(): apply it live; also save it */
    int  eq_out[EQ_BANDS];
    AudioEnh enh_out;                    /* the enhancer settings that go with eq_out (read with jf_ui_enh_out) */
    void (*enh_now_fn)(AudioEnh *e);     /* the enhancer settings in force now, asked when the panel opens; NULL = off */
    void (*eq_gains_fn)(int gains[EQ_BANDS]);   /* the gains in force now, asked when the panel opens; NULL = flat */
    TouchGuard guard;                    /* a touch from before a screen change does not reach the new screen (common.h) */
} JfUi;

void jf_ui_init(JfUi *ui, Framebuffer *fb, TouchInput *touch, GamepadManager *gm, const JfApp *app);
void jf_ui_free(JfUi *ui);

/** One frame of input.  Writes up to `max` intents to `out` and returns how many;
 *  may block in keyboard_enter() (then drains keys and pointer itself). */
int  jf_ui_input(JfUi *ui, const TouchState *ts, const InputState *in, Pointer *ptr,
                 uint32_t now, JfEvent *out, int max);

void jf_ui_draw(JfUi *ui);
/** A gain set the equaliser panel produced since the last call: true with the five dB values in `gains` (apply them to the
 *  audio stream) and `*save` set when the user pressed DONE (write them to the config).  Cancel returns the opening values, save false. */
bool jf_ui_eq_take(JfUi *ui, int gains[EQ_BANDS], bool *save);
/** The enhancer settings handed out with the gains of the last jf_ui_eq_take (apply them beside the gains; DONE saves both). */
const AudioEnh *jf_ui_enh_out(const JfUi *ui);
void jf_ui_set_played(JfUi *ui, int played_ms);
/** The list view (scroll top, cursor row) of the screen showing; false when the screen has none or has not been set up yet.
 *  The loop hands it to jf_note_view() each frame, before the frame's events reach the core. */
bool jf_ui_view(const JfUi *ui, int *top, int *cursor);
void jf_ui_mark_dirty(JfUi *ui);
/** True once when something wants a repaint (state change, button, bar tick). */
bool jf_ui_take_dirty(JfUi *ui);

#endif /* ARCA_JF_UI_H */
