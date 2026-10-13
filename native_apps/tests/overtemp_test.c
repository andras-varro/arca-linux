/* Host-side regression for the over-temperature warning's decisions
 * (common/overtemp.h), which arcamond and framebuffer.c both use.
 *
 *   cd native_apps && gcc -Wall -Wextra -I. -o build/overtemp_test \
 *       tests/overtemp_test.c sysmon/mon_ring.c && ./build/overtemp_test
 *
 * What it asserts, and why:
 *   A  the latch turns ON at OVERTEMP_ON_MC and not one millidegree below;
 *   B  once ON it stays ON down to just above OVERTEMP_OFF_MC and clears at it
 *      — the hysteresis band holds whichever state it entered in;
 *   C  an absent reading (MON_ABSENT) holds the state in both directions;
 *   D  the ring header's flag: bit 0 set reads 1 whatever the other bits,
 *      clear reads 0, and text that is not a version-1 header reads -1 — what
 *      mon_ring_format() writes is what this reads;
 *   E  freshness: a file within OVERTEMP_STALE_S of now is live, an older one
 *      (a SIGKILLed daemon's) or one from beyond the same span ahead is not;
 *   F  the once-a-second gate: first call due, then not until
 *      OVERTEMP_CHECK_MS has passed, and a clock that runs backwards re-arms.
 */
#include "common/overtemp.h"
#include "sysmon/mon_ring.h"

#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(g, cond, ...) do { if (!(cond)) { fails++; printf("FAIL " g ": "); \
    printf(__VA_ARGS__); printf("\n"); } } while (0)

static int ring_flag_of(unsigned flags) {
    static MonRing r;
    static char buf[MON_RING_TEXT_MAX];
    MonSample s = { 10, 20000, 72000 };
    mon_ring_clear(&r);
    mon_ring_push(&r, &s);
    if (mon_ring_format(&r, 12345ULL, flags, buf, sizeof(buf)) < 0) return -9;
    return overtemp_ring_flag(buf);
}

int main(void) {
    /* A */
    CHECK("A", overtemp_latch(0, OVERTEMP_ON_MC) == 1, "off + ON threshold should latch");
    CHECK("A", overtemp_latch(0, OVERTEMP_ON_MC - 1) == 0, "off + just below ON must stay off");
    CHECK("A", overtemp_latch(0, 72000) == 0, "idle reading must stay off");
    CHECK("A", overtemp_latch(0, 120000) == 1, "far above ON should latch");

    /* B */
    CHECK("B", overtemp_latch(1, OVERTEMP_OFF_MC + 1) == 1, "on + just above OFF must hold");
    CHECK("B", overtemp_latch(1, (OVERTEMP_ON_MC + OVERTEMP_OFF_MC) / 2) == 1, "on + mid-band must hold");
    CHECK("B", overtemp_latch(0, (OVERTEMP_ON_MC + OVERTEMP_OFF_MC) / 2) == 0, "off + mid-band must hold");
    CHECK("B", overtemp_latch(1, OVERTEMP_OFF_MC) == 0, "on + OFF threshold should clear");
    CHECK("B", overtemp_latch(1, 40000) == 0, "on + cool should clear");
    CHECK("B", OVERTEMP_OFF_MC < OVERTEMP_ON_MC, "band must be non-empty");

    /* C */
    CHECK("C", overtemp_latch(1, MON_ABSENT) == 1, "on + absent must hold");
    CHECK("C", overtemp_latch(0, MON_ABSENT) == 0, "off + absent must hold");

    /* D */
    CHECK("D", ring_flag_of(MON_FLAG_OVERTEMP) == 1, "flag set reads %d", ring_flag_of(MON_FLAG_OVERTEMP));
    CHECK("D", ring_flag_of(MON_FLAG_OVERTEMP | 6u) == 1, "flag set with other bits");
    CHECK("D", ring_flag_of(0u) == 0, "flag clear reads %d", ring_flag_of(0u));
    CHECK("D", ring_flag_of(6u) == 0, "other bits only must read clear");
    CHECK("D", overtemp_ring_flag("arcamond 1 99 1 0\n") == 1, "bare header, flag set");
    CHECK("D", overtemp_ring_flag("arcamond 2 99 1 0\n") == -1, "wrong version must refuse");
    CHECK("D", overtemp_ring_flag("") == -1, "empty must refuse");
    CHECK("D", overtemp_ring_flag("arcamond 1 99\n") == -1, "truncated header must refuse");
    CHECK("D", overtemp_ring_flag(NULL) == -1, "NULL must refuse");

    /* E */
    CHECK("E", overtemp_fresh(1000, 1000) == 1, "same second is fresh");
    CHECK("E", overtemp_fresh(1000, 1000 + OVERTEMP_STALE_S) == 1, "edge of span is fresh");
    CHECK("E", overtemp_fresh(1000, 1001 + OVERTEMP_STALE_S) == 0, "past span is stale");
    CHECK("E", overtemp_fresh(1000, 1000 + 3600) == 0, "hour-old is stale");
    CHECK("E", overtemp_fresh(1000 + OVERTEMP_STALE_S + 1, 1000) == 0, "far future is stale");

    /* F */
    long long last = -1;
    CHECK("F", overtemp_due(5000, &last) == 1 && last == 5000, "first call is due");
    CHECK("F", overtemp_due(5000 + OVERTEMP_CHECK_MS - 1, &last) == 0, "inside the period is not due");
    CHECK("F", overtemp_due(5000 + OVERTEMP_CHECK_MS, &last) == 1, "end of period is due");
    CHECK("F", overtemp_due(100, &last) == 1 && last == 100, "a backwards clock re-arms");
    CHECK("F", overtemp_due(0, &(long long){ -1 }) == 1, "time zero on a fresh gate is due");

    printf("%s: %d failed\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
