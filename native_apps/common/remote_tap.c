/* remote_tap — the pure half of arca-tap: command parsing and swipe geometry.
 * See remote_tap.h. No I/O, so it builds on the host. */
#include "remote_tap.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/* Read up to `max` whitespace-separated non-negative integers from *p.
 * Returns the count read, or -1 on a token that is not a plain number. */
static int read_ints(const char *p, int *v, int max) {
    int n = 0;
    for (;;) {
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
        if (*p == '\0') return n;
        if (n == max) return -1;           /* one field too many */
        if (!isdigit((unsigned char)*p)) return -1;
        long x = 0;
        while (isdigit((unsigned char)*p)) {
            x = x * 10 + (*p - '0');
            if (x > 1000000) return -1;
            p++;
        }
        if (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') return -1;
        v[n++] = (int)x;
    }
}

bool remote_tap_parse(const char *line, RemoteTapCmd *c) {
    if (!line || !c) return false;
    memset(c, 0, sizeof(*c));
    while (*line == ' ' || *line == '\t') line++;

    const char *verb[] = { "tap", "hold", "swipe" };
    const RemoteTapKind kind[] = { RTAP_CMD_TAP, RTAP_CMD_HOLD, RTAP_CMD_SWIPE };
    const int need[] = { 2, 3, 4 };      /* mandatory numbers */
    const int allow[] = { 2, 3, 5 };     /* swipe takes an optional time */
    int k = -1;
    const char *rest = line;
    for (int i = 0; i < 3; i++) {
        size_t n = strlen(verb[i]);
        if (strncmp(line, verb[i], n) == 0 &&
            (line[n] == ' ' || line[n] == '\t')) {
            k = i;
            rest = line + n;
            break;
        }
    }
    if (k < 0) return false;

    int v[5];
    int n = read_ints(rest, v, allow[k]);
    if (n < need[k]) return false;

    for (int i = 0; i < (k == 1 ? 2 : need[k]); i++)
        if (v[i] > RTAP_MAX_COORD) return false;

    c->kind = kind[k];
    c->x1 = v[0];
    c->y1 = v[1];
    if (k == 0) {
        c->ms = RTAP_TAP_MS;
    } else if (k == 1) {
        c->ms = v[2];
    } else {
        c->x2 = v[2];
        c->y2 = v[3];
        c->ms = (n == 5) ? v[4] : RTAP_SWIPE_MS;
    }
    if (c->ms < 1 || c->ms > RTAP_MAX_MS) {
        memset(c, 0, sizeof(*c));
        return false;
    }
    return true;
}

int remote_tap_swipe_steps(int ms) {
    int n = ms / RTAP_STEP_MS;
    return n < 2 ? 2 : n;
}

int remote_tap_lerp(int a, int b, int i, int n) {
    if (n <= 0) return b;
    long d = (long)(b - a) * i;           /* may be negative */
    long q = d >= 0 ? (d + n / 2) / n : -((-d + n / 2) / n);
    return a + (int)q;
}
