/* overtemp.h — the over-temperature warning's pure decisions.
 *
 * arcamond (sysmon/arcamond.c) latches the state and publishes it as
 * MON_FLAG_OVERTEMP in the ring header; framebuffer.c reads that header at
 * most once a second and paints a red square on every present while it is
 * set.  Header-only and static inline, so framebuffer.c — which ScummVM and
 * vnc_client link too — gains no object to add to their build lists.
 * Pure: no file or clock access; tests/overtemp_test.c drives it.
 */
#ifndef OVERTEMP_H
#define OVERTEMP_H

#include <stdio.h>
#include "../sysmon/mon_ring.h"

/* Hysteresis on the SoC bandgap reading, millidegrees C.  ON at or above
 * OVERTEMP_ON_MC, OFF again only at or below OVERTEMP_OFF_MC.  The OMAP3
 * bandgap is uncalibrated (one unit idles at ~72 C), so these are a coarse
 * alarm, not a junction measurement; the zone exposes no trip points. */
#define OVERTEMP_ON_MC   85000
#define OVERTEMP_OFF_MC  80000

/* A ring file older than this (seconds, by mtime) is a dead daemon's. */
#define OVERTEMP_STALE_S 5
/* How often a presenter re-reads the ring, milliseconds. */
#define OVERTEMP_CHECK_MS 1000

/* Next latch state from the current one and a reading.  An absent reading
 * (MON_ABSENT) holds the state: a sensor hiccup neither raises nor clears. */
static inline int overtemp_latch(int on, int temp_mc) {
    if (temp_mc == MON_ABSENT) return on;
    return on ? temp_mc > OVERTEMP_OFF_MC : temp_mc >= OVERTEMP_ON_MC;
}

/* The ring text's over-temperature bit: 1 set, 0 clear, -1 not a ring header. */
static inline int overtemp_ring_flag(const char *text) {
    unsigned long long cs;
    unsigned flags;
    if (!text || sscanf(text, "arcamond 1 %llu %u", &cs, &flags) != 2) return -1;
    return (flags & MON_FLAG_OVERTEMP) ? 1 : 0;
}

/* Whether a file last written at mtime is live at now (both wall seconds;
 * the same clock, so an unsynced clock does not matter beyond one write). */
static inline int overtemp_fresh(long long mtime, long long now) {
    return mtime <= now + OVERTEMP_STALE_S && now - mtime <= OVERTEMP_STALE_S;
}

/* Whether a check is due at now_ms; updates *last_ms when it is.  *last_ms
 * < 0 means never checked. */
static inline int overtemp_due(long long now_ms, long long *last_ms) {
    if (*last_ms >= 0 && now_ms - *last_ms < OVERTEMP_CHECK_MS
        && now_ms >= *last_ms)
        return 0;
    *last_ms = now_ms;
    return 1;
}

#endif
