/* status_line.h — the launcher's top strip: which message to show, the text on
 * its right, and the CPU share behind it.
 *
 * Pure: strings and numbers in, strings and numbers out.  The launcher reads
 * /proc and the clock and draws; a host test drives everything here with
 * fixtures.  The CPU arithmetic is control_panel/cpu_load.c's (wall-time
 * denominator, not /proc/stat's grand total), linked in rather than copied.
 */
#ifndef STATUS_LINE_H
#define STATUS_LINE_H

#include <stdbool.h>
#include <stddef.h>
#include "control_panel/cpu_load.h"

/* What the message conditions look at.  Add a field per new condition. */
typedef struct {
    bool calibrated;     /* /etc/touch_calibration.conf exists */
} StatusCtx;

/* One message: shown while active() is true; a tap or Enter on it launches
 * `exec` with `page` as its first argument (control_panel opens that page). */
typedef struct {
    bool       (*active)(const StatusCtx *ctx);
    const char  *text;
    const char  *exec;
    const char  *page;
} StatusMsg;

/* The first message in the table whose condition holds, or NULL. */
const StatusMsg *status_msg_pick(const StatusCtx *ctx);

/* A reading from the text of /proc/stat and /proc/uptime; self ticks are not
 * used here.  valid = both parsed. */
CpuSnap status_cpu_snap(const char *stat_text, const char *uptime_text);

/* Busy share over prev..cur as a whole percent 0..100 (rounded), or -1 when
 * there is no honest sample. */
int status_cpu_percent(const CpuSnap *prev, const CpuSnap *cur, unsigned clk_tck);

/* The right-hand text, "CPU 7%  14:05".  cpu_percent < 0 prints "CPU --%".
 * hour/min out of range yield "--:--". Always NUL-terminated within n;
 * returns the length written. */
int status_line_format(char *buf, size_t n, int cpu_percent, int hour, int min);

#endif
