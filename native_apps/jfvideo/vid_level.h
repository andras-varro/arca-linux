/*
 * vid_level.h — loudness for the video player's sound: float PCM from the decoder
 * (interleaved, +-1.0 full scale, 1 or 2 channels) to s16 for the mix bus, with a
 * slow automatic gain control and a hard limiter.
 *
 * Why it exists: the server's 5.1 -> stereo/mono downmix peaks near -18 dBFS, so
 * dialogue is barely audible on this speaker.  A fixed boost would clip the first
 * explosion, so the gain follows the programme level slowly (it must not pump with
 * every sentence) and the limiter handles what the slow loop cannot see coming.
 *
 * Block based: one call to vid_level_process() is one block (typically 1152 frames,
 * 26 ms at 44100 Hz).  There is no per-sample divide; the few divides per block are
 * float.
 */
#ifndef ARCA_VID_LEVEL_H
#define ARCA_VID_LEVEL_H

#include <stdint.h>

typedef struct {
    float inv_rate;      /**< seconds per frame                               */
    int   nch;           /**< 1 or 2                                          */
    int   agc_on;
    float fixed_gain;    /**< linear gain used when the AGC is off            */
    float gain;          /**< AGC state: the gain the loop wants (linear)      */
    float applied;       /**< gain at the end of the last block (after the limiter) */
    float pw;            /**< tracked mean-square of the speech level          */
    int   seeded;        /**< the first active block has been seen             */
} VidLevel;

/** `rate` in Hz, `nch` 1 or 2.  AGC on, fixed gain +6 dB until vid_level_set(). */
void  vid_level_init(VidLevel *l, int rate, int nch);
/** AGC on: gain follows the level (0..+18 dB).  AGC off: constant `fixed_gain_db`; the limiter stays active either way. */
void  vid_level_set(VidLevel *l, int agc_on, float fixed_gain_db);
/**
 * Convert one block.  `in` holds frames*nch floats, `out` frames*nch s16.  No output
 * sample exceeds -1 dBFS (29204), whatever the input.
 */
void  vid_level_process(VidLevel *l, const float *in, int frames, int16_t *out);
/** Linear gain applied at the end of the last block (1.0 = 0 dB).  The limiter can take it below 1 on a loud block. */
float vid_level_gain(const VidLevel *l);

#endif
