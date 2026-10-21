#ifndef EQ_PANEL_H
#define EQ_PANEL_H
/**
 * The equaliser panel: five vertical sliders (60 / 250 / 1K / 4K / 12K Hz, dB
 * readout), a preset row (FLAT BASS TREBLE VOCAL LOUDNESS), CANCEL and DONE.
 * One widget behind two doors (Jellyfin NOW PLAYING and the settings app's Audio
 * page); the app-agnostic contract is:
 *
 *   eq_panel_open()   with the gains in force now (audio_out_get_eq / eq_config_read)
 *   eq_panel_input()  once a frame; returns EQP_CHANGED after every edit (the caller
 *                     applies eq_panel_gains() to its AudioOut at once, so the change
 *                     is heard live), EQP_DONE (the caller saves: eq_panel_store*) or
 *                     EQP_CANCEL (the gains are back at what they were on open; the
 *                     caller applies them again and saves nothing)
 *   eq_panel_draw()   when eq_panel_take_dirty() says so
 *
 * Touch: press or drag on a column sets that band from the finger's height; tap a
 * preset or a button.  Pad / keyboard: Left / Right pick the band, Up / Down move it
 * 1 dB; Space / Enter / pad A / X on a band goes down to the presets; Shift / pad B
 * and PgDn move to the next row (sliders, presets, buttons), PgUp the previous one;
 * on the presets Left / Right pick and Enter applies; on the buttons Left / Right
 * pick and Enter acts; Esc / Backspace cancel.  The focused item is outlined in
 * yellow; the preset that equals the gains is filled yellow (none when custom).
 *
 * Every pure rule (layout, height -> dB, x -> band, key handling, preset match,
 * cancel restore, the config line) is a plain function the host test calls.
 */
#include <stdbool.h>
#include <stdint.h>
#include "audio_eq.h"
#include "audio_enh.h"
#include "ui_focus.h"
#include "framebuffer.h"
#include "touch_input.h"
#include "gamepad.h"
#include "common.h"
#include "config.h"

typedef enum { EQP_ZONE_SLIDERS = 0, EQP_ZONE_PRESETS, EQP_ZONE_BUTTONS, EQP_ZONES } EqPanelZone;
typedef enum { EQP_NONE = 0, EQP_CHANGED, EQP_DONE, EQP_CANCEL } EqPanelEvent;
typedef enum { EQK_UP, EQK_DOWN, EQK_LEFT, EQK_RIGHT, EQK_ACT, EQK_NEXT, EQK_PREV, EQK_BACK } EqPanelKey;

#define EQP_PRESETS 5

/* The ENHANCER page (reached from the EQ page by the middle button, and back): an ON/OFF toggle in the presets' row and
 * six vertical sliders 0..100 in three parts (HARM BASS, DRUM BASS, HARM TREBLE) x (AMOUNT, RANGE), in AudioEnh field order.
 * Same zones as the EQ page: sliders / the toggle (zone EQP_ZONE_PRESETS) / buttons.  Up / Down move a slider
 * EQP_ENH_STEP.  CANCEL restores the EQ gains and the enhancer both. */
#define EQP_ENH_SLIDERS 6
#define EQP_ENH_PARTS   3
#define EQP_ENH_STEP    5
#define EQP_ENH_MAX     100
enum { EQP_BTN_CANCEL = 0, EQP_BTN_PAGE, EQP_BTN_DONE, EQP_BTNS };
typedef enum { EQP_PAGE_EQ = 0, EQP_PAGE_ENH } EqPanelPage;

typedef struct {
    UiRect title;
    UiRect col[EQ_BANDS];        /* the whole column, labels included: the touch area */
    UiRect track[EQ_BANDS];      /* the part whose height is the dB value */
    UiRect preset[EQP_PRESETS];
    UiRect cancel, page, done;   /* page: the middle button, ENH on the EQ page and EQ on the enhancer page */
    UiRect ecol[EQP_ENH_SLIDERS];    /* enhancer page: column (touch area) and track */
    UiRect etrack[EQP_ENH_SLIDERS];
    UiRect group[EQP_ENH_PARTS];     /* the part headers */
    UiRect toggle;                   /* the ON/OFF bar, in the presets' row */
} EqPanelLayout;

typedef struct {
    int gains[EQ_BANDS];
    int orig[EQ_BANDS];
    EqPanelZone zone;
    int band, preset_focus, btn_focus;   /* btn_focus: EQP_BTN_CANCEL / _PAGE / _DONE */
    int drag;                            /* band under a finger, -1 none */
    EqPanelLayout L;
    Button preset_b[EQP_PRESETS], cancel_b, page_b, done_b, toggle_b;
    TouchGuard guard;
    bool dirty;
    EqPanelPage page;
    AudioEnh enh, enh_orig;
    int enh_i;                           /* focused enhancer slider */
    int enh_drag;                        /* enhancer slider under a finger, -1 none */
    int toggle_shown;                    /* 0 not styled yet, else enh.on + 1 as last styled */
} EqPanel;

/* ── pure rules ── */
/** Lay out inside a rectangle (the caller passes the SCREEN_SAFE_* rectangle). */
void eq_panel_layout(EqPanelLayout *L, int sl, int st, int sr, int sb);
/** The dB a finger at height y means on a track: +EQ_MAX_DB at the top, -EQ_MAX_DB at the bottom, clamped. */
int  eq_panel_db_at(const UiRect *track, int y);
/** The height of dB value `db` on a track (inverse of eq_panel_db_at at the track's resolution). */
int  eq_panel_y_of_db(const UiRect *track, int db);
/** The band whose column is nearest x; -1 when y is outside the sliders' vertical span. */
int  eq_panel_band_at(const EqPanelLayout *L, int x, int y);
/** Index of the preset equal to the gains, or -1 (custom). */
int  eq_panel_preset(const EqPanel *p);
/** The key rules.  Changes p and returns the event. */
EqPanelEvent eq_panel_key(EqPanel *p, EqPanelKey k);
/** The value text "+3" / "0" / "-12". */
void eq_panel_db_text(char *buf, int cap, int db);
/** Set p->gains[band] from a finger height; true when the value changed. */
bool eq_panel_set_from_y(EqPanel *p, int band, int y);
/** The config value for these gains: the preset's name when one matches, else five ints. */
void eq_panel_config_value(const int gains[EQ_BANDS], char *buf, int cap);

/* ── the enhancer page's pure rules ── */
/** Slider i (0..5: hb_amt hb_rng db_amt db_rng ht_amt ht_rng) of an AudioEnh. */
int  eq_panel_enh_get(const AudioEnh *e, int i);
/** Set slider i, clamped 0..100; true when the value changed (false for a bad i). */
bool eq_panel_enh_set(AudioEnh *e, int i, int v);
/** The value a finger at height y means on an enhancer track: 100 at the top, 0 at the bottom, clamped. */
int  eq_panel_enh_value_at(const UiRect *track, int y);
/** The height of value v on an enhancer track. */
int  eq_panel_enh_y_of_value(const UiRect *track, int v);
/** The enhancer slider whose column is nearest x; -1 when y is outside their vertical span. */
int  eq_panel_enh_at(const EqPanelLayout *L, int x, int y);
/** The config value for an enhancer setting ("on,hb,hbr,db,dbr,ht,htr"). */
void eq_panel_enh_config_value(const AudioEnh *e, char *buf, int cap);

/* ── lifecycle ── */
void eq_panel_open(EqPanel *p, const int gains[EQ_BANDS], int sl, int st, int sr, int sb, uint32_t now);
/** As eq_panel_open, with the enhancer settings in force now (audio_get_enh / enh_config_read); eq_panel_open opens it all-off. */
void eq_panel_open_enh(EqPanel *p, const int gains[EQ_BANDS], const AudioEnh *enh, int sl, int st, int sr, int sb, uint32_t now);
const int *eq_panel_gains(const EqPanel *p);
/** The enhancer settings now (after CANCEL: the ones the panel opened with).  A caller applies both after every event. */
const AudioEnh *eq_panel_enh(const EqPanel *p);
/** One frame.  `in` may be NULL. */
EqPanelEvent eq_panel_input(EqPanel *p, const TouchState *ts, const InputState *in, uint32_t now);
void eq_panel_draw(EqPanel *p, Framebuffer *fb);
bool eq_panel_take_dirty(EqPanel *p);

/* ── saving: the `eq` key of CONFIG_FILE_PATH, every other key kept ── */
/** Set the key in an already loaded Config and save it (the settings app's own Config). */
int  eq_panel_store(Config *cfg, const int gains[EQ_BANDS]);
/** Load the file at `path`, set the key, save atomically. */
int  eq_panel_store_file(const char *path, const int gains[EQ_BANDS]);
/** The same, with the `enh` key beside `eq`, in one save (what DONE writes). */
int  eq_panel_store_both(Config *cfg, const int gains[EQ_BANDS], const AudioEnh *enh);
int  eq_panel_store_file_both(const char *path, const int gains[EQ_BANDS], const AudioEnh *enh);

#endif
