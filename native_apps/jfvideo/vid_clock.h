/*
 * vid_clock.h — the playback clock of the video player.  Audio is the master: this
 * turns the audio side's frame counters into "what media time is the listener
 * hearing right now", and decides what to do with a decoded picture.
 *
 * Pure: no syscall, no thread.  The caller passes CLOCK_MONOTONIC in as seconds, and
 * nothing here divides by an integer (the target has no hardware divide): the rate is
 * stored as a reciprocal.
 *
 * Inputs come from the mix bus after each audio pump:
 *     rendered  frames the bus has handed to the output since the voice started
 *               (total handed - voice_pending)
 *     padded    of those, frames that were silence added because the decoder
 *               under-ran (the clock must not move for them)
 *     lead      frames the output holds that the listener has not heard yet
 * The raw value  base_pts + (rendered - padded - lead) / rate  is a staircase (the
 * pump renders in 2048-frame steps, ~46 ms), so between samples the clock is
 * extrapolated with the monotonic clock and each new step re-anchors it by at most
 * VID_CLK_SLEW seconds per call.  That slew limit is what absorbs the ~144 ppm
 * between the audio crystal and CLOCK_MONOTONIC and also hides pump jitter; a step
 * larger than VID_CLK_SNAP is a discontinuity (seek, resume after a long underrun)
 * and is taken at once.
 */
#ifndef ARCA_VID_CLOCK_H
#define ARCA_VID_CLOCK_H

#define VID_CLK_WAIT 0   /**< the frame is early: look again later          */
#define VID_CLK_SHOW 1   /**< present it now                                */
#define VID_CLK_DROP 2   /**< a whole frame period late: skip it            */

typedef struct {
    double inv_rate;        /**< seconds per audio frame                     */
    double base_pts;        /**< media time of the voice's first frame        */
    double offset;          /**< A/V offset, seconds: > 0 shows video later   */
    double est;             /**< estimate at est_mono                         */
    double est_mono;        /**< monotonic time of est                        */
    double deadline;        /**< monotonic time past which est stops advancing */
    double floor_t;         /**< highest value handed out (monotone output)   */
    double last_raw;        /**< raw staircase value of the previous sample   */
    long long last_padded;
    int    anchored;        /**< a sample has been taken since start()        */
    int    frozen;          /**< audio is starved: the clock is stopped       */
} VidClock;

/** `rate` in Hz. */
void vid_clock_init(VidClock *c, int rate);
/** A new voice (play, seek): forget the anchor.  `base_pts` is the media time of its first frame. */
void vid_clock_start(VidClock *c, double base_pts);
/** Call after each audio pump with the bus counters (see above) and CLOCK_MONOTONIC in seconds. */
void vid_clock_sample(VidClock *c, long long rendered, long long padded, long long lead_frames, double mono_now);
/**
 * Media time being heard at `mono_now`.  Never decreases within one anchor, and does
 * not run on while audio is starved or stalled (at most ~0.15 s past the last step).
 */
double vid_clock_now(VidClock *c, double mono_now);
/** Seconds; added to every frame pts in vid_clock_decide(). */
void vid_clock_set_offset(VidClock *c, double seconds);
/**
 * WAIT when the frame is more than 4 ms early, DROP when it is a full `period` or more
 * late, SHOW between.  `audio_t` is vid_clock_now().
 */
int vid_clock_decide(const VidClock *c, double frame_pts, double audio_t, double period);

#endif
