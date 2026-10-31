/* status_line.c — see status_line.h. */
#include "common/status_line.h"
#include "common/hour12.h"
#include <stdio.h>

static bool not_calibrated(const StatusCtx *ctx) { return !ctx->calibrated; }

/* The message table: first active entry wins, so order is priority.  Without
 * a calibration file the raw touch mapping is uncorrected, and the touch wizard
 * lives on control_panel's Input page. */
static const StatusMsg messages[] = {
    { not_calibrated, "NOT CALIBRATED", "/opt/games/control_panel", "input" },
};

const StatusMsg *status_msg_pick(const StatusCtx *ctx) {
    for (size_t i = 0; i < sizeof(messages) / sizeof(messages[0]); i++)
        if (messages[i].active(ctx)) return &messages[i];
    return NULL;
}

CpuSnap status_cpu_snap(const char *stat_text, const char *uptime_text) {
    CpuSnap s = { 0, 0, 0, 0 };
    if (stat_text && uptime_text &&
        cpu_parse_stat(stat_text, &s.busy) == 0 &&
        cpu_parse_uptime(uptime_text, &s.wall_cs) == 0)
        s.valid = 1;
    return s;
}

int status_cpu_percent(const CpuSnap *prev, const CpuSnap *cur, unsigned clk_tck) {
    int others = 0, self = 0;
    if (cpu_share(prev, cur, clk_tck, &others, &self) != 0) return -1;
    return (others + self + 5) / 10;      /* permille -> rounded percent */
}

int status_clock_text(char *buf, size_t n, int hour, int min, bool h24) {
    if (n == 0) return 0;
    int w;
    if (hour < 0 || hour > 23 || min < 0 || min > 59) w = snprintf(buf, n, "--:--");
    else if (h24) w = snprintf(buf, n, "%02d:%02d", hour, min);
    else w = snprintf(buf, n, "%d:%02d %s", hour12_of(hour), min, hour12_ampm(hour));
    if (w < 0) { buf[0] = '\0'; return 0; }
    return (size_t)w >= n ? (int)n - 1 : w;
}

int status_line_format(char *buf, size_t n, int cpu_percent, int hour, int min, bool h24) {
    if (n == 0) return 0;
    char cpu[8], clk[12];
    if (cpu_percent < 0) snprintf(cpu, sizeof cpu, "--");
    else snprintf(cpu, sizeof cpu, "%d", cpu_percent > 100 ? 100 : cpu_percent);
    status_clock_text(clk, sizeof clk, hour, min, h24);
    int w = snprintf(buf, n, "CPU %s%%  %s", cpu, clk);
    if (w < 0) { buf[0] = '\0'; return 0; }
    return (size_t)w >= n ? (int)n - 1 : w;
}
