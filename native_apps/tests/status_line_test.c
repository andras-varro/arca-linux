/* Host-side regression for the launcher's status strip (common/status_line.c).
 *
 *   cd native_apps && gcc -Wall -Wextra -I. -o build/status_line_test \
 *       tests/status_line_test.c common/status_line.c control_panel/cpu_load.c \
 *       && ./build/status_line_test
 *
 * What it asserts, and why:
 *   A  message selection: no calibration file picks NOT CALIBRATED, whose
 *      action opens control_panel on its Input page; a calibrated unit picks
 *      nothing (a NULL, so the strip shows no message and no tap target);
 *   B  the CPU percent comes from two /proc readings over WALL time (the
 *      cpu_load.c rule): 30 busy ticks in 1.00 s of uptime is 30 %, even when
 *      the idle column advanced less than wall time; a first or garbled
 *      reading is -1, never 0, so the strip prints "--" rather than a lie;
 *   C  the right-hand text: fixed shape, zero-padded clock, "--" for no CPU
 *      sample, out-of-range clock guarded, 100 % clamp, tiny buffers terminate.
 */
#include "common/status_line.h"

#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { fails++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main(void) {
    /* A — selection */
    StatusCtx no_cal = { .calibrated = false }, cal = { .calibrated = true };
    const StatusMsg *m = status_msg_pick(&no_cal);
    CHECK(m != NULL && strcmp(m->text, "NOT CALIBRATED") == 0,
          "A: uncalibrated unit must show NOT CALIBRATED");
    CHECK(m != NULL && strcmp(m->exec, "/opt/games/control_panel") == 0 &&
          strcmp(m->page, "input") == 0, "A: action must open control_panel input");
    CHECK(status_msg_pick(&cal) == NULL, "A: calibrated unit shows no message");

    /* B — CPU percent from two samples */
    CpuSnap p = status_cpu_snap("cpu  1000 0 500 40000 10 0 5 0 0 0\n", "581.75 487.42\n");
    CpuSnap c = status_cpu_snap("cpu  1020 0 510 40020 10 0 5 0 0 0\n", "582.75 487.60\n");
    CHECK(p.valid && c.valid, "B: both snaps parse");
    CHECK(status_cpu_percent(&p, &c, 100) == 30,
          "B: 30 busy ticks over 1.00 s wall is 30 %%, got %d", status_cpu_percent(&p, &c, 100));
    CpuSnap c2 = status_cpu_snap("cpu  1021 0 510 40020 10 0 5 0 0 0\n", "582.75 1.0\n");
    CHECK(status_cpu_percent(&p, &c2, 100) == 31,
          "B: 31 ticks over 1.00 s is 31 %%, got %d", status_cpu_percent(&p, &c2, 100));
    CpuSnap hot = status_cpu_snap("cpu  9999 0 0 0 0 0 0 0 0 0\n", "582.75 1.0\n");
    CHECK(status_cpu_percent(&p, &hot, 100) == 100, "B: saturated window clamps at 100");
    CpuSnap bad = status_cpu_snap("garbage", "582.75 1.0");
    CHECK(!bad.valid, "B: garbled /proc/stat is an invalid snap");
    CHECK(status_cpu_percent(&bad, &c, 100) == -1, "B: no first sample is -1, not 0");
    CHECK(status_cpu_percent(&c, &p, 100) == -1, "B: counters running backwards is -1");

    /* C — formatting */
    char buf[32];
    status_line_format(buf, sizeof buf, 7, 9, 5, true);
    CHECK(strcmp(buf, "CPU 7%  09:05") == 0, "C: shape, got '%s'", buf);
    status_line_format(buf, sizeof buf, -1, 14, 30, true);
    CHECK(strcmp(buf, "CPU --%  14:30") == 0, "C: no sample, got '%s'", buf);
    status_line_format(buf, sizeof buf, 250, 23, 59, true);
    CHECK(strcmp(buf, "CPU 100%  23:59") == 0, "C: clamp, got '%s'", buf);
    status_line_format(buf, sizeof buf, 0, 24, 0, true);
    CHECK(strcmp(buf, "CPU 0%  --:--") == 0, "C: bad hour, got '%s'", buf);
    char tiny[6];
    int w = status_line_format(tiny, sizeof tiny, 12, 1, 2, true);
    CHECK(strlen(tiny) == 5 && w == 5, "C: tiny buffer terminates, len %zu w %d", strlen(tiny), w);

    /* D — 12 h (the System page TIME FORMAT setting) */
    status_line_format(buf, sizeof buf, 7, 13, 5, false);
    CHECK(strcmp(buf, "CPU 7%  1:05 PM") == 0, "D: 13:05, got '%s'", buf);
    status_line_format(buf, sizeof buf, 7, 0, 9, false);
    CHECK(strcmp(buf, "CPU 7%  12:09 AM") == 0, "D: midnight hour, got '%s'", buf);
    status_line_format(buf, sizeof buf, 7, 12, 0, false);
    CHECK(strcmp(buf, "CPU 7%  12:00 PM") == 0, "D: noon, got '%s'", buf);
    status_line_format(buf, sizeof buf, 7, 11, 59, false);
    CHECK(strcmp(buf, "CPU 7%  11:59 AM") == 0, "D: 11:59, got '%s'", buf);
    status_line_format(buf, sizeof buf, 7, 23, 59, false);
    CHECK(strcmp(buf, "CPU 7%  11:59 PM") == 0, "D: 23:59, got '%s'", buf);
    status_line_format(buf, sizeof buf, 0, 24, 0, false);
    CHECK(strcmp(buf, "CPU 0%  --:--") == 0, "D: bad hour in 12 h, got '%s'", buf);
    status_clock_text(buf, sizeof buf, 1, 5, false);
    CHECK(strcmp(buf, "1:05 AM") == 0, "D: clock text alone, got '%s'", buf);
    status_clock_text(buf, sizeof buf, 1, 5, true);
    CHECK(strcmp(buf, "01:05") == 0, "D: clock text 24 h, got '%s'", buf);
    char widest[24];
    int ww = status_line_format(widest, sizeof widest, 100, 12, 59, false);
    CHECK(ww == (int)strlen(widest) && ww < 24, "D: widest line fits TopBar.right_buf (24), %d", ww);

    printf(fails ? "status_line_test: %d FAILED\n" : "status_line_test: all passed\n", fails);
    return fails ? 1 : 0;
}
