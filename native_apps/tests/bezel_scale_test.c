/* Host-side regression for the panel->surface bezel conversion.
 *
 * Runs on the DEV MACHINE with native gcc.  The function it tests is the pure
 * half of a device-only mechanism: fb_apply_viewport() reads the panel size from
 * /sys/devices/platform/omapdss/display0/timings, which exists on no host, so
 * the arithmetic was split out to be reachable here.  Build and run:
 *
 *   cd native_apps && gcc -Wall -Wextra -Wno-unused-parameter -I common \
 *       -o build/bezel_scale_test tests/bezel_scale_test.c \
 *       common/framebuffer.c common/font.c common/hardware.c common/config.c -lm \
 *       && ./build/bezel_scale_test
 *
 * What can go wrong, and why each group is here.  The margins name pixels the
 * plastic bezel physically covers, so they are PANEL pixels.  A scaling DSS
 * overlay draws a smaller surface upscaled to fill the panel, so subtracting a
 * panel-space margin from the surface spends it at the upscale factor: measured
 * on .188, a 400x240 surface upscaled 2x turned T=15 into 30 of the panel's 480
 * rows, and the operator saw the resulting band at top and bottom.
 *
 * ⚠️ Group 1 is the NEGATIVE CONTROL: at the shipped 800x480 geometry the
 * conversion must be a bit-exact no-op, or this is a regression on every unit in
 * the field.  ⚠️ But measured against three sabotaged variants, group 1 catches
 * LESS than it looks like it does, and the reason is worth knowing before
 * trusting it: at 1:1 every ratio is the identity, so NO error in the ratio can
 * fail group 1.  Scaling unconditionally (dropping the panel != surf guard, which
 * is therefore an optimisation and not a behaviour) passes all six groups.  An
 * inverted or axis-swapped ratio is caught by groups 2-4 and 6 instead, and the
 * axis-swapped variant does not even reach a verdict — it divides by group 5's
 * zero panel height and dies of SIGFPE, which is a detection by crash rather
 * than by assertion.  What group 1 does protect against is a version that scales
 * by anything that is not the ratio at all.
 * Group 5 is the other control: the pre-fix behaviour was "never scale", and that
 * variant passes groups 1 and 5 and fails 2-4 and 6 with 8 failures — measured,
 * so this test has been seen failing on the defect it exists for.
 */
#include <stdio.h>
#include "framebuffer.h"

static int fails;

static void expect4(const char *what,
                    int panel_w, int panel_h, int surf_w, int surf_h,
                    int t, int b, int l, int r,
                    int et, int eb, int el, int er)
{
    fb_scale_bezel_to_surface(panel_w, panel_h, surf_w, surf_h, &t, &b, &l, &r);
    int ok = (t == et && b == eb && l == el && r == er);
    if (!ok) fails++;
    printf("  %-4s %-44s T=%d B=%d L=%d R=%d (want T=%d B=%d L=%d R=%d)\n",
           ok ? "ok" : "FAIL", what, t, b, l, r, et, eb, el, er);
}

int main(void)
{
    printf("group 1  NEGATIVE CONTROL: surface == panel must not move a margin\n");
    expect4("800x480 panel, 800x480 surface, .188 margins",
            800, 480, 800, 480,  15, 13, 0, 0,   15, 13, 0, 0);
    expect4("800x480 both, all four margins non-zero",
            800, 480, 800, 480,  15, 13, 7, 9,   15, 13, 7, 9);
    expect4("480x800 portrait, surface == panel",
            480, 800, 480, 800,  0, 0, 15, 13,   0, 0, 15, 13);

    printf("group 2  the measured case: 400x240 surface on an 800x480 panel\n");
    expect4(".188 T=15 B=13 halve; L=R=0 stay 0",
            800, 480, 400, 240,  15, 13, 0, 0,   7, 6, 0, 0);
    expect4("all four halve",
            800, 480, 400, 240,  15, 13, 8, 9,   7, 6, 4, 4);

    printf("group 3  rounds DOWN, so the art runs under the bezel, never short\n");
    expect4("odd margins truncate rather than round up",
            800, 480, 400, 240,  1, 3, 5, 7,   0, 1, 2, 3);
    expect4("4x downscale of a 15px margin",
            800, 480, 200, 120,  15, 13, 0, 0,   3, 3, 0, 0);

    printf("group 4  each axis scales independently\n");
    expect4("width halves, height unchanged",
            800, 480, 400, 480,  15, 13, 8, 9,   15, 13, 4, 4);
    expect4("height halves, width unchanged",
            800, 480, 800, 240,  15, 13, 8, 9,   7, 6, 8, 9);

    printf("group 5  CONTROL: a panel size we could not read is left 1:1\n");
    expect4("panel 0x0 (timings unreadable) must not scale",
            0, 0, 400, 240,  15, 13, 8, 9,   15, 13, 8, 9);
    expect4("panel height 0 only: width still scales",
            800, 0, 400, 240,  15, 13, 8, 9,   15, 13, 4, 4);
    expect4("surface 0 (degenerate) must not scale",
            800, 480, 0, 0,  15, 13, 8, 9,   15, 13, 8, 9);

    printf("group 6  a surface LARGER than the panel scales up, not down\n");
    expect4("1600x960 surface downscaled to the panel",
            800, 480, 1600, 960,  15, 13, 8, 9,   30, 26, 16, 18);

    printf("\n%s (%d failure%s)\n", fails ? "REGRESSION" : "ALL PASS",
           fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
