/* Host-side regression for the design-pixel -> surface-pixel UI scale.
 *
 * Runs on the DEV MACHINE with native gcc.  Like bezel_scale_test.c it tests the
 * pure half of a device-only mechanism: the surface:panel ratio is only known
 * after fb_apply_viewport() has read /sys/devices/platform/omapdss/display0/
 * timings, which exists on no host, so the arithmetic was split out to be
 * reachable here.  Build and run:
 *
 *   cd native_apps && gcc -Wall -Wextra -Wno-unused-parameter -I common \
 *       -o build/ui_scale_test tests/ui_scale_test.c \
 *       common/framebuffer.c common/font.c common/hardware.c common/config.c -lm \
 *       && ./build/ui_scale_test
 *
 * What can go wrong, and why each group is here.  A control that must keep its
 * PHYSICAL size is a fixed fraction of the panel, and a scaling DSS overlay draws
 * a smaller surface upscaled to fill that panel — so a constant left in surface
 * pixels grows on the panel by the upscale factor.  Measured consequences at a
 * 400x240 surface before the conversion existed: the shared 3-button game-over
 * stack (3*60 + 2*15 = 210 design px) covered 88% of the 240 rows and buried the
 * title, score and leaderboard under itself, and modal_dialog's 420 px box was
 * WIDER than the 400 px surface, putting both side borders off it.
 *
 * ⚠️ Group 1 is the NEGATIVE CONTROL and it is the group that protects the field:
 * at the shipped 800x480 geometry every conversion must be a bit-exact no-op, or
 * this is a regression on every unit in service.  ⚠️ But note what it CANNOT
 * catch, for the same reason bezel_scale_test.c's group 1 cannot: at 1:1 every
 * ratio is the identity, so no error in the ratio itself can fail group 1.  A
 * variant that scaled unconditionally — dropping the panel == surf guard, which is
 * therefore an optimisation and not a behaviour — passes group 1.  Groups 3-5 are
 * what catch an inverted or transposed ratio.
 *
 * ⚠️ Group 2 states the PORTRAIT geometry, and against the pure function it is
 * arithmetically the same identity as group 1 — panel == surf there too, so it
 * catches nothing group 1 does not.  It is kept because the numbers are the ones a
 * reader needs, and the portrait control that DOES bite lives in group 8.
 *
 * ⚠️ Group 8 drives the WRAPPERS, and it is not optional: a fixed-reference or
 * transposed wrapper is invisible to every other group, because they all hand the
 * ratio in as parameters.  Measured — SAB 3 and SAB 4 below each fail group 8 and
 * NOTHING ELSE, one assertion apiece.  Without group 8 both defects ship green.
 * The fixed-divisor class already shipped once here: a benchmark tool derived its
 * text scale as h/120 and drew at scale 1 instead of 2, because the bezel had cut
 * the height it divided from 240 to 227.
 *
 * Group 6 is the floor: a positive design size must never round away to zero,
 * because a zero-pixel control is invisible AND untappable and the caller cannot
 * tell those apart afterwards.
 *
 * Seen failing — measured 2026-09-11 against four sabotaged variants of
 * common/framebuffer.c common/font.c, each applied to a pristine copy and the source restored
 * and md5-verified afterwards:
 *   - SAB 1, "never scale" (the pre-change behaviour, return px always):
 *     13 failures, groups 3, 4, 5, 6, 8.  Groups 1, 2 and 7 pass, as they must.
 *   - SAB 2, inverted ratio (px * panel_dim / surf_dim): 14 failures, same groups.
 *   - SAB 3, wrapper against a fixed 800x480 instead of screen_true_panel_*:
 *     1 failure, group 8 ONLY — the portrait assertion.
 *   - SAB 4, wrappers transposed (fb_ui_px_x reading the height pair):
 *     1 failure, group 8 ONLY — the differing-ratio assertion.
 */
#include <stdio.h>
#include "framebuffer.h"

static int fails;

static void expect(const char *what, int panel_dim, int surf_dim, int px, int want)
{
    int got = fb_scale_ui_px(panel_dim, surf_dim, px);
    int ok = (got == want);
    if (!ok) fails++;
    printf("  %-4s %-52s panel=%-4d surf=%-4d %4d -> %-4d (want %d)\n",
           ok ? "ok" : "FAIL", what, panel_dim, surf_dim, px, got, want);
}

/* Drive the WRAPPERS by publishing the geometry they read, the way
   fb_apply_viewport() does on a device.  Both axes at once, because the defect
   this catches is a wrapper reading the other axis's pair. */
static void expect_wrap(const char *what,
                        int panel_w, int panel_h, int surf_w, int surf_h,
                        int px_x, int px_y, int want_x, int want_y)
{
    screen_true_panel_width  = panel_w;
    screen_true_panel_height = panel_h;
    screen_panel_width       = surf_w;   /* misnamed: holds the SURFACE */
    screen_panel_height      = surf_h;

    int gx = fb_ui_px_x(px_x);
    int gy = fb_ui_px_y(px_y);
    int ok = (gx == want_x && gy == want_y);
    if (!ok) fails++;
    printf("  %-4s %-52s panel=%dx%d surf=%dx%d  x:%d->%d y:%d->%d (want %d/%d)\n",
           ok ? "ok" : "FAIL", what, panel_w, panel_h, surf_w, surf_h,
           px_x, gx, px_y, gy, want_x, want_y);
}

int main(void)
{
    printf("group 1  NEGATIVE CONTROL: surface == panel must not move one pixel\n");
    expect("800x480 landscape, BTN_MENU_WIDTH",   800, 800,  70,  70);
    expect("800x480 landscape, BTN_LARGE_HEIGHT", 480, 480,  60,  60);
    expect("800x480 landscape, dialog width",     800, 800, 420, 420);
    expect("800x480 landscape, inset cap",        480, 480,  48,  48);
    expect("800x480 landscape, a 1px gridline",   800, 800,   1,   1);

    printf("group 2  the PORTRAIT geometry — same identity as group 1, see header\n");
    /* fb_init() swaps both the surface and the timings into the app's
       orientation, so a portrait app sees panel == surf == 480x800.  A conversion
       written against a fixed 800x480 reference returns 42 and 100 here. */
    expect("480x800 portrait, BTN_MENU_WIDTH",    480, 480,  70,  70);
    expect("480x800 portrait, BTN_LARGE_HEIGHT",  800, 800,  60,  60);
    expect("480x800 portrait, dialog width",      480, 480, 420, 420);
    expect("480x800 portrait, inset cap",         800, 800,  48,  48);

    printf("group 3  the measured case: a 400x240 surface upscaled 2x by vid1\n");
    expect("BTN_MENU_WIDTH halves",               800, 400,  70,  35);
    expect("BTN_MENU_HEIGHT halves",              480, 240,  50,  25);
    expect("BTN_LARGE_WIDTH halves",              800, 400, 220, 110);
    expect("BTN_LARGE_HEIGHT halves",             480, 240,  60,  30);
    /* The two shared widgets that broke worst before the conversion. */
    expect("game-over stack 210 -> half the rows",  480, 240, 210, 105);
    expect("modal dialog 420 fits a 400 surface",   800, 400, 420, 210);

    printf("group 4  each axis scales by its OWN ratio, not the other's\n");
    /* Catches a transposed ratio, which groups 1-3 cannot: at 400x240 both axes
       halve, so swapping them is invisible.  A 400x480 surface is not a geometry
       we run, but it is the only shape that separates the two axes. */
    expect("400x480 surface: X halves",           800, 400, 220, 110);
    expect("400x480 surface: Y untouched",        480, 480,  60,  60);

    printf("group 5  a surface LARGER than the panel scales up, not down\n");
    expect("1600x960 surface downscaled to panel", 800, 1600,  70, 140);
    expect("1600x960 surface downscaled to panel", 480,  960,  60, 120);

    printf("group 6  the floor: a positive size never rounds away to nothing\n");
    expect("1px gridline on a 4x downscale",       800, 200,   1,   1);
    expect("2px border on a 4x downscale",         800, 200,   2,   1);
    expect("8px pad survives as 2",                800, 200,   8,   2);
    expect("zero stays zero, not floored to 1",    800, 200,   0,   0);

    printf("group 7  CONTROL: an unreadable panel size is left 1:1\n");
    /* fb_read_panel_size() failing leaves panel == surf, but a zero or negative
       dimension must not divide either — it would trap, and SIGFPE in a game is
       not a graceful fallback. */
    expect("panel width 0 (timings unreadable)",     0, 400, 220, 220);
    expect("surface width 0 (nothing mapped yet)", 800,   0, 220, 220);
    expect("negative panel dimension",            -800, 400, 220, 220);

    /* ── The wrappers, which is where a fixed reference or a swapped axis lives ──
       fb_ui_px_x/y read four plain globals.  Only fb_apply_viewport's *reading* of
       the timings is device-only; the globals themselves are assignable from here,
       so these two are host-testable and must be tested — every group above calls
       the pure function with the ratio handed to it, and therefore cannot see a
       wrapper that picks the wrong pair. */
    printf("group 8  the wrappers pick the right axis and the LIVE panel\n");
    expect_wrap("landscape 1:1 is a no-op through the wrapper",
                800, 480, 800, 480,  70,  50,  70,  50);
    expect_wrap("400x240 upscaled 2x halves both axes",
                800, 480, 400, 240,  70,  50,  35,  25);
    /* ⚠️ THE PORTRAIT CONTROL, and the reason this group exists.  fb_init() swaps
       both the surface and the timings, so a portrait app is 480x800 at FULL
       resolution and nothing may scale.  A wrapper written against a fixed 800x480
       reference instead of screen_true_panel_* returns 42 and 83 here and is
       correct in every landscape case above — this is the only assertion in the
       file that fails on it. */
    expect_wrap("PORTRAIT 480x800 is full resolution, not 60%",
                480, 800, 480, 800,  70,  50,  70,  50);
    /* A transposed wrapper — fb_ui_px_x reading the height pair — survives every
       square-ratio case because both axes halve together.  This shape separates
       them: X halves, Y does not. */
    expect_wrap("X and Y ratios differ: only X may move",
                800, 480, 400, 480, 220,  60, 110,  60);

    printf("\n%s (%d failure%s)\n", fails ? "REGRESSION" : "ALL PASS",
           fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
