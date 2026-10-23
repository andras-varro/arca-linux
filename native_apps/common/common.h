/**
 * Common UI Library for Arca
 * 
 * Unified button system combining best features from game_common and ui_button.
 * Provides text truncation, auto-sizing, icon support, and screen templates.
 */

#ifndef COMMON_H
#define COMMON_H

#include "framebuffer.h"
#include "touch_input.h"
#include "highscore.h"
#include "gamepad.h"
#include "ui_focus.h"
#include <stdint.h>
#include <stdbool.h>
#include <sys/time.h>

/* --- Frame timing constants for dirty-flag rendering pattern --- */
#define FRAME_DELAY_ACTIVE_US  33333   /* ~30 fps when actively rendering */
#define FRAME_DELAY_IDLE_US   100000   /* ~10 fps polling when idle       */

// ============================================================================
// BUTTON SYSTEM
// ============================================================================

// Button visual states
typedef enum {
    BTN_STATE_NORMAL,
    BTN_STATE_HIGHLIGHTED,
    BTN_STATE_PRESSED
} ButtonVisualState;

// Button structure
typedef struct {
    // Position and size
    int x, y, width, height;
    
    // Text (with truncation support)
    char text[128];
    int max_text_width;  // 0 = no limit, >0 = truncate with "..."
    
    // Colors
    uint32_t bg_color;
    uint32_t text_color;
    uint32_t highlight_color;
    uint32_t border_color;
    
    // Styling
    int text_scale;
    int border_width;
    
    // State management
    ButtonVisualState visual_state;
    bool was_pressed;
    // A disabled button acts on no input (touch, tap, press) and draws grey;
    // callers set it from their state and need no guard at each use.
    bool disabled;
    uint32_t last_press_time_ms;
    uint32_t debounce_ms;
    
    // Optional icon callback
    void (*draw_icon)(Framebuffer *fb, int x, int y, int size, uint32_t color);
} Button;

// ============================================================================
// BUTTON MANAGEMENT
// ============================================================================

// Initialize button with all parameters
void button_init_full(Button *btn, int x, int y, int width, int height,
                      const char *text, uint32_t bg_color, uint32_t text_color,
                      uint32_t highlight_color, int text_scale);

// Initialize with defaults (common use case)
void button_init_simple(Button *btn, int x, int y, int width, int height,
                        const char *text);

// Backward compatibility macro - auto-determines text scale.
//
// ⚠️ The 150 is a raw SURFACE-pixel threshold and is deliberately NOT converted
// with fb_ui_px_x(). Text is an integer glyph multiplier with no rung below 1, so
// a scale-3 label cannot halve: converting the threshold too would hold the scale
// at 3 on a half-size surface and draw the label at twice its design size on the
// panel. Leaving the threshold flat lets a scaled width fall through to 2, which
// is the closest available rung to the 1.5 that is wanted. It is not exact —
// measured, a scaled BTN_LARGE_WIDTH of 110 takes scale 2, whose "PLAY AGAIN" is
// 120 surface px against a 110 px button, so the label OVERRUNS. Sizing text to
// fit would pick the scale from text_measure_width(); left as is because no game
// runs at a reduced surface yet.
#define button_init(btn, x, y, w, h, text, bg, txt, hl) \
    button_init_full(btn, x, y, w, h, text, bg, txt, hl, ((w) > 150) ? 3 : 2)

// Set text with automatic truncation if max_width exceeded
void button_set_text(Button *btn, const char *text);

// Set max text width (0 = no limit)
void button_set_max_text_width(Button *btn, int max_width);

// Set colors
void button_set_colors(Button *btn, uint32_t bg, uint32_t text, uint32_t highlight);

// Set border
void button_set_border(Button *btn, uint32_t color, int width);

// Set debounce time
void button_set_debounce(Button *btn, uint32_t ms);

// Set custom icon drawer
void button_set_icon(Button *btn, void (*draw_icon)(Framebuffer*, int, int, int, uint32_t));

// ============================================================================
// TOUCH HANDLING
// ============================================================================

// Check if touch is within button bounds
bool button_is_touched(Button *btn, int touch_x, int touch_y);

// Update button state with touch (returns true if pressed)
bool button_update(Button *btn, int touch_x, int touch_y, bool is_touching, uint32_t current_time_ms);

// The box button_is_touched() hit-tests — the one home of that arithmetic, so
// a keyboard focus ring (ui_focus.h) is drawn and tapped where touch lands.
UiRect button_rect(const Button *btn);

// Append btn's rect to a focusables list (out[n], at most max) unless it is
// disabled — a disabled button takes no tap, so focus must skip it.  Returns
// the new count.
int focus_add_button(UiRect *out, int n, int max, const Button *btn);

// Legacy API for compatibility (used by games)
bool button_check_press(Button *btn, bool currently_pressed, uint32_t current_time_ms);

// "Was this button just tapped?" — call EVERY frame, quiet frames included.
// Prefer this over button_check_press() at a touch call site: the
//   if (button_is_touched(...) && button_check_press(..., true, now))
// shape never passes false, so button_check_press()'s latch is never cleared and
// the button fires exactly once per process (see common.c, and
// tests/button_latch_test.c).
bool button_check_tap(Button *btn, const TouchState *ts, uint32_t current_time_ms);

// ── TouchGuard: no touch that began before a screen change may act on the new screen ──
//
// A button fires on the leading edge of a press (button_check_press), so when a tap switches the
// screen, a button on the NEW screen at the same spot fires on ANY press that follows within a
// moment: the digitizer bounces (lift, then touch again at the same place) or a scroll gesture
// that dropped contact resumes.  touch_guard_screen_changed() disarms; touch_guard_pass() says
// whether widgets may see the touch this frame.  It re-arms only on a fresh press (or a quiet frame) at least
// TOUCH_GUARD_QUIET_MS after the last contact -- the finger has been up
// (and nothing has touched) for TOUCH_GUARD_QUIET_MS, so a contact that is down, or lifts and
// bounces, keeps the guard shut and only a press that starts after that quiet counts.
// The decision is pure (tests/button_latch_test.c group F).  Opt-in: an app that never calls
// touch_guard_screen_changed() is unaffected.
#define TOUCH_GUARD_QUIET_MS 200
typedef struct { bool armed; bool prev_down; uint32_t last_down_ms; } TouchGuard;
void touch_guard_init(TouchGuard *g);
void touch_guard_screen_changed(TouchGuard *g, uint32_t now_ms);
/** Call once per frame with whether a finger is down; true = widgets may act on this frame's touch. */
bool touch_guard_pass(TouchGuard *g, bool down, uint32_t now_ms);

// Has ANY button's visual_state been changed by button_update() or
// button_check_press() since the last call?  Clears the flag.  One process-wide
// question, so a caller that repaints only on change asks it once per loop
// iteration without enumerating the buttons its pages own.  It goes quiet once
// the buttons settle (quiet frames change nothing), so it is never an
// always-true repaint term.  A caller that writes visual_state directly (focus
// rings) owns that change and repaints for it itself.
bool button_take_dirty(void);

// ============================================================================
// RENDERING
// ============================================================================

// Draw button with current state
void button_draw(Framebuffer *fb, Button *btn);

// Draw button with hamburger menu icon
void button_draw_menu(Framebuffer *fb, Button *btn);

// Draw button with X exit icon
void button_draw_exit(Framebuffer *fb, Button *btn);

// Backward compatibility aliases for old game_common API
#define draw_menu_button button_draw_menu
#define draw_exit_button button_draw_exit

// ============================================================================
// TEXT UTILITIES
// ============================================================================

// Measure text width (8 pixels per character * scale)
int text_measure_width(const char *text, int scale);

// Measure text height (8 pixels * scale)
int text_measure_height(int scale);

// Draw centered text
void text_draw_centered(Framebuffer *fb, int center_x, int center_y,
                       const char *text, uint32_t color, int scale);

// Uppercase src into dest and cut it with "..." to fit max_width pixels
// (<= 0: no width limit) and dest_size bytes.  Always NUL-terminated within
// dest_size; writes nothing when dest_size is 0.
void text_truncate(char *dest, size_t dest_size, const char *src,
                   int max_width, int scale);

// Convert text to uppercase (for font compatibility)
void text_to_uppercase(char *dest, const char *src, size_t max_len);

// ============================================================================
// BUTTON AUTO-SIZING
// ============================================================================

// Calculate minimum width needed for text
int button_calc_min_width(const char *text, int scale, int padding);

// Auto-size button to fit text
void button_auto_size(Button *btn, int padding);

// ============================================================================
// ICON DRAWING HELPERS
// ============================================================================

// Draw hamburger menu icon (three horizontal lines)
void icon_draw_hamburger(Framebuffer *fb, int x, int y, int size, uint32_t color);

// Draw X icon (two diagonal lines)
void icon_draw_x(Framebuffer *fb, int x, int y, int size, uint32_t color);

// ============================================================================
// TIME UTILITIES
// ============================================================================

// Get current time in milliseconds
uint32_t get_time_ms(void);

// ============================================================================
// COLOR DEFINITIONS
// ============================================================================

// Common colors
#define COLOR_BLACK         RGB(0, 0, 0)
#define COLOR_WHITE         RGB(255, 255, 255)
#define COLOR_RED           RGB(255, 0, 0)
#define COLOR_GREEN         RGB(0, 255, 0)
#define COLOR_BLUE          RGB(0, 0, 255)
#define COLOR_CYAN          RGB(0, 255, 255)
#define COLOR_MAGENTA       RGB(255, 0, 255)
#define COLOR_YELLOW        RGB(255, 255, 0)

// Button colors
#define BTN_COLOR_PRIMARY       RGB(0, 150, 0)      // Green
#define BTN_COLOR_SECONDARY     RGB(100, 100, 100)  // Gray
#define BTN_COLOR_DANGER        RGB(200, 0, 0)      // Red
#define BTN_COLOR_WARNING       RGB(255, 165, 0)    // Orange
#define BTN_COLOR_INFO          RGB(0, 150, 200)    // Cyan
#define BTN_COLOR_HIGHLIGHT     RGB(255, 255, 100)  // Yellow

// Legacy aliases for compatibility with existing games
#define BTN_MENU_COLOR          BTN_COLOR_WARNING
#define BTN_EXIT_COLOR          BTN_COLOR_DANGER
#define BTN_START_COLOR         BTN_COLOR_PRIMARY
#define BTN_RESTART_COLOR       BTN_COLOR_PRIMARY
#define BTN_RESUME_COLOR        BTN_COLOR_PRIMARY
#define BTN_HIGHLIGHT_COLOR     BTN_COLOR_HIGHLIGHT

// Common button sizes, in DESIGN pixels at the panel's own resolution. They pass
// through fb_ui_px_*() so a control keeps its PHYSICAL size when a scaling DSS
// overlay presents a smaller surface — left flat, BTN_MENU_WIDTH 70 on a 400x240
// surface covers 140 of the panel's 800 columns. Bit-exact at 800x480 and in
// portrait; the guard in fb_scale_ui_px() short-circuits the divide entirely when
// surface == panel, so this costs two loads and a compare on every shipped unit.
// Contract and the two ways to get the ratio wrong: framebuffer.h.
#define BTN_MENU_WIDTH  fb_ui_px_x(70)
#define BTN_MENU_HEIGHT fb_ui_px_y(50)
#define BTN_EXIT_WIDTH  fb_ui_px_x(70)
#define BTN_EXIT_HEIGHT fb_ui_px_y(50)
#define BTN_LARGE_WIDTH  fb_ui_px_x(220)
#define BTN_LARGE_HEIGHT fb_ui_px_y(60)

// Debounce time in milliseconds
#define BTN_DEBOUNCE_MS 200

// ============================================================================
// LAYOUT HELPERS (using safe area constraints)
// ============================================================================

// Calculate centered X position within safe area
#define LAYOUT_CENTER_X(width) (SCREEN_SAFE_LEFT + (SCREEN_SAFE_WIDTH - (width)) / 2)

// Calculate centered Y position within safe area
#define LAYOUT_CENTER_Y(height) (SCREEN_SAFE_TOP + (SCREEN_SAFE_HEIGHT - (height)) / 2)

// Standard positions for common UI elements. The insets are design pixels and
// convert with the sizes above — a flat 10 px gap doubles its share of the panel
// on a half-size surface exactly as a flat button width does.
#define LAYOUT_TITLE_Y          (SCREEN_SAFE_TOP + fb_ui_px_y(20))   // Title text position
#define LAYOUT_MENU_BTN_X       (SCREEN_SAFE_LEFT + fb_ui_px_x(10))  // Menu button (top-left)
#define LAYOUT_MENU_BTN_Y       (SCREEN_SAFE_TOP + fb_ui_px_y(10))
#define LAYOUT_EXIT_BTN_X       (SCREEN_SAFE_RIGHT - BTN_EXIT_WIDTH - fb_ui_px_x(10))  // Exit button (top-right)
#define LAYOUT_EXIT_BTN_Y       (SCREEN_SAFE_TOP + fb_ui_px_y(10))
#define LAYOUT_BOTTOM_BTN_Y     (SCREEN_SAFE_BOTTOM - BTN_LARGE_HEIGHT - fb_ui_px_y(20))  // Bottom buttons

// Start-menu text metrics (start_menu.c): instruction/warning text scale, the
// gap between wrapped lines, and the gap between blocks (title, instructions,
// warning, entries).
#define WELCOME_INST_SCALE  2
#define WELCOME_LINE_GAP    8
#define WELCOME_BLOCK_GAP   16

// ============================================================================
// SINGLETON INSTANCE LOCK
// ============================================================================

// Acquire an exclusive process lock via flock().  Returns a file descriptor
// (keep it open for the lifetime of the process) or -1 if another instance
// already holds the lock.  The lock is automatically released when the
// process exits or the fd is closed.
int acquire_instance_lock(const char *app_name);

// ============================================================================
// PER-FRAME SERVICE FOR BLOCKING SUB-LOOPS
// ============================================================================

/*
 * A BLOCKING SUB-LOOP IS A RENDER LOOP: it owes every per-frame service the
 * main loop owes.  keyboard_enter() and hs_drain_touches() each run their own
 * draw/poll/usleep loop while the game's loop is stopped, and until 2026-08-22
 * neither serviced the audio mix bus — so a `NEW HIGH SCORE!` keyboard froze a
 * fade and DEFERRED (not dropped) the game-over fanfare until it closed, which
 * the operator experiences as no game-over sound.  The mixer advances by frames
 * RENDERED, so a loop that renders nothing stops time for every voice.
 *
 * ⚠️ The slot holds ONE service and audio is its only registrant, deliberately:
 * the alternative was threading a `void (*tick)(void *)` through
 * keyboard_enter() → hs_enter_name() → gameover_init() and every game's call
 * site, where a new game silently opts out by forgetting it.  Registered by
 * `audio_open()` and cleared by `audio_close()` — never by a game — so this
 * header stays free of `audio.h` (the same separation `hw_led_pulse_update()`
 * keeps) and no app changed a line to get the fix.
 *
 * ⚠️ Any NEW loop that draws while the main loop is stopped owes a call to
 * ui_frame_service() beside its fb_swap().  `hw_pulse_led()` and
 * `hw_blink_led()` block too and do NOT call it: their waits are single long
 * usleeps rather than a loop, and (measured 2026-10-01) their only caller is
 * control_panel, which runs no bed.
 */

// Install the per-frame service.  ctx is passed back verbatim.  fn == NULL
// clears the slot.
void ui_frame_service_set(void (*fn)(void *ctx), void *ctx);

// Run the installed service, if any.  Cheap and safe to call when nothing is
// registered; call it once per iteration of any loop that owns the screen.
void ui_frame_service(void);

/* The registered ctx, or NULL when the slot is empty.
 *
 * ⚠️ **This is `Audio *` and the cast belongs to the caller**, which is why the
 * return type is `void *`: the whole point of the slot is that this header does
 * not name `audio.h`.  Legitimate only because the invariant above is explicit —
 * audio is the slot's ONLY registrant, `audio_open()` installs it and
 * `audio_close()` clears it, so the pointer is an open `Audio` or nothing.  A
 * second registrant would make this unsound, and there must not be one.
 *
 * It exists so `gameover_update()` can sound the high-score chime without an
 * `Audio *` threaded through `gameover_init()` and thirteen call sites — the
 * same signature-threading the slot was created to avoid, and the reason no app
 * changed a line to get either fix. */
void *ui_frame_service_ctx(void);

// ============================================================================
// TOGGLE SWITCH CONTROL
// ============================================================================

typedef struct {
    int x, y;               // Position (top-left of track)
    int track_w, track_h;   // Track dimensions
    bool state;             // true = ON, false = OFF
    char label[64];         // Label text drawn to the right

    // Colors
    uint32_t on_color;      // Track color when ON
    uint32_t off_color;     // Track color when OFF
    uint32_t knob_color;    // Knob color
    uint32_t label_color;   // Label text color

    // State management
    bool was_pressed;
    // A disabled button acts on no input (touch, tap, press) and draws grey;
    // callers set it from their state and need no guard at each use.
    bool disabled;
    uint32_t last_press_time_ms;
    uint32_t debounce_ms;
} ToggleSwitch;

// Initialize toggle switch with position, size, and label
void toggle_init(ToggleSwitch *sw, int x, int y, int track_w, int track_h,
                 const char *label, bool initial_state);

// Set colors (defaults: green ON, grey OFF, white knob, white label)
void toggle_set_colors(ToggleSwitch *sw, uint32_t on_color, uint32_t off_color,
                       uint32_t knob_color, uint32_t label_color);

// Check touch and toggle state. Returns true if state CHANGED this frame.
bool toggle_check_press(ToggleSwitch *sw, int touch_x, int touch_y,
                        bool is_pressed, uint32_t current_time_ms);

// The generous box toggle_check_press() hit-tests (track + label + padding):
// the one home of that arithmetic, for focus rings and for layouts that place
// widgets beside a toggle.
UiRect toggle_hit_rect(const ToggleSwitch *sw);

// As focus_add_button(), for a toggle.
int focus_add_toggle(UiRect *out, int n, int max, const ToggleSwitch *sw);

// Draw the toggle switch
void toggle_draw(Framebuffer *fb, ToggleSwitch *sw);

// ============================================================================
// CYCLER CONTROL
// ============================================================================

/* One row-wide control that steps through a list:  `<  CURRENT ENTRY  >`.
 * The caller owns the list and the current index; the widget owns the
 * drawing, the hit-test and the step semantics.  Touch (and a mouse click,
 * which arrives as a tap) on the LEFT half steps -1, on the RIGHT half +1;
 * cycler_step() wraps at both ends.  The midpoint belongs to the right half,
 * so a keyboard activation — a synthetic tap at the rect's centre
 * (ui_tap_begin) — steps +1: Enter on a cycler means "next", never anything
 * destructive.  A disabled cycler takes no input and draws grey, as a
 * disabled button does. */
typedef struct {
    int x, y, width, height;   // the whole row, which is also the hit box
    int text_scale;            // entry text scale (default 2)

    // Colors
    uint32_t bg_color;
    uint32_t text_color;
    uint32_t arrow_color;
    uint32_t border_color;
    int border_width;

    // State management
    bool was_pressed;          // cycler_check_tap()'s press-edge latch
    // A disabled cycler acts on no input and draws grey; callers set it from
    // their state and need no guard at each use.
    bool disabled;
} Cycler;

// Initialize with position and size, default colors, scale 2, enabled.
void cycler_init(Cycler *c, int x, int y, int width, int height);

// Set colors (defaults: dark grey bg, white text, cyan arrows, grey border)
void cycler_set_colors(Cycler *c, uint32_t bg, uint32_t text, uint32_t arrow,
                       uint32_t border);

// The box cycler_hit() tests — the one home of that arithmetic, for focus
// rings and for layouts that place widgets beside it.
UiRect cycler_rect(const Cycler *c);

// As focus_add_button(), for a cycler.
int focus_add_cycler(UiRect *out, int n, int max, const Cycler *c);

// Which way a point steps it: -1 on the left half, +1 on the right half
// (the midpoint included), 0 outside the rect or when disabled.  Stateless.
int cycler_hit(const Cycler *c, int x, int y);

// cycler_hit() on the press edge only: call EVERY frame with the frame's
// touch point and whether it is touching; returns -1/+1 on the frame the
// press lands, 0 otherwise (held, released, outside, disabled).
int cycler_check_tap(Cycler *c, int touch_x, int touch_y, bool touching);

// The step semantics, for a tap's or an arrow key's dir: (index + dir)
// wrapped into 0..count-1.  count <= 0 returns 0; dir is -1, 0 or +1.
int cycler_step(int index, int count, int dir);

// Draw the row with `text` (the caller's current entry; NULL = empty)
// centred between the arrows, uppercased and cut with "..." so it never
// overruns them.
void cycler_draw(Framebuffer *fb, const Cycler *c, const char *text);

// ============================================================================
// UNIFIED GAME OVER SCREEN
// ============================================================================

// The arcade flow: SHOW (title, score, over the playfield) -> NAME (only for a
// score that makes the table) -> SCORES (full-screen table) -> MENU.  Every
// step is driven by pad, keyboard or touch; the caller goes back to its start
// menu on GAMEOVER_ACTION_MENU.
typedef enum {
    GAMEOVER_PHASE_SHOW,
    GAMEOVER_PHASE_NAME,
    GAMEOVER_PHASE_SCORES,
    GAMEOVER_PHASE_DONE,
} GameOverPhase;

typedef enum {
    GAMEOVER_ACTION_NONE,
    GAMEOVER_ACTION_MENU,       // flow finished: back to the start menu
} GameOverAction;

#define GAMEOVER_SHOW_MS    3000u    // SHOW holds this long unless skipped
#define GAMEOVER_ARM_MS     1000u    // input counts only after this much of SHOW
#define GAMEOVER_SCORES_MS  15000u   // = SM_ATTRACT_MS (checked in common.c)

// The phase decision, pure (no framebuffer, audio or touch) so a host test can
// drive it.  phase_ms = time in `cur`; input = a fresh button edge or touch
// press this frame; name_done = the name was entered and saved.  Returns the
// phase to be in: SHOW -> NAME (qualifies) or SCORES (table) or DONE (no table)
// once SHOW_MS has passed, or on input after ARM_MS; NAME -> SCORES when done;
// SCORES -> DONE after SCORES_MS or on input.
GameOverPhase gameover_phase_next(GameOverPhase cur, uint32_t phase_ms, bool input,
                                  bool qualifies, bool has_table, bool name_done);

typedef struct {
    GameOverPhase phase;
    int score;
    char title[64];           // Custom title or the default
    char info_line[64];       // Optional info (e.g., "LEVEL 5")
    char game_title[64];      // Heading of the SCORES page ("HIGH SCORES" if none)
    HighScoreTable *hs_table; // NULL if no highscore support
    TouchInput *touch;        // Required for blocking hs_enter_name()
    GamepadManager *gm;       // Pad / keyboard for name entry; NULL = touch only
    bool started;             // First update ran: qualification checked, clock set
    bool hs_qualifies;
    bool name_done;
    uint32_t phase_t0;        // get_time_ms() when the phase began
    bool prev_held[BTN_ID_COUNT];  // own edges, seeded TRUE: the press that ended the game does not skip
    bool was_touching;        // seeded TRUE likewise
    bool pending_draw;        // "I owe the screen a frame" — see gameover_needs_redraw()
} GameOverScreen;

// Initialize the game over screen. Call once when entering game-over state.
// game_title is the game's menu title for the SCORES page (NULL = "HIGH SCORES").
// hs_table can be NULL for games without highscore (2-player pong): no name
// entry and no SCORES page, SHOW then MENU.  gm may be NULL (touch only).
void gameover_init(GameOverScreen *gos, Framebuffer *fb,
                   int score, const char *title, const char *info_line,
                   const char *game_title, HighScoreTable *hs_table,
                   TouchInput *touch, GamepadManager *gm);

// Process the flow: reads input, runs the blocking name entry, draws the page.
// `in` is the game's InputState from this frame's gamepad_poll (may be NULL).
// Returns GAMEOVER_ACTION_MENU once, when the flow is over (and on every call
// after), NONE before.
GameOverAction gameover_update(GameOverScreen *gos, Framebuffer *fb,
                               int touch_x, int touch_y, bool touching,
                               const InputState *in);

// True while the component owes the screen a frame: a phase just changed, a
// phase's time is up, or there is input it has not been given a chance to act
// on.  An idle SCORES page reports false until its 15 s are up.
//
// gameover_update() is called from inside the game's *draw* function, which a
// dirty-flagged main loop runs only when needs_redraw is set. A loop that
// computes that flag without asking the component starves it. So OR this into
// the flag, handing it the same InputState the update gets:
//
//   if (current_screen == SCREEN_GAME_OVER && gameover_needs_redraw(&gos, &input))
//       needs_redraw = true;
//
// The input half matters as much as the draw half: a button edge is read
// inside the draw path, so a frame the loop declines to run is an input event
// never seen.  Do not assume the caller has its own "redraw on input activity"
// branch — samegame did not.
bool gameover_needs_redraw(const GameOverScreen *gos, const InputState *in);

// Draw the current page (SHOW overlay or SCORES), called from gameover_update
// but may also be called directly.
void gameover_draw(GameOverScreen *gos, Framebuffer *fb);

// ============================================================================
// MODAL DIALOG
// Reusable modal overlay with title, message, and 1–4 configurable buttons.
// Follows the GameOverScreen lifecycle pattern (init/update/draw).
// ============================================================================

#define MODAL_MAX_BUTTONS 4

typedef enum {
    MODAL_ACTION_NONE = -1,   /* No button pressed */
    MODAL_ACTION_BTN0 = 0,    /* First button pressed */
    MODAL_ACTION_BTN1 = 1,    /* Second button pressed */
    MODAL_ACTION_BTN2 = 2,    /* Third button pressed */
    MODAL_ACTION_BTN3 = 3     /* Fourth button pressed */
} ModalDialogAction;

typedef struct {
    bool active;
    char title[64];
    char message[128];        /* Optional description; "\n" starts a new line */
    uint8_t overlay_alpha;    /* 0=no overlay, 160-180=typical */
    int dialog_width;
    int dialog_height;
    uint32_t bg_color;        /* Dialog background */
    uint32_t border_color;    /* Dialog border */
    uint32_t title_color;     /* Title text color */
    uint32_t message_color;   /* Message text color */
    int button_count;         /* 1–4 */
    Button buttons[MODAL_MAX_BUTTONS];
    int focus;                /* keyboard focus, -1 none: modal_dialog_set_focus() */
} ModalDialog;

// Initialize modal dialog with title, message, and button count (1–4).
// After calling, configure each button with modal_dialog_set_button().
void modal_dialog_init(ModalDialog *dlg, const char *title, const char *message,
                       int button_count);

// Configure an individual button (index 0..button_count-1).
void modal_dialog_set_button(ModalDialog *dlg, int index, const char *text,
                             uint32_t bg_color, uint32_t text_color);

// Convenience: init a 2-button confirm/cancel dialog (wraps init + set_button).
void modal_dialog_init_confirm(ModalDialog *dlg, const char *title,
                               const char *message,
                               const char *confirm_text, uint32_t confirm_color,
                               const char *cancel_text, uint32_t cancel_color);

// Show/hide the dialog
void modal_dialog_show(ModalDialog *dlg);
void modal_dialog_hide(ModalDialog *dlg);
bool modal_dialog_is_active(ModalDialog *dlg);

// Keyboard/pad focus: the button Enter presses.  -1 (the default from init)
// is none, and the dialog then draws as it always has.  With a focus set it is
// ALWAYS drawn highlighted — it proposes the default answer, so it shows
// before any key is pressed.  Out-of-range indices are refused.
void modal_dialog_set_focus(ModalDialog *dlg, int index);
// Move the focus: Up/Left = previous button, Down/Right = next, wrapping.
// Does nothing while no focus is set.
void modal_dialog_focus_step(ModalDialog *dlg, UiDir d);

// Geometry of a dialog on a fb_w x fb_h screen, in screen pixels.  Pure: no
// framebuffer.  modal_dialog_draw() uses it and writes the button rects into
// dlg->buttons, which modal_dialog_update() hit-tests.  A zero w or h means the
// part is absent (no title, no message).
typedef struct {
    int x, y, w, h;
} ModalRect;

typedef struct {
    ModalRect dialog;
    ModalRect title;          /* title text box */
    ModalRect message;        /* bounding box of every message line */
    int title_cy;             /* centre y of the title text */
    int message_cy;           /* centre y of the first message line */
    ModalRect button[MODAL_MAX_BUTTONS];
} ModalDialogLayout;

void modal_dialog_layout(const ModalDialog *dlg, int fb_w, int fb_h,
                         ModalDialogLayout *out);

// Draw the dialog overlay and contents
void modal_dialog_draw(ModalDialog *dlg, Framebuffer *fb);

// Process touch input; returns MODAL_ACTION_NONE (-1) if no button pressed,
// or the button index (0–3) if pressed.  Auto-hides the dialog on press.
ModalDialogAction modal_dialog_update(ModalDialog *dlg, int touch_x, int touch_y,
                                       bool touch_active, uint32_t now_ms);

// Pad / keyboard on an open dialog, edge-triggered on `.pressed`: D-pad /
// arrows move the focus, A / Space / Enter press the focused button, and
// Start / Esc or Select / Backspace answer `cancel` (the caller's safe answer,
// e.g. RESUME).  A dialog with no focus adopts button 0, so the frame shows.
// Hides the dialog on an answer, like modal_dialog_update().  Call both.
ModalDialogAction modal_dialog_input(ModalDialog *dlg, const InputState *in,
                                     int cancel);

#endif // COMMON_H
