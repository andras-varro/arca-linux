#ifndef AUDIO_EQ_H
#define AUDIO_EQ_H

/**
 * The system equaliser: five biquad bands, pure (no device; the only file
 * access is the optional config reader below), fixed-point in the per-sample
 * path.
 *
 * Bands: 60 Hz low shelf, 250 Hz / 1 kHz / 4 kHz peaking (Q = 1), 12 kHz high
 * shelf (RBJ cookbook, shelf slope 1).  Each gain is an integer dB step in
 * -EQ_MAX_DB..+EQ_MAX_DB.  A band whose centre is not below 0.45 x the sample
 * rate is skipped (12 kHz at 22050 Hz), so a low rate never gets a shelf near
 * Nyquist.
 *
 * Coefficients are Q28 (range +-8), the signal inside a stage is Q8 (the int16
 * sample << 8) in an int32, products accumulate in int64 (smull/smlal on
 * ARM), and there is NO division in eq_process(): the Cortex-A8 has no
 * hardware divide.  eq_design() uses libm doubles and runs only when the
 * configuration changes.
 *
 * Headroom is automatic: the first active stage is scaled by
 * 10^(-max_boost/20), so a single boosted band cannot clip a full-scale sine;
 * overlapping boosts can still reach the rail, and every stage saturates.
 *
 * All gains 0 = bypass: eq_process() returns before touching the buffer.
 *
 * One thread owns an EqState; there is no locking.  To change the curve while
 * audio runs, call eq_design() into an EqParams and use it from the next
 * eq_process() call on: the history is kept, and a band that was off is primed
 * from its first sample so it does not start from silence.
 */

#include <stdint.h>
#include <stdbool.h>

#define EQ_BANDS    5
#define EQ_MAX_DB   12
#define EQ_MAX_CH   2

/** The one config key (arca_config.conf): eq=g1,g2,g3,g4,g5 or a preset name. */
#define EQ_CONFIG_KEY  "eq"

typedef struct {
    int32_t b0, b1, b2, a1, a2;      /* Q28, a0 normalised to 1 */
} EqCoef;

typedef struct {
    int     rate;
    int     gains_db[EQ_BANDS];
    int     n;                       /* active stages; 0 = bypass */
    EqCoef  c[EQ_BANDS];             /* the n active ones, packed */
    uint8_t band[EQ_BANDS];          /* band index of each packed stage */
} EqParams;

typedef struct {
    int32_t x1, x2, y1, y2;          /* Q8 history of one stage on one channel */
} EqHist;

typedef struct {
    EqHist  h[EQ_MAX_CH][EQ_BANDS];  /* by channel, by band */
    uint8_t primed[EQ_MAX_CH][EQ_BANDS];
} EqState;

/** Centre/corner frequency of band `b`, Hz. */
int  eq_band_hz(int b);

/** Fill `p` for `rate` and `gains_db[EQ_BANDS]` (each clamped to +-EQ_MAX_DB). */
void eq_design(EqParams *p, int rate, const int gains_db[EQ_BANDS]);

/** Forget all history. */
void eq_state_reset(EqState *s);

/** Filter `frames` interleaved frames in place.  channels 1..EQ_MAX_CH; any
 *  other count, or a bypass design, leaves the buffer untouched. */
void eq_process(EqState *s, const EqParams *p, int16_t *buf, long frames, int channels);

/* Presets: Flat, Bass, Treble, Vocal, Loudness. */
int         eq_preset_count(void);
const char *eq_preset_name(int i);
const int  *eq_preset_gains(int i);
/** Index of the preset equal to `gains`, or -1. */
int         eq_preset_match(const int gains[EQ_BANDS]);

/** Parse "a,b,c,d,e" or a preset name (case-insensitive) into gains.  0 ok, -1 bad. */
int  eq_parse(const char *text, int gains[EQ_BANDS]);
/** Write "a,b,c,d,e" into buf; returns the length. */
int  eq_format(const int gains[EQ_BANDS], char *buf, int cap);

/** Read the `eq` key from a config file (key=value lines).  0 found and valid;
 *  -1 otherwise, with gains set flat. */
int  eq_config_read(const char *path, int gains[EQ_BANDS]);

#endif
