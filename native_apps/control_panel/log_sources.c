/* log_sources.c — parse and scan the Log page's item files (log_sources.h). */
#include "log_sources.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LOGSRC_ITEM_MAX   512       /* an item is two short lines; more is not read */
#define LOGSRC_FILES_MAX  64        /* file names considered before sorting */
#define LOGSRC_FNAME_MAX  64

/* Trims blanks off both ends of s[0..n) into dst (size dstsz); returns 0 when it does not fit. */
static int copy_trimmed(char *dst, size_t dstsz, const char *s, size_t n) {
    while (n > 0 && isspace((unsigned char)*s)) { s++; n--; }
    while (n > 0 && isspace((unsigned char)s[n - 1])) n--;
    if (n >= dstsz) return 0;
    memcpy(dst, s, n);
    dst[n] = '\0';
    return 1;
}

int logsrc_parse(const char *text, LogSource *out) {
    char name[LOGSRC_NAME_MAX + 1] = "", path[LOGSRC_PATH_MAX] = "";
    int have_name = 0, have_path = 0;

    for (const char *p = text; p && *p; ) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        const char *eq = memchr(p, '=', len);
        if (eq && p[0] != '#') {
            char key[16];
            if (copy_trimmed(key, sizeof key, p, (size_t)(eq - p))) {
                const char *val = eq + 1;
                size_t vlen = len - (size_t)(val - p);
                if (strcmp(key, "name") == 0) {
                    char tmp[64];
                    have_name = copy_trimmed(tmp, sizeof tmp, val, vlen) && tmp[0];
                    if (have_name) {
                        size_t n = strlen(tmp);
                        if (n > LOGSRC_NAME_MAX) n = LOGSRC_NAME_MAX;
                        for (size_t i = 0; i < n; i++)
                            name[i] = (char)toupper((unsigned char)tmp[i]);
                        name[n] = '\0';
                    }
                } else if (strcmp(key, "path") == 0) {
                    have_path = copy_trimmed(path, sizeof path, val, vlen) && path[0] == '/';
                }
            }
        }
        p = eol ? eol + 1 : NULL;
    }
    if (!have_name || !have_path) return 0;
    strcpy(out->name, name);
    strcpy(out->path, path);
    return 1;
}

static int cmp_names(const void *a, const void *b) { return strcmp((const char *)a, (const char *)b); }

int logsrc_scan(const char *dir, LogSource *out, int max) {
    static char names[LOGSRC_FILES_MAX][LOGSRC_FNAME_MAX];
    int nn = 0, kept = 0;
    DIR *d = opendir(dir);
    if (!d) return 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && nn < LOGSRC_FILES_MAX) {
        size_t n = strlen(e->d_name);
        if (n <= 5 || n >= LOGSRC_FNAME_MAX || strcmp(e->d_name + n - 5, ".item") != 0) continue;
        memcpy(names[nn++], e->d_name, n + 1);
    }
    closedir(d);
    qsort(names, (size_t)nn, LOGSRC_FNAME_MAX, cmp_names);

    for (int i = 0; i < nn && kept < max; i++) {
        char path[256], text[LOGSRC_ITEM_MAX + 1];
        snprintf(path, sizeof path, "%s/%s", dir, names[i]);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        size_t got = fread(text, 1, LOGSRC_ITEM_MAX, f);
        fclose(f);
        text[got] = '\0';
        if (logsrc_parse(text, &out[kept])) kept++;
    }
    return kept;
}
