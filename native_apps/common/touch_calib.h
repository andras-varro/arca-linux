#ifndef TOUCH_CALIB_H
#define TOUCH_CALIB_H

#include <stdbool.h>
#include <stddef.h>
#include "touch_input.h"

/* Shared calibration measurement — the raw→PANEL fit and everything that
 * judges it.
 *
 * This module exists because the fit used to exist twice, independently
 * (control_panel's Calibration tab and tests/unified_calibrate.c), and both
 * copies carried the same defect: crosshairs inset only 40 px, i.e. inside the
 * band where raw compresses. The fit slope came out shallow, extrapolated
 * outside 0..4095, and invented a horizontal inset that does not exist. It took
 * a separate tool (touch_raw) and a day of measurement to work that out, so the
 * fit now lives in exactly one place and every caller uses it.
 *
 * Two rules the callers must not break:
 *   - fit against PANEL coordinates, never logical ones. Stage 2 of the touch
 *     map subtracts the bezel viewport; baking it into stage 1 subtracts twice.
 *   - fit from INTERIOR targets only. That is the whole correction; see
 *     touch_calib_interior_x/y().
 *
 * All maths is double or integer multiply — never a host integer divide inside
 * a hot path, and never an sdiv/udiv instruction (Cortex-A8 has no hardware
 * divide; see ../../CLAUDE.md).
 */

/* ---- targets ------------------------------------------------------------ *
 * The validated 11-target set: three along the interior X sweep, two along the
 * interior Y sweep, two off-axis, and four edge probes. Every target feeds BOTH
 * axis fits — an edge probe at (20,240) is an outlier on X but a perfectly good
 * interior sample on Y, which is why the interior mask is per-axis.
 *
 * Coordinates are PANEL pixels on the reference 800x480 panel. */
typedef struct { int px, py; } TouchCalibTarget;

extern const TouchCalibTarget TOUCH_CALIB_TARGETS[];
extern const int TOUCH_CALIB_N_TARGETS;

/* Index of the four edge probes within TOUCH_CALIB_TARGETS, for callers that
 * want to report their residuals separately (they are the honest test of the
 * fit: they never entered it). */
#define TOUCH_CALIB_PROBE_XLO 7    /* ( 20, 240) */
#define TOUCH_CALIB_PROBE_XHI 8    /* (780, 240) */
#define TOUCH_CALIB_PROBE_YLO 9    /* (400,  22) */
#define TOUCH_CALIB_PROBE_YHI 10   /* (400, 458) */

/* A target counts as interior along an axis when it is far enough from both
 * ends of that axis that edge compression cannot reach it. Deliberately far
 * more conservative than the 40 px that caused the original error. */
#define TOUCH_CALIB_MARGIN_X 100
#define TOUCH_CALIB_MARGIN_Y  80

bool touch_calib_interior_x(int panel_x, int panel_w);
bool touch_calib_interior_y(int panel_y, int panel_h);

/* ---- sampling ----------------------------------------------------------- */

#define TOUCH_CALIB_TAPS 3
int touch_calib_median3(int a, int b, int c);

/* ---- the fit ------------------------------------------------------------ */

typedef struct {
    int  in0,  in1;    /* interior-only fit: raw mapping to panel 0 / panel dim-1 */
    int  all0, all1;   /* all-points fit, for comparison only — never saved       */
    bool in_ok, all_ok;
    int  dim;          /* panel extent this fit was made against */
} TouchAxisFit;

/* Fit one axis. `raw`/`pos` are n parallel samples in raw and PANEL units;
 * `interior[i]` selects the samples that enter the interior-only fit. Both fits
 * are computed; only the interior one should ever be saved.
 *
 * Falls back to a plain 0..4095 range on a degenerate fit, so the caller always
 * has usable numbers to show — check in_ok before trusting them. */
void touch_calib_fit(TouchAxisFit *f, const int *raw, const int *pos,
                     const bool *interior, int n, int dim);

/* ---- the curve that actually ships ------------------------------------- *
 * The interior fit is a straight line, and it is what gets stored: knots on the
 * line at panel dim/4 and 3*dim/4, endpoints at the line's own values for panel
 * 0 and dim-1. Those endpoints legitimately fall OUTSIDE the emittable raw range
 * — the measured-good Y fit is -279..4382 — and that is not an error to correct.
 *
 * An earlier revision clamped them into [hw_min, hw_max]. That asserts "raw
 * hw_max is emitted at panel dim-1", which is false on this hardware: measured
 * on RW09 the digitiser saturates at raw 4095 from panel ~450 of 480 and at raw 0
 * from panel ~30, so the clamp tilted the upper outer segment and the reported
 * position ran ahead of the finger by up to +19 px across the bottom quarter.
 *
 * The consequence of storing the honest line is that the saturated bands really
 * are unreachable — about 30 panel rows at each end of Y on RW09, and none worth
 * mentioning on X. That is a property of the digitiser, not of the fit, and it is
 * reported rather than papered over: touch_calib_reach() says where raw hw_min
 * and hw_max land, and touch_calib_inset_from_reach() turns that into the
 * per-side logical inset apps use to keep controls pressable. The band stays
 * fully drawable — it is good for a status bar, a score row or a background.
 *
 * The three-segment form is kept even though the shipped curve is now a straight
 * line (knots on a line ARE that line). It costs nothing, the file format is
 * unchanged — which matters, because a component that links a stale
 * touch_input.o misparses a changed line 1 silently — and it leaves room if a
 * panel ever does show real interior curvature. */
typedef struct {
    int v0, k_lo, k_hi, v1;      /* raw at panel 0, dim/4, 3*dim/4, dim-1 */
    int overshoot_lo, overshoot_hi;  /* raw counts the fit asked for beyond the
                                      * emittable range; reporting only, and a
                                      * rough proxy for the dead band's size */
    int dim;
} TouchAxisCurve;

void touch_calib_curve_from_fit(const TouchAxisFit *f, int hw_min, int hw_max,
                                TouchAxisCurve *c);

/* Panel coordinate predicted for a raw value by a fitted LINE (knots implied on
 * the line). Use for residuals against the fit. Delegates to the production
 * axis map, so predictions and reality agree. Unlike scale_coordinates() it does
 * NOT clamp: the point of a prediction is to see where a value lands, including
 * off the panel. */
int touch_calib_predict_panel(int raw, int raw_at_0, int raw_at_max, int dim);

/* The inverse of the production axis map: the raw value that
 * touch_map_axis_panel(raw, v0, k_lo, k_hi, v1, dim) sends to `panel`, searched
 * within [raw_lo, raw_hi] (the range the digitiser can emit, normally 0..4095,
 * intersected with the calibrated endpoints). Bisection over the map itself, so
 * it cannot disagree with it. `*exact` (may be NULL) is false when `panel` is
 * unreachable in that range (the saturated band); the nearest raw is returned.
 * Used by arca-tap to inject a touch that apps read back as the same pixel. */
int touch_calib_raw_for_panel(int panel, int v0, int k_lo, int k_hi, int v1,
                              int dim, int raw_lo, int raw_hi, bool *exact);

/* Panel coordinate predicted by the shipped three-segment curve. */
int touch_calib_predict_curve(int raw, const TouchAxisCurve *c);

/* ---- judging the fit ---------------------------------------------------- */

/* How far past the emittable raw range a fitted endpoint may land before the
 * saturated band is worth calling out. 40 counts is ~8 px of panel — smaller
 * than the effect under test, larger than the run-to-run tap noise. */
#define TOUCH_CALIB_REACH_TOL_RAW 40

/* Per-axis report on the band the digitiser cannot address.
 *
 * This is a statement about the HARDWARE, measured through the calibration in
 * force: where does raw hw_min/hw_max actually land on the panel? On RW09 that
 * is panel ~28 and ~449 on Y (a real ~30 px saturated band at each end) and
 * effectively the panel edges on X.
 *
 * Two earlier wordings were wrong and both are worth remembering. The original
 * H1/H4 text blamed a missing-electrode inset that does not exist. Its
 * replacement swung the other way and claimed "edges still reach", which was
 * only true because the endpoint clamp forced it to be — the clamp was the bug.
 *
 * Writes a one-line summary into buf. Returns true when the axis reaches both
 * edges, i.e. there is no dead band. */
bool touch_calib_axis_verdict(const TouchAxisCurve *c, int hw_min, int hw_max,
                              const char *axis_name, char *buf, size_t buflen);

/* Panel range the digitiser can reach under the stored curve: where raw hw_min
 * and hw_max land. Computed, never assumed — this is the measurement that says
 * how wide the unreachable band is, and on this hardware it is not zero. A value
 * outside 0..dim-1 means the edge is reached with room to spare. */
void touch_calib_reach(const TouchAxisCurve *c, int hw_min, int hw_max,
                       int *panel_lo, int *panel_hi);

/* One axis of reach, in PANEL pixels, turned into the LOGICAL inset apps need:
 * how many logical pixels at each end are visible but not pressable. Floors at
 * zero, because reach landing under the bezel costs nothing. Landscape only —
 * calibration is landscape only, and the live per-app inset is computed through
 * the production map instead (see fb_set_touch_inset()). */
void touch_calib_inset_from_reach(int reach_lo, int reach_hi,
                                  int view_origin, int logical_dim,
                                  int *inset_lo, int *inset_hi);

/* ---- edge sweep --------------------------------------------------------- *
 * Slide one finger along an edge and keep the extreme raw on the perpendicular
 * axis, per bucket along the edge. Answers "what raw does the PHYSICAL edge
 * emit?", which is the premise the reach calculation rests on: if the top edge
 * never drives raw to hw_min, the dead band is WIDER than reach alone predicts.
 *
 * A press against the bezel cannot answer this — it reads the same whether
 * saturation begins at the edge or 30 px inside it, and that ambiguity is what
 * kept the endpoint bug alive across three sessions.
 *
 * Edge numbering is fixed: 0 TOP, 1 BOTTOM, 2 LEFT, 3 RIGHT. TOP/BOTTOM measure
 * raw_y and run along x; LEFT/RIGHT measure raw_x and run along y. TOP and LEFT
 * look for the raw minimum, BOTTOM and RIGHT the maximum.
 *
 * Shared by the Control Panel wizard's REACH step and the touch_raw diagnostic,
 * so the tool that validates the mapping and the wizard that measures it cannot
 * disagree. */

#define TOUCH_CALIB_SWEEP_BUCKETS      16
#define TOUCH_CALIB_SWEEP_NEED         12   /* buckets before an edge counts as swept */
#define TOUCH_CALIB_SWEEP_SENTINEL_MIN (-(1 << 20))
#define TOUCH_CALIB_SWEEP_SENTINEL_MAX  (1 << 20)

typedef struct {
    int  extreme;                               /* min raw for TOP/LEFT, max for BOTTOM/RIGHT */
    int  bucket[TOUCH_CALIB_SWEEP_BUCKETS];     /* per-bucket extreme; sentinel = untouched */
    bool bucket_hit[TOUCH_CALIB_SWEEP_BUCKETS];
    int  covered;
    long samples, pinned;
    bool done;
} TouchCalibSweep;

bool touch_calib_sweep_edge_is_y(int edge);
bool touch_calib_sweep_wants_min(int edge);

void touch_calib_sweep_reset(TouchCalibSweep *s, int edge);

/* Accumulate one sample. `raw` is the reading on the perpendicular axis, `along`
 * the position along the edge and `span` that axis's extent; `hw_limit` is the
 * value this edge should drive raw to, used only to count pinned samples. */
void touch_calib_sweep_add(TouchCalibSweep *s, int edge, int raw,
                           int along, int span, int hw_limit);

bool touch_calib_sweep_reached(const TouchCalibSweep *s, int edge, int hw_limit);

/* The raw extreme to compute reach from: the measured one once the sweep covered
 * enough of the edge, otherwise `hw_limit`. An incomplete sweep says nothing
 * about the edge, so it must not be allowed to widen the reported dead band. */
int touch_calib_sweep_extreme_or(const TouchCalibSweep *s, int edge, int hw_limit);

/* Is a fitted raw range plausible against what the hardware declares?
 *
 * NOT "inside 0..4095" — a correct fit on this panel legitimately extrapolates
 * outside it, because the interior line is steeper than the compressed outer
 * bands. The measured-good Y fit is -279..4382. That overshoot is expected and
 * the curve's outer segments absorb it; rejecting it would reject the right
 * answer.
 *
 * What a bad fit actually looks like is a span wildly out of proportion to the
 * hardware's, so require the overlap to be at least half of the larger span:
 *
 *     2 * overlap  >=  max(fit_span, hw_span)
 *
 * -279..4382 vs 0..4095: overlap 4095, max span 4661 -> passes.
 * 0..60000   vs 0..4095: overlap 4095, max span 60000 -> rejected. */
bool touch_calib_range_sane(int fit_min, int fit_max, int hw_min, int hw_max);

/* ---- file handling ------------------------------------------------------ */

/* Copy `path` to the first free "<path>.bakN" (N from 1). Writes the chosen
 * name into out_path. Returns 0 on success, -1 if there is nothing to back up
 * or no free slot. Always call this before overwriting a working calibration:
 * a wrong one is hard to undo from a screen you can no longer press. */
int touch_calib_backup(const char *path, char *out_path, size_t out_len);

/* Read the hardware's declared raw range via EVIOCGABS. Falls back to 0..4095
 * and returns -1 if the ioctl fails. */
int touch_calib_hw_range(const TouchInput *touch,
                         int *min_x, int *max_x, int *min_y, int *max_y);

#endif
