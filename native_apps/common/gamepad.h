/**
 * Gamepad Input Module for Arca
 *
 * Reusable input abstraction that merges gamepad (Xbox 360 via evdev),
 * keyboard, mouse, and touch into a unified API.  Used by all native games.
 *
 * Hardware: TI AM335x, Linux 4.14.52, evdev input subsystem.
 *
 * Mouse support added for USB mice — provides absolute cursor position
 * updated from relative movements, with configurable acceleration.
 *
 * Gamepad button mapping allows remapping evdev codes for clone controllers.
 *
 * Configuration persistence via /etc/input_config.conf (key=value format).
 */

#ifndef GAMEPAD_H
#define GAMEPAD_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>

/* InputConfig (the one /etc/input_config.conf parser, and its defaults) and
 * InputSigGate (the hot-plug check) live there. */
#include "input_scan.h"
/* InputSlotTable: which player (P1..P4) each pad or keyboard is. */
#include "input_slots.h"
/* Config: the slot pins persist as slot_p1..slot_p4 in CONFIG_FILE_PATH. */
#include "config.h"

/* Most pads held open at once: one per player slot. */
#define GAMEPAD_MAX_PADS INPUT_SLOTS
/* The latch bucket of a device the slot table had no room for (or of a pad fd
 * planted by hand): it counts for gamepad_poll()'s any-device state only. */
#define GAMEPAD_NO_SLOT  INPUT_SLOTS
#define GAMEPAD_BUCKETS  (INPUT_SLOTS + 1)

/* Room for the "<name> at <path>" string remembered per slot, so a rescan can
   tell an unchanged device from a swapped one: input_scan.h reads EVIOCGNAME
   into 128 bytes and the path into 32, plus " at " and the terminator. */
#define GAMEPAD_ANNOUNCE_LEN 200

/* Most keyboard nodes, and most mouse nodes, held open at once.  Every node of
   those two kinds is read, because one physical device commonly exposes more
   than one (a keyboard with a built-in touchpad has a keyboard node AND a mouse
   node) and a 2.4 GHz receiver plugged in beside it adds more. */
#define GAMEPAD_MAX_PER_KIND 4

/* Axis dead zone (for analog sticks) — legacy default, now configurable */
#define GAMEPAD_DEADZONE 200

/* Axis range (normalized to +/-1000) */
#define GAMEPAD_AXIS_MAX 1000

/* Maximum touch regions */
#define GAMEPAD_MAX_TOUCH_REGIONS 10

/* Last-resort mouse bounds, used only if the framebuffer globals are unset.
 * gamepad_init() takes the bounds from screen_base_width/height, whose own
 * defaults are these same numbers. */
#define GAMEPAD_DEFAULT_SCREEN_W 800
#define GAMEPAD_DEFAULT_SCREEN_H 480

/* Maximum axes tracked for calibration */
#define GAMEPAD_MAX_AXES 8

/* Button state with edge detection.
 *
 * All three fields are pure *outputs* of gamepad_poll() — it recomputes them
 * from scratch every call, so writing them from an app has no effect beyond
 * the next poll.  `held` is the OR of two kinds of source: event-driven ones
 * that report press/release and whose level therefore lives in the manager
 * (GamepadManager.held_latched), and per-frame ones (touch regions, analog
 * stick) that report an absolute position and are rebuilt each poll. */
typedef struct {
    bool held;      /* Currently held down (level) */
    bool pressed;   /* Just pressed this frame (edge) */
    bool released;  /* Just released this frame (edge) */
} ButtonState;

/* Abstract input buttons (unified across gamepad/keyboard/touch) */
typedef enum {
    BTN_ID_UP,
    BTN_ID_DOWN,
    BTN_ID_LEFT,
    BTN_ID_RIGHT,
    BTN_ID_JUMP,      /* Gamepad A/South, Keyboard Space, Touch jump button */
    BTN_ID_RUN,       /* Gamepad B/East, Keyboard Shift */
    BTN_ID_ACTION,    /* Gamepad X/West, Keyboard Enter */
    BTN_ID_PAUSE,     /* Gamepad Start, Keyboard Escape */
    BTN_ID_BACK,      /* Gamepad Select/Back, Keyboard Backspace */
    BTN_ID_COUNT
} ButtonId;

/**
 * Configurable gamepad button mapping.
 *
 * Maps abstract game actions to evdev button/axis codes.
 * Defaults match a genuine Xbox 360 controller.  Override for
 * clone/knockoff controllers that use different codes.
 */
typedef struct {
    /* Face buttons — evdev key codes */
    int btn_jump;       /* Default: BTN_SOUTH (304) — A on Xbox */
    int btn_run;        /* Default: BTN_EAST  (305) — B on Xbox */
    int btn_action;     /* Default: BTN_WEST  (308) — X on Xbox */
    int btn_pause;      /* Default: BTN_START (315) */
    int btn_back;       /* Default: BTN_SELECT (314) */

    /* D-pad axes */
    int hat_x_axis;     /* Default: ABS_HAT0X (16) */
    int hat_y_axis;     /* Default: ABS_HAT0Y (17) */

    /* Analog stick axes */
    int stick_x_axis;   /* Default: ABS_X  (0) — left stick X */
    int stick_y_axis;   /* Default: ABS_Y  (1) — left stick Y */
    int stick_rx_axis;  /* Default: ABS_RX (3) — right stick X */
    int stick_ry_axis;  /* Default: ABS_RY (4) — right stick Y */
} GamepadButtonMap;

/**
 * Per-axis calibration data for dead zone and center offset.
 */
typedef struct {
    int center;         /* Measured center value (some cheap pads don't center at 0) */
    int deadzone_pct;   /* Dead zone as percentage of half-range (0-100) */
} AxisCalibration;

/**
 * Mouse acceleration parameters.
 */
typedef struct {
    float sensitivity;      /* Base sensitivity multiplier (default 1.5) */
    float acceleration;     /* Extra acceleration factor for fast moves (default 2.0) */
    int   low_threshold;    /* Speed below this = 1:1 precision (default 3) */
    int   high_threshold;   /* Speed above this = extra acceleration (default 15) */
} MouseAccelConfig;

/* Unified input state */
typedef struct {
    ButtonState buttons[BTN_ID_COUNT];
    int axis_lx;    /* Left stick X: -1000 to +1000 */
    int axis_ly;    /* Left stick Y: -1000 to +1000 */
    int axis_rx;    /* Right stick X: -1000 to +1000 */
    int axis_ry;    /* Right stick Y: -1000 to +1000 */
    bool gamepad_connected;
    bool keyboard_connected;

    /* Mouse cursor state (absolute position on screen, updated by relative movements) */
    int mouse_x, mouse_y;              /* Current cursor position (0..screen_w-1, 0..screen_h-1) */
    int mouse_connected;               /* Whether a mouse device is detected */
    int mouse_left_held,   mouse_left_pressed,   mouse_left_released;
    int mouse_right_held,  mouse_right_pressed,  mouse_right_released;
    int mouse_middle_held, mouse_middle_pressed,  mouse_middle_released;
    /* This poll's motion after acceleration — what moved mouse_x/y, before
     * its clamp — for a pointer that keeps its own position (pointer.c,
     * clamped to SCREEN_SAFE_*).  0 on a quiet poll. */
    int mouse_dx, mouse_dy;
} InputState;

/* Touch button region — maps a screen area to an abstract button */
typedef struct {
    int x, y, w, h;
    ButtonId button;
} TouchRegion;

/* Gamepad manager (holds evdev fds and internal state) */
typedef struct {
    /* Up to GAMEPAD_MAX_PADS pads, in event-node order.  Each pad, and each
     * keyboard (all of its nodes), is a player: input_slots.h gives it a slot
     * P1..P4 by its identity, and its latched levels, axes and edges are kept
     * per slot (gamepad_player()).  A device the table has no room for still
     * counts for the any-device state, through the GAMEPAD_NO_SLOT bucket.
     * Mice and touch belong to nobody: one cursor, no slot. */
    union {
        int gamepad_fd;                  /* the first pad's fd (pad_fds[0]) */
        int pad_fds[GAMEPAD_MAX_PADS];   /* -1 = none; not packed, any may be open */
    };
    /* Each pad's InputPadLayout (input_scan.h), taken with the fd: every EV_KEY
     * code read from it goes through input_pad_key() before button_map sees it.
     * 0 is the native layout. */
    int pad_layout[GAMEPAD_MAX_PADS];
    int pad_slot[GAMEPAD_MAX_PADS];      /* player slot, or -1 (no slot) */
    int keyboard_fds[GAMEPAD_MAX_PER_KIND];
    int keyboard_slot[GAMEPAD_MAX_PER_KIND];
    int keyboard_count;
    int mouse_fds[GAMEPAD_MAX_PER_KIND];
    /* The slot a mouse node's keys latch into; -1 when it has none.  A node
     * that also carries a keyboard (mouse_keys: a keyboard+touchpad combo on
     * one node) owns its slot as a keyboard does; a plain mouse borrows the
     * slot of a keyboard node with its identity, if one is present. */
    int mouse_slot[GAMEPAD_MAX_PER_KIND];
    bool mouse_keys[GAMEPAD_MAX_PER_KIND];
    int mouse_count;

    /* Button level per mouse node (left, right, middle), so the output is the
     * OR across mice: releasing a button on one mouse must not release it for
     * another that is still holding it.  Seeded from EVIOCGKEY at open, so a
     * button held across a rescan is still held after it. */
    bool mouse_btn[GAMEPAD_MAX_PER_KIND][3];

    /* Internal previous-frame state for edge detection (abstract buttons) of
     * the any-device state, and of each player */
    bool prev_held[BTN_ID_COUNT];
    bool player_prev[INPUT_SLOTS][BTN_ID_COUNT];

    /* Level state for the event-driven sources (gamepad keys, D-pad hat,
     * keyboard), per slot plus GAMEPAD_NO_SLOT.  Those arrive as discrete
     * press/release events, so their level has to persist between polls — and
     * it persists *here* rather than in the caller's InputState, so a caller
     * that zeroes or swaps its InputState can never desync it.  Sources that
     * report an absolute position instead (touch regions, analog stick) are
     * deliberately NOT in here: they are rebuilt per frame, which is what
     * stops them latching. */
    bool held_latched[GAMEPAD_BUCKETS][BTN_ID_COUNT];
    /* Each bucket's stick positions (lx, ly, rx, ry), kept across a rescan
     * (an unmoved stick sends no event on the reopened node) and zeroed while
     * no pad of that bucket is open. */
    int bucket_axis[GAMEPAD_BUCKETS][4];
    /* gamepad_player()'s answers, rebuilt by every gamepad_poll(). */
    InputState players[INPUT_SLOTS];

    /* Internal previous-frame state for mouse button edge detection */
    bool prev_mouse_left;
    bool prev_mouse_right;
    bool prev_mouse_middle;
    /* A BTN_LEFT down event arrived during this poll.  A click shorter than
     * the poll interval (at FRAME_DELAY_IDLE_US, 100 ms) ends released with no
     * level change to see, so this is what gives it its press and release. */
    bool mouse_left_down_ev;

    /* Axis calibration data per pad (up to GAMEPAD_MAX_AXES axes) */
    int axis_min[GAMEPAD_MAX_PADS][GAMEPAD_MAX_AXES];
    int axis_max[GAMEPAD_MAX_PADS][GAMEPAD_MAX_AXES];
    int axis_flat[GAMEPAD_MAX_PADS][GAMEPAD_MAX_AXES];

    /* Per-axis center calibration and configurable dead zone */
    AxisCalibration axis_calib[GAMEPAD_MAX_PADS][GAMEPAD_MAX_AXES];

    /* Configurable dead zone percentage (0-100), applied uniformly unless
     * per-axis overrides are set via axis_calib[].deadzone_pct */
    int deadzone_pct;

    /* Touch regions for virtual controls */
    TouchRegion touch_regions[GAMEPAD_MAX_TOUCH_REGIONS];
    int touch_region_count;

    /* Configurable button mapping for gamepad */
    GamepadButtonMap button_map;

    /* Mouse state */
    int mouse_x, mouse_y;                  /* Accumulated absolute position */
    int mouse_screen_w, mouse_screen_h;    /* Bounds for clamping */
    MouseAccelConfig mouse_accel;          /* Acceleration parameters */

    /* Last announcement made for each slot, as "<name> at <path>", so that a
     * rescan finding the same device again stays silent.  Deliberately NOT
     * cleared by gamepad_close(): gamepad_rescan() calls that first, and a
     * close/reopen of an unchanged device is not a change worth a log line.
     * gamepad_init()'s memset is what makes an empty string mean "nothing
     * announced yet". */
    char announced_gamepad[GAMEPAD_MAX_PADS][GAMEPAD_ANNOUNCE_LEN];
    char announced_keyboard[GAMEPAD_MAX_PER_KIND][GAMEPAD_ANNOUNCE_LEN];
    char announced_mouse[GAMEPAD_MAX_PER_KIND][GAMEPAD_ANNOUNCE_LEN];

    /* gamepad_tick()'s hot-plug check.  Every full scan makes its own
     * input_node_sig() the baseline, so a re-init (app_launcher, after each
     * child exits) starts from the nodes it just opened. */
    InputSigGate node_gate;
    /* A read on a held fd failed with ENODEV/EBADF: that fd is already closed,
     * and the next gamepad_tick() rescans whatever the fingerprint says. */
    bool rescan_pending;

    /* The player slots.  Like announced_*, it survives gamepad_close() and so
     * every rescan — a pad unplugged and plugged back gets its slot again —
     * and gamepad_init() clears it: an app exit forgets every reservation. */
    InputSlotTable slots;

    /* Each open node's identity, name and path, taken at the scan, so a pin
     * can re-bucket the open nodes without reopening them and
     * gamepad_devices() can list them.  Indexed like the fd arrays. */
    InputIdent pad_ident[GAMEPAD_MAX_PADS];
    InputIdent keyboard_ident[GAMEPAD_MAX_PER_KIND];
    InputIdent mouse_ident[GAMEPAD_MAX_PER_KIND];
    char pad_name[GAMEPAD_MAX_PADS][INPUT_SCAN_NAME_LEN];
    char keyboard_name[GAMEPAD_MAX_PER_KIND][INPUT_SCAN_NAME_LEN];
    char pad_path[GAMEPAD_MAX_PADS][INPUT_SCAN_PATH_LEN];
    char keyboard_path[GAMEPAD_MAX_PER_KIND][INPUT_SCAN_PATH_LEN];
    char mouse_name[GAMEPAD_MAX_PER_KIND][INPUT_SCAN_NAME_LEN];
    char mouse_path[GAMEPAD_MAX_PER_KIND][INPUT_SCAN_PATH_LEN];

    /* The file the pins are read from and written to: CONFIG_FILE_PATH from
     * gamepad_init(), or the path last given to gamepad_load_slot_pins(). */
    char slot_config_path[128];
} GamepadManager;

/* One connected pad or keyboard, as gamepad_devices() lists it. */
typedef struct {
    char       name[INPUT_SCAN_NAME_LEN]; /* EVIOCGNAME of its first node */
    char       path[INPUT_SCAN_PATH_LEN]; /* that node, /dev/input/eventN */
    InputIdent ident;
    int        slot;       /* 0..INPUT_SLOTS-1, or -1: the table had no room */
    bool       pinned;     /* slot >= 0 and the operator pinned it there */
    bool       keyboard;   /* false: a pad */
} GamepadDevice;

/**
 * The per-kind limits the scan hands to input_scan(), indexed by InputKind
 * (common/input_scan.h), for a test to drive input_select() with: the first
 * GAMEPAD_MAX_PADS pads, and every keyboard and every mouse up to
 * GAMEPAD_MAX_PER_KIND each.  Classification, the touchscreen
 * exclusion and the /dev/input/event* walk are input_scan's, not this file's.
 */
const int *gamepad_scan_caps(void);

/**
 * Initialize the gamepad manager — scans /dev/input/event* for gamepad,
 * keyboard, and mouse.  Automatically loads config from INPUT_CONFIG_PATH
 * if the file exists.
 * Returns 0 on success (even if no devices found — they can be hot-plugged).
 */
int gamepad_init(GamepadManager *gm);

/**
 * Close all open device fds.
 */
void gamepad_close(GamepadManager *gm);

/**
 * Poll all input devices and update the input state.
 * Call once per frame before reading state.
 * Also accepts touch coordinates for virtual button mapping.
 */
void gamepad_poll(GamepadManager *gm, InputState *state,
                  int touch_x, int touch_y, bool touch_active);

/**
 * One player's input, as of the last gamepad_poll(): the buttons, edges and
 * stick axes of the pad or keyboard in `slot` (0..INPUT_SLOTS-1 = P1..P4) —
 * no touch, no mouse.  Its gamepad_connected / keyboard_connected say what
 * that slot holds now.  gamepad_poll()'s own state stays "any device": the OR
 * of every slot, plus touch and the mouse.  Out of range: an all-zero state.
 */
const InputState *gamepad_player(const GamepadManager *gm, int slot);

/**
 * Bit s set when slot s holds an open pad or keyboard right now.
 */
int gamepad_player_mask(const GamepadManager *gm);

/* ── Pinning a device to a player slot (the Control Panel's Input page) ──
 *
 * A pin is input_slots_pin() (input_slots.h, Pinning) plus persistence: the
 * four keys slot_p1..slot_p4 hold each pinned slot's identity in
 * input_ident_format()'s text form, and an unpinned slot's key is REMOVED,
 * not written empty (config_get returns "" for "slot_p1=", which parses as no
 * pin too, but a removed key keeps the file free of four dead lines).
 * gamepad_init() loads them, so every app process sees the operator's choice.
 */

/**
 * Read slot_p1..slot_p4 from `path` and pin each value that parses; a missing
 * file or a bad value is no pin, silently.  `path` becomes the file
 * gamepad_slot_pin()/unpin() write.  Re-buckets the open nodes at once.
 * gamepad_init() calls it with CONFIG_FILE_PATH.  Returns the pins applied.
 */
int gamepad_load_slot_pins(GamepadManager *gm, const char *path);

/**
 * Every connected pad and keyboard, one entry per identity (a keyboard's
 * several nodes are one; a keyboard+touchpad combo's mouse node counts as a
 * keyboard), into out[max], ordered by event node number so a list does not
 * reshuffle between frames.  Returns the count written.
 */
int gamepad_devices(const GamepadManager *gm, GamepadDevice *out, int max);

/**
 * What `slot` holds: false when it holds no identity (then the outputs are
 * untouched).  `present`: the device is open now — false for a pinned pad
 * that is unplugged.  Any output pointer may be NULL.
 */
bool gamepad_slot_info(const GamepadManager *gm, int slot, InputIdent *id,
                       bool *pinned, bool *present);

/**
 * Pin `id` to `slot` (it leaves any other slot, which reverts to auto),
 * re-bucket the open nodes so gamepad_player() follows on the next poll, and
 * persist all four keys: into the file (re-read, set, saved — another app's
 * keys survive) and, when `mem` is non-NULL, into that in-memory Config too,
 * so its owner's later whole-file save does not write the old pins back.
 * Returns slot; -1 for a bad slot or a NULL id (nothing changed); -2 when the
 * pin took effect but the file could not be written.
 */
int gamepad_slot_pin(GamepadManager *gm, const InputIdent *id, int slot,
                     Config *mem);

/**
 * `slot` back to auto: its holder stays as an ordinary reservation.  Persists
 * as gamepad_slot_pin() does.  Returns 0; -1 for a bad slot; -2 when the file
 * could not be written.
 */
int gamepad_slot_unpin(GamepadManager *gm, int slot, Config *mem);

/**
 * Close every device and scan again.  Apps call gamepad_tick() instead, which
 * calls this only when something under /dev/input changed.
 */
void gamepad_rescan(GamepadManager *gm);

/**
 * The hot-plug check: call once per frame, after gamepad_poll(), with the
 * app's millisecond clock.  Every INPUT_SIG_CHECK_MS it lists /dev/input
 * (input_node_sig(), which opens no device) and calls gamepad_rescan() only if
 * that listing changed — or at once, if a read found a device gone.  An
 * unchanged set of nodes is never closed and reopened.
 */
void gamepad_tick(GamepadManager *gm, uint32_t now_ms);

/**
 * Configure touch button regions for virtual controls.
 * These define screen areas that map to abstract buttons.
 */
void gamepad_set_touch_regions(GamepadManager *gm, TouchRegion *regions, int count);

/**
 * Draw virtual touch controls on screen (if no gamepad/keyboard detected).
 * This is optional — games can draw their own or skip.
 * fb parameter is cast internally to Framebuffer*.
 */
void gamepad_draw_touch_controls(void *fb, InputState *state);

/* ── Mouse API ──────────────────────────────────────────────────────────── */

/**
 * Set the screen bounds used for clamping the mouse cursor position.
 *
 * Rarely needed: gamepad_init() already takes them from the framebuffer's
 * logical size, so call this only to override that. ⚠️ It does NOT survive a
 * gamepad_init() — the launcher apps re-init after every child exits, which
 * resets the bounds to the framebuffer's size and re-centres the cursor.
 */
void gamepad_set_mouse_bounds(GamepadManager *gp, int width, int height);

/**
 * Warp the mouse cursor to an absolute screen position.
 * Clamped to current screen bounds.
 */
void gamepad_set_mouse_position(GamepadManager *gp, int x, int y);

/* ── Button Mapping API ─────────────────────────────────────────────────── */

/**
 * Override the gamepad button mapping.  Pass NULL to reset to defaults.
 */
void gamepad_set_button_map(GamepadManager *gp, const GamepadButtonMap *map);

/**
 * Return a GamepadButtonMap initialized with Xbox 360 defaults.
 */
GamepadButtonMap gamepad_get_default_button_map(void);

/* ── Configuration Persistence ──────────────────────────────────────────── */

/**
 * Load input configuration from a key=value file.
 * Missing keys get sensible defaults.  Unknown keys are ignored.
 * Returns 0 on success, -1 if the file could not be opened (defaults apply).
 */
int gamepad_load_config(GamepadManager *gp, const char *path);

/**
 * Save current input configuration to a key=value file.
 * Returns 0 on success, -1 on write error.
 */
int gamepad_save_config(const GamepadManager *gp, const char *path);

#ifdef __cplusplus
}
#endif

#endif /* GAMEPAD_H */
