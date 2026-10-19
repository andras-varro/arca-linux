/* remote_tap_test — the pure half of arca_tap: the inverse of the touch axis map
 * and the command parser.
 *   gcc -Wall -Wextra -Wno-unused-parameter -I common -o build/remote_tap_test \
 *       tests/remote_tap_test.c common/remote_tap.c common/touch_calib.c \
 *       common/touch_input.c common/framebuffer.c common/hardware.c common/config.c -lm
 */
#include "remote_tap.h"
#include "touch_calib.h"
#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); fails++; } } while (0)

static void roundtrip(int v0, int kl, int kh, int v1, int dim, const char *name) {
    int bad = 0, unexact = 0;
    for (int p = 0; p < dim; p++) {
        bool ex;
        int lo = v0 < v1 ? v0 : v1, hi = v0 < v1 ? v1 : v0;
        if (lo < 0) lo = 0;
        if (hi > 4095) hi = 4095;
        int raw = touch_calib_raw_for_panel(p, v0, kl, kh, v1, dim, lo, hi, &ex);
        if (!ex) { unexact++; continue; }
        if (touch_map_axis_panel(raw, v0, kl, kh, v1, dim) != p) bad++;
    }
    printf("%s: %d mismatches, %d unreachable of %d\n", name, bad, unexact, dim);
    CHECK(bad == 0, name);
}

int main(void) {
    int kl, kh;
    touch_knots_on_line(17, 4084, 800, &kl, &kh);
    roundtrip(17, kl, kh, 4084, 800, "X line 17..4084");
    touch_knots_on_line(-279, 4382, 480, &kl, &kh);
    roundtrip(-279, kl, kh, 4382, 480, "Y line -279..4382 (saturated ends unreachable)");
    roundtrip(0, 1000, 3000, 4095, 800, "X three segments");
    roundtrip(0, 0, 0, 4095, 480, "Y no curve");

    bool ex = true;
    touch_calib_raw_for_panel(0, -279, 0, 0, 4382, 480, 0, 4095, &ex);
    CHECK(!ex, "panel 0 unreachable when its raw is below 0 (negative control)");

    RemoteTapCmd c;
    CHECK(remote_tap_parse("tap 400 240", &c) && c.kind == RTAP_CMD_TAP && c.x1 == 400 && c.y1 == 240, "tap");
    CHECK(remote_tap_parse("hold 1 2 500\n", &c) && c.kind == RTAP_CMD_HOLD && c.ms == 500, "hold");
    CHECK(remote_tap_parse("swipe 1 2 3 4", &c) && c.ms == RTAP_SWIPE_MS && c.x2 == 3, "swipe default ms");
    CHECK(remote_tap_parse("swipe 1 2 3 4 90", &c) && c.ms == 90, "swipe ms");
    CHECK(!remote_tap_parse("tap 400", &c), "tap missing y");
    CHECK(!remote_tap_parse("tap 1 2 3", &c), "tap extra field");
    CHECK(!remote_tap_parse("tap -1 2", &c), "negative");
    CHECK(!remote_tap_parse("tap 5000 2", &c), "coord too large");
    CHECK(!remote_tap_parse("hold 1 2 0", &c), "hold zero ms");
    CHECK(!remote_tap_parse("tapx 1 2", &c), "verb prefix");
    CHECK(!remote_tap_parse("", &c), "empty");
    CHECK(remote_tap_lerp(0, 100, 5, 10) == 50 && remote_tap_lerp(100, 0, 5, 10) == 50, "lerp mid");
    CHECK(remote_tap_lerp(10, 20, 10, 10) == 20 && remote_tap_lerp(10, 20, 0, 10) == 10, "lerp ends");
    CHECK(remote_tap_swipe_steps(5) == 2 && remote_tap_swipe_steps(320) == 20, "steps");
    printf(fails ? "FAILED\n" : "OK\n");
    return fails != 0;
}
