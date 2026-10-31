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
 *   G  the schedule survives the uint32_t millisecond clock wrapping;
 *   H  the red LED: solid for over-temperature, three 200 ms flashes at the
 *      start of every 10 s for a latched fault (the sidebars' cadence), released
 *      when neither, and over-temperature wins;
 *   I  three errors inside 30 s cut the port once even though the devices are
 *      back at every T+5 s check, and the retry follows 120 s after the cut;
 *   J  negative controls: two errors, or three spread past 30 s, do not cut;
 *   K  a cut due while a recover runs waits for the child, then fires once;
 *   L  the retry's own loop cuts again, 120 s later retry, across a clock wrap.
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


    /* I: the loop.  Errors 3 s apart with the devices back within 2 s
     * (so the T+5 s check always sees them) cuts at the third error inside
     * 30 s, once, and the retry is 120 s after the cut. */
    {
        UsbFault f; usbf_init(&f); int offs = 0, off_s = -1, recs = 0, rec_s = -1;
        for (int s = 0; s <= 200; s++) {
            int err = (s == 0 || s == 3 || s == 6);
            int dev = !f.latched;                       /* present until cut */
            if (f.latched) dev = 0;
            int a = usbf_step(&f, 1000u + (uint32_t)s * 1000u, err, dev, 0);
            if (a == USBF_ACT_OFF) { offs++; off_s = s; }
            else if (a == 1) { if (!recs) rec_s = s; recs++; }
        }
        CHECK("I", offs == 1 && off_s == 6, "cut %d times, at %d s (want once, at 6)", offs, off_s);
        CHECK("I", rec_s == 126, "retry at %d s, want 126 (120 s after the cut)", rec_s);
        CHECK("I", usbf_flag(&f), "not latched after the cut");
    }
    /* J: negative controls: 2 errors in 30 s, and 3 spread over 40 s, do not cut. */
    {
        static const int pat[][3] = { {0, 10, -1}, {0, 20, 40}, {0, 31, 62}, {0, 14, 31} };
        for (unsigned p = 0; p < 4; p++) {
            UsbFault f; usbf_init(&f); int offs = 0;
            for (int s = 0; s <= 100; s++) {
                int err = s == pat[p][0] || s == pat[p][1] || s == pat[p][2];
                offs += usbf_step(&f, 5000u + (uint32_t)s * 1000u, err, 1, 0) == USBF_ACT_OFF;
            }
            CHECK("J", offs == 0, "pattern %u cut %d times", p, offs);
        }
        /* positive edge: exactly 30 s from first to third cuts */
        UsbFault f; usbf_init(&f); int offs = 0;
        for (int s = 0; s <= 40; s++)
            offs += usbf_step(&f, (uint32_t)s * 1000u, s == 0 || s == 15 || s == 30, 1, 0) == USBF_ACT_OFF;
        CHECK("J", offs == 1, "0,15,30 s cut %d times, want 1", offs);
    }
    /* K: a cut due while a recover child is alive waits for it, then fires once. */
    {
        UsbFault f; usbf_init(&f); int offs = 0, off_s = -1;
        for (int s = 0; s <= 20; s++) {
            int a = usbf_step(&f, (uint32_t)s * 1000u, s == 0 || s == 1 || s == 2, 1, s < 10);
            if (a == USBF_ACT_OFF) { offs++; off_s = s; }
        }
        CHECK("K", offs == 1 && off_s == 10, "cut %d times, at %d (want once at 10)", offs, off_s);
    }
    /* L: after the retry the loop returns: the next 3 errors cut again, and the
     * following retry is 120 s later; the clock wrapping changes nothing. */
    {
        UsbFault f; usbf_init(&f); int offs = 0, recs = 0, off2 = -1, rec2 = -1;
        uint32_t base = 0xFFFFFFFFu - 100000u;
        for (int s = 0; s <= 400; s++) {
            int err = s == 0 || s == 2 || s == 4 || (s >= 130 && s <= 132);
            int a = usbf_step(&f, base + (uint32_t)s * 1000u, err, !f.latched, 0);
            if (a == USBF_ACT_OFF) { offs++; if (offs == 2) off2 = s; }
            else if (a == 1) { recs++; if (recs == 2) rec2 = s; }
        }
        CHECK("L", offs == 2 && off2 == 132, "cuts %d, second at %d (want 2, 132)", offs, off2);
        CHECK("L", rec2 == 252, "retry after the second cut at %d, want 252", rec2);
    }
    for (uint32_t t = 0; t < 30000u; t += 50u) {   /* H */
        uint32_t m = t % 10000u;
        int want = (m < 1200u && (m / 200u) % 2u == 0u) ? USBF_LED_LIT : USBF_LED_DARK;
        CHECK("H", usbf_led(0, 1, t) == want, "flash at %u: %d", t, usbf_led(0, 1, t));
        CHECK("H", usbf_led(1, 1, t) == USBF_LED_LIT && usbf_led(1, 0, t) == USBF_LED_LIT,
              "over-temperature not solid at %u", t);
        CHECK("H", usbf_led(0, 0, t) == USBF_LED_RELEASE, "idle not released at %u", t);
    }
    { int on = 0; for (uint32_t t = 0; t < 10000u; t += 10u) on += usbf_led(0, 1, t) == USBF_LED_LIT;
      CHECK("H", on == 3 * 20, "lit %d ticks of 10 ms per period, want 60", on); }

    if (fails) { printf("usbfault_test: %d FAILED\n", fails); return 1; }
    printf("usbfault_test: ok\n");
    return 0;
}
