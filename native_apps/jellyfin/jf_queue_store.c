/* jf_queue_store.c — see jf_queue_store.h. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "config.h"
#include "jf_queue_store.h"
#include "jf_url.h"

#define MAGIC "ARCA-JF-QUEUE 1"
#define NSTR 8
#define NINT 4
#define NFIELDS (NSTR + NINT + 1)
#define LINE_MAX_BYTES 1536

typedef struct { char *p; size_t len, cap; bool oom; } Buf;

static void put(Buf *b, const char *s, size_t n)
{
    if (b->oom) return;
    if (b->len + n + 1 > b->cap) {
        size_t nc = b->cap ? b->cap : 4096;
        while (nc < b->len + n + 1) nc *= 2;
        char *q = realloc(b->p, nc);
        if (!q) { b->oom = true; return; }
        b->p = q;
        b->cap = nc;
    }
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = '\0';
}

static void put_str(Buf *b, const char *s)
{
    for (; *s; s++) {
        switch (*s) {
        case '\\': put(b, "\\\\", 2); break;
        case '\t': put(b, "\\t", 2); break;
        case '\n': put(b, "\\n", 2); break;
        case '\r': put(b, "\\r", 2); break;
        default: put(b, s, 1); break;
        }
    }
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

char *jf_queue_serialize(const JfItem *items, int n, int cur, int pos_ms, size_t *len)
{
    Buf b = { NULL, 0, 0, false };
    char num[64];
    if (n < 0) n = 0;
    cur = n > 0 ? clampi(cur, 0, n - 1) : 0;
    if (pos_ms < 0) pos_ms = 0;
    put(&b, MAGIC "\n", sizeof MAGIC);
    snprintf(num, sizeof num, "cur\t%d\t%d\n", cur, pos_ms);
    put(&b, num, strlen(num));
    for (int i = 0; i < n; i++) {
        const JfItem *it = &items[i];
        const char *s[NSTR] = { it->id, it->name, it->artist, it->album_artist, it->album, it->album_id, it->image_tag, it->artist_id };
        for (int k = 0; k < NSTR; k++) { put_str(&b, s[k]); put(&b, "\t", 1); }
        snprintf(num, sizeof num, "%d\t%d\t%d\t%d\tE\n", it->runtime_s, it->norm_gain_mdb, it->year, it->index);
        put(&b, num, strlen(num));
    }
    if (b.oom) { free(b.p); return NULL; }
    if (len) *len = b.len;
    return b.p;
}

/* Cut `line` at its TABs into f[]; returns the field count, stopping at max+1 (a surplus reads as max+1). */
static int split_tabs(char *line, char *f[], int max)
{
    int c = 0;
    char *p = line;
    for (;;) {
        if (c >= max + 1) return c;
        f[c++] = p;
        char *t = strchr(p, '\t');
        if (!t) return c;
        *t = '\0';
        p = t + 1;
    }
}

/* Unescape in place; false on a malformed escape. */
static bool unescape(char *s)
{
    char *w = s;
    for (const char *r = s; *r; r++) {
        if (*r != '\\') { *w++ = *r; continue; }
        r++;
        switch (*r) {
        case '\\': *w++ = '\\'; break;
        case 't': *w++ = '\t'; break;
        case 'n': *w++ = '\n'; break;
        case 'r': *w++ = '\r'; break;
        default: return false;
        }
    }
    *w = '\0';
    return true;
}

static bool fits(const char *s, size_t cap) { return strlen(s) < cap; }

static bool parse_int(const char *s, int *out)
{
    char *end;
    long v = strtol(s, &end, 10);
    if (end == s || *end || v < -1000000000L || v > 1000000000L) return false;
    *out = (int)v;
    return true;
}

static bool parse_item(char *line, JfItem *it)
{
    char *f[NFIELDS + 1];
    if (split_tabs(line, f, NFIELDS) != NFIELDS || strcmp(f[NFIELDS - 1], "E") != 0) return false;
    for (int k = 0; k < NSTR; k++) if (!unescape(f[k])) return false;
    if (!jf_id_valid(f[0]) || !f[1][0]) return false;
    if (!fits(f[1], sizeof it->name) || !fits(f[2], sizeof it->artist) || !fits(f[3], sizeof it->album_artist) ||
        !fits(f[4], sizeof it->album) || !fits(f[5], sizeof it->album_id) || !fits(f[6], sizeof it->image_tag) ||
        !fits(f[7], sizeof it->artist_id)) return false;
    int rt, ng, yr, ix;
    if (!parse_int(f[NSTR], &rt) || !parse_int(f[NSTR + 1], &ng) || !parse_int(f[NSTR + 2], &yr) || !parse_int(f[NSTR + 3], &ix)) return false;
    memset(it, 0, sizeof *it);
    snprintf(it->id, sizeof it->id, "%s", f[0]);
    snprintf(it->name, sizeof it->name, "%s", f[1]);
    snprintf(it->artist, sizeof it->artist, "%s", f[2]);
    snprintf(it->album_artist, sizeof it->album_artist, "%s", f[3]);
    snprintf(it->album, sizeof it->album, "%s", f[4]);
    snprintf(it->album_id, sizeof it->album_id, "%s", f[5]);
    snprintf(it->image_tag, sizeof it->image_tag, "%s", f[6]);
    snprintf(it->artist_id, sizeof it->artist_id, "%s", f[7]);
    it->type = JF_ITEM_TRACK;
    it->runtime_s = rt < 0 ? 0 : rt;
    it->norm_gain_mdb = ng;
    it->year = yr < 0 ? 0 : yr;
    it->index = ix < 0 ? 0 : ix;
    return true;
}

int jf_queue_parse(const char *buf, size_t len, JfItem *items, int cap, int *cur, int *pos_ms)
{
    *cur = 0;
    *pos_ms = 0;
    size_t ml = strlen(MAGIC);
    if (len < ml + 1 || memcmp(buf, MAGIC, ml) != 0 || buf[ml] != '\n') return -1;
    int n = 0, want_cur = 0, want_pos = 0;
    size_t i = ml + 1;
    bool first = true;
    while (i < len) {
        size_t e = i;
        while (e < len && buf[e] != '\n') e++;
        bool whole = e < len;                 /* a last line with no LF was cut short */
        size_t ll = e - i;
        char line[LINE_MAX_BYTES];
        if (whole && ll < sizeof line) {
            memcpy(line, buf + i, ll);
            line[ll] = '\0';
            if (first) {
                char *f[4];
                if (split_tabs(line, f, 3) == 3 && strcmp(f[0], "cur") == 0) {
                    if (!parse_int(f[1], &want_cur)) want_cur = 0;
                    if (!parse_int(f[2], &want_pos)) want_pos = 0;
                }
            } else if (n < cap) {
                JfItem it;
                if (parse_item(line, &it)) items[n++] = it;
            }
        }
        first = false;
        i = e + 1;
    }
    *cur = n > 0 ? clampi(want_cur, 0, n - 1) : 0;
    *pos_ms = want_pos < 0 ? 0 : want_pos;
    return n;
}

static void mkdir_parents(const char *path)
{
    char d[160];
    snprintf(d, sizeof d, "%s", path);
    for (char *p = d + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        mkdir(d, 0755);
        *p = '/';
    }
}

int jf_queue_save(const char *path, const JfItem *items, int n, int cur, int pos_ms)
{
    size_t len = 0;
    char *text = jf_queue_serialize(items, n, cur, pos_ms, &len);
    if (!text) return -1;
    mkdir_parents(path);
    char tmp[160];
    FILE *f = file_write_atomic_open(path, tmp, sizeof tmp);
    if (!f) { free(text); return -1; }
    if (fwrite(text, 1, len, f) != len) { file_write_atomic_abort(f, tmp); free(text); return -1; }
    free(text);
    return file_write_atomic_commit(f, tmp, path) == 0 ? 0 : -1;
}

int jf_queue_load(const char *path, JfItem *items, int cap, int *cur, int *pos_ms)
{
    *cur = *pos_ms = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long sz = ftell(f);
    if (sz < 0 || sz > 4L * 1024 * 1024 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return -1; }
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return -1; }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    int n = jf_queue_parse(buf, rd, items, cap, cur, pos_ms);
    free(buf);
    return n;
}
