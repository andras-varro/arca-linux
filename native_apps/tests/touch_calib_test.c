/* Host-side regression for the touch calibration fit and the curve it produces.
 *
 * Runs on the DEV MACHINE with native gcc, not on the device — it is pure
 * arithmetic with no hardware in the loop, which is exactly why it is worth
 * having: a wrong fit is invisible until someone is mis-tapping by 30 px on a
 * wall-mounted screen, and it took a day of on-device measurement to unpick
 * last time.
 *
 * Inputs are the 11 per-target medians recorded in the repo's reference
 * capture, ../../touch_raw-2026-07-31-rw09.tsv.  Build and run:
 *
 *   cd native_apps && gcc -Wall -Wextra -Wno-unused-parameter -I common \
 *       -o build/touch_calib_test tests/touch_calib_test.c \
 *       common/touch_calib.c common/touch_input.c common/framebuffer.c common/font.c \
 *       common/hardware.c common/config.c -lm && ./build/touch_calib_test
 *
 * Exit 0 = the fit still reproduces the reference measurement AND the curve
 * derived from it still reports the measured digitiser reach.  That second half
 * is the point, and it is the assertion that changed: an earlier revision
 * asserted the curve reaches panel 0 and 479 on Y, which it only did because
 * touch_calib_curve_from_fit() clamped the endpoints into 0..4095 to force it.
 * The clamp was the bug.  Measured with touch_raw on 2026-08-01, the digitiser
 * saturates at raw 4095 from panel ~450 and at raw 0 from panel ~30, so the
 * honest curve reaches 28..449 and the bands outside that are unreachable.  They
 * remain fully drawable — see SCREEN_SAFE_* vs SCREEN_VISIBLE_* in framebuffer.h.
 *
 * NOT part of build-and-deploy.sh: that script cross-compiles for ARM and this
 * is a host binary.  Run it by hand after touching common/touch_calib.c,
 * touch_fit_axis_range() or touch_map_axis_panel().
 */
#include <stdio.h>
#include "touch_calib.h"

static const int RAWX[] = { 772,2044,3332,2044,2052,1056,3072,  84,4020,2060,2044};
static const int RAWY[] = {2064,2044,2044, 884,3236,1204,2932,2052,2044,  20,4095};

static int fails = 0;
static void expect(const char *what, int got, int want) {
    if (got != want) { printf("  FAIL %-30s got %d want %d\n", what, got, want); fails++; }
    else             { printf("  ok   %-30s %d\n", what, got); }
}

int main(void) {
    const int W = 800, H = 480, N = TOUCH_CALIB_N_TARGETS;
    int px[16], py[16];
    bool ix[16], iy[16];

    for (int i = 0; i < N; i++) {
        px[i] = TOUCH_CALIB_TARGETS[i].px;
        py[i] = TOUCH_CALIB_TARGETS[i].py;
        ix[i] = touch_calib_interior_x(px[i], W);
        iy[i] = touch_calib_interior_y(py[i], H);
    }

    TouchAxisFit fx, fy;
    touch_calib_fit(&fx, RAWX, px, ix, N, W);
    touch_calib_fit(&fy, RAWY, py, iy, N, H);

    printf("interior-only fit (the line through the middle of each axis)\n");
    expect("X raw @ panel 0",   fx.in0,   17);
    expect("X raw @ panel 799", fx.in1, 4084);
    expect("Y raw @ panel 0",   fy.in0, -279);
    expect("Y raw @ panel 479", fy.in1, 4382);

    printf("all-points fit (shown for contrast, never saved)\n");
    expect("X all @ panel 0",   fx.all0,   -4);
    expect("X all @ panel 799", fx.all1, 4104);
    expect("Y all @ panel 0",   fy.all0, -217);
    expect("Y all @ panel 479", fy.all1, 4320);

    /* The curve: knots on the fitted line, endpoints AT the fitted line. Y's
     * endpoints fall outside 0..4095 and stay there — that is the measurement,
     * not an error to correct. */
    TouchAxisCurve cx, cy;
    touch_calib_curve_from_fit(&fx, 0, 4095, &cx);
    touch_calib_curve_from_fit(&fy, 0, 4095, &cy);

    printf("stored curve — raw at panel 0 / dim-4 / 3*dim-4 / dim-1\n");
    expect("X v0",   cx.v0,     17);
    expect("X k_lo", cx.k_lo, 1035);
    expect("X k_hi", cx.k_hi, 3071);
    expect("X v1",   cx.v1,   4084);
    expect("Y v0",   cy.v0,   -279);   /* NOT clamped to 0 — the clamp was the bug */
    expect("Y k_lo", cy.k_lo,  888);
    expect("Y k_hi", cy.k_hi, 3224);
    expect("Y v1",   cy.v1,   4382);   /* NOT clamped to 4095 */

    /* With the endpoints on the line, the knots are on the line too, so the whole
     * curve IS the line — exactly, not approximately: touch_calib_curve_from_fit()
     * and touch_knots_on_line() place the knots with the same arithmetic, and
     * predict_panel() and predict_curve() then run the same segment code. Assert
     * zero deviation, so a future knot change that bends the interior shows up
     * here instead of as a mis-tap on a wall-mounted screen. */
    printf("the stored curve is a straight line (knots on it, endpoints on it)\n");
    {
        int worst = 0;
        for (int raw = cy.v0; raw <= cy.v1; raw++) {
            int line = touch_calib_predict_panel(raw, fy.in0, fy.in1, H);
            int d = touch_calib_predict_curve(raw, &cy) - line;
            if (d < 0) d = -d;
            if (d > worst) worst = d;
        }
        expect("Y curve vs line, worst px", worst, 0);
    }

    printf("overshoot past the emittable range (reporting only now)\n");
    expect("X overshoot lo", cx.overshoot_lo,   0);
    expect("X overshoot hi", cx.overshoot_hi,   0);
    expect("Y overshoot lo", cy.overshoot_lo, 279);
    expect("Y overshoot hi", cy.overshoot_hi, 287);

    /* THE regression that matters, and the one whose expected values changed.
     * Where do the raw values the digitiser really emits land on the panel?
     * X: past both edges, so X is fully reachable. Y: 28 and 449, i.e. a genuine
     * ~28 px band at the top and ~30 at the bottom that no finger can address.
     * touch_raw's INSET taps measured 30 and 450 independently on 2026-08-01 —
     * the same answer from a method that never saw an interior target. */
    printf("reach — where raw 0 and raw 4095 actually land\n");
    int lo, hi;
    touch_calib_reach(&cx, 0, 4095, &lo, &hi);
    expect("X reach lo <= 0",   lo <= 0,       1);
    expect("X reach hi >= 799", hi >= W - 1,   1);
    touch_calib_reach(&cy, 0, 4095, &lo, &hi);
    expect("Y reach lo",        lo,           28);
    expect("Y reach hi",        hi,          449);

    /* Reach in panel pixels becomes the inset apps lay out against. At the bezel
     * measured on RW09 (T=11 B=14, logical height 455) the top 17 and bottom 16
     * logical rows are visible but not pressable. */
    printf("touch-safe inset at the RW09 bezel (T=11 B=14, logical 455)\n");
    {
        int in_lo, in_hi;
        touch_calib_inset_from_reach(28, 449, 11, 455, &in_lo, &in_hi);
        expect("Y inset top",    in_lo, 17);
        expect("Y inset bottom", in_hi, 16);
        /* X reaches past both edges, so nothing is lost even though the reach
         * values are negative / beyond the panel. */
        touch_calib_inset_from_reach(-3, 801, 0, 800, &in_lo, &in_hi);
        expect("X inset left",  in_lo, 0);
        expect("X inset right", in_hi, 0);
    }

    printf("interior accuracy preserved (error vs the tapped target, px)\n");
    expect("x=200", touch_calib_predict_curve(RAWX[5], &cx) - px[5], +4);
    expect("x=400", touch_calib_predict_curve(RAWX[1], &cx) - px[1], -2);
    expect("x=600", touch_calib_predict_curve(RAWX[6], &cx) - px[6],  0);
    expect("y=120", touch_calib_predict_curve(RAWY[3], &cy) - py[3], -1);
    expect("y=240", touch_calib_predict_curve(RAWY[1], &cy) - py[1], -2);
    expect("y=360", touch_calib_predict_curve(RAWY[4], &cy) - py[4], +1);

    /* The two Y edge probes sit inside the saturated band, where raw is clipped
     * and position is physically unrecoverable. Their error is now the same as
     * the fitted line's residual — because the curve IS the line. Under the old
     * clamped curve these read -20 / +21, i.e. the clamp was pushing the reported
     * position 20 px outward to manufacture edge reach it did not have. */
    printf("saturated-band error (raw is clipped here; == the line's residual)\n");
    expect("y=22",  touch_calib_predict_curve(RAWY[TOUCH_CALIB_PROBE_YLO], &cy)
                    - py[TOUCH_CALIB_PROBE_YLO], +8);
    expect("y=458", touch_calib_predict_curve(RAWY[TOUCH_CALIB_PROBE_YHI], &cy)
                    - py[TOUCH_CALIB_PROBE_YHI], -9);

    printf("edge-probe residuals vs the fitted LINE (never entered the fit)\n");
    expect("line x=20",  touch_calib_predict_panel(RAWX[TOUCH_CALIB_PROBE_XLO], fx.in0, fx.in1, W) - px[TOUCH_CALIB_PROBE_XLO], -7);
    expect("line x=780", touch_calib_predict_panel(RAWX[TOUCH_CALIB_PROBE_XHI], fx.in0, fx.in1, W) - px[TOUCH_CALIB_PROBE_XHI], +6);
    expect("line y=22",  touch_calib_predict_panel(RAWY[TOUCH_CALIB_PROBE_YLO], fy.in0, fy.in1, H) - py[TOUCH_CALIB_PROBE_YLO], +8);
    expect("line y=458", touch_calib_predict_panel(RAWY[TOUCH_CALIB_PROBE_YHI], fy.in0, fy.in1, H) - py[TOUCH_CALIB_PROBE_YHI], -9);

    printf("per-axis verdict (about the hardware, measured through the curve)\n");
    char v[128];
    expect("X reaches both edges", touch_calib_axis_verdict(&cx, 0, 4095, "X", v, sizeof v), 1);
    printf("       %s\n", v);
    expect("Y has a dead band",    touch_calib_axis_verdict(&cy, 0, 4095, "Y", v, sizeof v), 0);
    printf("       %s\n", v);

    /* Legacy 4-number configs are migrated on load by putting the knots on the
     * line they described — and nothing else. This is the config that was
     * actually on RW09, produced by the old 40 px-inset fit, and it genuinely
     * cannot reach the right edge or either Y edge. Reproducing it exactly is the
     * honest migration: a legacy file carries no edge measurement, so there is no
     * basis for moving its endpoints, and the revision that clamped them into
     * 0..4095 is what tilted the outer segments. A user with this config should
     * recalibrate, and the TOUCHABLE: row in Control Panel now tells them so. */
    printf("legacy migration of 6 4181 -268 4394 (the broken config)\n");
    int kxl, kxh, kyl, kyh;
    touch_knots_on_line(6, 4181, W, &kxl, &kxh);
    touch_knots_on_line(-268, 4394, H, &kyl, &kyh);
    expect("migrated X k_lo", kxl, 1051);
    expect("migrated X k_hi", kxh, 3141);
    expect("migrated Y k_lo", kyl,  899);
    expect("migrated Y k_hi", kyh, 3235);
    expect("X right edge reaches only",
           touch_map_axis_panel(4095, 6, kxl, kxh, 4181, W), 782);
    expect("Y top edge reaches only",
           touch_map_axis_panel(0, -268, kyl, kyh, 4394, H), 27);
    expect("Y bottom edge reaches only",
           touch_map_axis_panel(4095, -268, kyl, kyh, 4394, H), 448);

    /* Migration must not shift the interior. It cannot be bit-exact: the knots
     * are integers and each segment does its own truncating division, so a
     * sample can land 1 px off the single-division result. Assert that bound —
     * 1 px against the 12-17 px of unreachable edge the migration recovers. */
    printf("knots on the line reproduce the line to within 1 px\n");
    {
        int worst = 0;
        for (int raw = -268; raw <= 4394; raw++) {
            int line = (int)((long)(raw + 268) * (H - 1) / (4394 + 268));
            int d = touch_map_axis_panel(raw, -268, kyl, kyh, 4394, H) - line;
            if (d < 0) d = -d;
            if (d > worst) worst = d;
        }
        expect("Y worst deviation from the line", worst, 1);
    }

    /* The k_lo == k_hi == 0 sentinel means "no curve measured": map linearly. */
    printf("linear sentinel (k_lo == k_hi == 0)\n");
    {
        int mismatches = 0;
        for (int raw = 0; raw <= 4095; raw += 7)
            if (touch_map_axis_panel(raw, 0, 0, 0, 4095, H) != raw * (H - 1) / 4095)
                mismatches++;
        expect("sentinel mismatches over full span", mismatches, 0);
    }

    printf("sanity gate (must accept the measured-good ranges, reject junk)\n");
    expect("accept Y -279..4382", touch_calib_range_sane(-279, 4382, 0, 4095), 1);
    expect("accept X 17..4084",   touch_calib_range_sane(  17, 4084, 0, 4095), 1);
    expect("reject 0..60000",     touch_calib_range_sane(   0,60000, 0, 4095), 0);
    expect("reject 9000..14000",  touch_calib_range_sane(9000,14000, 0, 4095), 0);
    expect("reject 0..10",        touch_calib_range_sane(   0,   10, 0, 4095), 0);
    expect("reject inverted",     touch_calib_range_sane(4000, 100, 0, 4095), 0);

    printf("\n%s (%d failure%s)\n", fails ? "REGRESSION" : "ALL PASS",
           fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
