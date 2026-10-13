/*
 * product.c — the product name from /etc/os-release.  See product.h.
 */
#include "product.h"

#include <stdio.h>
#include <string.h>

#define PRODUCT_OS_RELEASE "/etc/os-release"

const char *product_name_from(const char *path) {
    static char name[64];
    char line[256];
    FILE *f = path ? fopen(path, "r") : NULL;

    snprintf(name, sizeof(name), "%s", PRODUCT_NAME_FALLBACK);
    if (!f) return name;

    while (fgets(line, sizeof(line), f)) {
        /* Exactly "NAME=": PRETTY_NAME= and a commented-out line do not match. */
        if (strncmp(line, "NAME=", 5) != 0) continue;

        char *v = line + 5;
        size_t n = strlen(v);
        while (n > 0 && (v[n - 1] == '\n' || v[n - 1] == '\r' ||
                         v[n - 1] == ' '  || v[n - 1] == '\t'))
            v[--n] = '\0';
        if (n >= 2 && (v[0] == '"' || v[0] == '\'') && v[n - 1] == v[0]) {
            v[n - 1] = '\0';
            v++;
        }
        if (*v) snprintf(name, sizeof(name), "%s", v);
        break;                                /* the first NAME= decides */
    }
    fclose(f);
    return name;
}

const char *product_name(void) {
    static char cached[64];
    static int have;
    if (!have) {
        snprintf(cached, sizeof(cached), "%s", product_name_from(PRODUCT_OS_RELEASE));
        have = 1;
    }
    return cached;
}
