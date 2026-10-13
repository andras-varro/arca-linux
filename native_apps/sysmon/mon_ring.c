/* mon_ring.c — see mon_ring.h. */
#include "mon_ring.h"

#include <stdio.h>
#include <string.h>

void mon_ring_clear(MonRing *r) {
    memset(r, 0, sizeof(*r));
}

void mon_ring_push(MonRing *r, const MonSample *s) {
    r->s[r->head] = *s;
    r->head = r->head + 1 == MON_RING_N ? 0 : r->head + 1;
    if (r->count < MON_RING_N) r->count++;
}

const MonSample *mon_ring_get(const MonRing *r, int i) {
    if (i < 0 || i >= r->count) return NULL;
    int k = r->head - r->count + i;
    if (k < 0) k += MON_RING_N;
    return &r->s[k];
}

/* One field: "-" (MON_ABSENT) or an optionally negative decimal int. */
static int put_field(char *p, size_t room, int v, char sep) {
    int n = v == MON_ABSENT ? snprintf(p, room, "-%c", sep)
                            : snprintf(p, room, "%d%c", v, sep);
    return n < 0 || (size_t)n >= room ? -1 : n;
}

int mon_ring_format(const MonRing *r, unsigned long long wall_cs,
                    unsigned flags, char *buf, size_t len) {
    size_t at = 0;
    int n = snprintf(buf, len, "arcamond 1 %llu %u %d\n", wall_cs, flags, r->count);
    if (n < 0 || (size_t)n >= len) return -1;
    at = (size_t)n;
    for (int i = 0; i < r->count; i++) {
        const MonSample *s = mon_ring_get(r, i);
        int a = put_field(buf + at, len - at, s->cpu_pm, ' ');
        if (a < 0) return -1;
        at += (size_t)a;
        a = put_field(buf + at, len - at, s->mem_kb, ' ');
        if (a < 0) return -1;
        at += (size_t)a;
        a = put_field(buf + at, len - at, s->temp_mc, '\n');
        if (a < 0) return -1;
        at += (size_t)a;
    }
    return (int)at;
}

/* Parses one field at *pp, then expects sep.  0 or -1. */
static int get_field(const char **pp, int *v, char sep) {
    const char *p = *pp;
    if (*p == '-' && p[1] == sep) {
        *v = MON_ABSENT;
        *pp = p + 2;
        return 0;
    }
    int neg = 0;
    if (*p == '-') { neg = 1; p++; }
    if (*p < '0' || *p > '9') return -1;
    long long acc = 0;
    while (*p >= '0' && *p <= '9') {
        acc = acc * 10 + (*p++ - '0');
        if (acc > 2147483647LL) return -1;
    }
    if (*p != sep) return -1;
    *v = (int)(neg ? -acc : acc);
    *pp = p + 1;
    return 0;
}

int mon_ring_parse(const char *text, MonRing *r,
                   unsigned long long *wall_cs, unsigned *flags) {
    unsigned long long cs;
    unsigned fl;
    int count, used = 0;
    mon_ring_clear(r);
    if (!text || sscanf(text, "arcamond 1 %llu %u %d\n%n", &cs, &fl, &count, &used) != 3
        || used == 0 || text[used - 1] != '\n')
        return -1;
    if (count < 0 || count > MON_RING_N) return -1;
    const char *p = text + used;
    for (int i = 0; i < count; i++) {
        MonSample s;
        if (get_field(&p, &s.cpu_pm, ' ') || get_field(&p, &s.mem_kb, ' ')
            || get_field(&p, &s.temp_mc, '\n')) {
            mon_ring_clear(r);
            return -1;
        }
        mon_ring_push(r, &s);
    }
    *wall_cs = cs;
    *flags = fl;
    return 0;
}

int mon_ring_fresh(unsigned long long file_cs, unsigned long long now_cs,
                   unsigned long long max_age_cs) {
    return file_cs <= now_cs && now_cs - file_cs <= max_age_cs;
}

int mon_parse_meminfo(const char *text, int *used_kb) {
    const char *t = text ? strstr(text, "MemTotal:") : NULL;
    const char *a = text ? strstr(text, "MemAvailable:") : NULL;
    long total, avail;
    if (!t || !a || sscanf(t, "MemTotal: %ld", &total) != 1
        || sscanf(a, "MemAvailable: %ld", &avail) != 1)
        return -1;
    *used_kb = (int)(total - avail);
    return 0;
}
