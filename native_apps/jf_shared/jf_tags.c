/*
 * jf_tags.c — see jf_tags.h.  ID3v2.3/2.4 and FLAC Vorbis comments, bounded everywhere: every length read from the file is
 * checked against the bytes in hand before it is used.
 */

#include "jf_tags.h"

#include <ctype.h>
#include <string.h>

typedef unsigned char uchar;

/* ── small helpers ─────────────────────────────────────────────────────── */

static int ci_eq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++)
        if (tolower((uchar)*a) != tolower((uchar)*b)) return 0;
    return *a == *b;
}

/* Copy `s` into dst[cap], trimmed, cut at a UTF-8 boundary. */
static void set_str(char *dst, size_t cap, const char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n')) n--;
    if (n >= cap) {
        n = cap - 1;
        while (n && ((uchar)s[n] & 0xC0) == 0x80) n--;      /* do not end inside a multi-byte sequence */
    }
    memcpy(dst, s, n);
    dst[n] = 0;
}

/* Append ", s" (or s when dst is empty) while it fits whole. */
static void join_str(char *dst, size_t cap, const char *s)
{
    char t[JF_TAGS_NAME_MAX];
    set_str(t, sizeof t, s);
    if (!t[0]) return;
    size_t d = strlen(dst);
    if (d == 0) { set_str(dst, cap, t); return; }
    if (d + 2 + strlen(t) >= cap) return;
    memcpy(dst + d, ", ", 2);
    strcpy(dst + d + 2, t);
}

static int year_of(const char *s)
{
    while (*s == ' ') s++;
    if (!isdigit((uchar)s[0]) || !isdigit((uchar)s[1]) || !isdigit((uchar)s[2]) || !isdigit((uchar)s[3])) return 0;
    int y = (s[0] - '0') * 1000 + (s[1] - '0') * 100 + (s[2] - '0') * 10 + (s[3] - '0');
    return y >= 1000 ? y : 0;
}

static void add_genre(JfTags *t, const char *g)
{
    char v[40];
    set_str(v, sizeof v, g);
    if (!v[0] || t->genre_n >= JF_TAGS_GENRE_MAX) return;
    for (int i = 0; i < t->genre_n; i++) if (ci_eq(t->genre[i], v)) return;
    memcpy(t->genre[t->genre_n++], v, sizeof v);
}

static void add_perf(JfTags *t, const char *name, const char *role)
{
    if (t->perf_n >= JF_TAGS_PERF_MAX) return;
    char n[64], r[32];
    set_str(n, sizeof n, name);
    set_str(r, sizeof r, role);
    if (!n[0]) return;
    for (int i = 0; i < t->perf_n; i++)
        if (ci_eq(t->perf[i].name, n) && ci_eq(t->perf[i].role, r)) return;
    memcpy(t->perf[t->perf_n].name, n, sizeof n);
    memcpy(t->perf[t->perf_n].role, r, sizeof r);
    t->perf_n++;
}

/* A genre value of ID3: "(17)Rock" -> "Rock"; "(17)" and "17" are v1 numbers (not named here) -> "". */
static const char *genre_text(const char *g)
{
    while (*g == '(') {
        const char *e = strchr(g, ')');
        if (!e) return "";
        g = e + 1;
    }
    const char *p = g;
    while (isdigit((uchar)*p)) p++;
    if (p > g && *p == 0) return "";
    return g;
}

/* ── text decoding to UTF-8; NUL separators between values are kept ─────── */

static size_t put8(char *out, size_t cap, size_t o, unsigned cp)
{
    if (cp < 0x80) { if (o + 1 <= cap) out[o++] = (char)cp; }
    else if (cp < 0x800) { if (o + 2 <= cap) { out[o++] = (char)(0xC0 | (cp >> 6)); out[o++] = (char)(0x80 | (cp & 0x3F)); } }
    else if (cp < 0x10000) {
        if (o + 3 <= cap) { out[o++] = (char)(0xE0 | (cp >> 12)); out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[o++] = (char)(0x80 | (cp & 0x3F)); }
    } else {
        if (o + 4 <= cap) {
            out[o++] = (char)(0xF0 | (cp >> 18)); out[o++] = (char)(0x80 | ((cp >> 12) & 0x3F));
            out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[o++] = (char)(0x80 | (cp & 0x3F));
        }
    }
    return o;
}

/* enc: 0 Latin-1, 1 UTF-16 with BOM, 2 UTF-16BE, 3 UTF-8.  Output is NUL-terminated at out[ret]; ret < cap. */
static size_t to_utf8(int enc, const uchar *p, size_t n, char *out, size_t cap)
{
    size_t o = 0, lim = cap - 1;
    if (enc == 0) {
        for (size_t i = 0; i < n && o < lim; i++) o = put8(out, lim, o, p[i]);
    } else if (enc == 3) {
        for (size_t i = 0; i < n && o < lim; i++) out[o++] = (char)p[i];
    } else {
        bool be = enc == 2;
        for (size_t i = 0; i + 1 < n && o < lim; i += 2) {
            unsigned u = be ? (unsigned)(p[i] << 8 | p[i + 1]) : (unsigned)(p[i + 1] << 8 | p[i]);
            if (u == 0xFEFF) continue;                         /* BOM in the order we already use */
            if (u == 0xFFFE) { be = !be; continue; }           /* BOM in the other order */
            if (u >= 0xD800 && u < 0xDC00 && i + 3 < n) {
                unsigned v = be ? (unsigned)(p[i + 2] << 8 | p[i + 3]) : (unsigned)(p[i + 3] << 8 | p[i + 2]);
                if (v >= 0xDC00 && v < 0xE000) { u = 0x10000 + ((u - 0xD800) << 10) + (v - 0xDC00); i += 2; }
                else u = '?';
            } else if (u >= 0xD800 && u < 0xE000) u = '?';
            o = put8(out, lim, o, u);
        }
    }
    out[o] = 0;
    return o;
}

/* Split the converted text at its NULs into at most `max` values; returns the count (a trailing empty value is dropped). */
static int split_vals(char *buf, size_t len, const char **v, int max)
{
    int n = 0;
    size_t i = 0;
    while (i <= len && n < max) {
        v[n++] = buf + i;
        i += strlen(buf + i) + 1;
    }
    if (n > 0 && v[n - 1][0] == 0) n--;
    return n;
}

/* ── ID3v2 ─────────────────────────────────────────────────────────────── */

static unsigned syncsafe(const uchar *p) { return (unsigned)(p[0] & 0x7F) << 21 | (unsigned)(p[1] & 0x7F) << 14 | (unsigned)(p[2] & 0x7F) << 7 | (p[3] & 0x7F); }
static unsigned be32(const uchar *p) { return (unsigned)p[0] << 24 | (unsigned)p[1] << 16 | (unsigned)p[2] << 8 | p[3]; }

static void id3_frame(JfTags *t, const char *id, const uchar *d, size_t n)
{
    if (n < 2) return;
    bool txxx = strcmp(id, "TXXX") == 0;
    if (id[0] != 'T') return;
    int enc = d[0];
    if (enc > 3) return;
    char buf[640];
    size_t len = to_utf8(enc, d + 1, n - 1, buf, sizeof buf);
    const char *v[16];
    int nv = split_vals(buf, len, v, 16);
    if (nv == 0) return;

    if (txxx) {
        if (nv < 2) return;
        if (ci_eq(v[0], "WORK")) set_str(t->work, sizeof t->work, v[1]);
        else if (ci_eq(v[0], "CONDUCTOR") && !t->conductor[0]) set_str(t->conductor, sizeof t->conductor, v[1]);
        else if (ci_eq(v[0], "PERFORMER")) add_perf(t, v[1], "");
        return;
    }
    if (strcmp(id, "TPE1") == 0 && !t->artist[0]) { for (int i = 0; i < nv; i++) join_str(t->artist, sizeof t->artist, v[i]); }
    else if (strcmp(id, "TPE2") == 0 && !t->album_artist[0]) { for (int i = 0; i < nv; i++) join_str(t->album_artist, sizeof t->album_artist, v[i]); }
    else if (strcmp(id, "TPE3") == 0 && !t->conductor[0]) { for (int i = 0; i < nv; i++) join_str(t->conductor, sizeof t->conductor, v[i]); }
    else if (strcmp(id, "TIT1") == 0 && !t->work[0]) set_str(t->work, sizeof t->work, v[0]);
    else if ((strcmp(id, "TDRC") == 0 || strcmp(id, "TYER") == 0) && !t->year) t->year = year_of(v[0]);
    else if (strcmp(id, "TCON") == 0) { for (int i = 0; i < nv; i++) add_genre(t, genre_text(v[i])); }
    else if (strcmp(id, "TMCL") == 0) { for (int i = 0; i + 1 < nv; i += 2) add_perf(t, v[i + 1], v[i]); }
    else if (strcmp(id, "TIPL") == 0) {
        for (int i = 0; i + 1 < nv; i += 2)
            if (ci_eq(v[i], "conductor") && !t->conductor[0]) set_str(t->conductor, sizeof t->conductor, v[i + 1]);
    }
}

static void parse_id3(const uchar *b, size_t len, JfTags *t)
{
    int ver = b[3];
    if (ver != 3 && ver != 4) return;                  /* v2.2 (3-letter frames) and anything newer: not covered */
    unsigned flags = b[5];
    size_t tag_end = 10 + (size_t)syncsafe(b + 6);
    size_t lim = tag_end < len ? tag_end : len;
    t->have = true;
    if (tag_end > len) { t->truncated = true; t->need = tag_end; }
    size_t pos = 10;
    if (flags & 0x40) {                                 /* extended header: skip it */
        if (pos + 4 > lim) return;
        size_t xs = ver == 4 ? syncsafe(b + pos) : (size_t)be32(b + pos) + 4;
        pos += xs;
    }
    while (pos + 10 <= lim) {
        if (b[pos] == 0) break;                         /* padding */
        char id[5];
        memcpy(id, b + pos, 4);
        id[4] = 0;
        size_t fs = ver == 4 ? syncsafe(b + pos + 4) : (size_t)be32(b + pos + 4);
        unsigned f1 = b[pos + 8], f2 = b[pos + 9];
        size_t body = pos + 10;
        if (fs > lim - body) break;                     /* cut by the range (or a bad size): nothing past it is read */
        bool skip = ver == 4 ? (f2 & 0x0C) != 0 : (f1 & 0xC0) != 0;   /* compressed or encrypted */
        size_t o = 0;
        if (ver == 4 && (f2 & 0x01)) o = 4;             /* data length indicator */
        if (!skip && fs >= o) id3_frame(t, id, b + body + o, fs - o);
        pos = body + fs;
    }
}

/* ── FLAC / Vorbis comments ─────────────────────────────────────────────── */

static unsigned le32(const uchar *p) { return (unsigned)p[0] | (unsigned)p[1] << 8 | (unsigned)p[2] << 16 | (unsigned)p[3] << 24; }

static void vorbis_entry(JfTags *t, const char *kv)
{
    const char *eq = strchr(kv, '=');
    if (!eq || eq == kv || eq - kv > 24) return;
    char k[32];
    memcpy(k, kv, (size_t)(eq - kv));
    k[eq - kv] = 0;
    const char *v = eq + 1;
    if (ci_eq(k, "ARTIST")) join_str(t->artist, sizeof t->artist, v);
    else if (ci_eq(k, "ALBUMARTIST") || ci_eq(k, "ALBUM ARTIST")) join_str(t->album_artist, sizeof t->album_artist, v);
    else if (ci_eq(k, "CONDUCTOR")) join_str(t->conductor, sizeof t->conductor, v);
    else if (ci_eq(k, "WORK") && !t->work[0]) set_str(t->work, sizeof t->work, v);
    else if ((ci_eq(k, "DATE") || ci_eq(k, "YEAR")) && !t->year) t->year = year_of(v);
    else if (ci_eq(k, "GENRE")) add_genre(t, v);
    else if (ci_eq(k, "PERFORMER")) {                   /* "Name (role)" */
        char name[64], role[32] = "";
        set_str(name, sizeof name, v);
        size_t nl = strlen(name);
        if (nl > 2 && name[nl - 1] == ')') {
            char *open = strrchr(name, '(');
            if (open && open > name) {
                *(name + nl - 1) = 0;
                set_str(role, sizeof role, open + 1);
                *open = 0;
            }
        }
        add_perf(t, name, role);
    }
}

static void vorbis_block(JfTags *t, const uchar *p, size_t n)
{
    if (n < 4) return;
    size_t vl = le32(p);
    if (vl > n - 4 || n - 4 - vl < 4) return;
    size_t pos = 4 + vl;
    unsigned cnt = le32(p + pos);
    pos += 4;
    for (unsigned i = 0; i < cnt && i < 512; i++) {
        if (n - pos < 4) break;
        size_t el = le32(p + pos);
        pos += 4;
        if (el > n - pos) break;
        char e[512];
        size_t c = el < sizeof e - 1 ? el : sizeof e - 1;
        memcpy(e, p + pos, c);
        e[c] = 0;
        if (!memchr(e, 0, c)) vorbis_entry(t, e);
        pos += el;
    }
}

static void parse_flac(const uchar *b, size_t len, JfTags *t)
{
    t->have = true;
    size_t pos = 4;
    for (int guard = 0; guard < 64; guard++) {
        if (pos + 4 > len) { t->truncated = true; t->need = pos + 4; return; }
        unsigned hdr = b[pos];
        size_t bl = (size_t)b[pos + 1] << 16 | (size_t)b[pos + 2] << 8 | b[pos + 3];
        size_t body = pos + 4;
        bool last = (hdr & 0x80) != 0;
        if ((hdr & 0x7F) == 4) {
            if (bl > len - body) { t->truncated = true; t->need = body + bl; return; }
            vorbis_block(t, b + body, bl);
            return;
        }
        if (last) return;                               /* no comment block */
        pos = body + bl;
    }
}

/* ── entry points ──────────────────────────────────────────────────────── */

bool jf_tags_parse(const unsigned char *buf, size_t len, JfTags *out)
{
    memset(out, 0, sizeof *out);
    if (!buf) return false;
    if (len >= 10 && memcmp(buf, "ID3", 3) == 0) parse_id3(buf, len, out);
    else if (len >= 4 && memcmp(buf, "fLaC", 4) == 0) parse_flac(buf, len, out);
    return out->have;
}

bool jf_tags_show_album_artist(const char *artist, const char *album_artist)
{
    if (!album_artist || !album_artist[0]) return false;
    if (artist && ci_eq(artist, album_artist)) return false;
    return !ci_eq(album_artist, "Various Artists");
}
