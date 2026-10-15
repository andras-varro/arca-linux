/*
 * product.c — the product name and image version from /etc/os-release.  See product.h.
 */
#include "product.h"

#include <stdio.h>
#include <string.h>

#define PRODUCT_OS_RELEASE "/etc/os-release"

/* The value of the first line that starts exactly `key` (which carries its '='), copied
 * into out: one pair of matching quotes and trailing blanks / CR removed, truncated to
 * cap - 1.  Returns 1 when that line exists and its value is not empty, else 0 and out
 * is untouched.  Exactly the key: PRETTY_NAME= and a commented-out line do not match
 * "NAME=", and the first matching line decides even when it is empty. */
static int os_release_value(const char *path, const char *key, char *out, size_t cap) {
    char line[256];
    size_t klen = strlen(key);
    int found = 0;
    FILE *f = path ? fopen(path, "r") : NULL;

    if (!f) return 0;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, key, klen) != 0) continue;

        char *v = line + klen;
        size_t n = strlen(v);
        while (n > 0 && (v[n - 1] == '\n' || v[n - 1] == '\r' ||
                         v[n - 1] == ' '  || v[n - 1] == '\t'))
            v[--n] = '\0';
        if (n >= 2 && (v[0] == '"' || v[0] == '\'') && v[n - 1] == v[0]) {
            v[n - 1] = '\0';
            v++;
        }
        if (*v) { snprintf(out, cap, "%s", v); found = 1; }
        break;                                /* the first match decides */
    }
    fclose(f);
    return found;
}

const char *product_name_from(const char *path) {
    static char name[64];
    char v[sizeof(name)];

    snprintf(name, sizeof(name), "%s", PRODUCT_NAME_FALLBACK);
    if (os_release_value(path, "NAME=", v, sizeof(v)))
        snprintf(name, sizeof(name), "%s", v);
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

const char *product_version_from(const char *path) {
    static char version[64];

    if (!os_release_value(path, "VERSION_ID=", version, sizeof(version))) return NULL;
    return version;
}

const char *product_version(void) {
    static char cached[64];
    static int have, present;
    if (!have) {
        const char *v = product_version_from(PRODUCT_OS_RELEASE);
        if (v) { snprintf(cached, sizeof(cached), "%s", v); present = 1; }
        have = 1;
    }
    return present ? cached : NULL;
}
