/*
 * vid_clock.c — see vid_clock.h.
 *
 * Why the estimate is rebuilt only when the raw staircase CHANGES: the raw value
 * moves in one step per pump and sits still in between, so a sample taken on an
 * unchanged raw carries no information about the time since the step; re-anchoring
 * to it would pull the estimate back by up to a pump interval every call.  A step
 * is accurate at the moment it appears, so that is when the estimate is corrected,
 * and it is corrected by a bounded amount: pump timing jitter would otherwise move
 * the video clock as much as the jitter, and the picture would stutter.
 *
 * Starvation: when the decoder under-runs the bus pads silence and counts it in
 * `padded`.  Those frames are subtracted from the raw value, so raw stands still;
 * the extrapolation is frozen on the first sample where `padded` advanced, or the
 * clock would run on through a gap in the sound.  A voice that is merely paused
 * (nothing rendered, nothing padded) is caught by the deadline.
 */
#include "vid_clock.h"

#define VC_SLEW   0.002     /* largest correction per accepted step, s            */
#define VC_SNAP   0.25      /* beyond this the jump is real: take it at once, s   */
#define VC_CAP    0.15      /* extrapolate at most this far past the last step, s */
#define VC_EARLY  0.004     /* a frame this early or less is shown, s             */

static double clampd(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }

void vid_clock_init(VidClock *c, int rate)
{
    c->inv_rate = rate > 0 ? 1.0 / (double)rate : 0.0;
    c->offset = 0.0;
    vid_clock_start(c, 0.0);
}

void vid_clock_start(VidClock *c, double base_pts)
{
    c->base_pts = base_pts;
    c->est = base_pts;
    c->est_mono = 0.0;
    c->deadline = 0.0;
    c->floor_t = base_pts;
    c->last_raw = base_pts;
    c->last_padded = 0;
    c->anchored = 0;
    c->frozen = 0;
}

/* The estimate before the monotone floor: est advanced by the monotonic clock, held
 * while frozen, and not past the deadline. */
static double extrapolate(const VidClock *c, double mono_now)
{
    if (!c->anchored) return c->base_pts;
    if (c->frozen) return c->est;
    double m = mono_now < c->deadline ? mono_now : c->deadline;
    return m > c->est_mono ? c->est + (m - c->est_mono) : c->est;
}

void vid_clock_sample(VidClock *c, long long rendered, long long padded, long long lead_frames, double mono_now)
{
    const long long pad = padded;
    double raw = c->base_pts + (double)(rendered - pad - lead_frames) * c->inv_rate;

    if (!c->anchored) {
        c->est = raw; c->est_mono = mono_now; c->deadline = mono_now + VC_CAP;
        c->floor_t = raw; c->last_raw = raw; c->last_padded = pad;
        c->anchored = 1; c->frozen = 0;
        return;
    }

    double cur = extrapolate(c, mono_now);

    if (pad > c->last_padded) {                 /* starved: stop where we are */
        c->est = cur; c->est_mono = mono_now; c->frozen = 1;
        c->last_padded = pad;
        c->last_raw = raw;
        return;
    }
    c->last_padded = pad;

    if (raw == c->last_raw) return;             /* no new step: keep extrapolating */
    c->last_raw = raw;

    double err = raw - cur;
    if (err > VC_SNAP || err < -VC_SNAP) {      /* seek / long underrun: real jump */
        c->est = raw;
        c->floor_t = raw;
    } else {
        double step = clampd(err, -VC_SLEW, VC_SLEW);
        c->est = cur + step;
    }
    c->est_mono = mono_now;
    c->deadline = mono_now + VC_CAP;
    c->frozen = 0;
}

double vid_clock_now(VidClock *c, double mono_now)
{
    double v = extrapolate(c, mono_now);
    if (v < c->floor_t) v = c->floor_t;
    else c->floor_t = v;
    return v;
}

void vid_clock_set_offset(VidClock *c, double seconds) { c->offset = seconds; }

int vid_clock_decide(const VidClock *c, double frame_pts, double audio_t, double period)
{
    double d = frame_pts + c->offset - audio_t;
    if (d > VC_EARLY) return VID_CLK_WAIT;
    if (d > -period)  return VID_CLK_SHOW;
    return VID_CLK_DROP;
}
