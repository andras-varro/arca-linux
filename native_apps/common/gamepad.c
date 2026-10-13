/**
 * Gamepad Input Module for Arca — Implementation
 *
 * Scans evdev devices for gamepad, keyboard, and mouse, reads events via
 * non-blocking reads, normalizes axes, and provides unified input state
 * with edge detection.  Touch regions map screen areas to virtual buttons.
 *
 * Mouse support: reads REL_X/REL_Y relative movements and accumulates them
 * into an absolute cursor position with configurable acceleration.
 *
 * Gamepad button mapping: remappable evdev codes for clone controllers.
 *
 * Config persistence: key=value file at /etc/input_config.conf.
 */

#include "gamepad.h"
#include "framebuffer.h"
#include "hardware.h"
#include "input_scan.h"
#include "blank_decide.h"
#define LOGGER_LIB_CLIENT
#include "logger.h"

/* Level of gamepad_init()'s routine reports (pins, config, the summary line).
 * app_launcher re-runs gamepad_init() around every child, so only the process's
 * first one reports at INFO and every later one at DEBUG.  The found / no longer
 * present announcements keep INFO: they already print only on a change. */
static bool     gp_announced;
static LogLevel gp_note = LOG_LEVEL_INFO;

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <math.h>
#include <sys/ioctl.h>
#include <linux/input.h>
#include <linux/input-event-codes.h>
#include <errno.h>

/* ── Forward declarations ───────────────────────────────────────────────── */
static void apply_defaults(GamepadManager *gm);

/* ── Return Xbox 360 default button map ─────────────────────────────────── */
GamepadButtonMap gamepad_get_default_button_map(void) {
    InputConfig def;
    input_config_defaults(&def);   /* the one set of defaults, shared */
    GamepadButtonMap map;
    map.btn_jump      = def.gamepad_btn_jump;
    map.btn_run       = def.gamepad_btn_run;
    map.btn_action    = def.gamepad_btn_action;
    map.btn_pause     = def.gamepad_btn_pause;
    map.btn_back      = def.gamepad_btn_back;
    map.hat_x_axis    = def.gamepad_hat_x;
    map.hat_y_axis    = def.gamepad_hat_y;
    map.stick_x_axis  = def.gamepad_stick_lx;
    map.stick_y_axis  = def.gamepad_stick_ly;
    map.stick_rx_axis = def.gamepad_stick_rx;
    map.stick_ry_axis = def.gamepad_stick_ry;
    return map;
}

/* ── Axis index mapping ─────────────────────────────────────────────────── */
/* We track 8 axes: indices 0-7 correspond to the evdev axis codes listed
 * in the axes[] array used by load_axis_calibration.  This helper returns
 * the internal index for a given evdev axis code, or -1 if not tracked. */
static const int g_tracked_axes[] = {
    /* 0 */ 0 /* ABS_X */,
    /* 1 */ 1 /* ABS_Y */,
    /* 2 */ 2 /* ABS_Z */,
    /* 3 */ 3 /* ABS_RX */,
    /* 4 */ 4 /* ABS_RY */,
    /* 5 */ 5 /* ABS_RZ */,
    /* 6 */ 16 /* ABS_HAT0X */,
    /* 7 */ 17 /* ABS_HAT0Y */
};

static int axis_to_index(int evdev_code) {
    for (int i = 0; i < GAMEPAD_MAX_AXES; i++) {
        if (g_tracked_axes[i] == evdev_code)
            return i;
    }
    return -1;
}

/* ── Load axis calibration data from EVIOCGABS ──────────────────────────── */
static void load_axis_calibration(GamepadManager *gm, int p) {
    if (gm->pad_fds[p] < 0) return;

    /* Axes we care about: ABS_X(0), ABS_Y(1), ABS_Z(2), ABS_RX(3), ABS_RY(4), ABS_RZ(5), ABS_HAT0X(16), ABS_HAT0Y(17) */
    static const int axes[] = { ABS_X, ABS_Y, ABS_Z, ABS_RX, ABS_RY, ABS_RZ, ABS_HAT0X, ABS_HAT0Y };

    for (int i = 0; i < GAMEPAD_MAX_AXES; i++) {
        struct input_absinfo info;
        if (ioctl(gm->pad_fds[p], EVIOCGABS(axes[i]), &info) == 0) {
            gm->axis_min[p][i]  = info.minimum;
            gm->axis_max[p][i]  = info.maximum;
            gm->axis_flat[p][i] = info.flat;

            /* BUG-INPUT-003 FIX: Compute center as midpoint of the axis range
             * instead of trusting info.value from EVIOCGABS.  The kernel reports
             * info.value as the *current* axis position at device-open time —
             * if the stick isn't perfectly centered (e.g. user is touching it,
             * or cheap analog has drift), this reads a non-centered value as
             * "center", causing permanent directional drift in all games.
             * The axis range (minimum/maximum) is always correct, so the
             * geometric midpoint is a far more reliable center reference. */
            gm->axis_calib[p][i].center = (info.minimum + info.maximum) / 2;
        } else {
            gm->axis_min[p][i]  = 0;
            gm->axis_max[p][i]  = 0;
            gm->axis_flat[p][i] = 0;
            gm->axis_calib[p][i].center = 0;
        }
        /* Apply global dead zone default unless already configured */
        if (gm->axis_calib[p][i].deadzone_pct <= 0)
            gm->axis_calib[p][i].deadzone_pct = gm->deadzone_pct;
    }
}

/* ── Normalize axis value to -1000..+1000 with configurable dead zone ──── */
static int normalize_axis_calibrated(int value, int min_val, int max_val,
                                     int center, int deadzone_pct) {
    if (max_val == min_val) return 0;

    int half_range = (max_val - min_val) / 2;
    if (half_range == 0) return 0;

    /* Use calibrated center (midpoint of axis range — see load_axis_calibration) */
    int offset = value - center;

    /* Compute dead zone in raw axis units */
    int dz = (half_range * deadzone_pct) / 100;

    /* Apply dead zone */
    if (offset > -dz && offset < dz)
        return 0;

    /* Remove dead zone from effective range */
    int effective_range = half_range - dz;
    if (effective_range <= 0) return 0;

    if (offset > 0)
        offset -= dz;
    else
        offset += dz;

    int normalized = (offset * GAMEPAD_AXIS_MAX) / effective_range;
    if (normalized < -GAMEPAD_AXIS_MAX) normalized = -GAMEPAD_AXIS_MAX;
    if (normalized >  GAMEPAD_AXIS_MAX) normalized =  GAMEPAD_AXIS_MAX;
    return normalized;
}

/* ── Legacy normalize (for backward compat in edge cases) ───────────────── */
static int normalize_axis(int value, int min_val, int max_val) {
    if (max_val == min_val) return 0;
    int mid = (min_val + max_val) / 2;
    int half_range = (max_val - min_val) / 2;
    if (half_range == 0) return 0;
    int normalized = ((value - mid) * GAMEPAD_AXIS_MAX) / half_range;
    if (normalized < -GAMEPAD_AXIS_MAX) normalized = -GAMEPAD_AXIS_MAX;
    if (normalized >  GAMEPAD_AXIS_MAX) normalized =  GAMEPAD_AXIS_MAX;
    return normalized;
}

/* ── Announce a binding, but only when it is a CHANGE ───────────────────── */
/* gamepad_rescan() closes every device and re-opens it (it once ran on a 5 s
 * timer in all nine apps), so the `fd < 0` test that guards each of these prints is always
 * true by the time scan_devices() runs — an unchanged pad used to be announced
 * once per tick.  Measured on .188: 1720 identical "found gamepad" lines in one
 * session, in a log whose whole value is that it is the only instrument an
 * audio verification with no microphone can read.
 *
 * The remembered string is "<name> at <path>", so a pad that was swapped for a
 * different one, or that came back on a different event node, still prints —
 * those are real changes.  The printed line is byte-identical to the old one,
 * because it is what a reader greps for. */
static void announce_found(char *slot, size_t slot_sz, const char *kind,
                           const char *name, const char *path) {
    char desc[GAMEPAD_ANNOUNCE_LEN];
    snprintf(desc, sizeof(desc), "%s at %s", name, path);
    if (strcmp(slot, desc) == 0) return;
    snprintf(slot, slot_sz, "%s", desc);
    LIB_LOG(LOG_LEVEL_INFO, "gamepad: found %s '%s' at %s", kind, name, path);
}

/* The other half of the same rule: a device going away is a change too, and
 * without this the log would fall silent at unplug and say nothing at all when
 * the device came back a different way.  Clearing the slot is what lets the
 * next successful bind announce itself. */
static void announce_lost(char *slot, const char *kind, int fd) {
    if (fd >= 0 || slot[0] == '\0') return;
    LIB_LOG(LOG_LEVEL_INFO, "gamepad: %s no longer present (%s)", kind, slot);
    slot[0] = '\0';
}

/* ── Which scanned nodes to keep (see gamepad.h) ────────────────────────── */
/* One pad per player slot; every keyboard and every mouse node up to
 * GAMEPAD_MAX_PER_KIND each.  The touchscreen is never kept: input_scan
 * classifies it as nothing. */
static const int g_scan_cap[INPUT_KIND_COUNT] = {
    [INPUT_KIND_PAD]      = GAMEPAD_MAX_PADS,
    [INPUT_KIND_KEYBOARD] = GAMEPAD_MAX_PER_KIND,
    [INPUT_KIND_MOUSE]    = GAMEPAD_MAX_PER_KIND,
};
#define GAMEPAD_SCAN_SLOTS (GAMEPAD_MAX_PADS + 2 * GAMEPAD_MAX_PER_KIND)

const int *gamepad_scan_caps(void) { return g_scan_cap; }

/* The latch bucket of a device in `slot` (-1: none, see GAMEPAD_NO_SLOT). */
static int bucket_of(int slot) {
    return (slot >= 0 && slot < INPUT_SLOTS) ? slot : GAMEPAD_NO_SLOT;
}

/* What input_slots keys a device by.  Read here, not in input_scan.c, which
 * vnc_client and ScummVM link too.  A failed ioctl leaves its field empty (no
 * uniq is the normal case for a wired device); a node that reports no phys is
 * told apart by its path instead, so two such nodes are never one player. */
static void read_ident(int fd, const char *path, InputIdent *id) {
    struct input_id iid;
    memset(id, 0, sizeof(*id));
    if (ioctl(fd, EVIOCGID, &iid) == 0) {
        id->bus = iid.bustype; id->vid = iid.vendor; id->pid = iid.product;
    }
    if (ioctl(fd, EVIOCGUNIQ(sizeof(id->uniq) - 1), id->uniq) < 0)
        memset(id->uniq, 0, sizeof(id->uniq));
    if (ioctl(fd, EVIOCGPHYS(sizeof(id->phys) - 1), id->phys) < 0)
        memset(id->phys, 0, sizeof(id->phys));
    if (id->phys[0] == '\0')
        snprintf(id->phys, sizeof(id->phys), "%.*s", (int)sizeof(id->phys) - 1, path);
}

/* A button already held when the node is opened (typically: held across a
 * rescan) produces no press event, so ask the kernel for the current level. */
static void seed_mouse_buttons(int fd, bool *btn) {
    unsigned long keys[INPUT_SCAN_NLONGS(KEY_MAX + 1)];
    memset(keys, 0, sizeof(keys));
    btn[0] = btn[1] = btn[2] = false;
    if (ioctl(fd, EVIOCGKEY(sizeof(keys)), keys) < 0) return;
    btn[0] = input_caps_test(keys, BTN_LEFT);
    btn[1] = input_caps_test(keys, BTN_RIGHT);
    btn[2] = input_caps_test(keys, BTN_MIDDLE);
}

static void latch_key(bool *latched, int code, bool down);

/* The same, for the latched buttons, after a rescan has reopened every node:
 * each device's levels go into its own slot's bucket.  Only ever ORs a level
 * in: called with held_latched[] already zeroed.  Not called at
 * gamepad_init(), so the key that launched an app is not seen as a fresh
 * press by it. */
static void seed_latched_levels(GamepadManager *gm) {
    unsigned long keys[INPUT_SCAN_NLONGS(KEY_MAX + 1)];
    const GamepadButtonMap *m = &gm->button_map;

    /* Keyboards, and the keys of a keyboard+touchpad combo's mouse node
     * (latch_key ignores the mouse buttons themselves). */
    for (int k = 0; k < gm->keyboard_count + gm->mouse_count; k++) {
        bool kbd = k < gm->keyboard_count;
        int j = kbd ? k : k - gm->keyboard_count;
        int fd = kbd ? gm->keyboard_fds[j] : gm->mouse_fds[j];
        bool *latched = gm->held_latched[bucket_of(kbd ? gm->keyboard_slot[j]
                                                       : gm->mouse_slot[j])];
        memset(keys, 0, sizeof(keys));
        if (fd < 0 || ioctl(fd, EVIOCGKEY(sizeof(keys)), keys) < 0) continue;
        for (int code = 0; code <= KEY_MAX; code++)
            if (input_caps_test(keys, code)) latch_key(latched, code, true);
    }

    for (int p = 0; p < GAMEPAD_MAX_PADS; p++) {
        int fd = gm->pad_fds[p];
        if (fd < 0) continue;
        bool *latched = gm->held_latched[bucket_of(gm->pad_slot[p])];
        memset(keys, 0, sizeof(keys));
        if (ioctl(fd, EVIOCGKEY(sizeof(keys)), keys) >= 0) {
            /* The same mapping poll_gamepad() applies to EV_KEY.  The bitmap
             * is indexed by RAW code and the map holds native codes, so walk
             * the held raw codes and translate each, as poll_gamepad() does
             * an event. */
            const struct { int btn_id; int code; } pad_btns[] = {
                { BTN_ID_JUMP,   m->btn_jump   },
                { BTN_ID_RUN,    m->btn_run    },
                { BTN_ID_ACTION, m->btn_action },
                { BTN_ID_PAUSE,  m->btn_pause  },
                { BTN_ID_BACK,   m->btn_back   },
            };
            for (int raw = 0; raw <= KEY_MAX; raw++) {
                if (!input_caps_test(keys, raw)) continue;
                int code = input_pad_key((InputPadLayout)gm->pad_layout[p], raw);
                for (size_t i = 0; i < sizeof(pad_btns) / sizeof(pad_btns[0]); i++)
                    if (code == pad_btns[i].code)
                        latched[pad_btns[i].btn_id] = true;
            }
        }
        struct input_absinfo ai;
        if (m->hat_x_axis >= 0 && m->hat_x_axis <= ABS_MAX &&
            ioctl(fd, EVIOCGABS(m->hat_x_axis), &ai) == 0) {
            if (ai.value < 0) latched[BTN_ID_LEFT]  = true;
            if (ai.value > 0) latched[BTN_ID_RIGHT] = true;
        }
        if (m->hat_y_axis >= 0 && m->hat_y_axis <= ABS_MAX &&
            ioctl(fd, EVIOCGABS(m->hat_y_axis), &ai) == 0) {
            if (ai.value < 0) latched[BTN_ID_UP]   = true;
            if (ai.value > 0) latched[BTN_ID_DOWN] = true;
        }
    }
}

/* An open mouse node that also carries a keyboard: it takes a slot of its own
 * and is listed, counted and connected as a keyboard. */
static bool mouse_is_keyboard(const GamepadManager *gm, int k) {
    return gm->mouse_fds[k] >= 0 && gm->mouse_keys[k];
}

/* A plain mouse node takes no slot of its own; it borrows the slot of a
 * keyboard present now with the same identity (a keyboard+touchpad combo on
 * two nodes), for the keys it carries. */
static void assign_mouse_slots(GamepadManager *gm) {
    for (int k = 0; k < gm->mouse_count; k++) {
        if (gm->mouse_keys[k]) continue;   /* assigned with the keyboards */
        int s = input_slots_find(&gm->slots, &gm->mouse_ident[k]);
        gm->mouse_slot[k] = (s >= 0 && gm->slots.slot[s].present) ? s : -1;
    }
}

/* A pin changed the table: give every open node the slot its identity holds
 * now, without reopening anything, so gamepad_player() follows on the next
 * poll rather than at the next rescan.  The same assign a scan does, over the
 * identities the scan kept — pads first, then keyboards, which matters only
 * to a device the table holds no reservation for (one a pin evicted): it takes
 * the first free slot.  The latched levels move with their devices, as after
 * a rescan. */
static void rebucket(GamepadManager *gm) {
    input_slots_begin_scan(&gm->slots);
    for (int p = 0; p < GAMEPAD_MAX_PADS; p++)
        gm->pad_slot[p] = gm->pad_fds[p] >= 0
                        ? input_slots_assign(&gm->slots, &gm->pad_ident[p]) : -1;
    for (int k = 0; k < gm->keyboard_count; k++)
        gm->keyboard_slot[k] = gm->keyboard_fds[k] >= 0
                        ? input_slots_assign(&gm->slots, &gm->keyboard_ident[k]) : -1;
    for (int k = 0; k < gm->mouse_count; k++)
        if (gm->mouse_keys[k])
            gm->mouse_slot[k] = mouse_is_keyboard(gm, k)
                              ? input_slots_assign(&gm->slots, &gm->mouse_ident[k]) : -1;
    assign_mouse_slots(gm);
    memset(gm->held_latched, 0, sizeof(gm->held_latched));
    seed_latched_levels(gm);
}

/* ── Scan /dev/input/event* for gamepads, keyboards, and mice ───────────── */
/* Always called with nothing held (gamepad_init() and gamepad_rescan() both
 * start from closed), so input_scan() starts from an empty list and returns
 * the kept nodes in event-number order — the order the fd arrays are filled
 * in.  Player slots do NOT follow that order: input_slots gives each pad and
 * keyboard the slot its identity already holds. */
static void scan_devices(GamepadManager *gm) {
    /* Fingerprint first: a node that appears while the walk below is past
     * its number then differs from this baseline, and the next tick catches it. */
    input_sig_gate_baseline(&gm->node_gate, input_node_sig());
    gm->rescan_pending = false;
    input_slots_begin_scan(&gm->slots);
    InputNode nodes[GAMEPAD_SCAN_SLOTS];
    int n = input_scan(nodes, 0, GAMEPAD_SCAN_SLOTS, g_scan_cap);
    int pads = 0;

    for (int i = 0; i < n; i++) {
        const InputNode *nd = &nodes[i];
        InputIdent id;
        read_ident(nd->fd, nd->path, &id);
        if (nd->kind == INPUT_KIND_PAD) {
            int p = pads++;
            gm->pad_fds[p] = nd->fd;
            gm->pad_layout[p] = nd->pad_layout;
            gm->pad_slot[p] = input_slots_assign(&gm->slots, &id);
            gm->pad_ident[p] = id;
            memcpy(gm->pad_name[p], nd->name, sizeof(nd->name));   /* same INPUT_SCAN_*_LEN */
            memcpy(gm->pad_path[p], nd->path, sizeof(nd->path));   /* same INPUT_SCAN_*_LEN */
            load_axis_calibration(gm, p);
            announce_found(gm->announced_gamepad[p], sizeof(gm->announced_gamepad[p]),
                           "gamepad", nd->name, nd->path);
        } else if (nd->kind == INPUT_KIND_KEYBOARD) {
            int k = gm->keyboard_count++;
            gm->keyboard_fds[k] = nd->fd;
            gm->keyboard_slot[k] = input_slots_assign(&gm->slots, &id);
            gm->keyboard_ident[k] = id;
            memcpy(gm->keyboard_name[k], nd->name, sizeof(nd->name));   /* same INPUT_SCAN_*_LEN */
            memcpy(gm->keyboard_path[k], nd->path, sizeof(nd->path));   /* same INPUT_SCAN_*_LEN */
            announce_found(gm->announced_keyboard[k], sizeof(gm->announced_keyboard[k]),
                           "keyboard", nd->name, nd->path);
        } else if (nd->kind == INPUT_KIND_MOUSE) {
            int k = gm->mouse_count++;
            gm->mouse_fds[k] = nd->fd;
            gm->mouse_ident[k] = id;
            gm->mouse_keys[k] = nd->keys;
            if (nd->keys) gm->mouse_slot[k] = input_slots_assign(&gm->slots, &id);
            memcpy(gm->mouse_name[k], nd->name, sizeof(nd->name));   /* same INPUT_SCAN_*_LEN */
            memcpy(gm->mouse_path[k], nd->path, sizeof(nd->path));   /* same INPUT_SCAN_*_LEN */
            seed_mouse_buttons(nd->fd, gm->mouse_btn[k]);
            announce_found(gm->announced_mouse[k], sizeof(gm->announced_mouse[k]),
                           "mouse", nd->name, nd->path);
        } else {
            close(nd->fd);   /* unreachable: input_scan keeps only capped kinds */
        }
    }

    /* After the loop, so node order does not matter. */
    assign_mouse_slots(gm);

    /* Anything still unbound after a full scan, that we had previously
     * announced, is gone.  The arrays are filled in node order, so an entry
     * past the count is one whose device has left. */
    for (int p = 0; p < GAMEPAD_MAX_PADS; p++)
        announce_lost(gm->announced_gamepad[p], "gamepad", gm->pad_fds[p]);
    for (int k = 0; k < GAMEPAD_MAX_PER_KIND; k++) {
        announce_lost(gm->announced_keyboard[k], "keyboard",
                      k < gm->keyboard_count ? gm->keyboard_fds[k] : -1);
        announce_lost(gm->announced_mouse[k], "mouse",
                      k < gm->mouse_count ? gm->mouse_fds[k] : -1);
    }
}

/* ── Apply sensible defaults to all configurable fields ─────────────────── */
static void apply_defaults(GamepadManager *gm) {
    gm->button_map = gamepad_get_default_button_map();

    /* The mouse ranges over the LOGICAL surface, so take it from the same
     * framebuffer globals touch_init() reads rather than from a compile-time
     * 800x480. Six of the nine call sites never call gamepad_set_mouse_bounds(),
     * and app_launcher, the one that actually feeds mouse_x/y
     * into hit-testing, re-inits the manager after every child exits, which
     * re-runs this function and would undo a startup-only call. Making the
     * default right is the only form of the fix it cannot lose.
     * screen_base_width/height default to 800x480, so this is byte-identical
     * before fb_init() and on any full-size surface; it differs only where the
     * surface really is smaller, which is the case that was broken. */
    gm->mouse_screen_w = (screen_base_width  > 0) ? screen_base_width
                                                  : GAMEPAD_DEFAULT_SCREEN_W;
    gm->mouse_screen_h = (screen_base_height > 0) ? screen_base_height
                                                  : GAMEPAD_DEFAULT_SCREEN_H;
    gm->mouse_x = gm->mouse_screen_w / 2;
    gm->mouse_y = gm->mouse_screen_h / 2;

    InputConfig def;
    input_config_defaults(&def);   /* the one set of defaults, shared */
    gm->mouse_accel.sensitivity   = def.mouse_sensitivity;
    gm->mouse_accel.acceleration  = def.mouse_acceleration;
    gm->mouse_accel.low_threshold = def.mouse_low_threshold;
    gm->mouse_accel.high_threshold = def.mouse_high_threshold;

    gm->deadzone_pct = def.gamepad_deadzone;

    for (int i = 0; i < GAMEPAD_MAX_AXES; i++) {
        for (int p = 0; p < GAMEPAD_MAX_PADS; p++) {
            gm->axis_calib[p][i].center = 0;
            gm->axis_calib[p][i].deadzone_pct = gm->deadzone_pct;
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Configuration File I/O
 * ═══════════════════════════════════════════════════════════════════════════ */

/* The file is parsed by input_config_load() (common/input_scan.c), the one
 * parser vnc_client and ScummVM call too; this copies its fields in and out. */
int gamepad_load_config(GamepadManager *gp, const char *path) {
    InputConfig cfg;
    cfg.mouse_sensitivity    = gp->mouse_accel.sensitivity;
    cfg.mouse_acceleration   = gp->mouse_accel.acceleration;
    cfg.mouse_low_threshold  = gp->mouse_accel.low_threshold;
    cfg.mouse_high_threshold = gp->mouse_accel.high_threshold;
    cfg.gamepad_deadzone     = gp->deadzone_pct;
    cfg.gamepad_btn_jump     = gp->button_map.btn_jump;
    cfg.gamepad_btn_run      = gp->button_map.btn_run;
    cfg.gamepad_btn_action   = gp->button_map.btn_action;
    cfg.gamepad_btn_pause    = gp->button_map.btn_pause;
    cfg.gamepad_btn_back     = gp->button_map.btn_back;
    cfg.gamepad_hat_x        = gp->button_map.hat_x_axis;
    cfg.gamepad_hat_y        = gp->button_map.hat_y_axis;
    cfg.gamepad_stick_lx     = gp->button_map.stick_x_axis;
    cfg.gamepad_stick_ly     = gp->button_map.stick_y_axis;
    cfg.gamepad_stick_rx     = gp->button_map.stick_rx_axis;
    cfg.gamepad_stick_ry     = gp->button_map.stick_ry_axis;
    /* ScummVM-only keys: parsed, unused here. */
    cfg.gamepad_btn_north = cfg.gamepad_btn_tl = cfg.gamepad_btn_tr = 0;

    int applied = input_config_load(&cfg, path);
    if (applied < 0) {
        LIB_LOG(gp_note, "gamepad: no config file at %s (using defaults)", path);
        return -1;
    }

    gp->mouse_accel.sensitivity    = cfg.mouse_sensitivity;
    gp->mouse_accel.acceleration   = cfg.mouse_acceleration;
    gp->mouse_accel.low_threshold  = cfg.mouse_low_threshold;
    gp->mouse_accel.high_threshold = cfg.mouse_high_threshold;
    /* The uniform dead zone overwrites the per-axis ones only when the file
     * changed it, as the key being present always did. */
    if (cfg.gamepad_deadzone != gp->deadzone_pct) {
        gp->deadzone_pct = cfg.gamepad_deadzone;
        for (int p = 0; p < GAMEPAD_MAX_PADS; p++)
            for (int i = 0; i < GAMEPAD_MAX_AXES; i++)
                gp->axis_calib[p][i].deadzone_pct = cfg.gamepad_deadzone;
    }
    gp->button_map.btn_jump      = cfg.gamepad_btn_jump;
    gp->button_map.btn_run       = cfg.gamepad_btn_run;
    gp->button_map.btn_action    = cfg.gamepad_btn_action;
    gp->button_map.btn_pause     = cfg.gamepad_btn_pause;
    gp->button_map.btn_back      = cfg.gamepad_btn_back;
    gp->button_map.hat_x_axis    = cfg.gamepad_hat_x;
    gp->button_map.hat_y_axis    = cfg.gamepad_hat_y;
    gp->button_map.stick_x_axis  = cfg.gamepad_stick_lx;
    gp->button_map.stick_y_axis  = cfg.gamepad_stick_ly;
    gp->button_map.stick_rx_axis = cfg.gamepad_stick_rx;
    gp->button_map.stick_ry_axis = cfg.gamepad_stick_ry;

    LIB_LOG(gp_note, "gamepad: loaded config from %s (%d settings)", path, applied);
    return 0;
}

int gamepad_save_config(const GamepadManager *gp, const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) {
        perror("gamepad: failed to save config");
        return -1;
    }

    fprintf(f, "# Arca Input Configuration\n");
    fprintf(f, "# Auto-generated — edit carefully or use device tools\n\n");

    fprintf(f, "# Mouse settings\n");
    fprintf(f, "mouse_sensitivity=%.1f\n", gp->mouse_accel.sensitivity);
    fprintf(f, "mouse_acceleration=%.1f\n", gp->mouse_accel.acceleration);
    fprintf(f, "mouse_low_threshold=%d\n", gp->mouse_accel.low_threshold);
    fprintf(f, "mouse_high_threshold=%d\n\n", gp->mouse_accel.high_threshold);

    fprintf(f, "# Gamepad dead zone (percentage, 0-100)\n");
    fprintf(f, "gamepad_deadzone=%d\n\n", gp->deadzone_pct);

    GamepadButtonMap defaults = gamepad_get_default_button_map();

    fprintf(f, "# Gamepad button mapping (evdev codes)\n");
    fprintf(f, "# Uncomment and change to remap buttons for your controller\n");

    /* Write button mappings — comment out if still at defaults */
    if (gp->button_map.btn_jump != defaults.btn_jump)
        fprintf(f, "gamepad_btn_jump=%d\n", gp->button_map.btn_jump);
    else
        fprintf(f, "#gamepad_btn_jump=%d\n", gp->button_map.btn_jump);

    if (gp->button_map.btn_run != defaults.btn_run)
        fprintf(f, "gamepad_btn_run=%d\n", gp->button_map.btn_run);
    else
        fprintf(f, "#gamepad_btn_run=%d\n", gp->button_map.btn_run);

    if (gp->button_map.btn_action != defaults.btn_action)
        fprintf(f, "gamepad_btn_action=%d\n", gp->button_map.btn_action);
    else
        fprintf(f, "#gamepad_btn_action=%d\n", gp->button_map.btn_action);

    if (gp->button_map.btn_pause != defaults.btn_pause)
        fprintf(f, "gamepad_btn_pause=%d\n", gp->button_map.btn_pause);
    else
        fprintf(f, "#gamepad_btn_pause=%d\n", gp->button_map.btn_pause);

    if (gp->button_map.btn_back != defaults.btn_back)
        fprintf(f, "gamepad_btn_back=%d\n", gp->button_map.btn_back);
    else
        fprintf(f, "#gamepad_btn_back=%d\n", gp->button_map.btn_back);

    if (gp->button_map.hat_x_axis != defaults.hat_x_axis)
        fprintf(f, "gamepad_hat_x=%d\n", gp->button_map.hat_x_axis);
    else
        fprintf(f, "#gamepad_hat_x=%d\n", gp->button_map.hat_x_axis);

    if (gp->button_map.hat_y_axis != defaults.hat_y_axis)
        fprintf(f, "gamepad_hat_y=%d\n", gp->button_map.hat_y_axis);
    else
        fprintf(f, "#gamepad_hat_y=%d\n", gp->button_map.hat_y_axis);

    if (gp->button_map.stick_x_axis != defaults.stick_x_axis)
        fprintf(f, "gamepad_stick_lx=%d\n", gp->button_map.stick_x_axis);
    else
        fprintf(f, "#gamepad_stick_lx=%d\n", gp->button_map.stick_x_axis);

    if (gp->button_map.stick_y_axis != defaults.stick_y_axis)
        fprintf(f, "gamepad_stick_ly=%d\n", gp->button_map.stick_y_axis);
    else
        fprintf(f, "#gamepad_stick_ly=%d\n", gp->button_map.stick_y_axis);

    if (gp->button_map.stick_rx_axis != defaults.stick_rx_axis)
        fprintf(f, "gamepad_stick_rx=%d\n", gp->button_map.stick_rx_axis);
    else
        fprintf(f, "#gamepad_stick_rx=%d\n", gp->button_map.stick_rx_axis);

    if (gp->button_map.stick_ry_axis != defaults.stick_ry_axis)
        fprintf(f, "gamepad_stick_ry=%d\n", gp->button_map.stick_ry_axis);
    else
        fprintf(f, "#gamepad_stick_ry=%d\n", gp->button_map.stick_ry_axis);

    fclose(f);
    LIB_LOG(LOG_LEVEL_INFO, "gamepad: saved config to %s", path);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Public API
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Any pad open right now: what gamepad_connected has always meant. */
static bool any_pad_open(const GamepadManager *gm) {
    for (int p = 0; p < GAMEPAD_MAX_PADS; p++)
        if (gm->pad_fds[p] >= 0) return true;
    return false;
}

int gamepad_init(GamepadManager *gm) {
    gp_note = gp_announced ? LOG_LEVEL_DEBUG : LOG_LEVEL_INFO;
    memset(gm, 0, sizeof(*gm));
    input_slots_clear(&gm->slots);   /* an app exit forgets every reservation */
    for (int p = 0; p < GAMEPAD_MAX_PADS; p++) {
        gm->pad_fds[p] = -1;
        gm->pad_slot[p] = -1;
    }
    for (int k = 0; k < GAMEPAD_MAX_PER_KIND; k++) {
        gm->keyboard_fds[k] = gm->mouse_fds[k] = -1;
        gm->keyboard_slot[k] = gm->mouse_slot[k] = -1;
    }
    gm->touch_region_count = 0;

    /* The operator's pins (the Control Panel's Input page), before the scan
     * so a pinned device lands in its slot as it is found. */
    gamepad_load_slot_pins(gm, CONFIG_FILE_PATH);

    /* Apply sensible defaults before loading config */
    apply_defaults(gm);

    /* Attempt to load persistent config (overrides defaults for any keys present) */
    gamepad_load_config(gm, INPUT_CONFIG_PATH);

    scan_devices(gm);

    LIB_LOG(gp_note, "gamepad: init complete (gamepad=%s, keyboard=%s, mouse=%s)",
           any_pad_open(gm) ? "connected" : "none",
           gm->keyboard_count > 0 ? "connected" : "none",
           gm->mouse_count > 0 ? "connected" : "none");
    gp_announced = true;
    gp_note = LOG_LEVEL_INFO;   /* explicit loads after init still report */
    return 0;
}

/* Closes every device.  The slot table (gm->slots) and the announced_*
 * strings are kept: a rescan comes straight back through scan_devices(). */
void gamepad_close(GamepadManager *gm) {
    for (int p = 0; p < GAMEPAD_MAX_PADS; p++) {
        if (gm->pad_fds[p] >= 0) close(gm->pad_fds[p]);
        gm->pad_fds[p] = -1;
        gm->pad_layout[p] = INPUT_PAD_NATIVE;
        gm->pad_slot[p] = -1;
    }
    for (int k = 0; k < GAMEPAD_MAX_PER_KIND; k++) {
        if (gm->keyboard_fds[k] >= 0) close(gm->keyboard_fds[k]);
        if (gm->mouse_fds[k] >= 0)    close(gm->mouse_fds[k]);
        gm->keyboard_fds[k] = gm->mouse_fds[k] = -1;
        gm->keyboard_slot[k] = gm->mouse_slot[k] = -1;
        gm->mouse_keys[k] = false;
    }
    gm->keyboard_count = gm->mouse_count = 0;
    memset(gm->mouse_btn, 0, sizeof(gm->mouse_btn));
}

void gamepad_rescan(GamepadManager *gm) {
    /* Close existing devices and re-scan */
    gamepad_close(gm);
    /* Drop the latched levels: the key-up for anything held at unplug time
     * will never arrive, so keeping it would freeze that button on. */
    memset(gm->held_latched, 0, sizeof(gm->held_latched));
    scan_devices(gm);
    /* ...then read them back from the devices that are still there.  A
     * reopened node sends no press for a key already down, and the hat's
     * EV_ABS value is filtered when it repeats, so without this every app's
     * rescan released a held pad button or direction until it was pressed
     * again (and a held key until its autorepeat, as a fresh press edge).
     * prev_held and prev_mouse_* are deliberately kept: a level held across
     * the rescan produces no edge, and one released (or whose device left)
     * during it produces a release edge. */
    seed_latched_levels(gm);
}

void gamepad_tick(GamepadManager *gm, uint32_t now_ms) {
    /* input_sig_gate_poll() is evaluated first so a forced rescan still
     * consumes the check and refreshes the baseline's clock. */
    bool changed = input_sig_gate_poll(&gm->node_gate, now_ms);
    if (changed || gm->rescan_pending)
        gamepad_rescan(gm);
}

/* A read() that returned r failed because its device is gone: ENODEV once the
 * node is unplugged, EBADF if the fd was closed under us.  EAGAIN (drained) and
 * a short read are not. */
static bool read_gone(ssize_t r) {
    return r < 0 && (errno == ENODEV || errno == EBADF);
}

/* Close a gone fd, and have the next gamepad_tick() rescan.  The fingerprint
 * alone would also catch an unplug, but this also recovers an fd that went
 * stale with /dev/input unchanged. */
static void drop_gone_fd(GamepadManager *gm, int *fd) {
    close(*fd);
    *fd = -1;
    gm->rescan_pending = true;
}

void gamepad_set_touch_regions(GamepadManager *gm, TouchRegion *regions, int count) {
    if (count > GAMEPAD_MAX_TOUCH_REGIONS)
        count = GAMEPAD_MAX_TOUCH_REGIONS;
    for (int i = 0; i < count; i++)
        gm->touch_regions[i] = regions[i];
    gm->touch_region_count = count;
}

/* ── Mouse API ──────────────────────────────────────────────────────────── */

void gamepad_set_mouse_bounds(GamepadManager *gp, int width, int height) {
    if (width > 0) gp->mouse_screen_w = width;
    if (height > 0) gp->mouse_screen_h = height;
    /* Clamp current position to new bounds */
    if (gp->mouse_x >= gp->mouse_screen_w)
        gp->mouse_x = gp->mouse_screen_w - 1;
    if (gp->mouse_y >= gp->mouse_screen_h)
        gp->mouse_y = gp->mouse_screen_h - 1;
    if (gp->mouse_x < 0) gp->mouse_x = 0;
    if (gp->mouse_y < 0) gp->mouse_y = 0;
}

void gamepad_set_mouse_position(GamepadManager *gp, int x, int y) {
    gp->mouse_x = x;
    gp->mouse_y = y;
    /* Clamp */
    if (gp->mouse_x < 0) gp->mouse_x = 0;
    if (gp->mouse_y < 0) gp->mouse_y = 0;
    if (gp->mouse_x >= gp->mouse_screen_w)
        gp->mouse_x = gp->mouse_screen_w - 1;
    if (gp->mouse_y >= gp->mouse_screen_h)
        gp->mouse_y = gp->mouse_screen_h - 1;
}

/* ── Button Mapping API ─────────────────────────────────────────────────── */

void gamepad_set_button_map(GamepadManager *gp, const GamepadButtonMap *map) {
    if (map)
        gp->button_map = *map;
    else
        gp->button_map = gamepad_get_default_button_map();
}

/* ── Read gamepad events (using configurable button map) ────────────────── */
/* One stick axis event into the 4-axis array (lx, ly, rx, ry) of the pad's
 * bucket, through pad p's calibration. */
static int pad_axis_value(const GamepadManager *gm, int p, int code, int value) {
    int idx = axis_to_index(code);
    if (idx < 0)   /* Fallback: unknown index, use basic normalize */
        return normalize_axis(value, -32768, 32767);
    return normalize_axis_calibrated(value,
        gm->axis_min[p][idx], gm->axis_max[p][idx],
        gm->axis_calib[p][idx].center, gm->axis_calib[p][idx].deadzone_pct);
}

/* True: the kernel dropped events on this pad (SYN_DROPPED), so its latched
 * levels and axes may be stale and gamepad_poll() re-reads them. */
static bool poll_gamepad(GamepadManager *gm, int p) {
    struct input_event ev;
    GamepadButtonMap *m = &gm->button_map;
    int b = bucket_of(gm->pad_slot[p]);
    bool *latched = gm->held_latched[b];
    int *axis = gm->bucket_axis[b];
    InputSynDrop sd = { false, false };

    ssize_t r;
    while ((r = read(gm->pad_fds[p], &ev, sizeof(ev))) == (ssize_t)sizeof(ev)) {
        if (input_syn_drop_skip(&sd, &ev)) continue;

        if (ev.type == EV_ABS) {
            int code = ev.code;

            /* Blanking: a hat press or a stick past its dead zone is input
             * (rest jitter and the return to centre are not). */
            {
                int ai = axis_to_index(code);
                bool is_hat = (code == m->hat_x_axis || code == m->hat_y_axis);
                bool real = is_hat
                    ? blank_abs_is_input(ev.value, -1, 1, 0, 0)
                    : (ai >= 0 && blank_abs_is_input(ev.value,
                          gm->axis_min[p][ai], gm->axis_max[p][ai],
                          gm->axis_calib[p][ai].center,
                          gm->axis_calib[p][ai].deadzone_pct));
                if (real && hw_blank_note_activity())
                    continue;               /* woke a dark panel: not a press */
            }

            /* Left stick X / Y — configurable axis codes */
            if (code == m->stick_x_axis)
                axis[0] = pad_axis_value(gm, p, code, ev.value);
            else if (code == m->stick_y_axis)
                axis[1] = pad_axis_value(gm, p, code, ev.value);
            /* Right stick X — accept ABS_Z or the mapped rx axis */
            else if (code == m->stick_rx_axis || code == ABS_Z)
                axis[2] = pad_axis_value(gm, p, code, ev.value);
            /* Right stick Y — accept ABS_RZ or the mapped ry axis */
            else if (code == m->stick_ry_axis || code == ABS_RZ)
                axis[3] = pad_axis_value(gm, p, code, ev.value);
            /* D-pad horizontal */
            else if (code == m->hat_x_axis) {
                latched[BTN_ID_LEFT]  = (ev.value < 0);
                latched[BTN_ID_RIGHT] = (ev.value > 0);
            }
            /* D-pad vertical */
            else if (code == m->hat_y_axis) {
                latched[BTN_ID_UP]   = (ev.value < 0);
                latched[BTN_ID_DOWN] = (ev.value > 0);
            }
        } else if (ev.type == EV_KEY) {
            bool down = (ev.value != 0);
            if (down && ev.value == 1 && hw_blank_note_activity())
                continue;                   /* woke a dark panel: not a press */
            hw_blank_keepalive();
            int code = input_pad_key((InputPadLayout)gm->pad_layout[p], ev.code);

            if (code == m->btn_jump)
                latched[BTN_ID_JUMP] = down;
            else if (code == m->btn_run)
                latched[BTN_ID_RUN] = down;
            else if (code == m->btn_action)
                latched[BTN_ID_ACTION] = down;
            else if (code == m->btn_pause)
                latched[BTN_ID_PAUSE] = down;
            else if (code == m->btn_back)
                latched[BTN_ID_BACK] = down;
        }
    }
    if (read_gone(r)) {
        drop_gone_fd(gm, &gm->pad_fds[p]);   /* axes zero on the next poll */
        return false;
    }
    return sd.resync;
}

static bool poll_gamepads(GamepadManager *gm) {
    bool open[GAMEPAD_BUCKETS] = { false };
    bool lost = false;
    for (int p = 0; p < GAMEPAD_MAX_PADS; p++) {
        if (gm->pad_fds[p] < 0) continue;
        open[bucket_of(gm->pad_slot[p])] = true;
        lost |= poll_gamepad(gm, p);
    }
    /* A bucket with no pad attached zeroes its axes: a stick that was
     * deflected when the controller was unplugged must not keep asserting a
     * direction through merge_stick_dpad() forever. */
    for (int b = 0; b < GAMEPAD_BUCKETS; b++)
        if (!open[b])
            memset(gm->bucket_axis[b], 0, sizeof(gm->bucket_axis[b]));
    return lost;
}

/* ── Merge the left analog stick into the D-pad directions ──────────────── */
/*
 * Runs from gamepad_poll(), not from poll_gamepad(), and writes the per-frame
 * `derived` array rather than any persistent state.  Both of those matter:
 *
 *  - The stick reports an absolute position, so its contribution has to be
 *    recomputed every poll.  It used to write `.held = true` directly and
 *    nothing ever cleared it, so one deflection stuck a direction on for the
 *    rest of the process's life.
 *  - It has to run *after* poll_gamepads() has consumed this frame's EV_ABS
 *    events, and after a bucket with no pad has had its axes zeroed.
 *
 * BUG-INPUT-003: the threshold is applied to the already-dead-zoned normalized
 * value from normalize_axis_calibrated(), which returns exactly 0 inside the
 * calibrated dead zone.  The old code compared against the legacy
 * GAMEPAD_DEADZONE (200) on the ±1000 scale, which disagreed with the
 * calibrated dead zone and could make the stick appear stuck in a direction.
 */
#define STICK_DPAD_THRESHOLD 100  /* 10% of normalized ±1000 range */

static void merge_stick_dpad(int lx, int ly, bool *derived) {
    if (lx < -STICK_DPAD_THRESHOLD)
        derived[BTN_ID_LEFT] = true;
    else if (lx > STICK_DPAD_THRESHOLD)
        derived[BTN_ID_RIGHT] = true;

    if (ly < -STICK_DPAD_THRESHOLD)
        derived[BTN_ID_UP] = true;
    else if (ly > STICK_DPAD_THRESHOLD)
        derived[BTN_ID_DOWN] = true;
}

/* ── Read keyboard events (each keyboard into its own slot's latches) ───── */
/* down: value 1 = press, 2 = repeat, 0 = release */
static void latch_key(bool *latched, int code, bool down) {
    switch (code) {
        case KEY_UP:
        case KEY_W:
            latched[BTN_ID_UP] = down;
            break;
        case KEY_DOWN:
        case KEY_S:
            latched[BTN_ID_DOWN] = down;
            break;
        case KEY_LEFT:
        case KEY_A:
            latched[BTN_ID_LEFT] = down;
            break;
        case KEY_RIGHT:
        case KEY_D:
            latched[BTN_ID_RIGHT] = down;
            break;
        case KEY_SPACE:
            latched[BTN_ID_JUMP] = down;
            break;
        case KEY_LEFTSHIFT:
        case KEY_RIGHTSHIFT:
            latched[BTN_ID_RUN] = down;
            break;
        case KEY_ENTER:
            latched[BTN_ID_ACTION] = down;
            break;
        case KEY_ESC:
            latched[BTN_ID_PAUSE] = down;
            break;
        case KEY_BACKSPACE:
            latched[BTN_ID_BACK] = down;
            break;
        default:
            break;
    }
}

/* True: SYN_DROPPED, the latches may be stale (see poll_gamepad). */
static bool poll_keyboard_fd(GamepadManager *gm, int *fd, bool *latched) {
    struct input_event ev;
    InputSynDrop sd = { false, false };
    ssize_t r;
    while ((r = read(*fd, &ev, sizeof(ev))) == (ssize_t)sizeof(ev)) {
        if (input_syn_drop_skip(&sd, &ev)) continue;
        if (ev.type == EV_KEY) {
            if (ev.value == 1 && hw_blank_note_activity()) continue;
            hw_blank_keepalive();
            latch_key(latched, ev.code, ev.value != 0);
        }
    }
    if (read_gone(r)) {
        drop_gone_fd(gm, fd);
        return false;
    }
    return sd.resync;
}

static bool poll_keyboard(GamepadManager *gm) {
    bool lost = false;
    for (int k = 0; k < gm->keyboard_count; k++)
        if (gm->keyboard_fds[k] >= 0)
            lost |= poll_keyboard_fd(gm, &gm->keyboard_fds[k],
                                     gm->held_latched[bucket_of(gm->keyboard_slot[k])]);
    return lost;
}

/* ── Read mouse events with acceleration ────────────────────────────────── */
/* Every mouse node feeds one cursor: relative motion is summed across nodes
 * before acceleration, and each button is the OR of its level on every node. */
/* True: SYN_DROPPED on a node, so the latches its keys feed (a
 * keyboard+touchpad combo) may be stale; its own buttons are re-read here,
 * before they are ORed into this frame's held state. */
static bool poll_mouse(GamepadManager *gm, InputState *state) {
    state->mouse_dx = state->mouse_dy = 0;
    gm->mouse_left_down_ev = false;
    if (gm->mouse_count <= 0) {
        /* No mouse (or it left): nothing can be holding a mouse button. */
        state->mouse_left_held = state->mouse_right_held = 0;
        state->mouse_middle_held = 0;
        return false;
    }

    struct input_event ev;
    int accum_dx = 0, accum_dy = 0;
    bool left_held = false, right_held = false, middle_held = false;
    bool lost = false;

    for (int k = 0; k < gm->mouse_count; k++) {
        bool *btn = gm->mouse_btn[k];
        int fd = gm->mouse_fds[k];
        InputSynDrop sd = { false, false };

        ssize_t r = 0;
        while (fd >= 0 && (r = read(fd, &ev, sizeof(ev))) == (ssize_t)sizeof(ev)) {
            if (input_syn_drop_skip(&sd, &ev)) continue;
            if (ev.type == EV_REL || (ev.type == EV_KEY && ev.value == 1)) {
                if (hw_blank_note_activity()) continue;   /* woke a dark panel */
            } else if (ev.type == EV_KEY) {
                hw_blank_keepalive();
            }
            if (ev.type == EV_REL) {
                if (ev.code == REL_X)
                    accum_dx += ev.value;
                else if (ev.code == REL_Y)
                    accum_dy += ev.value;
            } else if (ev.type == EV_KEY) {
                bool down = (ev.value != 0);

                if (ev.code == BTN_LEFT) {
                    btn[0] = down;
                    if (down) gm->mouse_left_down_ev = true;
                }
                else if (ev.code == BTN_RIGHT)
                    btn[1] = down;
                else if (ev.code == BTN_MIDDLE)
                    btn[2] = down;
                else
                    latch_key(gm->held_latched[bucket_of(gm->mouse_slot[k])],
                              ev.code, down);   /* a keyboard+touchpad combo node */
            }
            /* SYN_REPORT ignored — we batch all events in the read loop */
        }
        if (fd >= 0 && read_gone(r)) {
            drop_gone_fd(gm, &gm->mouse_fds[k]);
            btn[0] = btn[1] = btn[2] = false;   /* no release will arrive */
        } else if (sd.resync) {
            /* A release (or press) may have been among the dropped events:
             * take the level from the kernel, as the open-time seed does.
             * The edges follow from prev_mouse_* below. */
            seed_mouse_buttons(fd, btn);
            lost = true;
        }
        left_held   = left_held   || btn[0];
        right_held  = right_held  || btn[1];
        middle_held = middle_held || btn[2];
    }

    /* Apply mouse acceleration to accumulated delta */
    if (accum_dx != 0 || accum_dy != 0) {
        float speed = sqrtf((float)(accum_dx * accum_dx + accum_dy * accum_dy));
        float multiplier;

        if (speed < (float)gm->mouse_accel.low_threshold) {
            /* Precision mode — 1:1 mapping */
            multiplier = 1.0f;
        } else if (speed < (float)gm->mouse_accel.high_threshold) {
            /* Medium speed — apply base sensitivity */
            multiplier = gm->mouse_accel.sensitivity;
        } else {
            /* Fast movement — extra acceleration */
            multiplier = gm->mouse_accel.sensitivity * gm->mouse_accel.acceleration;
        }

        int final_dx = (int)(accum_dx * multiplier);
        int final_dy = (int)(accum_dy * multiplier);

        gm->mouse_x += final_dx;
        gm->mouse_y += final_dy;
        state->mouse_dx = final_dx;
        state->mouse_dy = final_dy;

        /* Clamp to screen bounds */
        if (gm->mouse_x < 0) gm->mouse_x = 0;
        if (gm->mouse_y < 0) gm->mouse_y = 0;
        if (gm->mouse_x >= gm->mouse_screen_w)
            gm->mouse_x = gm->mouse_screen_w - 1;
        if (gm->mouse_y >= gm->mouse_screen_h)
            gm->mouse_y = gm->mouse_screen_h - 1;
    }

    /* Update mouse button state */
    state->mouse_left_held   = left_held ? 1 : 0;
    state->mouse_right_held  = right_held ? 1 : 0;
    state->mouse_middle_held = middle_held ? 1 : 0;

    /* Update cursor position in state */
    state->mouse_x = gm->mouse_x;
    state->mouse_y = gm->mouse_y;
    return lost;
}

/* After a SYN_DROPPED on any node: rebuild every latch from the kernel's
 * levels, as a rescan does, and re-read each pad's stick position, which is a
 * level too.  The whole table rather than one device's share, because a
 * bucket is shared by every device in a slot and holds no record of which of
 * them set a level.  prev_held is kept, so a level lost in the drop comes out
 * as a release edge.  Runs after every reader has drained its queue this
 * poll, so the EV_KEY flush EVIOCGKEY performs discards nothing unread. */
static void resync_levels(GamepadManager *gm) {
    const GamepadButtonMap *m = &gm->button_map;
    memset(gm->held_latched, 0, sizeof(gm->held_latched));
    seed_latched_levels(gm);
    for (int p = 0; p < GAMEPAD_MAX_PADS; p++) {
        int fd = gm->pad_fds[p];
        if (fd < 0) continue;
        int *axis = gm->bucket_axis[bucket_of(gm->pad_slot[p])];
        const int codes[4] = { m->stick_x_axis, m->stick_y_axis,
                               m->stick_rx_axis, m->stick_ry_axis };
        for (int i = 0; i < 4; i++) {
            struct input_absinfo ai;
            if (codes[i] >= 0 && codes[i] <= ABS_MAX &&
                ioctl(fd, EVIOCGABS(codes[i]), &ai) == 0)
                axis[i] = pad_axis_value(gm, p, codes[i], ai.value);
        }
    }
}

/* ── Apply touch regions ────────────────────────────────────────────────── */
/*
 * Writes the per-frame `derived` array, never any persistent state: a touch
 * region is asserted exactly on the frames the finger is inside it.  It used
 * to set `.held = true` on the caller's InputState, which nothing ever
 * cleared, so the first tap latched a virtual D-pad direction on permanently
 * (which is why frogger's and platformer's virtual pads were removed).
 */
static void poll_touch(GamepadManager *gm, bool *derived,
                       int touch_x, int touch_y, bool touch_active) {
    if (!touch_active || gm->touch_region_count <= 0) return;

    for (int i = 0; i < gm->touch_region_count; i++) {
        TouchRegion *r = &gm->touch_regions[i];
        if (r->button < 0 || r->button >= BTN_ID_COUNT) continue;

        if (touch_x >= r->x && touch_x < r->x + r->w &&
            touch_y >= r->y && touch_y < r->y + r->h) {
            derived[r->button] = true;
        }
    }
}

/* ── Edge detection (abstract buttons) ──────────────────────────────────── */
static void compute_edges(bool *prev_held, InputState *state) {
    for (int i = 0; i < BTN_ID_COUNT; i++) {
        bool now  = state->buttons[i].held;
        bool prev = prev_held[i];
        state->buttons[i].pressed  = (now && !prev);
        state->buttons[i].released = (!now && prev);
        prev_held[i] = now;
    }
}

/* The stick value of larger magnitude: two pads' sticks in one any-device
 * state, where the deflected one should win over the one at rest. */
static int stronger(int a, int b) {
    return (abs(b) > abs(a)) ? b : a;
}

/* ── Edge detection (mouse buttons) ─────────────────────────────────────── */
static void compute_mouse_edges(GamepadManager *gm, InputState *state) {
    bool left_now   = state->mouse_left_held ? true : false;
    bool right_now  = state->mouse_right_held ? true : false;
    bool middle_now = state->mouse_middle_held ? true : false;

    /* A down event that ends released within the poll was a whole click:
     * report both edges, as touch does for a tap inside one poll. */
    bool left_click = gm->mouse_left_down_ev && !left_now;
    state->mouse_left_pressed    = ((left_now && !gm->prev_mouse_left) || left_click) ? 1 : 0;
    state->mouse_left_released   = ((!left_now && gm->prev_mouse_left) || left_click) ? 1 : 0;
    state->mouse_right_pressed   = (right_now && !gm->prev_mouse_right) ? 1 : 0;
    state->mouse_right_released  = (!right_now && gm->prev_mouse_right) ? 1 : 0;
    state->mouse_middle_pressed  = (middle_now && !gm->prev_mouse_middle) ? 1 : 0;
    state->mouse_middle_released = (!middle_now && gm->prev_mouse_middle) ? 1 : 0;

    gm->prev_mouse_left   = left_now;
    gm->prev_mouse_right  = right_now;
    gm->prev_mouse_middle = middle_now;
}

/* Rebuild each player's InputState from its slot's bucket: the latched levels,
 * the stick (and its D-pad contribution), the edges, and what it holds. */
static void build_players(GamepadManager *gm) {
    for (int s = 0; s < INPUT_SLOTS; s++) {
        InputState *ps = &gm->players[s];
        const int *axis = gm->bucket_axis[s];
        memset(ps, 0, sizeof(*ps));
        bool derived[BTN_ID_COUNT];
        memset(derived, 0, sizeof(derived));
        merge_stick_dpad(axis[0], axis[1], derived);
        for (int i = 0; i < BTN_ID_COUNT; i++)
            ps->buttons[i].held = gm->held_latched[s][i] || derived[i];
        ps->axis_lx = axis[0]; ps->axis_ly = axis[1];
        ps->axis_rx = axis[2]; ps->axis_ry = axis[3];
        compute_edges(gm->player_prev[s], ps);
    }
    for (int p = 0; p < GAMEPAD_MAX_PADS; p++)
        if (gm->pad_fds[p] >= 0 && gm->pad_slot[p] >= 0)
            gm->players[gm->pad_slot[p]].gamepad_connected = true;
    for (int k = 0; k < gm->keyboard_count; k++)
        if (gm->keyboard_fds[k] >= 0 && gm->keyboard_slot[k] >= 0)
            gm->players[gm->keyboard_slot[k]].keyboard_connected = true;
    for (int k = 0; k < gm->mouse_count; k++)
        if (mouse_is_keyboard(gm, k) && gm->mouse_slot[k] >= 0)
            gm->players[gm->mouse_slot[k]].keyboard_connected = true;
}

void gamepad_poll(GamepadManager *gm, InputState *state,
                  int touch_x, int touch_y, bool touch_active) {
    state->gamepad_connected  = any_pad_open(gm);
    state->keyboard_connected = (gm->keyboard_count > 0);
    for (int k = 0; k < gm->mouse_count; k++)
        if (mouse_is_keyboard(gm, k)) state->keyboard_connected = true;
    state->mouse_connected    = (gm->mouse_count > 0) ? 1 : 0;

    /* Read from each input source */
    hw_blank_poll();
    bool lost = poll_gamepads(gm);    /* latches keys/hat, updates the axes */
    for (int b = 0; b < GAMEPAD_BUCKETS; b++)       /* a held stick is activity */
        for (int i = 0; i < 4; i++)
            if (gm->bucket_axis[b][i]) hw_blank_keepalive();
    lost |= poll_keyboard(gm);        /* latches keys */
    lost |= poll_mouse(gm, state);
    if (lost)                         /* SYN_DROPPED: events were discarded */
        resync_levels(gm);

    /* Level state from the sources that report an absolute position rather
     * than press/release events — touch regions and the analog stick.  Zeroed
     * every poll and rebuilt below, which is what stops them latching.
     * The event-driven sources are the other half: their level lives in
     * gm->held_latched[] because a key-up may be many frames away. */
    bool derived[BTN_ID_COUNT];
    memset(derived, 0, sizeof(derived));
    poll_touch(gm, derived, touch_x, touch_y, touch_active);

    /* Any device: every bucket's latches and stick, each stick merged into
     * the D-pad on its own (after poll_gamepads: needs this frame's axes). */
    bool latched[BTN_ID_COUNT];
    memset(latched, 0, sizeof(latched));
    state->axis_lx = state->axis_ly = state->axis_rx = state->axis_ry = 0;
    for (int b = 0; b < GAMEPAD_BUCKETS; b++) {
        const int *axis = gm->bucket_axis[b];
        for (int i = 0; i < BTN_ID_COUNT; i++)
            latched[i] = latched[i] || gm->held_latched[b][i];
        merge_stick_dpad(axis[0], axis[1], derived);
        state->axis_lx = stronger(state->axis_lx, axis[0]);
        state->axis_ly = stronger(state->axis_ly, axis[1]);
        state->axis_rx = stronger(state->axis_rx, axis[2]);
        state->axis_ry = stronger(state->axis_ry, axis[3]);
    }

    /* `held` is a pure output — never read back as state, so a caller that
     * zeroes its InputState between polls cannot lose a physically held key. */
    for (int i = 0; i < BTN_ID_COUNT; i++)
        state->buttons[i].held = latched[i] || derived[i];

    /* Compute pressed/released edges */
    compute_edges(gm->prev_held, state);
    compute_mouse_edges(gm, state);
    build_players(gm);
}

const InputState *gamepad_player(const GamepadManager *gm, int slot) {
    static const InputState none;
    if (!gm || slot < 0 || slot >= INPUT_SLOTS) return &none;
    return &gm->players[slot];
}

int gamepad_player_mask(const GamepadManager *gm) {
    int mask = 0;
    for (int p = 0; p < GAMEPAD_MAX_PADS; p++)
        if (gm->pad_fds[p] >= 0 && gm->pad_slot[p] >= 0)
            mask |= 1 << gm->pad_slot[p];
    for (int k = 0; k < gm->keyboard_count; k++)
        if (gm->keyboard_fds[k] >= 0 && gm->keyboard_slot[k] >= 0)
            mask |= 1 << gm->keyboard_slot[k];
    for (int k = 0; k < gm->mouse_count; k++)
        if (mouse_is_keyboard(gm, k) && gm->mouse_slot[k] >= 0)
            mask |= 1 << gm->mouse_slot[k];
    return mask;
}

/* ── Draw virtual touch controls ────────────────────────────────────────── */
void gamepad_draw_touch_controls(void *fb_ptr, InputState *state) {
    Framebuffer *fb = (Framebuffer *)fb_ptr;
    if (!fb || !state) return;

    /* Only show touch controls if no physical controllers connected */
    if (state->gamepad_connected || state->keyboard_connected) return;

    int sw = (int)fb->width;
    int sh = (int)fb->height;

    /* D-pad: bottom-left */
    int dpad_cx = 80;
    int dpad_cy = sh - 100;
    int dpad_sz = 45;
    uint32_t col_off = RGB(60, 60, 80);
    uint32_t col_on  = RGB(0, 200, 255);

    /* Up */
    fb_fill_rect(fb, dpad_cx - dpad_sz/2, dpad_cy - dpad_sz*2, dpad_sz, dpad_sz,
                 state->buttons[BTN_ID_UP].held ? col_on : col_off);
    /* Down */
    fb_fill_rect(fb, dpad_cx - dpad_sz/2, dpad_cy + dpad_sz, dpad_sz, dpad_sz,
                 state->buttons[BTN_ID_DOWN].held ? col_on : col_off);
    /* Left */
    fb_fill_rect(fb, dpad_cx - dpad_sz*2, dpad_cy - dpad_sz/2, dpad_sz, dpad_sz,
                 state->buttons[BTN_ID_LEFT].held ? col_on : col_off);
    /* Right */
    fb_fill_rect(fb, dpad_cx + dpad_sz, dpad_cy - dpad_sz/2, dpad_sz, dpad_sz,
                 state->buttons[BTN_ID_RIGHT].held ? col_on : col_off);

    /* Action buttons: bottom-right */
    int btn_cx = sw - 80;
    int btn_cy = sh - 100;
    int btn_r  = 25;

    /* Jump (A) — bottom */
    fb_fill_circle(fb, btn_cx, btn_cy + 35, btn_r,
                   state->buttons[BTN_ID_JUMP].held ? col_on : col_off);
    fb_draw_text(fb, btn_cx - 3, btn_cy + 31, "A", COLOR_WHITE, 1);

    /* Run (B) — right */
    fb_fill_circle(fb, btn_cx + 40, btn_cy, btn_r,
                   state->buttons[BTN_ID_RUN].held ? col_on : col_off);
    fb_draw_text(fb, btn_cx + 37, btn_cy - 4, "B", COLOR_WHITE, 1);

    /* Action (X) — left */
    fb_fill_circle(fb, btn_cx - 40, btn_cy, btn_r,
                   state->buttons[BTN_ID_ACTION].held ? col_on : col_off);
    fb_draw_text(fb, btn_cx - 43, btn_cy - 4, "X", COLOR_WHITE, 1);

    (void)sw; /* suppress unused warning when only dpad drawn */
}

/* ── Pinning a device to a player slot (see gamepad.h) ──────────────────── */

/* Ordering of /dev/input/eventN paths by N: a shorter path is a smaller
 * number, so "event2" sorts before "event10". */
static int node_cmp(const char *a, const char *b) {
    size_t la = strlen(a), lb = strlen(b);
    if (la != lb) return la < lb ? -1 : 1;
    return strcmp(a, b);
}

int gamepad_devices(const GamepadManager *gm, GamepadDevice *out, int max) {
    GamepadDevice all[GAMEPAD_MAX_PADS + 2 * GAMEPAD_MAX_PER_KIND];
    int n = 0, w = 0;
    if (!gm || !out || max <= 0) return 0;

    /* Pads, then keyboard nodes, then mouse nodes that carry a keyboard. */
    int kbds = GAMEPAD_MAX_PADS + gm->keyboard_count;
    for (int i = 0; i < kbds + gm->mouse_count; i++) {
        bool kbd = i >= GAMEPAD_MAX_PADS, mouse = i >= kbds;
        int j = mouse ? i - kbds : kbd ? i - GAMEPAD_MAX_PADS : i;
        if (mouse ? !mouse_is_keyboard(gm, j)
                  : (kbd ? gm->keyboard_fds[j] : gm->pad_fds[j]) < 0) continue;
        GamepadDevice *d = &all[n];
        snprintf(d->name, sizeof(d->name), "%s", mouse ? gm->mouse_name[j]
                 : kbd ? gm->keyboard_name[j] : gm->pad_name[j]);
        snprintf(d->path, sizeof(d->path), "%s", mouse ? gm->mouse_path[j]
                 : kbd ? gm->keyboard_path[j] : gm->pad_path[j]);
        d->ident = mouse ? gm->mouse_ident[j] : kbd ? gm->keyboard_ident[j] : gm->pad_ident[j];
        d->slot = mouse ? gm->mouse_slot[j] : kbd ? gm->keyboard_slot[j] : gm->pad_slot[j];
        if (d->slot < 0 || d->slot >= INPUT_SLOTS) d->slot = -1;
        d->pinned = d->slot >= 0 && gm->slots.slot[d->slot].pinned;
        d->keyboard = kbd;
        /* Insertion sort by node number: at most twelve entries. */
        for (int k = n; k > 0 && node_cmp(all[k - 1].path, all[k].path) > 0; k--) {
            GamepadDevice t = all[k]; all[k] = all[k - 1]; all[k - 1] = t;
        }
        n++;
    }
    /* One entry per identity — a keyboard's several nodes are one device —
     * named after its lowest node, which the sort put first. */
    for (int i = 0; i < n && w < max; i++) {
        bool dup = false;
        for (int k = 0; k < w && !dup; k++)
            dup = input_ident_equal(&out[k].ident, &all[i].ident);
        if (!dup) out[w++] = all[i];
    }
    return w;
}

bool gamepad_slot_info(const GamepadManager *gm, int slot, InputIdent *id,
                       bool *pinned, bool *present) {
    if (!gm || slot < 0 || slot >= INPUT_SLOTS) return false;
    const InputSlot *s = &gm->slots.slot[slot];
    if (!s->reserved) return false;
    if (id)      *id = s->ident;
    if (pinned)  *pinned = s->pinned;
    if (present) *present = s->present;
    return true;
}

static void slot_key(int slot, char *key, size_t n) {
    snprintf(key, n, "slot_p%d", slot + 1);
}

/* The four keys as the table holds them: a pinned slot's identity, and no key
 * at all for a slot on auto (an identity with no text form included). */
static void pins_to_config(const GamepadManager *gm, Config *cfg) {
    for (int s = 0; s < INPUT_SLOTS; s++) {
        char key[16], val[CONFIG_VAL_LEN];
        const InputSlot *sl = &gm->slots.slot[s];
        slot_key(s, key, sizeof(key));
        if (sl->reserved && sl->pinned &&
            input_ident_format(&sl->ident, val, sizeof(val)))
            config_set(cfg, key, val);
        else
            config_remove(cfg, key);
    }
}

/* Re-read the file, set the four keys, save it — so keys another app wrote
 * since this one started survive — and keep `mem` in step when given.
 * 0, or -1 when the file could not be written. */
static int persist_pins(const GamepadManager *gm, Config *mem) {
    Config disk;
    config_init_path(&disk, gm->slot_config_path);
    config_load(&disk);                 /* a missing file just starts empty */
    pins_to_config(gm, &disk);
    if (mem) pins_to_config(gm, mem);
    if (config_save(&disk) != 0) {
        fprintf(stderr, "gamepad: saving slot pins to %s failed\n", gm->slot_config_path);
        return -1;
    }
    return 0;
}

int gamepad_load_slot_pins(GamepadManager *gm, const char *path) {
    Config cfg;
    int applied = 0;
    if (!gm || !path) return 0;
    snprintf(gm->slot_config_path, sizeof(gm->slot_config_path), "%s", path);
    config_init_path(&cfg, path);
    if (config_load(&cfg) == 0) {
        for (int s = 0; s < INPUT_SLOTS; s++) {
            char key[16];
            InputIdent id;
            slot_key(s, key, sizeof(key));
            const char *v = config_get(&cfg, key, NULL);
            if (v && input_ident_parse(v, &id) && input_slots_pin(&gm->slots, &id, s) == s) {
                LIB_LOG(gp_note, "gamepad: P%d pinned to %s", s + 1, v);
                applied++;
            }
        }
    }
    rebucket(gm);
    return applied;
}

int gamepad_slot_pin(GamepadManager *gm, const InputIdent *id, int slot,
                     Config *mem) {
    if (!gm || !id || slot < 0 || slot >= INPUT_SLOTS) return -1;
    if (input_slots_pin(&gm->slots, id, slot) != slot) return -1;
    rebucket(gm);
    return persist_pins(gm, mem) == 0 ? slot : -2;
}

int gamepad_slot_unpin(GamepadManager *gm, int slot, Config *mem) {
    if (!gm || slot < 0 || slot >= INPUT_SLOTS) return -1;
    /* The holder keeps the slot as an ordinary reservation, so no node moves
     * and there is nothing to re-bucket. */
    input_slots_unpin(&gm->slots, slot);
    return persist_pins(gm, mem) == 0 ? 0 : -2;
}
