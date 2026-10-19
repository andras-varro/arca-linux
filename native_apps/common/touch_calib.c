#include "touch_calib.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/input.h>

/* Panel coordinates on the reference 800x480 panel. Order matters: the four
 * edge probes are last, and TOUCH_CALIB_PROBE_* index them. */
const TouchCalibTarget TOUCH_CALIB_TARGETS[] = {
    {150, 240}, {400, 240}, {650, 240},   /* interior X sweep     */
    {400, 120}, {400, 360},               /* interior Y sweep     */
    {200, 150}, {600, 330},               /* off-axis, both interior */
    { 20, 240}, {780, 240},               /* X edge probes        */
    {400,  22}, {400, 458},               /* Y edge probes        */
};
const int TOUCH_CALIB_N_TARGETS =
    (int)(sizeof(TOUCH_CALIB_TARGETS) / sizeof(TOUCH_CALIB_TARGETS[0]));

bool touch_calib_interior_x(int panel_x, int panel_w) {
    return panel_x >= TOUCH_CALIB_MARGIN_X &&
           panel_x <= panel_w - 1 - TOUCH_CALIB_MARGIN_X;
}

bool touch_calib_interior_y(int panel_y, int panel_h) {
    return panel_y >= TOUCH_CALIB_MARGIN_Y &&
           panel_y <= panel_h - 1 - TOUCH_CALIB_MARGIN_Y;
}

int touch_calib_median3(int a, int b, int c) {
    if (a > b) { int t = a; a = b; b = t; }
    if (b > c) { int t = b; b = c; c = t; }
    if (a > b) { int t = a; a = b; b = t; }
    return b;
}

int touch_calib_predict_panel(int raw, int raw_at_0, int raw_at_max, int dim) {
    /* Delegates to the production axis map with the knots on the endpoint line,
     * i.e. the linear case — so a residual against the fit and the live mapping
     * cannot disagree. */
    int k_lo, k_hi;
    touch_knots_on_line(raw_at_0, raw_at_max, dim, &k_lo, &k_hi);
    return touch_map_axis_panel(raw, raw_at_0, k_lo, k_hi, raw_at_max, dim);
}

int touch_calib_raw_for_panel(int panel, int v0, int k_lo, int k_hi, int v1,
                              int dim, int raw_lo, int raw_hi, bool *exact) {
    /* Bisection over the production axis map, not a second copy of its
     * arithmetic: whatever the integer truncation does there, the answer is
     * the first raw that the map sends to `panel`. The map is monotone on each
     * axis (the knot check in the map falls back to a line otherwise). */
    if (raw_hi < raw_lo) { int t = raw_lo; raw_lo = raw_hi; raw_hi = t; }
    bool inc = touch_map_axis_panel(raw_hi, v0, k_lo, k_hi, v1, dim) >=
               touch_map_axis_panel(raw_lo, v0, k_lo, k_hi, v1, dim);
    int lo = raw_lo, hi = raw_hi;
    while (lo < hi) {
        int mid = lo + ((hi - lo) >> 1);
        int m = touch_map_axis_panel(mid, v0, k_lo, k_hi, v1, dim);
        if (inc ? (m >= panel) : (m <= panel)) hi = mid;
        else lo = mid + 1;
    }
    if (exact)
        *exact = touch_map_axis_panel(lo, v0, k_lo, k_hi, v1, dim) == panel;
    return lo;
}

int touch_calib_predict_curve(int raw, const TouchAxisCurve *c) {
    return touch_map_axis_panel(raw, c->v0, c->k_lo, c->k_hi, c->v1, c->dim);
}

void touch_calib_curve_from_fit(const TouchAxisFit *f, int hw_min, int hw_max,
                                TouchAxisCurve *c) {
    int dim  = (f->dim > 1) ? f->dim : 480;
    int last = dim - 1;

    c->dim = dim;

    /* Knots sit on the fitted interior line, at the fixed knot positions. This
     * is the part of the mapping that is genuinely measured, so it is preserved
     * exactly. long: a skewed fit can put the span in the tens of thousands. */
    long span = (long)f->in1 - f->in0;
    c->k_lo = (int)(f->in0 + span * TOUCH_KNOT_LO(dim) / last);
    c->k_hi = (int)(f->in0 + span * TOUCH_KNOT_HI(dim) / last);

    /* Endpoints ARE the fit. An earlier revision clamped them into
     * [hw_min, hw_max], which asserts that raw hw_max is emitted at panel dim-1.
     * Measured on RW09 it is emitted at panel ~450 of 480, so the clamp made the
     * upper outer segment too steep and the reported position ran ahead of the
     * finger across the whole bottom quarter. The line is what was measured;
     * store it. Where the sensor saturates before the edge, the resulting panel
     * band is genuinely unreachable and is reported as such — see
     * touch_calib_reach() and touch_calib_inset_from_reach(). */
    c->v0 = f->in0;
    c->v1 = f->in1;

    /* Reporting only: how far outside the emittable range the line reaches. It
     * is a good proxy for the size of the unreachable band, but the band itself
     * is measured properly by mapping hw_min/hw_max through the curve. */
    c->overshoot_lo = (f->in0 < hw_min) ? hw_min - f->in0 : 0;
    c->overshoot_hi = (f->in1 > hw_max) ? f->in1 - hw_max : 0;
}

void touch_calib_fit(TouchAxisFit *f, const int *raw, const int *pos,
                     const bool *interior, int n, int dim) {
    int ri[64], pi[64], ni = 0;
    for (int i = 0; i < n && ni < (int)(sizeof(ri) / sizeof(ri[0])); i++) {
        if (!interior[i]) continue;
        ri[ni] = raw[i]; pi[ni] = pos[i]; ni++;
    }
    f->dim = dim;
    f->in_ok  = (ni >= 2) &&
                touch_fit_axis_range(ri, pi, ni, dim, &f->in0, &f->in1) == 0;
    f->all_ok = (n >= 2) &&
                touch_fit_axis_range(raw, pos, n, dim, &f->all0, &f->all1) == 0;
    /* Degenerate fits still get printable numbers, so a summary screen can show
     * something instead of uninitialised memory. in_ok says whether to trust it. */
    if (!f->in_ok)  { f->in0  = 0; f->in1  = 4095; }
    if (!f->all_ok) { f->all0 = 0; f->all1 = 4095; }
}

bool touch_calib_axis_verdict(const TouchAxisCurve *c, int hw_min, int hw_max,
                              const char *axis_name, char *buf, size_t buflen) {
    int lo, hi;
    touch_calib_reach(c, hw_min, hw_max, &lo, &hi);

    /* Panel rows/columns the sensor cannot address. Negative reach means the
     * curve maps the raw limit past the panel edge, i.e. the edge is reachable
     * with room to spare — that is a zero band, not a negative one. */
    int dead_lo = (lo > 0) ? lo : 0;
    int dead_hi = (hi < c->dim - 1) ? (c->dim - 1 - hi) : 0;

    if (dead_lo == 0 && dead_hi == 0) {
        snprintf(buf, buflen, "%s: reaches both edges, no dead band", axis_name);
        return true;
    }
    snprintf(buf, buflen,
             "%s: sensor saturates %d px inside the low edge, %d inside the high "
             "- still drawable, not pressable", axis_name, dead_lo, dead_hi);
    return false;
}

void touch_calib_reach(const TouchAxisCurve *c, int hw_min, int hw_max,
                       int *panel_lo, int *panel_hi) {
    *panel_lo = touch_calib_predict_curve(hw_min, c);
    *panel_hi = touch_calib_predict_curve(hw_max, c);
}

void touch_calib_inset_from_reach(int reach_lo, int reach_hi,
                                  int view_origin, int logical_dim,
                                  int *inset_lo, int *inset_hi) {
    /* Panel -> logical is a translation by the viewport origin. Reach that lands
     * under the bezel is reach the app never sees, so it costs nothing and the
     * inset floors at zero. */
    int lo = reach_lo - view_origin;
    int hi = (view_origin + logical_dim - 1) - reach_hi;
    if (lo < 0) lo = 0;
    if (hi < 0) hi = 0;
    if (inset_lo) *inset_lo = lo;
    if (inset_hi) *inset_hi = hi;
}

/* ---- edge sweep --------------------------------------------------------- */

bool touch_calib_sweep_edge_is_y(int edge)    { return edge < 2; }
bool touch_calib_sweep_wants_min(int edge)    { return edge == 0 || edge == 2; }

void touch_calib_sweep_reset(TouchCalibSweep *s, int edge) {
    memset(s, 0, sizeof(*s));
    s->extreme = touch_calib_sweep_wants_min(edge) ? TOUCH_CALIB_SWEEP_SENTINEL_MAX
                                                   : TOUCH_CALIB_SWEEP_SENTINEL_MIN;
    for (int i = 0; i < TOUCH_CALIB_SWEEP_BUCKETS; i++)
        s->bucket[i] = s->extreme;
}

void touch_calib_sweep_add(TouchCalibSweep *s, int edge, int raw,
                           int along, int span, int hw_limit) {
    bool want_min = touch_calib_sweep_wants_min(edge);

    /* Bucket along the edge rather than averaging: a corner that reads
     * differently from the middle is exactly what a single extreme would hide. */
    int bk = along * TOUCH_CALIB_SWEEP_BUCKETS / (span > 0 ? span : 1);
    if (bk < 0) bk = 0;
    if (bk >= TOUCH_CALIB_SWEEP_BUCKETS) bk = TOUCH_CALIB_SWEEP_BUCKETS - 1;

    if (want_min ? (raw < s->extreme) : (raw > s->extreme)) s->extreme = raw;
    if (!s->bucket_hit[bk]) {
        s->bucket_hit[bk] = true;
        s->bucket[bk] = raw;
        s->covered++;
    } else if (want_min ? (raw < s->bucket[bk]) : (raw > s->bucket[bk])) {
        s->bucket[bk] = raw;
    }
    s->samples++;
    if (want_min ? (raw <= hw_limit) : (raw >= hw_limit)) s->pinned++;
    if (s->covered >= TOUCH_CALIB_SWEEP_NEED) s->done = true;
}

bool touch_calib_sweep_reached(const TouchCalibSweep *s, int edge, int hw_limit) {
    if (s->covered <= 0) return false;
    return touch_calib_sweep_wants_min(edge) ? (s->extreme <= hw_limit)
                                             : (s->extreme >= hw_limit);
}

int touch_calib_sweep_extreme_or(const TouchCalibSweep *s, int edge, int hw_limit) {
    /* A partial sweep is not evidence about the edge — the operator may simply
     * not have covered it — so fall back to the hardware limit, which is the
     * optimistic answer. A completed sweep that fell short IS evidence, and
     * using it makes the reported dead band larger, which is the honest
     * direction to be wrong in. */
    if (!s->done) return hw_limit;
    return s->extreme;
}

bool touch_calib_range_sane(int fit_min, int fit_max, int hw_min, int hw_max) {
    if (fit_max <= fit_min) return false;          /* inverted or collapsed */
    if (fit_max - fit_min < 16) return false;      /* matches touch_set_raw_range's floor */

    int lo = (fit_min > hw_min) ? fit_min : hw_min;
    int hi = (fit_max < hw_max) ? fit_max : hw_max;
    int overlap = hi - lo;
    if (overlap <= 0) return false;                /* disjoint from the hardware */

    int fit_span = fit_max - fit_min;
    int hw_span  = hw_max - hw_min;
    int max_span = (fit_span > hw_span) ? fit_span : hw_span;

    /* 2*overlap >= max_span, by multiply — no divide (Cortex-A8). */
    return (long)overlap * 2 >= (long)max_span;
}

int touch_calib_backup(const char *path, char *out_path, size_t out_len) {
    if (access(path, F_OK) != 0) return -1;   /* nothing to preserve */

    for (int n = 1; n < 100; n++) {
        char bak[256];
        snprintf(bak, sizeof(bak), "%s.bak%d", path, n);
        if (access(bak, F_OK) == 0) continue;

        FILE *src = fopen(path, "rb");
        if (!src) return -1;
        FILE *dst = fopen(bak, "wb");
        if (!dst) { fclose(src); return -1; }

        char buf[1024];
        size_t got;
        while ((got = fread(buf, 1, sizeof(buf), src)) > 0)
            fwrite(buf, 1, got, dst);
        fclose(src);
        fclose(dst);

        if (out_path && out_len) snprintf(out_path, out_len, "%s", bak);
        return 0;
    }
    return -1;
}

int touch_calib_hw_range(const TouchInput *touch,
                         int *min_x, int *max_x, int *min_y, int *max_y) {
    *min_x = 0; *max_x = 4095;
    *min_y = 0; *max_y = 4095;

    struct input_absinfo abs_x, abs_y;
    if (touch->fd < 0 ||
        ioctl(touch->fd, EVIOCGABS(ABS_X), &abs_x) != 0 ||
        ioctl(touch->fd, EVIOCGABS(ABS_Y), &abs_y) != 0)
        return -1;

    *min_x = abs_x.minimum; *max_x = abs_x.maximum;
    *min_y = abs_y.minimum; *max_y = abs_y.maximum;
    return 0;
}
