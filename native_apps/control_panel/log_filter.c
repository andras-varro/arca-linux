/* log_filter.c — see log_filter.h. */
#include "log_filter.h"
#include <ctype.h>
#include <string.h>

int log_split_tail(const char *buf, int len, int partial_head,
                   LogLine *out, int max) {
    int n = 0, end = len;
    if (!buf || !out || len <= 0 || max <= 0) return 0;
    /* Walk from the end, so a 256 KB buffer of short lines costs one pass and
     * only the newest `max` lines are ever kept. */
    while (end > 0 && n < max) {
        int s = end;
        while (s > 0 && buf[s - 1] != '\n') s--;
        int e = end;
        while (e > s && buf[e - 1] == '\r') e--;
        if (s == 0 && partial_head) break;
        if (e > s) { out[n].off = s; out[n].len = e - s; n++; }
        if (s == 0) break;
        end = s - 1;                    /* the '\n' ending the previous line */
    }
    for (int i = 0, j = n - 1; i < j; i++, j--) {
        LogLine t = out[i]; out[i] = out[j]; out[j] = t;
    }
    return n;
}

int log_prio(const char *line, int len) {
    if (len >= 3 && line[0] == '<' && line[1] >= '0' && line[1] <= '7' && line[2] == '>')
        return line[1] - '0';
    return -1;
}

int log_prefix_len(const char *line, int len) {
    return log_prio(line, len) >= 0 ? 3 : 0;
}

static int has_ci(const char *line, int len, const char *word) {
    int wl = (int)strlen(word);
    for (int i = 0; i + wl <= len; i++) {
        int k = 0;
        while (k < wl && tolower((unsigned char)line[i + k]) == word[k]) k++;
        if (k == wl) return 1;
    }
    return 0;
}

int log_is_error(const char *line, int len) {
    static const char *const words[] = {
        "error", "fail", "warn", "oops", "bug:", "segfault", "timeout"
    };
    if (!line || len <= 0) return 0;
    int p = log_prio(line, len);
    if (p >= 0 && p <= 3) return 1;
    for (unsigned i = 0; i < sizeof(words) / sizeof(words[0]); i++)
        if (has_ci(line, len, words[i])) return 1;
    return 0;
}

int log_clamp_top(int top, int n, int rows) {
    int max_top = n - rows;
    if (max_top < 0) max_top = 0;
    if (top > max_top) top = max_top;
    if (top < 0) top = 0;
    return top;
}
