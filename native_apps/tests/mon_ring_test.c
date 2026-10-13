/* Host-side regression for the history ring arcamond publishes and the Monitor
 * page reads (sysmon/mon_ring.c).
 *
 *   cd native_apps && gcc -Wall -Wextra -I. -o build/mon_ring_test \
 *       tests/mon_ring_test.c sysmon/mon_ring.c && ./build/mon_ring_test
 *
 * What it asserts, and why:
 *   A  the ring holds the newest MON_RING_N samples oldest-first, across a wrap;
 *   B  format -> parse is lossless: every field, "-" for an absent one, a
 *      negative temperature, the header's wall_cs and flags;
 *   C  parse refuses what is not a whole file of this version — a reader that
 *      half-used a truncated file would draw a graph with a hole in it;
 *   D  freshness: no older than the limit, never from the future;
 *   E  /proc/meminfo: used = MemTotal - MemAvailable, missing line refused;
 *   F  a full ring of the widest values fits MON_RING_TEXT_MAX.
 */
#include "sysmon/mon_ring.h"

#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { fails++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static MonSample mk(int c, int m, int t) { MonSample s = { c, m, t }; return s; }

int main(void) {
    static MonRing r, q;
    static char buf[MON_RING_TEXT_MAX];
    unsigned long long cs;
    unsigned flags;

    /* A */
    mon_ring_clear(&r);
    CHECK(r.count == 0 && mon_ring_get(&r, 0) == NULL, "A: empty ring has no sample 0");
    for (int i = 0; i < MON_RING_N + 5; i++) { MonSample s = mk(i, 1000 + i, 40000 + i); mon_ring_push(&r, &s); }
    CHECK(r.count == MON_RING_N, "A: count saturates at %d, got %d", MON_RING_N, r.count);
    CHECK(mon_ring_get(&r, 0) && mon_ring_get(&r, 0)->cpu_pm == 5, "A: oldest after wrap is push #5");
    CHECK(mon_ring_get(&r, MON_RING_N - 1) && mon_ring_get(&r, MON_RING_N - 1)->cpu_pm == MON_RING_N + 4,
          "A: newest is the last push");
    CHECK(mon_ring_get(&r, MON_RING_N) == NULL && mon_ring_get(&r, -1) == NULL, "A: out of range is NULL");

    /* B */
    mon_ring_clear(&r);
    { MonSample s = mk(MON_ABSENT, 61234, 71000);   mon_ring_push(&r, &s); }
    { MonSample s = mk(250, 61300, MON_ABSENT);     mon_ring_push(&r, &s); }
    { MonSample s = mk(1000, MON_ABSENT, -3500);    mon_ring_push(&r, &s); }
    int n = mon_ring_format(&r, 123456789ULL, 0u, buf, sizeof(buf));
    CHECK(n > 0 && (size_t)n == strlen(buf), "B: format returns the length (%d)", n);
    CHECK(strncmp(buf, "arcamond 1 123456789 0 3\n", 25) == 0, "B: header, got \"%.30s\"", buf);
    CHECK(mon_ring_parse(buf, &q, &cs, &flags) == 0, "B: own output parses");
    CHECK(cs == 123456789ULL && flags == 0 && q.count == 3, "B: header fields round-trip");
    CHECK(q.count == 3 && mon_ring_get(&q, 0)->cpu_pm == MON_ABSENT && mon_ring_get(&q, 0)->mem_kb == 61234
          && mon_ring_get(&q, 0)->temp_mc == 71000, "B: sample 0");
    CHECK(q.count == 3 && mon_ring_get(&q, 1)->cpu_pm == 250 && mon_ring_get(&q, 1)->temp_mc == MON_ABSENT,
          "B: sample 1 (absent temperature)");
    CHECK(q.count == 3 && mon_ring_get(&q, 2)->mem_kb == MON_ABSENT && mon_ring_get(&q, 2)->temp_mc == -3500,
          "B: sample 2 (negative temperature)");
    CHECK(mon_ring_format(&r, 5, MON_FLAG_OVERTEMP, buf, sizeof(buf)) > 0
          && mon_ring_parse(buf, &q, &cs, &flags) == 0 && flags == MON_FLAG_OVERTEMP,
          "B: flags round-trip");
    mon_ring_clear(&r);
    CHECK(mon_ring_format(&r, 7, 0u, buf, sizeof(buf)) > 0 && mon_ring_parse(buf, &q, &cs, &flags) == 0
          && q.count == 0, "B: an empty ring round-trips");

    /* C */
    CHECK(mon_ring_parse("", &q, &cs, &flags) == -1, "C: empty text");
    CHECK(mon_ring_parse("rwmonx 1 5 0 0\n", &q, &cs, &flags) == -1, "C: wrong magic");
    CHECK(mon_ring_parse("arcamond 2 5 0 0\n", &q, &cs, &flags) == -1, "C: wrong version");
    {   /* well-formed in every line, so only the count can refuse it */
        int at = snprintf(buf, sizeof(buf), "arcamond 1 5 0 %d\n", MON_RING_N + 1);
        for (int i = 0; i <= MON_RING_N; i++) at += snprintf(buf + at, sizeof(buf) - (size_t)at, "1 2 3\n");
        CHECK(mon_ring_parse(buf, &q, &cs, &flags) == -1, "C: count above MON_RING_N");
    }
    CHECK(mon_ring_parse("arcamond 1 5 0 -1\n", &q, &cs, &flags) == -1, "C: negative count");
    CHECK(mon_ring_parse("arcamond 1 5 0 2\n10 20 30\n", &q, &cs, &flags) == -1, "C: fewer lines than count");
    CHECK(mon_ring_parse("arcamond 1 5 0 1\n10 20\n", &q, &cs, &flags) == -1, "C: a line short of a field");
    CHECK(mon_ring_parse("arcamond 1 5 0 1\n10 2x 30\n", &q, &cs, &flags) == -1, "C: a non-numeric field");
    CHECK(mon_ring_parse("arcamond 1 5 0 1\n10 20 30 40\n", &q, &cs, &flags) == -1, "C: an extra field");
    CHECK(mon_ring_parse("arcamond 1 5 0 1\n10 20 30\n", &q, &cs, &flags) == 0 && q.count == 1,
          "C: the minimal good file parses");

    /* D */
    CHECK(mon_ring_fresh(1000, 1000, 300), "D: same moment is fresh");
    CHECK(mon_ring_fresh(1000, 1300, 300), "D: at the limit is fresh");
    CHECK(!mon_ring_fresh(1000, 1301, 300), "D: past the limit is stale");
    CHECK(!mon_ring_fresh(1001, 1000, 300), "D: from the future is refused");

    /* E */
    int used = -1;
    CHECK(mon_parse_meminfo("MemTotal:         227328 kB\nMemFree:           12000 kB\n"
                            "MemAvailable:     150000 kB\nBuffers: 1 kB\n", &used) == 0 && used == 77328,
          "E: used = total - available, got %d", used);
    used = 42;
    CHECK(mon_parse_meminfo("MemTotal: 227328 kB\nMemFree: 12000 kB\n", &used) == -1 && used == 42,
          "E: no MemAvailable is refused and leaves *used");

    /* F */
    mon_ring_clear(&r);
    for (int i = 0; i < MON_RING_N; i++) { MonSample s = mk(-2147483647, -2147483647, -2147483647); mon_ring_push(&r, &s); }
    n = mon_ring_format(&r, 18446744073709551615ULL, 0xffffffffu, buf, sizeof(buf));
    CHECK(n > 0 && n < (int)sizeof(buf), "F: worst case fits (%d of %d)", n, (int)sizeof(buf));
    CHECK(mon_ring_format(&r, 1, 0, buf, 64) == -1, "F: a short buffer is refused, not overrun");

    printf("mon_ring_test: %s (%d failure(s))\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
