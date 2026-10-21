/* Host-side regression for the raw -> panel -> logical touch map.
 *
 * Runs on the DEV MACHINE with native gcc, not on the device.  touch_map_raw()
 * is a pass-through to scale_coordinates(), which is pure arithmetic over the
 * fields of one TouchInput — no fd, no ioctl, no framebuffer — so a struct
 * filled in by hand drives the production path exactly as touch_poll() does.
 * That is the whole reason this is worth having on the host: the symptom of a
 * wrong map is a cursor that runs ahead of the finger on a wall-mounted panel,
 * and touch cannot be scripted on the device at all (no /dev/uinput — see
 * ../CLAUDE.md), so every question that does NOT need a finger has to be asked
 * here or it is not asked.  touch_init() is deliberately never called: it
 * open()s the device, and the touch_set_* helpers printf and mutate the
 * framebuffer globals.  Nothing in this file needs either.
 *
 *   cd native_apps && gcc -Wall -Wextra -Wno-unused-parameter -I common \
 *       -o build/touch_map_test tests/touch_map_test.c common/touch_input.c \
 *       common/framebuffer.c common/font.c common/hardware.c common/config.c -lm \
 *       && ./build/touch_map_test
 *
 * What it asserts, and why.  Ten groups, each pinning one thing the two stages
 * are supposed to do.  A and B are the arithmetic itself: the uncalibrated
 * sentinel range (knots 0,0) must behave as one straight line onto panel
 * 0..dim-1, and a real three-segment curve must put its two stored knots on
 * panel dim/4 and 3*dim/4 exactly — those fractions are implicit in the file
 * format, so a shifted knot silently reinterprets every calibration on every
 * unit.  C is the raw clamp, and it is asserted on an OVERSIZED logical screen
 * on purpose: at 800x480 the final logical clamp produces the same number, so a
 * removed raw clamp is invisible, whereas with screen 1000x600 the unclamped
 * value (818 on X, 504 on Y) is in range and shows.  The low side of that clamp
 * has no such witness — below the low endpoint the segment is negative whatever
 * you do, so the non-negativity there belongs to the final clamp and not to the
 * raw one, and this file cannot separate them.  D is the invariant an app
 * actually depends on: no output is ever negative or >= the logical dimension,
 * swept over a grid of raw values far outside 0..4095 in six geometries, plus
 * per-axis monotonicity so a sign slip cannot hide inside the bounds.  E is the
 * SIDE EFFECT, which is the surprising part of this function: a raw span of
 * <= 0 does not merely map badly, it REWRITES the struct it was handed to
 * 0..4095 with the knots zeroed, so the assertions are on the struct afterwards
 * as well as on the value, one axis at a time so a mutation cannot leak across.
 * E also pins the two things that are NOT degenerate: a span of 1, and a valid
 * span, which must come back untouched.  F is the portrait rotation, where the
 * logical X comes from raw Y inverted and the logical Y from raw X — the axis
 * swap is what a fresh reading of this code always gets backwards.  G is stage
 * 2 alone (the bezel viewport origin, subtracted, never added).  H pins that
 * the clamp reads screen_width/screen_height rather than the panel 800x480,
 * which is what a component running a smaller surface depends on.  I is the
 * defensive fallback: a knot set that is not strictly increasing must map
 * coarsely-but-linearly rather than folding the axis back on itself.
 *
 * J is the scaled-surface question, and it settled one the other way.  A scaling
 * DSS overlay draws a smaller surface that the hardware upscales to fill the
 * panel, and it was written down in two places as fact that touch on such a node
 * is wrong by the scale factor and needs dividing.  It is not.  fb_apply_viewport()
 * publishes the SURFACE dims as screen_panel_width/height, touch_init() copies
 * them into panel_width/height, and axis_dims() hands them to
 * touch_map_axis_panel() as `dim` — whose knots are dim/4 and 3*dim/4, so they
 * move with the surface.  The view origin subtracted at stage 2 has already been
 * converted to surface pixels.  The map is therefore proportional by
 * construction, and adding the divide BREAKS it.  Measured: three surfaces
 * (800x480, 400x240, 200x120) against one calibration captured at full size,
 * expectations from an independent integer model of the documented map.
 *
 * ⚠️ J1 is the 1:1 case and it is the weak column, in the same way
 * bezel_scale_test.c's group 1 is: at div 1 every ratio is the identity, so no
 * ratio error can fail it.  Measured against the divide that was prescribed
 * (`mx = mx * panel_width / 800`, the shape a fix would have taken): 12 J
 * failures, all of them J2 and J3, and all six J1 cases PASSED.  A second
 * sabotage — the curve's low knot pinned at a fixed panel 200 instead of dim/4,
 * the same fixed-divisor class of bug — gives 15 J failures.  So the ratio is
 * carried entirely by the two scaled columns; keep both.
 *
 * Expected values were derived by hand from the model in touch_input.h and
 * cross-checked against an independent integer model, not read off a run of the
 * code under test.
 *
 * NOT part of build-and-deploy.sh: that script cross-compiles for ARM and this
 * is a host binary.  Run it by hand after touching scale_coordinates(),
 * touch_map_axis_panel(), axis_dims() or the calibration file format.
 */
#include <stdio.h>
#include <string.h>
#include "touch_input.h"
#include "framebuffer.h"

static int fails = 0;

static void expect(const char *what, int got, int want) {
    if (got != want) { printf("  FAIL %-38s got %d want %d\n", what, got, want); fails++; }
    else             { printf("  ok   %-38s %d\n", what, got); }
}

static void expect_xy(const char *what, int gx, int gy, int wx, int wy) {
    if (gx != wx || gy != wy) {
        printf("  FAIL %-38s got %d,%d want %d,%d\n", what, gx, gy, wx, wy);
        fails++;
    } else {
        printf("  ok   %-38s %d,%d\n", what, gx, gy);
    }
}

/* One mapping, on a throwaway copy: scale_coordinates() may rewrite the struct,
 * so nothing here shares a TouchInput between two calls unless the point of the
 * case is what the first call left behind. */
static void map1(TouchInput t, int rx, int ry, int *ox, int *oy) {
    *ox = rx; *oy = ry;
    touch_map_raw(&t, ox, oy);
}

static void expect_map(const char *what, TouchInput t, int rx, int ry,
                       int wx, int wy) {
    int x, y;
    map1(t, rx, ry, &x, &y);
    expect_xy(what, x, y, wx, wy);
}

/* The base fixture: landscape, no bezel, logical == panel, and the "no curve
 * measured" sentinel (knots 0,0) over the full digitiser range.  screen_width
 * and screen_height are ALWAYS set explicitly — zero-init is not neutral here,
 * it makes the clamp yield -1. */
static TouchInput base_fx(void) {
    TouchInput t;
    memset(&t, 0, sizeof t);
    t.raw_min_x = 0; t.raw_max_x = 4095;
    t.raw_min_y = 0; t.raw_max_y = 4095;
    t.panel_width = 800; t.panel_height = 480;
    t.view_x = 0; t.view_y = 0;
    t.screen_width = 800; t.screen_height = 480;
    return t;
}

/* The reference shape of a real calibration: four raw values per axis, strictly
 * increasing, knots chosen so each segment lands on a round panel number. */
static TouchInput curve_fx(void) {
    TouchInput t = base_fx();
    t.raw_min_x = 100; t.raw_knot_lo_x = 1100; t.raw_knot_hi_x = 3100; t.raw_max_x = 4095;
    t.raw_min_y = 200; t.raw_knot_lo_y = 1200; t.raw_knot_hi_y = 3200; t.raw_max_y = 4000;
    return t;
}

/* A calibrated range narrower than the emittable one, on a logical screen big
 * enough that an unclamped raw value would land inside it. */
static TouchInput narrow_big_screen_fx(void) {
    TouchInput t = base_fx();
    t.raw_min_x = 100; t.raw_max_x = 4000;
    t.raw_min_y = 200; t.raw_max_y = 3900;
    t.screen_width = 1000; t.screen_height = 600;
    return t;
}

static TouchInput bezel_fx(void) {
    TouchInput t = base_fx();
    t.view_x = 8;  t.view_y = 20;
    t.screen_width = 800 - 8 - 8; t.screen_height = 480 - 20 - 20;
    return t;
}

/* Portrait: panel_width/panel_height are in APP orientation, so the physical
 * 800x480 has to be un-swapped by axis_dims() before raw is interpreted. */
static TouchInput portrait_fx(void) {
    TouchInput t = base_fx();
    t.portrait_mode = true;
    t.panel_width = 480; t.panel_height = 800;
    t.screen_width = 480; t.screen_height = 800;
    return t;
}

static TouchInput small_screen_fx(void) {
    TouchInput t = base_fx();
    t.screen_width = 640; t.screen_height = 400;
    return t;
}

/* Two ways a knot set fails to describe a usable curve: out of order, and equal
 * but non-zero (i.e. not the 0,0 sentinel). */
static TouchInput nonmono_fx(void) {
    TouchInput t = base_fx();
    t.raw_min_x = 100; t.raw_knot_lo_x = 3000; t.raw_knot_hi_x = 1000; t.raw_max_x = 4095;
    t.raw_min_y = 200; t.raw_knot_lo_y = 2000; t.raw_knot_hi_y = 2000; t.raw_max_y = 4000;
    return t;
}

/* The geometry fb_apply_viewport() actually publishes for a surface that a
 * scaling DSS overlay upscales to fill the panel.  The load-bearing fact is that
 * screen_panel_width/height are the SURFACE dims and not the panel's — the
 * assignment in common/framebuffer.c common/font.c carries the argument for that — and
 * touch_init()/touch_set_screen_size() copy them straight into
 * panel_width/panel_height.  So `dim` inside touch_map_axis_panel() is a SURFACE
 * extent whenever a node is scaled, and the knots at dim/4 and 3*dim/4 move with
 * it.  div 1 is fb0 at 800x480; div 2 is the 400x240 fb1 measured on .188.
 *
 * The margins come from the production converter rather than being restated
 * here, so a change to it cannot leave this fixture describing a geometry that
 * no device ever has. */
static TouchInput scaled_fx(int div) {
    TouchInput t;
    memset(&t, 0, sizeof t);
    /* The reference capture's interior fit, with the knots touch_knots_on_line()
     * derives from it AT FULL SIZE — a calibration is always captured on fb0 and
     * reused unchanged on a smaller surface, which is the whole question here. */
    t.raw_min_x = 17;   t.raw_knot_lo_x = 1035; t.raw_knot_hi_x = 3071;
    t.raw_max_x = 4084;
    t.raw_min_y = -279; t.raw_knot_lo_y = 888;  t.raw_knot_hi_y = 3224;
    t.raw_max_y = 4382;

    int surf_w = 800 / div, surf_h = 480 / div;
    int top = 15, bot = 13, left = 0, right = 0;   /* .188's measured margins */
    fb_scale_bezel_to_surface(800, 480, surf_w, surf_h, &top, &bot, &left, &right);

    t.panel_width  = surf_w;    /* the SURFACE dims, as framebuffer.c publishes */
    t.panel_height = surf_h;
    t.view_x = left;
    t.view_y = top;
    t.screen_width  = surf_w - left - right;
    t.screen_height = surf_h - top - bot;
    return t;
}

/* Grid sweep: every combination of a coarse raw ladder that runs well outside
 * the emittable 0..4095, so the clamps are exercised on all four sides at once
 * rather than only on the diagonal. */
static int sweep_escapes(TouchInput (*mk)(void)) {
    int bad = 0;
    for (int rx = -8192; rx <= 12287; rx += 257) {
        for (int ry = -8192; ry <= 12287; ry += 257) {
            TouchInput t = mk();
            int x, y;
            map1(t, rx, ry, &x, &y);
            if (x < 0 || y < 0) bad++;
            if (x >= t.screen_width || y >= t.screen_height) bad++;
        }
    }
    return bad;
}

/* Per-axis monotonicity, the other half of the sweep: a sign slip inside the
 * segment arithmetic stays within the bounds D asserts, so bounds alone cannot
 * see it.  Landscape only — in portrait logical X falls as raw Y rises, which
 * F pins directly. */
static int sweep_backsteps(TouchInput (*mk)(void)) {
    int bad = 0;
    int px = -1, py = -1;
    for (int raw = -8192; raw <= 12287; raw += 31) {
        int x, y;
        map1(mk(), raw, 2048, &x, &y);
        if (x < px) bad++;
        px = x;
        map1(mk(), 2048, raw, &x, &y);
        if (y < py) bad++;
        py = y;
    }
    return bad;
}

int main(void) {
    printf("A  uncalibrated sentinel range: one straight line onto the panel\n");
    expect_map("A1 raw origin",           base_fx(),    0,    0,   0,   0);
    expect_map("A2 raw maximum",          base_fx(), 4095, 4095, 799, 479);
    expect_map("A3 raw centre",           base_fx(), 2048, 2048, 399, 239);
    expect_map("A4 raw quarter",          base_fx(), 1024, 1024, 199, 119);
    expect_map("A5 raw three quarters",   base_fx(), 3072, 3072, 599, 359);
    {   /* panel geometry unset (touch before fb_init) falls back to 800x480 */
        TouchInput t = base_fx();
        t.panel_width = 0; t.panel_height = 0;
        expect_map("A6 unset panel geometry", t, 2048, 2048, 399, 239);
    }

    printf("B  three-segment curve: the knots land on dim/4 and 3*dim/4\n");
    expect_map("B1 both endpoints low",   curve_fx(),  100,  200,   0,   0);
    expect_map("B2 mid outer segment",    curve_fx(),  600,  700, 100,  60);
    expect_map("B3 the low knot",         curve_fx(), 1100, 1200, 200, 120);
    expect_map("B4 mid fitted interior",  curve_fx(), 2100, 2200, 400, 240);
    expect_map("B5 the high knot",        curve_fx(), 3100, 3200, 600, 360);
    expect_map("B6 mid upper segment",    curve_fx(), 3597, 3600, 699, 419);
    expect_map("B7 both endpoints high",  curve_fx(), 4095, 4000, 799, 479);

    printf("C  the raw clamp, on a screen too big for the logical clamp to hide it\n");
    expect_map("C1 at raw_min",  narrow_big_screen_fx(),  100,  200,   0,   0);
    expect_map("C2 at raw_max",  narrow_big_screen_fx(), 4000, 3900, 799, 479);
    expect_map("C3 just past raw_max",
                                 narrow_big_screen_fx(), 4095, 4095, 799, 479);
    expect_map("C4 far past raw_max",
                                 narrow_big_screen_fx(), 9999, 99999, 799, 479);
    expect_map("C5 below raw_min",
                                 narrow_big_screen_fx(), -500, -1000,  0,   0);
    expect_map("C6 far below raw_min",
                                 narrow_big_screen_fx(), -8192, -8192, 0,   0);

    printf("D  nothing escapes the logical screen, and neither axis backsteps\n");
    expect("D1 escapes, sentinel 800x480", sweep_escapes(base_fx),              0);
    expect("D2 escapes, curve",            sweep_escapes(curve_fx),             0);
    expect("D3 escapes, bezel viewport",   sweep_escapes(bezel_fx),             0);
    expect("D4 escapes, portrait",         sweep_escapes(portrait_fx),          0);
    expect("D5 escapes, 640x400 logical",  sweep_escapes(small_screen_fx),      0);
    expect("D6 escapes, narrow on 1000x600",
                                           sweep_escapes(narrow_big_screen_fx), 0);
    expect("D7 backsteps, sentinel",       sweep_backsteps(base_fx),            0);
    expect("D8 backsteps, curve",          sweep_backsteps(curve_fx),           0);

    printf("E  a raw span <= 0 REWRITES the struct, one axis at a time\n");
    {   /* X span exactly zero, with knots set, and a healthy Y beside it */
        TouchInput t = base_fx();
        t.raw_min_x = 2000; t.raw_max_x = 2000;
        t.raw_knot_lo_x = 1500; t.raw_knot_hi_x = 2500;
        t.raw_min_y = 100;  t.raw_max_y = 4000;
        int x = 2048, y = 2048;
        touch_map_raw(&t, &x, &y);
        expect_xy("E1 zero X span maps as 0..4095", x, y, 399, 239);
        expect("E2 raw_min_x after",      t.raw_min_x,      0);
        expect("E3 raw_max_x after",      t.raw_max_x,   4095);
        expect("E4 raw_knot_lo_x after",  t.raw_knot_lo_x,  0);
        expect("E5 raw_knot_hi_x after",  t.raw_knot_hi_x,  0);
        expect("E6 raw_min_y untouched",  t.raw_min_y,    100);
        expect("E7 raw_max_y untouched",  t.raw_max_y,   4000);
    }
    {   /* Y span inverted — same reset, and X must not be dragged into it */
        TouchInput t = base_fx();
        t.raw_min_y = 4000; t.raw_max_y = 100;
        t.raw_knot_lo_y = 1200; t.raw_knot_hi_y = 3200;
        int x = 2048, y = 2048;
        touch_map_raw(&t, &x, &y);
        expect_xy("E8 inverted Y span maps as 0..4095", x, y, 399, 239);
        expect("E9 raw_min_y after",      t.raw_min_y,      0);
        expect("E10 raw_max_y after",     t.raw_max_y,   4095);
        expect("E11 raw_knot_lo_y after", t.raw_knot_lo_y,  0);
        expect("E12 raw_knot_hi_y after", t.raw_knot_hi_y,  0);
        expect("E13 raw_min_x untouched", t.raw_min_x,      0);
        expect("E14 raw_max_x untouched", t.raw_max_x,   4095);
    }
    {   /* a valid span is NOT degenerate: the curve survives the call intact */
        TouchInput t = base_fx();
        t.raw_min_x = 100; t.raw_max_x = 4000;
        t.raw_knot_lo_x = 500; t.raw_knot_hi_x = 3000;
        int x = 2048, y = 2048;
        touch_map_raw(&t, &x, &y);
        expect_xy("E15 valid span maps by its curve", x, y, 447, 239);
        expect("E16 raw_min_x survives",     t.raw_min_x,     100);
        expect("E17 raw_max_x survives",     t.raw_max_x,    4000);
        expect("E18 raw_knot_lo_x survives", t.raw_knot_lo_x,  500);
        expect("E19 raw_knot_hi_x survives", t.raw_knot_hi_x, 3000);
    }
    {   /* a span of 1 is the smallest NON-degenerate one */
        TouchInput t = base_fx();
        t.raw_min_x = 2000; t.raw_max_x = 2001;
        int x = 2048, y = 2048;
        touch_map_raw(&t, &x, &y);
        expect_xy("E20 span of 1 clamps to its top", x, y, 799, 239);
        expect("E21 raw_min_x survives",  t.raw_min_x,  2000);
        expect("E22 raw_max_x survives",  t.raw_max_x,  2001);
        expect_map("E23 span of 1 at its bottom", t, 2000, 0, 0, 0);
    }

    printf("F  portrait: logical X is inverted raw Y, logical Y is raw X\n");
    expect_map("F1 raw origin",    portrait_fx(),    0,    0, 479,   0);
    expect_map("F2 raw maximum",   portrait_fx(), 4095, 4095,   0, 799);
    expect_map("F3 raw centre",    portrait_fx(), 2048, 2048, 240, 399);
    expect_map("F4 low X, high Y", portrait_fx(),    0, 4095,   0,   0);
    expect_map("F5 high X, low Y", portrait_fx(), 4095,    0, 479, 799);

    printf("G  stage 2: the viewport origin is SUBTRACTED\n");
    expect_map("G1 centre less the origin", bezel_fx(), 2048, 2048, 391, 219);
    expect_map("G2 off-axis point",         bezel_fx(), 1024, 3072, 191, 339);
    expect_map("G3 panel origin under the bezel",
                                            bezel_fx(),    0,    0,   0,   0);
    expect_map("G4 panel end under the bezel",
                                            bezel_fx(), 4095, 4095, 783, 439);

    printf("H  the clamp honours screen_width/height, not the panel 800x480\n");
    expect_map("H1 raw maximum on 640x400", small_screen_fx(), 4095, 4095, 639, 399);
    expect_map("H2 interior point unclamped",
                                            small_screen_fx(), 2048, 2048, 399, 239);
    expect_map("H3 also unclamped at 3/4",  small_screen_fx(), 3072, 3072, 599, 359);

    printf("I  knots that do not strictly increase fall back to linear\n");
    expect_map("I1 out-of-order knots",     nonmono_fx(), 2098, 2100, 399, 239);
    expect_map("I2 linear at the low end",  nonmono_fx(),  100,  200,   0,   0);
    expect_map("I3 linear at the high end", nonmono_fx(), 4095, 4000, 799, 479);

    printf("J  a scaled overlay surface maps PROPORTIONALLY — no scale divide\n");
    {
        static const int raw[] = { 0, 500, 1024, 2048, 3072, 4095 };
        /* One raw ladder, three surfaces, expectations from an independent
         * integer model of the documented two-stage map — not read off a run of
         * the code under test.  Each column is the one above it halved, which is
         * the property: the curve's knots are dim/4 and 3*dim/4, so they track
         * the surface, and the view origin subtracted at stage 2 has already
         * been converted to surface pixels by fb_scale_bezel_to_surface(). */
        static const int lx1[] = {  0,  94, 197, 399, 600, 799 };
        static const int ly1[] = { 13,  65, 118, 224, 329, 434 };
        static const int lx2[] = {  0,  47,  98, 199, 300, 399 };
        static const int ly2[] = {  7,  33,  59, 112, 165, 217 };
        static const int lx4[] = {  0,  23,  49,  99, 150, 199 };
        static const int ly4[] = {  4,  17,  30,  56,  83, 108 };
        const int *wx[3] = { lx1, lx2, lx4 };
        const int *wy[3] = { ly1, ly2, ly4 };
        const int dv[3]  = { 1, 2, 4 };
        for (int d = 0; d < 3; d++) {
            for (int i = 0; i < (int)(sizeof raw / sizeof raw[0]); i++) {
                char what[48];
                snprintf(what, sizeof what, "J%d.%d %dx%d surface, raw %d",
                         d + 1, i + 1, 800 / dv[d], 480 / dv[d], raw[i]);
                expect_map(what, scaled_fx(dv[d]), raw[i], raw[i],
                           wx[d][i], wy[d][i]);
            }
        }
    }

    printf("\n%s (%d failure%s)\n", fails ? "REGRESSION" : "ALL PASS",
           fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
