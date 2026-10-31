/* Host regression for the USB-overload recovery's decisions (common/usbfault.h).
 *
 *   cd native_apps && gcc -Wall -Wextra -I. -o build/usbfault_test \
 *       tests/usbfault_test.c && ./build/usbfault_test
 *
 *   A  an error with a device present at T+5 s clears: no recover, no flag;
 *   B  an error with no device latches at T+5 s and asks for one recover;
 *   C  retries fall at 15 s and 45 s after the error, then every 120 s, and
 *      none earlier, over a long run with the device never returning;
 *   D  a device appearing while latched clears flag and schedule at once;
 *   E  a running recover defers a due retry, never doubles it;
 *   F  no error means no action, and the flag is off before the first check;
 *   G  the schedule survives the uint32_t millisecond clock wrapping.
 */
#include "common/usbfault.h"
#include <stdio.h>

static int fails;
#define CHECK(g, cond, ...) do { if (!(cond)) { fails++; printf("FAIL " g ": "); \
    printf(__VA_ARGS__); printf("\n"); } } while (0)

/* Run seconds [0, secs] after an error at base; record recover start times. */
static int run(uint32_t base, int secs, int dev_from_s, int *at, int max) {
    UsbFault f; usbf_init(&f);
    int n = 0;
    for (int s = 0; s <= secs; s++) {
        uint32_t now = base + (uint32_t)s * 1000u;
        int dev = dev_from_s >= 0 && s >= dev_from_s;
        if (usbf_step(&f, now, s == 0, dev, 0)) { if (n < max) at[n] = s; n++; }
    }
    return n;
}

int main(void) {
    int at[32];
    int n;

    n = run(1000, 60, 5, at, 32);
    CHECK("A", n == 0, "device at T+5 ran %d recovers", n);
    { UsbFault f; usbf_init(&f); usbf_step(&f, 0, 1, 0, 0);
      CHECK("A", !usbf_flag(&f), "flag set before the first check");
      usbf_step(&f, 5000, 0, 1, 0);
      CHECK("A", !usbf_flag(&f) && !f.armed, "present at T+5 left state armed/latched"); }

    n = run(1000, 4, -1, at, 32);
    CHECK("F", n == 0, "recover before T+5 s (%d)", n);
    n = run(1000, 400, -1, at, 32);
    CHECK("B", n > 0 && at[0] == 5, "first recover at %d s, want 5", n ? at[0] : -1);
    CHECK("C", n >= 5, "only %d recovers in 400 s", n);
    if (n >= 5) {
        CHECK("C", at[1] == 15, "second at %d, want 15", at[1]);
        CHECK("C", at[2] == 45, "third at %d, want 45", at[2]);
        CHECK("C", at[3] - at[2] == 120, "fourth %d s after third, want 120", at[3] - at[2]);
        CHECK("C", at[4] - at[3] == 120, "fifth %d s after fourth, want 120", at[4] - at[3]);
    }

    { UsbFault f; usbf_init(&f); int s;
      for (s = 0; s <= 20; s++) usbf_step(&f, (uint32_t)s * 1000u, s == 0, 0, 0);
      CHECK("D", usbf_flag(&f), "not latched at 20 s");
      usbf_step(&f, 21000u, 0, 1, 0);
      CHECK("D", !usbf_flag(&f) && !f.armed && f.tries == 0, "device did not clear the latch");
      CHECK("D", usbf_step(&f, 400000u, 0, 0, 0) == 0, "recover after clear"); }

    { UsbFault f; usbf_init(&f); int started = 0;
      for (int s = 0; s <= 30; s++)   /* child alive the whole time after first start */
          started += usbf_step(&f, (uint32_t)s * 1000u, s == 0, 0, s > 5);
      CHECK("E", started == 1, "%d recovers while one was running", started);
      CHECK("E", usbf_step(&f, 31000u, 0, 0, 0) == 1, "deferred retry not started when child exits"); }

    n = run(0xFFFFFFFFu - 20000u, 400, -1, at, 32);
    CHECK("G", n >= 5 && at[0] == 5 && at[1] == 15 && at[2] == 45 && at[3] == 165,
          "wrap: n=%d at=%d,%d,%d,%d", n, at[0], at[1], at[2], at[3]);

    if (fails) { printf("usbfault_test: %d FAILED\n", fails); return 1; }
    printf("usbfault_test: ok\n");
    return 0;
}
