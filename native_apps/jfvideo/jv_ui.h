/*
 * jv_ui — the video client's screens: draw from JvCore, map touch / keyboard / pad to JV_EV_* events.  No policy: every
 * decision is jv_core's.  Screens: sign-in, home tiles, movie / show poster grid, lists (resume, next up, search hits,
 * seasons, episodes, similar), details, cast, error overlay and a placeholder PLAYER (the play request and BACK).
 *
 * jv_ui_layout() is the one place the rectangles are computed; it is pure (the screen extents are an argument), so
 * tests/jv_ui_layout_test.c calls the very function the app draws and hit-tests with.  It lives in jv_ui_layout.c.
 */
#ifndef ARCA_JV_UI_H
#define ARCA_JV_UI_H

#include <stdbool.h>
#include <stdint.h>
#include "framebuffer.h"
#include "common.h"
#include "touch_input.h"
#include "gamepad.h"
#include "list_view.h"
#include "top_bar.h"
#include "ui_focus.h"
#include "ui_zone.h"
#include "pointer.h"
#include "jf_art.h"
#include "jv_core.h"

#define JV_UI_FIELDS    3     /* sign-in: SERVER, USER, PASSWORD */
#define JV_UI_TILES     6     /* home: slots for CONTINUE, NEXT UP, MOVIES, SHOWS, SEARCH (present ones fill from slot 0) */
#define JV_UI_COLS      5     /* poster grid */
#define JV_UI_ROWS      2
#define JV_UI_CELLS     (JV_UI_COLS * JV_UI_ROWS)
#define JV_UI_ACT       4     /* details: PLAY or RESUME, FROM START, CAST, SIMILAR (present ones fill from slot 0) */
#define JV_UI_TRK       2     /* details: the AUDIO and SUBTITLES rows, each one change button */
#define JV_UI_BTN_H     44
#define JV_UI_ART_GRID  120   /* art box of a grid tile */
#define JV_UI_ART_THUMB 64    /* art box of a list row */
#define JV_UI_ART_POSTER 200  /* art box of the details poster */

/* The screen the layout is taken against (SCREEN_* macros in the app). */
typedef struct {
    int vis_l, vis_t, vis_r, vis_b;
    int safe_l, safe_t, safe_r, safe_b;
} JvExtents;

typedef struct {
    int    bar_bottom;              /* top_bar_bottom(): the rule is drawn on this row */
    UiRect banner;                  /* notice / busy line under the bar, drawn only */
    UiRect list;                    /* a list screen's rows: pressable */
    UiRect list_s;                  /* the same above the SEARCH button (search hits) */
    UiRect sbtn;                    /* NEW SEARCH under the search hits; pressable */
    UiRect cell[JV_UI_CELLS];       /* poster grid cells, row-major; pressable */
    UiRect gup, gdn;                /* poster grid scroll buttons; pressable */
    UiRect field[JV_UI_FIELDS];     /* sign-in rows; pressable */
    UiRect signin, reuse, signexit; /* sign-in buttons; pressable */
    UiRect tile[JV_UI_TILES];       /* home tiles; pressable */
    UiRect poster;                  /* details: the poster, drawn only */
    UiRect text;                    /* details: title, facts and overview as scrolling lines; pressable (it scrolls) */
    UiRect trk[JV_UI_TRK];          /* details: AUDIO, SUBTITLES; pressable */
    UiRect act[JV_UI_ACT];          /* details: the action row; pressable */
    UiRect pinfo;                   /* player placeholder: the request text, drawn only */
    UiRect pback;                   /* player placeholder: BACK; pressable */
} JvLayout;

void jv_ui_layout(JvLayout *L, const JvExtents *e);
void jv_ui_extents_live(JvExtents *e);

#define JV_UI_LINES 80
#define JV_UI_LINE_W 60

typedef struct {
    Framebuffer *fb;
    TouchInput *touch;
    GamepadManager *gm;
    const JvCore *core;
    JvLayout L;
    TopBar bar;
    ListView lv;                         /* the list of the screen showing (rows, details text, cast) */
    UiZoneFocus zf;
    TouchGuard guard;
    JfArt *art_grid, *art_thumb, *art_poster;
    ModalDialog dlg;
    bool dlg_up;
    unsigned dlg_buttons;
    JvScreen last_screen;
    JvKind last_kind;
    bool lv_ready;
    char fields[JV_UI_FIELDS][64];       /* server, user, password (wiped after the POST) */
    char pre_server[64], pre_user[64];   /* the core's prefill as last copied into the fields */
    int  gtop, gcur;                     /* poster grid: first visible row, cursor item */
    int  press_zone, press_id;           /* the widget a finger went down on, -1 none */
    bool exit_press, back_press;
    bool dirty;
    unsigned seen_restore;
    bool restore_pend;
    char lines[JV_UI_LINES][JV_UI_LINE_W];   /* details text, wrapped */
    int  nlines;
    char detail_for[33];                 /* item id the lines were built for */
    char rowtxt[32][64];                 /* detail strings of the rows being drawn */
    int  rowtxt_i;
    unsigned seen_art;
} JvUi;

/** `art_dir` is the shared disk cache dir (JF_ART_CACHE_DIR).  The art instances are created here and freed by jv_ui_free. */
void jv_ui_init(JvUi *ui, Framebuffer *fb, TouchInput *touch, GamepadManager *gm, const JvCore *core, const char *art_dir);
void jv_ui_free(JvUi *ui);

/** Point the three art instances at the signed-in server (call when the core emits JV_CMD_SET_SERVER / SET_AUTH, with the same values). */
void jv_ui_art_server(JvUi *ui, const char *host, int port, bool tls, const char *client, const char *device,
                      const char *device_id, const char *token);
/** Once per frame: advances the three art fetches; marks the screen dirty when a tile became ready. */
void jv_ui_art_poll(JvUi *ui, long long now_ms);

/** One frame of input.  Writes up to `max` events to `out` and returns how many; may block in keyboard_enter() (then drains keys and pointer itself). */
int  jv_ui_input(JvUi *ui, const TouchState *ts, const InputState *in, Pointer *ptr, uint32_t now, JvEvent *out, int max);
/** Draws the core's current state (a full frame; the caller swaps). */
void jv_ui_draw(JvUi *ui);
/** The list view (scroll top, cursor row) of the screen showing; false when the screen has none or is not set up yet.  The grid reports its first visible item and cursor item.  The loop hands it to jv_note_view() before the frame's events reach the core. */
bool jv_ui_view(const JvUi *ui, int *top, int *cursor);
void jv_ui_mark_dirty(JvUi *ui);
/** True once when something wants a repaint (state change, button, bar tick). */
bool jv_ui_take_dirty(JvUi *ui);

#endif /* ARCA_JV_UI_H */
