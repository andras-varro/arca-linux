/* backup_parse.c — see backup_parse.h. */
#include "backup_parse.h"

#include <stdio.h>
#include <string.h>

bool bk_name_ok(const char *s) {
    if (!s || !*s || s[0] == '-' || s[0] == '.') return false;
    for (; *s; s++) {
        char c = *s;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-'))
            return false;
    }
    return true;
}

bool bk_is_disk_name(const char *name) {
    if (!name || name[0] != 's' || name[1] != 'd') return false;
    const char *p = name + 2;
    int letters = 0;
    while (*p >= 'a' && *p <= 'z') { p++; letters++; }
    if (letters < 1 || letters > 2) return false;
    while (*p >= '0' && *p <= '9') p++;
    return *p == '\0';
}

/* line starts with kw followed by one space; *rest is what follows. */
static bool keyword(const char *line, const char *kw, const char **rest) {
    size_t n = strlen(kw);
    if (!line || strncmp(line, kw, n) != 0 || line[n] != ' ') return false;
    *rest = line + n + 1;
    return true;
}

static void trim_eol(char *s) {
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ')) s[--n] = '\0';
}

/* One word of *p into out (n bytes); false when empty or too long.  Skips
 * leading spaces, leaves *p after the word. */
static bool word(const char **p, char *out, size_t n) {
    const char *s = *p;
    while (*s == ' ') s++;
    const char *e = s;
    while (*e && *e != ' ' && *e != '\n' && *e != '\r') e++;
    if (e == s || (size_t)(e - s) >= n) return false;
    memcpy(out, s, (size_t)(e - s));
    out[e - s] = '\0';
    *p = e;
    return true;
}

/* "<key>=yes|no" as the next word. */
static bool flag(const char **p, const char *key, bool *out) {
    char w[24];
    if (!word(p, w, sizeof w)) return false;
    size_t k = strlen(key);
    if (strncmp(w, key, k) != 0 || w[k] != '=') return false;
    if (!strcmp(w + k + 1, "yes")) *out = true;
    else if (!strcmp(w + k + 1, "no")) *out = false;
    else return false;
    return true;
}

bool bk_parse_item(const char *line, BkItem *out) {
    const char *p;
    BkItem it;
    memset(&it, 0, sizeof it);
    if (!keyword(line, "item", &p)) return false;
    if (!word(&p, it.id, sizeof it.id) || !bk_name_ok(it.id)) return false;
    if (!flag(&p, "unit", &it.unit) || !flag(&p, "secret", &it.secret) ||
        !flag(&p, "reboot", &it.reboot)) return false;
    while (*p == ' ') p++;
    strncpy(it.name, p, sizeof it.name - 1);
    trim_eol(it.name);
    if (!it.name[0]) snprintf(it.name, sizeof it.name, "%s", it.id);   /* never an empty row */
    *out = it;
    return true;
}

bool bk_parse_archive(const char *line, char *out, size_t n) {
    const char *p;
    if (!keyword(line, "archive", &p)) return false;
    char w[BK_FILE_MAX];
    if (!word(&p, w, sizeof w) || !bk_name_ok(w) || strlen(w) >= n) return false;
    strcpy(out, w);
    return true;
}

static bool kv(const char **p, const char *key, char *out, size_t n) {
    char w[BK_HOST_MAX + 16];
    if (!word(p, w, sizeof w)) return false;
    size_t k = strlen(key);
    if (strncmp(w, key, k) != 0 || w[k] != '=' || strlen(w + k + 1) >= n) return false;
    strcpy(out, w + k + 1);
    return true;
}

bool bk_parse_info(const char *line, char *host, size_t hn, char *version, size_t vn,
                   char *date, size_t dn) {
    const char *p;
    if (!keyword(line, "info", &p)) return false;
    return kv(&p, "host", host, hn) && kv(&p, "version", version, vn) &&
           kv(&p, "date", date, dn);
}

static bool one_id(const char *line, const char *kw, char *id, size_t n) {
    const char *p;
    char w[BK_ID_MAX];
    if (!keyword(line, kw, &p) || !word(&p, w, sizeof w) || !bk_name_ok(w) || strlen(w) >= n)
        return false;
    strcpy(id, w);
    return true;
}

bool bk_parse_unknown(const char *line, char *id, size_t n)  { return one_id(line, "unknown", id, n); }
bool bk_parse_imported(const char *line, char *id, size_t n) { return one_id(line, "imported", id, n); }

bool bk_parse_done(const char *line, bool *reboot) {
    const char *p;
    if (!keyword(line, "done", &p)) return false;
    return flag(&p, "reboot", reboot);
}

bool bk_last_error(const char *out, char *msg, size_t n) {
    const char *found = NULL;
    for (const char *l = out; l && *l; ) {
        if (!strncmp(l, "error ", 6)) found = l + 6;
        const char *nl = strchr(l, '\n');
        l = nl ? nl + 1 : NULL;
    }
    if (!found || n == 0) return false;
    size_t i = 0;
    while (found[i] && found[i] != '\n' && found[i] != '\r' && i < n - 1) { msg[i] = found[i]; i++; }
    msg[i] = '\0';
    return true;
}

bool bk_start_marked(const BkItem *it, bool foreign) {
    if (it->secret) return false;
    if (it->unit && foreign) return false;
    return true;
}
