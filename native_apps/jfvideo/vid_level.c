/*
 * vid_level.c — see vid_level.h.
 *
 * Per block: measure peak and mean-square, advance the AGC, then apply the limiter.
 *
 * AGC.  A power tracker follows the programme but only on blocks above a silence
 * gate (-55 dBFS RMS): in a pause the noise floor must not be mistaken for quiet
 * speech, or the gain would climb during every pause and blast the next word (the
 * classic pumping).  On gated blocks the gain is simply HELD.  The wanted gain is
 * sqrt(target power / tracked power), clamped to 0..+18 dB.  It is smoothed with
 * different speeds each way: falling (loud passage) in 50 ms, rising in 2 s, using
 * coefficient = min(1, block duration / tau) per block.  The first active block
 * seeds both the tracker and the gain so the start of a film is not spent ramping.
 *
 * Limiter.  The block is known before it is played, so the limiter is exact rather
 * than predictive: the gain that would push the block's peak over the ceiling is
 * cap = ceiling/peak.  If that is below the gain used so far, the whole block gets
 * the lower gain as a constant (a step at the block edge: no sample can exceed the
 * ceiling, and a step is better than letting one through) and the AGC state is
 * pulled down to it, so the loop does not fight the limiter.  If the gain is rising
 * it ramps linearly across the block from the previous block's gain, which is
 * below the new gain and so below the cap.  A final clamp is a safety net for
 * float rounding.
 */
#include "vid_level.h"

#include <math.h>

#define VL_CEIL      0.8912509f          /* -1 dBFS                                 */
#define VL_CEIL_S    29204.0f            /* VL_CEIL * 32767, rounded down           */
#define VL_GATE_PW   3.1622777e-6f       /* -55 dBFS RMS as mean-square             */
#define VL_TARGET_PW 0.01f               /* -20 dBFS RMS as mean-square             */
#define VL_TAU_PW    0.7f                /* power tracker, s                        */
#define VL_TAU_DOWN  0.05f               /* gain must fall, s                       */
#define VL_TAU_UP    2.0f                /* gain may rise, s                        */
#define VL_GAIN_MAX  7.943282f           /* +18 dB                                  */
#define VL_FIXED_DB  6.0f

static float minf(float a, float b) { return a < b ? a : b; }
static float db_to_lin(float db) { return powf(10.0f, db * 0.05f); }

void vid_level_init(VidLevel *l, int rate, int nch)
{
    l->inv_rate = rate > 0 ? 1.0f / (float)rate : 0.0f;
    l->nch = nch == 2 ? 2 : 1;
    l->agc_on = 1;
    l->fixed_gain = db_to_lin(VL_FIXED_DB);
    l->gain = 1.0f;
    l->applied = 1.0f;
    l->pw = VL_TARGET_PW;
    l->seeded = 0;
}

void vid_level_set(VidLevel *l, int agc_on, float fixed_gain_db)
{
    l->agc_on = agc_on != 0;
    l->fixed_gain = db_to_lin(fixed_gain_db);
    if (!l->agc_on) l->gain = l->fixed_gain;
    else l->seeded = 0;                          /* re-seed from the next active block */
}

float vid_level_gain(const VidLevel *l) { return l->applied; }

static float wanted_gain(float pw)
{
    float g = sqrtf(VL_TARGET_PW / pw);
    return g < 1.0f ? 1.0f : (g > VL_GAIN_MAX ? VL_GAIN_MAX : g);
}

void vid_level_process(VidLevel *l, const float *in, int frames, int16_t *out)
{
    if (frames <= 0) return;
    const int n = frames * l->nch;
    float peak = 0.0f, sq = 0.0f;
    for (int i = 0; i < n; i++) {
        float a = in[i] < 0 ? -in[i] : in[i];
        if (a > peak) peak = a;
        sq += in[i] * in[i];
    }
    const float ms = sq / (float)n;
    const float dur = (float)frames * l->inv_rate;

    if (l->agc_on) {
        if (ms > VL_GATE_PW) {
            if (!l->seeded) {
                l->pw = ms;
                l->gain = wanted_gain(l->pw);
                l->seeded = 1;
            } else {
                l->pw += minf(1.0f, dur / VL_TAU_PW) * (ms - l->pw);
                float want = wanted_gain(l->pw);
                float a = minf(1.0f, dur / (want < l->gain ? VL_TAU_DOWN : VL_TAU_UP));
                l->gain += a * (want - l->gain);
            }
        }                                         /* gated block: hold the gain */
    } else {
        l->gain = l->fixed_gain;
    }

    /* limiter */
    float g1 = l->gain;
    if (peak > 0.0f) {
        float cap = VL_CEIL / peak;
        if (cap < g1) g1 = cap;
    }
    const float g0 = l->applied;
    float g, step;
    if (g1 < g0) {                                /* constant lower gain over the whole block */
        g = g1; step = 0.0f;
        if (g1 < l->gain) l->gain = g1;           /* pull the AGC state down to it */
    } else {                                      /* ramp up from the previous block */
        g = g0; step = (g1 - g0) * (1.0f / (float)frames);
    }
    for (int f = 0; f < frames; f++) {
        for (int c = 0; c < l->nch; c++) {
            float v = in[f * l->nch + c] * g * 32767.0f;
            if (v > VL_CEIL_S) v = VL_CEIL_S;
            else if (v < -VL_CEIL_S) v = -VL_CEIL_S;
            out[f * l->nch + c] = (int16_t)(v >= 0.0f ? v + 0.5f : v - 0.5f);
        }
        g += step;
    }
    l->applied = g1;
}
