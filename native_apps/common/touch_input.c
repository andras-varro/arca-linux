#include "touch_input.h"
#include "framebuffer.h"
#include "hardware.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <linux/input.h>

#define LOGGER_LIB_CLIENT
#include "logger.h"
#include "input_scan.h"   // InputSynDrop: header-only, links nothing

// Default calibration file path
#define TOUCH_CALIB_FILE "/etc/touch_calibration.conf"

// Level of the routine "what was set" reports. app_launcher re-runs touch_init()
// around every child it starts, so only the process's first touch_init() reports
// at INFO; inside every later one these drop to DEBUG, or the same banner is
// appended to the launcher's log once per game played. Warnings are unaffected.
static bool     touch_announced;
static LogLevel touch_note = LOG_LEVEL_INFO;

// Unrotated panel extents for the two raw axes. Raw X always spans the physical
// 800 px width and raw Y the 480 px height; in portrait the app-visible axes are
// swapped, so the stored geometry has to be un-swapped to interpret raw.
static void axis_dims(const TouchInput *touch, int *phys_w, int *phys_h) {
    int w, h;
    if (touch->portrait_mode) {
        w = touch->panel_height;
        h = touch->panel_width;
    } else {
        w = touch->panel_width;
        h = touch->panel_height;
    }
    *phys_w = (w > 1) ? w : 800;
    *phys_h = (h > 1) ? h : 480;
}

void touch_knots_on_line(int v0, int v1, int dim, int *k_lo, int *k_hi) {
    // Evaluate the line ON the knot positions. NOT v0 + span/4: the knots sit at
    // panel dim/4 and 3*dim/4 of a dim-1 span, and 120/479 is not 1/4, so the
    // shortcut shifts a migrated legacy config by a pixel in the lower half.
    long span = (long)v1 - v0;
    int last = (dim > 1) ? dim - 1 : 1;
    if (k_lo) *k_lo = (int)(v0 + span * TOUCH_KNOT_LO(dim) / last);
    if (k_hi) *k_hi = (int)(v0 + span * TOUCH_KNOT_HI(dim) / last);
}

int touch_map_axis_panel(long raw, int v0, int k_lo, int k_hi, int v1, int dim) {
    if (dim <= 1) return 0;
    int q1 = TOUCH_KNOT_LO(dim);
    int q2 = TOUCH_KNOT_HI(dim);
    int last = dim - 1;

    // Linear whenever the knots do not describe a usable curve. Two cases reach
    // here: k_lo == k_hi == 0, the explicit "no curve measured" sentinel that an
    // uncalibrated or endpoint-only calibration installs; and a non-monotone set,
    // which would fold the axis back on itself — a hand-broken config, better
    // mapped coarsely than nonsensically.
    if (!(v0 < k_lo && k_lo < k_hi && k_hi < v1)) {
        long span = (long)v1 - v0;
        if (span == 0) return 0;
        return (int)((raw - v0) * last / span);
    }

    if (raw <= k_lo)                     // outer segment, low end
        return (int)((raw - v0) * q1 / (k_lo - v0));
    if (raw <= k_hi)                     // fitted interior
        return (int)(q1 + (raw - k_lo) * (q2 - q1) / (k_hi - k_lo));
    return (int)(q2 + (raw - k_hi) * (last - q2) / (v1 - k_hi));
}

// Map raw touch coordinates to logical screen coordinates.
//
//   raw -> clamp to the calibrated range
//       -> panel      (per-axis piecewise curve; stage 1, see touch_input.h)
//       -> rotate 90° CCW if portrait
//       -> logical    (subtract the viewport origin; stage 2)
//       -> clamp to the logical screen
static void scale_coordinates(TouchInput *touch, int *x, int *y) {
    // The hardware panel is always 800×480; in portrait the app-visible axes
    // are swapped. Map into unrotated panel space first, rotate at the end.
    int phys_w, phys_h;
    axis_dims(touch, &phys_w, &phys_h);

    if (touch->raw_max_x - touch->raw_min_x <= 0) {
        touch->raw_min_x = 0; touch->raw_max_x = 4095;
        touch->raw_knot_lo_x = touch->raw_knot_hi_x = 0;
    }
    if (touch->raw_max_y - touch->raw_min_y <= 0) {
        touch->raw_min_y = 0; touch->raw_max_y = 4095;
        touch->raw_knot_lo_y = touch->raw_knot_hi_y = 0;
    }

    // Clamp raw into the calibrated range so a touch slightly past the sampled
    // edge still lands exactly on the panel border rather than overshooting.
    long rx = *x, ry = *y;
    if (rx < touch->raw_min_x) rx = touch->raw_min_x;
    if (rx > touch->raw_max_x) rx = touch->raw_max_x;
    if (ry < touch->raw_min_y) ry = touch->raw_min_y;
    if (ry > touch->raw_max_y) ry = touch->raw_max_y;

    int mx = touch_map_axis_panel(rx, touch->raw_min_x, touch->raw_knot_lo_x,
                                  touch->raw_knot_hi_x, touch->raw_max_x, phys_w);
    int my = touch_map_axis_panel(ry, touch->raw_min_y, touch->raw_knot_lo_y,
                                  touch->raw_knot_hi_y, touch->raw_max_y, phys_h);

    // Portrait mode: rotate panel coords → virtual coords (90° CCW)
    if (touch->portrait_mode) {
        int px = mx, py = my;
        mx = phys_h - 1 - py;
        my = px;
    }

    // Panel → logical: the drawing surface starts at the viewport origin, so a
    // touch on the bezel maps outside it and clamps to the nearest edge.
    mx -= touch->view_x;
    my -= touch->view_y;

    if (mx < 0) mx = 0;
    if (mx >= touch->screen_width)  mx = touch->screen_width - 1;
    if (my < 0) my = 0;
    if (my >= touch->screen_height) my = touch->screen_height - 1;

    *x = mx;
    *y = my;
}

void touch_map_raw(TouchInput *touch, int *x, int *y) {
    scale_coordinates(touch, x, y);
}

// Measure the visible-but-not-touchable band and publish it to framebuffer.c.
//
// The four raw edge extremes are pushed through scale_coordinates() — the
// production path, not a re-derivation — and the logical bounding box of the
// results is what a finger can actually reach. Going through the real map is what
// makes this correct in portrait (where the raw axes are swapped relative to the
// logical ones) and under any bezel, with no second copy of the arithmetic to
// drift out of step. scale_coordinates() clamps into the logical screen, which is
// exactly right here: reach that lands under the bezel costs nothing.
//
// reach_* is what the EDGES emit, not the calibrated range: the stored curve
// legitimately extrapolates outside it, and the gap between the two is the dead
// band. It defaults to the EVIOCGABS range and is refined by the wizard's sweep.
static void publish_safe_area(TouchInput *touch) {
    if (touch->screen_width <= 1 || touch->screen_height <= 1) return;

    int lo_x = touch->screen_width, hi_x = -1;
    int lo_y = touch->screen_height, hi_y = -1;

    const int rxs[2] = { touch->reach_min_x, touch->reach_max_x };
    const int rys[2] = { touch->reach_min_y, touch->reach_max_y };
    for (int i = 0; i < 2; i++) {
        for (int j = 0; j < 2; j++) {
            int x = rxs[i], y = rys[j];
            scale_coordinates(touch, &x, &y);
            if (x < lo_x) lo_x = x;
            if (x > hi_x) hi_x = x;
            if (y < lo_y) lo_y = y;
            if (y > hi_y) hi_y = y;
        }
    }

    fb_set_touch_inset(lo_y, touch->screen_height - 1 - hi_y,
                       lo_x, touch->screen_width  - 1 - hi_x);

    if (lo_x || lo_y || hi_x != touch->screen_width - 1
                     || hi_y != touch->screen_height - 1)
        LIB_LOG(touch_note, "Touch-safe area: logical X %d..%d Y %d..%d of %dx%d "
               "(the rest is drawable but not pressable)",
               lo_x, hi_x, lo_y, hi_y, touch->screen_width, touch->screen_height);
}

int touch_init(TouchInput *touch, const char *device) {
    touch->fd = open(device, O_RDONLY | O_NONBLOCK);
    if (touch->fd == -1) {
        perror("Error opening touch device");
        LIB_LOG(LOG_LEVEL_ERROR, "ERROR: Failed to open %s, fd=%d", device, touch->fd);
        return -1;
    }

    touch_note = touch_announced ? LOG_LEVEL_DEBUG : LOG_LEVEL_INFO;
    LIB_LOG(touch_note, "Touch device opened successfully: %s (fd=%d)", device, touch->fd);

    touch->state.x = 0;
    touch->state.y = 0;
    touch->state.pressed = false;
    touch->state.released = false;
    touch->state.held = false;
    touch->last_x = 0;
    touch->last_y = 0;
    touch->touching = false;

    // Query the hardware coordinate range via EVIOCGABS. This is the default
    // raw→screen mapping (works out of the box on this panel); calibration
    // replaces it with a measured curve.
    struct input_absinfo abs_x, abs_y;
    if (ioctl(touch->fd, EVIOCGABS(ABS_X), &abs_x) == 0 &&
        ioctl(touch->fd, EVIOCGABS(ABS_Y), &abs_y) == 0) {
        touch->raw_min_x = abs_x.minimum;
        touch->raw_max_x = abs_x.maximum;
        touch->raw_min_y = abs_y.minimum;
        touch->raw_max_y = abs_y.maximum;
        LIB_LOG(touch_note, "Touch hardware range: X [%d..%d], Y [%d..%d]",
               abs_x.minimum, abs_x.maximum, abs_y.minimum, abs_y.maximum);
    } else {
        touch->raw_min_x = 0; touch->raw_max_x = 4095;
        touch->raw_min_y = 0; touch->raw_max_y = 4095;
        LIB_LOG(LOG_LEVEL_WARN, "Touch EVIOCGABS failed, using default 0-4095 range");
    }
    // Keep the hardware range: loading a calibration overwrites raw_min/raw_max
    // with the curve's endpoints, which legitimately fall OUTSIDE what the sensor
    // emits. The difference between the two is the unreachable band, so the
    // hardware values have to survive to be able to measure it.
    touch->hw_min_x = touch->raw_min_x; touch->hw_max_x = touch->raw_max_x;
    touch->hw_min_y = touch->raw_min_y; touch->hw_max_y = touch->raw_max_y;
    // Until an edge sweep says otherwise, assume each edge drives raw all the way
    // to the hardware limit. That is the optimistic assumption, and it is the one
    // an unmeasured panel deserves.
    touch->reach_min_x = touch->hw_min_x; touch->reach_max_x = touch->hw_max_x;
    touch->reach_min_y = touch->hw_min_y; touch->reach_max_y = touch->hw_max_y;
    // Uncalibrated default: no curve measured, so map linearly.
    touch->raw_knot_lo_x = touch->raw_knot_hi_x = 0;
    touch->raw_knot_lo_y = touch->raw_knot_hi_y = 0;
    // Geometry from the framebuffer globals. fb_init() must run first (it does
    // in every app); before it, these are an identity viewport on a 800×480
    // panel, so an uninitialised framebuffer yields plain panel coordinates.
    touch->panel_width   = screen_panel_width;
    touch->panel_height  = screen_panel_height;
    touch->view_x        = screen_view_x;
    touch->view_y        = screen_view_y;
    touch->screen_width  = screen_base_width;
    touch->screen_height = screen_base_height;
    touch->calibrated = false;

    touch->calib.enabled = false;

    // Detect portrait mode
    touch->portrait_mode = (access("/opt/games/portrait.mode", F_OK) == 0);

    // Seed the bezel margins from the framebuffer so that saving a calibration
    // preserves them even when the config file has no margin line. In portrait
    // the globals are rotated and would not round-trip, so leave them at zero —
    // calibration is landscape-only.
    if (touch->portrait_mode) {
        touch->calib.bezel_top = touch->calib.bezel_bottom = 0;
        touch->calib.bezel_left = touch->calib.bezel_right = 0;
    } else {
        touch->calib.bezel_top    = screen_bezel_top;
        touch->calib.bezel_bottom = screen_bezel_bottom;
        touch->calib.bezel_left   = screen_bezel_left;
        touch->calib.bezel_right  = screen_bezel_right;
    }

    // Auto-load calibration so all apps get the measured range (if calibrated).
    if (touch_load_calibration(touch, TOUCH_CALIB_FILE) == 0) {
        touch_enable_calibration(touch, true);
        LIB_LOG(touch_note, "Touch auto-loaded calibration from %s", TOUCH_CALIB_FILE);
    }

    // Last, so it sees the final curve and geometry. Apps that resize the logical
    // screen afterwards go through touch_set_screen_size(), which republishes.
    publish_safe_area(touch);

    LIB_LOG(touch_note, "Touch input initialized: %s", device);
    touch_announced = true;
    touch_note = LOG_LEVEL_INFO;   // explicit calls after init still report
    return 0;
}

void touch_set_screen_size(TouchInput *touch, int width, int height) {
    touch->screen_width = width;
    touch->screen_height = height;
    // Re-read the viewport too: an app that calls this after fb_init() ends up
    // consistent even if it initialised touch before the framebuffer.
    touch->panel_width  = screen_panel_width;
    touch->panel_height = screen_panel_height;
    touch->view_x       = screen_view_x;
    touch->view_y       = screen_view_y;
    LIB_LOG(touch_note, "Touch screen size set to: %dx%d (panel %dx%d, view %d,%d)",
           width, height, touch->panel_width, touch->panel_height,
           touch->view_x, touch->view_y);
    publish_safe_area(touch);
}

void touch_set_viewport(TouchInput *touch, int panel_w, int panel_h,
                        int view_x, int view_y) {
    touch->panel_width  = panel_w;
    touch->panel_height = panel_h;
    touch->view_x       = view_x;
    touch->view_y       = view_y;
    LIB_LOG(touch_note, "Touch viewport set: panel %dx%d, view %d,%d",
           panel_w, panel_h, view_x, view_y);
    publish_safe_area(touch);
}

void touch_close(TouchInput *touch) {
    if (touch->fd != -1) {
        close(touch->fd);
    }
}

static void touch_resync(TouchInput *touch);

int touch_wait_for_press_raw(TouchInput *touch, int *raw_x, int *raw_y) {
    // Wait for a touch press event (blocking), returning RAW hardware coords.
    //
    // CRITICAL: Linux input events arrive in this order:
    //   1. ABS_X, ABS_Y (coordinates)
    //   2. BTN_TOUCH (press/release)
    //   3. SYN_REPORT (frame complete)
    // We must capture coordinates BEFORE checking for press.

    struct input_event ev;
    int current_x = -1, current_y = -1;
    bool got_press = false;
    int rc = -1;
    InputSynDrop sd = { false, false };

    int flags = fcntl(touch->fd, F_GETFL, 0);
    fcntl(touch->fd, F_SETFL, flags & ~O_NONBLOCK);

    // poll() rather than a bare blocking read: the old loop had no `else` on
    // the read, so any error spun at 100% CPU forever with `return -1` as dead
    // code, and glibc signal() implies SA_RESTART so SIGTERM could not break it
    // either. Waking every POLL_SLICE_MS lets the caller's signal handler run
    // and lets a genuine error propagate.
    const int POLL_SLICE_MS = 200;
    struct pollfd pfd = { .fd = touch->fd, .events = POLLIN, .revents = 0 };

    while (1) {
        int pr = poll(&pfd, 1, POLL_SLICE_MS);
        if (pr < 0) {
            if (errno == EINTR) continue;   // signal during the wait; retry
            break;                          // real poll failure
        }
        if (pr == 0) continue;              // idle slice, just loop
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) break;

        ssize_t n = read(touch->fd, &ev, sizeof(ev));
        if (n != (ssize_t)sizeof(ev)) {
            if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
            break;                          // short read or device gone
        }

        if (input_syn_drop_skip(&sd, &ev)) {
            // At the end of the torn packet: a lift lost in the drop would
            // otherwise leave `touching` set and swallow the next press; a
            // touch-down lost in it is the press being waited for.
            if (sd.resync && !sd.dropping) {
                sd.resync = false;
                got_press = false;
                touch->state.pressed = false;
                touch_resync(touch);
                if (touch->state.pressed) {
                    *raw_x = touch->last_x;
                    *raw_y = touch->last_y;
                    rc = 0;
                    break;
                }
            }
            continue;
        }

        if (ev.type == EV_ABS) {
            if (ev.code == ABS_X) current_x = ev.value;
            else if (ev.code == ABS_Y) current_y = ev.value;
        } else if (ev.type == EV_KEY && ev.code == BTN_TOUCH) {
            if (ev.value == 0) {
                touch->touching = false;
                got_press = false;
            } else if (ev.value == 1 && !touch->touching) {
                touch->touching = true;
                got_press = true;
            }
        } else if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
            if (got_press && current_x >= 0 && current_y >= 0) {
                *raw_x = current_x;
                *raw_y = current_y;
                rc = 0;
                break;
            }
            got_press = false;
        }
    }

    fcntl(touch->fd, F_SETFL, flags);
    return rc;
}

int touch_wait_for_press(TouchInput *touch, int *x, int *y) {
    // Blocking press returning SCALED/calibrated screen coordinates.
    if (touch_wait_for_press_raw(touch, x, y) < 0)
        return -1;
    scale_coordinates(touch, x, y);
    return 0;
}

/* The press that woke a blanked panel is held back until its release. */
static bool blank_swallow = false;

// After a SYN_DROPPED (input_scan.h): the finger lift — or touch-down — may
// have been among the events the kernel discarded, so take the level and the
// position from the kernel and synthesise whichever edge differs. A failed
// EVIOCGKEY reads as lifted: a touch stuck down is the worse failure.
static void touch_resync(TouchInput *touch) {
    unsigned long keys[INPUT_SCAN_NLONGS(KEY_MAX + 1)];
    const int lb = (int)(8 * sizeof(unsigned long));
    struct input_absinfo ai;
    memset(keys, 0, sizeof(keys));
    if (ioctl(touch->fd, EVIOCGABS(ABS_X), &ai) == 0) touch->last_x = ai.value;
    if (ioctl(touch->fd, EVIOCGABS(ABS_Y), &ai) == 0) touch->last_y = ai.value;
    bool down = ioctl(touch->fd, EVIOCGKEY(sizeof(keys)), keys) >= 0 &&
                ((keys[BTN_TOUCH / lb] >> (BTN_TOUCH % lb)) & 1UL);
    if (down && !touch->touching) {
        touch->touching = true;
        blank_swallow = hw_blank_note_activity();
        if (!blank_swallow) {
            touch->state.pressed = true;
            touch->state.held = true;
        }
    } else if (!down && touch->touching) {
        touch->touching = false;
        if (blank_swallow) {
            blank_swallow = false;
        } else {
            touch->state.released = true;
            touch->state.held = false;
        }
    }
    if (touch->touching) {
        int x = touch->last_x, y = touch->last_y;
        scale_coordinates(touch, &x, &y);
        touch->state.x = x;
        touch->state.y = y;
    }
}


int touch_poll(TouchInput *touch) {
    hw_blank_poll();
    // Non-blocking poll. Process events in arrival order:
    //   1. ABS_X, ABS_Y → last_x/last_y (RAW)
    //   2. BTN_TOUCH    → press/release (uses last_x/last_y)
    //   3. SYN_REPORT   → update calibrated state.x/y

    struct input_event ev;
    int events_read = 0;
    InputSynDrop sd = { false, false };

    touch->state.pressed = false;
    touch->state.released = false;

    while (read(touch->fd, &ev, sizeof(ev)) == sizeof(ev)) {
        events_read++;
        if (input_syn_drop_skip(&sd, &ev)) continue;

        if (ev.type == EV_ABS) {
            if (ev.code == ABS_X) touch->last_x = ev.value;
            else if (ev.code == ABS_Y) touch->last_y = ev.value;
        } else if (ev.type == EV_KEY && ev.code == BTN_TOUCH) {
            if (ev.value == 1 && !touch->touching) {
                touch->touching = true;
                int x = touch->last_x, y = touch->last_y;
                scale_coordinates(touch, &x, &y);
                touch->state.x = x;
                touch->state.y = y;
                /* The touch that wakes a dark panel presses nothing. */
                blank_swallow = hw_blank_note_activity();
                if (!blank_swallow) {
                    touch->state.pressed = true;
                    touch->state.held = true;
                }
            } else if (ev.value == 0 && touch->touching) {
                touch->touching = false;
                if (blank_swallow) {
                    blank_swallow = false;      /* its release too */
                } else {
                    touch->state.released = true;
                    touch->state.held = false;
                }
                hw_blank_keepalive();
            }
        } else if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
            if (touch->touching) {
                hw_blank_keepalive();
                int x = touch->last_x, y = touch->last_y;
                scale_coordinates(touch, &x, &y);
                touch->state.x = x;
                touch->state.y = y;
            }
        }
    }
    if (sd.resync)
        touch_resync(touch);

    return events_read;
}

TouchState touch_get_state(TouchInput *touch) {
    return touch->state;
}

void touch_set_raw_range(TouchInput *touch,
                         int min_x, int max_x, int min_y, int max_y) {
    // Guard against degenerate ranges (would divide badly / invert axes).
    if (max_x - min_x < 16) { min_x = 0; max_x = 4095; }
    if (max_y - min_y < 16) { min_y = 0; max_y = 4095; }
    touch->raw_min_x = min_x; touch->raw_max_x = max_x;
    touch->raw_min_y = min_y; touch->raw_max_y = max_y;
    // No curve: the sentinel that makes the axis map map linearly end to end.
    touch->raw_knot_lo_x = touch->raw_knot_hi_x = 0;
    touch->raw_knot_lo_y = touch->raw_knot_hi_y = 0;
    touch->calib.enabled = true;
    touch->calibrated = true;
    LIB_LOG(touch_note, "Touch raw range set (linear): X [%d..%d] Y [%d..%d]",
           min_x, max_x, min_y, max_y);
    publish_safe_area(touch);
}

void touch_set_raw_curve(TouchInput *touch,
                         int x0, int xk_lo, int xk_hi, int x1,
                         int y0, int yk_lo, int yk_hi, int y1) {
    if (x1 - x0 < 16) { x0 = 0; x1 = 4095; xk_lo = xk_hi = 0; }
    if (y1 - y0 < 16) { y0 = 0; y1 = 4095; yk_lo = yk_hi = 0; }

    touch->raw_min_x = x0; touch->raw_max_x = x1;
    touch->raw_min_y = y0; touch->raw_max_y = y1;

    // A non-monotone axis is a corrupt or hand-broken config; fall back to the
    // endpoint line for that axis alone rather than rejecting the whole file.
    if (x0 < xk_lo && xk_lo < xk_hi && xk_hi < x1) {
        touch->raw_knot_lo_x = xk_lo; touch->raw_knot_hi_x = xk_hi;
    } else {
        touch->raw_knot_lo_x = touch->raw_knot_hi_x = 0;   // linear
        if (xk_lo || xk_hi)
            LIB_LOG(LOG_LEVEL_WARN, "Touch calibration: X knots non-monotone, using linear map");
    }
    if (y0 < yk_lo && yk_lo < yk_hi && yk_hi < y1) {
        touch->raw_knot_lo_y = yk_lo; touch->raw_knot_hi_y = yk_hi;
    } else {
        touch->raw_knot_lo_y = touch->raw_knot_hi_y = 0;   // linear
        if (yk_lo || yk_hi)
            LIB_LOG(LOG_LEVEL_WARN, "Touch calibration: Y knots non-monotone, using linear map");
    }

    touch->calib.enabled = true;
    touch->calibrated = true;
    LIB_LOG(touch_note, "Touch raw curve set: X [%d %d %d %d] Y [%d %d %d %d]",
           touch->raw_min_x, touch->raw_knot_lo_x, touch->raw_knot_hi_x, touch->raw_max_x,
           touch->raw_min_y, touch->raw_knot_lo_y, touch->raw_knot_hi_y, touch->raw_max_y);
    publish_safe_area(touch);
}

void touch_enable_calibration(TouchInput *touch, bool enable) {
    touch->calib.enabled = enable;
    LIB_LOG(touch_note, "Touch calibration %s", enable ? "enabled" : "disabled");
}

int touch_fit_axis_range(const int *raw, const int *scr, int n, int dim,
                         int *raw_at_0, int *raw_at_max) {
    if (n < 2) return -1;
    double Sr = 0, Ss = 0, Srr = 0, Srs = 0;
    for (int i = 0; i < n; i++) {
        double r = raw[i], s = scr[i];
        Sr += r; Ss += s; Srr += r * r; Srs += r * s;
    }
    double denom = (double)n * Srr - Sr * Sr;
    if (denom < 1e-6 && denom > -1e-6) return -1;   // collinear-in-raw / degenerate
    double m = ((double)n * Srs - Sr * Ss) / denom; // screen = m*raw + b
    if (m > -1e-9 && m < 1e-9) return -1;
    double b = (Ss - m * Sr) / (double)n;
    double r0 = (0.0 - b) / m;
    double r1 = ((double)(dim - 1) - b) / m;
    *raw_at_0   = (int)(r0 + (r0 >= 0 ? 0.5 : -0.5));
    *raw_at_max = (int)(r1 + (r1 >= 0 ? 0.5 : -0.5));
    return 0;
}

int touch_save_calibration(TouchInput *touch, const char *filename) {
    FILE *f = fopen(filename, "w");
    if (!f) {
        perror("Failed to open calibration file for writing");
        return -1;
    }

    fprintf(f, "# Screen configuration\n");
    fprintf(f, "# Line 1: touch calibration — the raw→panel curve, per axis the raw\n");
    fprintf(f, "#         readings at panel 0, dim/4, 3*dim/4 and dim-1:\n");
    fprintf(f, "#           x0 x_knot_lo x_knot_hi x1   y0 y_knot_lo y_knot_hi y1\n");
    fprintf(f, "#         Three linear segments, least-squares fitted over the interior\n");
    fprintf(f, "#         targets. The endpoints are the FIT's own values and may fall\n");
    fprintf(f, "#         outside 0..4095 — that is correct and must not be clamped: the\n");
    fprintf(f, "#         digitiser saturates before the physical edge, so clamping tilts\n");
    fprintf(f, "#         the outer segment and the cursor runs ahead of the finger.\n");
    fprintf(f, "#         Four numbers here is the legacy endpoint-only form and is still\n");
    fprintf(f, "#         read (and migrated on load).\n");
    fprintf(f, "%d %d %d %d  %d %d %d %d\n",
            touch->raw_min_x, touch->raw_knot_lo_x,
            touch->raw_knot_hi_x, touch->raw_max_x,
            touch->raw_min_y, touch->raw_knot_lo_y,
            touch->raw_knot_hi_y, touch->raw_max_y);
    fprintf(f, "# Line 2: bezel margins — top bottom left right\n");
    fprintf(f, "#         (panel pixels hidden by the bezel; the drawing surface is\n");
    fprintf(f, "#          the rectangle inside them. Omit the line for the defaults.)\n");
    fprintf(f, "%d %d %d %d\n",
            touch->calib.bezel_top, touch->calib.bezel_bottom,
            touch->calib.bezel_left, touch->calib.bezel_right);
    fprintf(f, "# Line 3 (optional): 'reach' — the raw extremes the physical EDGES\n");
    fprintf(f, "#         actually emit, from the wizard's edge sweep:\n");
    fprintf(f, "#           reach x_lo x_hi y_lo y_hi\n");
    fprintf(f, "#         Pushed through line 1's curve, these give the band that is\n");
    fprintf(f, "#         visible but not touchable (SCREEN_SAFE_* vs SCREEN_VISIBLE_*).\n");
    fprintf(f, "#         Omit for 'every edge reaches the hardware limit'.\n");
    fprintf(f, "reach %d %d %d %d\n",
            touch->reach_min_x, touch->reach_max_x,
            touch->reach_min_y, touch->reach_max_y);

    fclose(f);
    LIB_LOG(LOG_LEVEL_INFO, "Calibration saved to: %s  (X[%d %d %d %d] Y[%d %d %d %d] reach X[%d..%d] Y[%d..%d])",
           filename,
           touch->raw_min_x, touch->raw_knot_lo_x,
           touch->raw_knot_hi_x, touch->raw_max_x,
           touch->raw_min_y, touch->raw_knot_lo_y,
           touch->raw_knot_hi_y, touch->raw_max_y,
           touch->reach_min_x, touch->reach_max_x,
           touch->reach_min_y, touch->reach_max_y);
    return 0;
}

int touch_load_calibration(TouchInput *touch, const char *filename) {
    FILE *f = fopen(filename, "r");
    if (!f) {
        LIB_LOG(touch_note, "No calibration file found: %s (using hardware range)", filename);
        return -1;
    }

    // Panel extents per raw axis, needed to place migrated knots on the line the
    // legacy file described. touch_init() sets geometry and portrait before this.
    int phys_w, phys_h;
    axis_dims(touch, &phys_w, &phys_h);

    char line[256];
    bool got_range = false;
    bool got_margins = false;

    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n') continue;

        // Line 3 is keyword-tagged so it cannot be mistaken for the positional
        // lines above it — which matters if line 2 is ever missing from a
        // hand-edited file.
        int rv[4];
        if (sscanf(line, "reach %d %d %d %d",
                   &rv[0], &rv[1], &rv[2], &rv[3]) == 4) {
            if (rv[1] - rv[0] >= 16 && rv[3] - rv[2] >= 16) {
                touch->reach_min_x = rv[0]; touch->reach_max_x = rv[1];
                touch->reach_min_y = rv[2]; touch->reach_max_y = rv[3];
                LIB_LOG(touch_note, "  Measured edge reach: X [%d..%d] Y [%d..%d]",
                       rv[0], rv[1], rv[2], rv[3]);
            } else {
                LIB_LOG(LOG_LEVEL_WARN, "  Ignoring degenerate 'reach' line (X %d..%d Y %d..%d)",
                       rv[0], rv[1], rv[2], rv[3]);
            }
            continue;
        }

        if (!got_range) {
            // Line 1: eight numbers (piecewise) or four (legacy endpoints).
            int v[8];
            int n = sscanf(line, "%d %d %d %d %d %d %d %d",
                           &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]);
            if (n == 8) {
                touch_set_raw_curve(touch, v[0], v[1], v[2], v[3],
                                           v[4], v[5], v[6], v[7]);
                got_range = true;
                LIB_LOG(touch_note, "Calibration loaded from: %s (piecewise)", filename);
            } else if (n == 4) {
                // Legacy: reproduce exactly the mapping this file described, by
                // putting the knots on its line. Nothing is clamped — a legacy
                // file carries no edge measurement, so there is no honest basis
                // for moving its endpoints, and an earlier revision that pulled
                // them into 0..4095 is precisely what tilted the outer segments
                // and made the cursor run ahead of the finger near the bottom.
                int kx_lo, kx_hi, ky_lo, ky_hi;
                touch_knots_on_line(v[0], v[1], phys_w, &kx_lo, &kx_hi);
                touch_knots_on_line(v[2], v[3], phys_h, &ky_lo, &ky_hi);
                touch_set_raw_curve(touch, v[0], kx_lo, kx_hi, v[1],
                                           v[2], ky_lo, ky_hi, v[3]);
                got_range = true;
                LIB_LOG(touch_note, "Calibration loaded from: %s (legacy 4-value, migrated "
                       "onto its own line)", filename);
            }
        } else if (!got_margins) {
            // Line 2: bezel margins (top bottom left right)
            int m[4];
            if (sscanf(line, "%d %d %d %d", &m[0], &m[1], &m[2], &m[3]) == 4) {
                touch->calib.bezel_top    = m[0];
                touch->calib.bezel_bottom = m[1];
                touch->calib.bezel_left   = m[2];
                touch->calib.bezel_right  = m[3];
                got_margins = true;
                LIB_LOG(touch_note, "  Bezel margins: T=%d B=%d L=%d R=%d", m[0], m[1], m[2], m[3]);
            }
        }
    }

    fclose(f);

    if (!got_range) {
        fprintf(stderr, "Failed to parse calibration file: %s\n", filename);
        return -1;
    }
    if (!got_margins)
        LIB_LOG(touch_note, "  No bezel margins in file (framebuffer defaults apply)");

    touch->calib.enabled = true;
    // The 'reach' line may come after line 1, whose touch_set_raw_curve() already
    // published a safe area from the previous reach values. Republish so the two
    // are consistent whatever order the file is in.
    publish_safe_area(touch);
    return 0;
}

void touch_set_edge_reach(TouchInput *touch,
                          int x_lo, int x_hi, int y_lo, int y_hi) {
    if (x_hi - x_lo < 16 || y_hi - y_lo < 16) {
        LIB_LOG(LOG_LEVEL_WARN, "Touch edge reach rejected as degenerate: X %d..%d Y %d..%d",
               x_lo, x_hi, y_lo, y_hi);
        return;
    }
    touch->reach_min_x = x_lo; touch->reach_max_x = x_hi;
    touch->reach_min_y = y_lo; touch->reach_max_y = y_hi;
    LIB_LOG(touch_note, "Touch edge reach set: X [%d..%d] Y [%d..%d]", x_lo, x_hi, y_lo, y_hi);
    publish_safe_area(touch);
}

void touch_drain_events(TouchInput *touch) {
    // Read and discard all pending events (call after reopening the device so
    // stale events don't trigger a spurious press in a menu).
    struct input_event ev;
    int flags = fcntl(touch->fd, F_GETFL, 0);
    fcntl(touch->fd, F_SETFL, flags | O_NONBLOCK);

    int drained = 0;
    while (read(touch->fd, &ev, sizeof(ev)) == sizeof(ev))
        drained++;

    fcntl(touch->fd, F_SETFL, flags);

    touch->touching = false;
    touch->state.pressed  = false;
    touch->state.released = false;
    touch->state.held     = false;

    if (drained > 0)
        LIB_LOG(LOG_LEVEL_DEBUG, "touch_drain_events: discarded %d stale events", drained);
}
