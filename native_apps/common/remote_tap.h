#ifndef REMOTE_TAP_H
#define REMOTE_TAP_H

#include <stdbool.h>
#include "config.h"

/* Remote tap: lets a script drive the touch panel through arca-tap.
 *
 * ONE setting, ONE home: the config key below, written only by the Control
 * Panel's Input page (REMOTE TAP) and read by arca-tap, which refuses to run and
 * removes its virtual device while it is off. Off unless the key says true, so
 * a missing file, a missing key and RESET DEFAULTS all mean off.
 *
 * Header-only on purpose: config.c is linked by ScummVM too, so a function added
 * there would force a three-component redeploy for a setting only two read. */
#define REMOTE_TAP_KEY "remote_tap"

static inline bool remote_tap_enabled(const Config *cfg) {
    return config_get_bool(cfg, REMOTE_TAP_KEY, false);
}

static inline void remote_tap_set(Config *cfg, bool on) {
    config_set_bool(cfg, REMOTE_TAP_KEY, on);
}

/* ---- command line, pure -------------------------------------------------- */

typedef enum { RTAP_CMD_NONE = 0, RTAP_CMD_TAP, RTAP_CMD_HOLD, RTAP_CMD_SWIPE } RemoteTapKind;

typedef struct {
    RemoteTapKind kind;
    int x1, y1, x2, y2;   /* LOGICAL screen pixels (what a screenshot shows) */
    int ms;               /* hold: press length; swipe: travel time */
} RemoteTapCmd;

#define RTAP_TAP_MS    80
#define RTAP_SWIPE_MS  300
#define RTAP_MAX_MS    10000
#define RTAP_MAX_COORD 4000

/* Parse "tap X Y", "hold X Y MS", "swipe X1 Y1 X2 Y2 [MS]" (one line, a
 * trailing newline allowed). Returns true and fills *c, or false for anything
 * else: unknown verb, missing/extra/non-numeric fields, a coordinate outside
 * 0..RTAP_MAX_COORD, a duration outside 1..RTAP_MAX_MS. */
bool remote_tap_parse(const char *line, RemoteTapCmd *c);

/* Number of intermediate pointer positions for a swipe of `ms`: one per
 * RTAP_STEP_MS, at least 2. */
#define RTAP_STEP_MS 16
int remote_tap_swipe_steps(int ms);

/* Point `i` of `n` (i = 0..n) on the line a->b, rounded to nearest. No divide
 * by a negative and no overflow for values within RTAP_MAX_COORD. */
int remote_tap_lerp(int a, int b, int i, int n);

#endif
