/* mon_ring.h — the history ring arcamond publishes and the Monitor page reads.
 *
 * Pure: no file or /proc access, so a host test drives it with strings.
 * arcamond.c owns the sampling and the file; monitor_page.c the reading.
 *
 * The published file (MON_RING_PATH) is text, rewritten whole once a second
 * and renamed into place, so a reader sees either the previous or the next
 * version, never a half-written one:
 *
 *   arcamond 1 <wall_cs> <flags> <count>
 *   <cpu_pm> <mem_kb> <temp_mc>        count lines, oldest first
 *
 * wall_cs is /proc/uptime in centiseconds when the file was written, which is
 * what makes freshness checkable without trusting the (unsynced) wall clock.
 * Any field may be "-": no reading (the first second has no CPU share; a
 * kernel without the bandgap driver has no temperature).  flags is a bit set
 * for the daemon's state; bit 0 (MON_FLAG_OVERTEMP) is the over-temperature
 * warning, which framebuffer.c paints as a red square (common/overtemp.h).
 */
#ifndef MON_RING_H
#define MON_RING_H

#include <stddef.h>

#define MON_RING_PATH  "/var/run/arcamond.ring"   /* tmpfs: /var/volatile/run */
#define MON_RING_N     120                      /* 2 minutes at 1 Hz */
#define MON_ABSENT     (-2147483647 - 1)        /* a "-" field */
#define MON_FLAG_OVERTEMP 1u                    /* over-temperature warning */

/* The buffer a formatted ring always fits: header + MON_RING_N lines of
 * three fields of at most 11 characters each. */
#define MON_RING_TEXT_MAX (64 + MON_RING_N * 40)

typedef struct {
    int cpu_pm;    /* busy share of wall time, permille, or MON_ABSENT */
    int mem_kb;    /* MemTotal - MemAvailable, or MON_ABSENT */
    int temp_mc;   /* SoC millidegrees C, or MON_ABSENT */
} MonSample;

typedef struct {
    MonSample s[MON_RING_N];
    int head;      /* where the next push lands */
    int count;     /* 0..MON_RING_N */
} MonRing;

void mon_ring_clear(MonRing *r);
void mon_ring_push(MonRing *r, const MonSample *s);
/* i = 0 is the oldest held sample, count - 1 the newest; NULL out of range. */
const MonSample *mon_ring_get(const MonRing *r, int i);

/* Writes the file text into buf.  Returns its length, or -1 if it does not
 * fit (never with len >= MON_RING_TEXT_MAX). */
int mon_ring_format(const MonRing *r, unsigned long long wall_cs,
                    unsigned flags, char *buf, size_t len);

/* Parses file text into *r (cleared first).  0, or -1 on any malformed
 * header or line, a count above MON_RING_N, or fewer lines than the count
 * promised — a truncated file is refused, not half-used. */
int mon_ring_parse(const char *text, MonRing *r,
                   unsigned long long *wall_cs, unsigned *flags);

/* Whether a file written at file_cs is usable at now_cs: not from the future
 * and no older than max_age_cs. */
int mon_ring_fresh(unsigned long long file_cs, unsigned long long now_cs,
                   unsigned long long max_age_cs);

/* The /proc/meminfo text's MemTotal - MemAvailable in kB.  0, or -1 when
 * either line is missing. */
int mon_parse_meminfo(const char *text, int *used_kb);

#endif
