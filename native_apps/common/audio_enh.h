#ifndef AUDIO_ENH_H
#define AUDIO_ENH_H

/**
 * The system "enhancer": three added-content parts and a peak guard, applied
 * after the equaliser.  Pure (no device; the only file access is the optional
 * config reader), fixed-point in the per-sample path, in the style of
 * audio_eq.h: Q28 biquad coefficients, Q8 signal in an int32, int64
 * accumulate, a saturation per stage, and NO division or libm in
 * enh_process() (the Cortex-A8 has no hardware divide).  enh_design() uses
 * libm doubles and runs only when the configuration changes.
 *
 *  Harmonic bass   mono sum -> low-pass (200..250 Hz) -> soft cubic shaper
 *                  (drive 1..4x, odd harmonics) -> low-pass 160 Hz.  Range =
 *                  drive and the first corner.
 *  Drum bass       difference of two low-passes on the mono sum (a band-pass-
 *                  like punch band).  Range moves both corners (about
 *                  100..400 Hz at 50, 150..600 Hz at 100).
 *  Harmonic treble per channel high-pass (5..3 kHz) -> sine table shaper
 *                  (odd harmonics, bounded).  Range moves the corner.
 *  Peak guard      the dry signal stays at unity; only the ADDED content is
 *                  scaled, by a gain that drops at once (-25 % steps) when
 *                  dry + added would leave int16 and recovers in about 90 ms.
 *
 * Each amount and range is 0..100.  Enhancer off, or all three amounts 0, is a
 * bit-exact bypass: enh_process() returns before touching the buffer.
 * One thread owns an EnhState; there is no locking.
 */

#include <stdint.h>
#include <stddef.h>

#define ENH_MAX_CH      2
#define ENH_CONFIG_KEY  "enh"

/** The user-facing settings; all fields 0..100 except `on` (0/1). */
typedef struct { int on; int hb_amt, hb_rng, db_amt, db_rng, ht_amt, ht_rng; } AudioEnh;

typedef struct {
    int32_t b0, b1, b2, a1, a2;      /* Q28, a0 normalised to 1 */
    int32_t hp;                      /* 1: high-pass (primes to zero output) */
} EnhCoef;

typedef struct {
    int32_t x1, x2, y1, y2;          /* Q8 history */
    int32_t primed;
} EnhHist;

typedef struct {
    int      rate;
    AudioEnh cfg;                    /* the clamped settings this was designed from */
    int      active;                 /* 0 = bypass */
    int      hb, db, ht;             /* part enabled */
    int32_t  hb_g, db_g, ht_g;       /* Q15 output gains */
    int32_t  hb_drive;               /* Q8, 256..1024 */
    int32_t  ht_drive;               /* Q8 */
    EnhCoef  hb1, hb2, db_lo, db_hi, ht_hp;
} EnhParams;

typedef struct {
    EnhHist hb1, hb2, db_lo, db_hi;  /* on the mono sum */
    EnhHist ht[ENH_MAX_CH];
    int32_t guard;                   /* Q15 gain on the added content; 0 = unset */
} EnhState;

/** Clamp every field of `e` to its range. */
void enh_clamp(AudioEnh *e);
/** Design `p` for `rate` from `e` (clamped). */
void enh_design(EnhParams *p, int rate, const AudioEnh *e);
void enh_state_reset(EnhState *s);
/** Process `frames` interleaved frames in place; channels 1..ENH_MAX_CH, else
 *  (or for a bypass design) the buffer is untouched. */
void enh_process(EnhState *s, const EnhParams *p, int16_t *buf, long frames, int channels);

/** Parse "on,hb,hbr,db,dbr,ht,htr".  0 ok (fields clamped), -1 bad. */
int  audio_enh_parse(const char *value, AudioEnh *out);
void audio_enh_format(const AudioEnh *e, char *buf, size_t n);
/** Read the `enh` key of a key=value file.  0 found and valid; -1 otherwise,
 *  with `*e` set to off (ranges 50). */
int  enh_config_read(const char *path, AudioEnh *e);

#endif
