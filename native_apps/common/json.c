/*
 * json.c — the JSON tokenizer behind json.h.  See that header for the contract;
 * the comments here carry only reasons local to the code.
 *
 * Recursive descent, depth capped at JSON_MAX_DEPTH, so the stack use is a fixed
 * handful of frames whatever the input says.  Every read of the text is guarded
 * by `pos < len` first: a document is not NUL-terminated.
 */

#include "json.h"

#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *s;
    int         len;
    int         pos;
    JsonTok    *toks;
    int         max;
    int         count;   /* tokens handed out; never above max */
    bool        count_only;   /* json_count(): every write goes to `scratch` */
    JsonTok     scratch;
} Parser;

static void skip_ws(Parser *p)
{
    while (p->pos < p->len) {
        char c = p->s[p->pos];
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') break;
        p->pos++;
    }
}

/* The one place a token is written.  In count-only mode every write lands in a
 * scratch slot, so the grammar runs unchanged and nothing is stored. */
static JsonTok *tok_at(Parser *p, int idx)
{
    return p->count_only ? &p->scratch : &p->toks[idx];
}

/* Hand out the next token slot.  The only allocator of slots, and it refuses at
 * `max`, which is what keeps every store below max_toks. */
static int tok_new(Parser *p, JsonType type, int start)
{
    if (!p->count_only && p->count >= p->max) return JSON_ERR_NOMEM;
    JsonTok *t = tok_at(p, p->count);
    t->type = type;
    t->start = start;
    t->end = start;
    t->size = 0;
    t->next = p->count + 1;
    return p->count++;
}

static bool is_digit(char c) { return c >= '0' && c <= '9'; }

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int parse_value(Parser *p, int depth);

static int parse_string(Parser *p)
{
    int start = p->pos + 1;   /* caller saw the opening quote */
    int i = start;
    for (;;) {
        if (i >= p->len) return JSON_ERR_PART;
        unsigned char c = (unsigned char)p->s[i];
        if (c == '"') break;
        if (c < 0x20) return JSON_ERR_INVAL;
        if (c != '\\') { i++; continue; }
        i++;
        if (i >= p->len) return JSON_ERR_PART;
        switch (p->s[i]) {
        case '"': case '\\': case '/': case 'b': case 'f': case 'n': case 'r': case 't':
            i++;
            break;
        case 'u':
            for (int k = 1; k <= 4; k++) {
                if (i + k >= p->len) return JSON_ERR_PART;
                if (hex_val(p->s[i + k]) < 0) return JSON_ERR_INVAL;
            }
            i += 5;
            break;
        default:
            return JSON_ERR_INVAL;
        }
    }
    int idx = tok_new(p, JSON_STRING, start);
    if (idx < 0) return idx;
    tok_at(p, idx)->end = i;
    p->pos = i + 1;
    return idx;
}

static int parse_number(Parser *p)
{
    int start = p->pos, i = start;
    if (p->s[i] == '-') {
        i++;
        if (i >= p->len) return JSON_ERR_PART;
    }
    if (p->s[i] == '0') {
        i++;   /* a following digit is left for the caller to reject */
    } else if (p->s[i] >= '1' && p->s[i] <= '9') {
        while (i < p->len && is_digit(p->s[i])) i++;
    } else {
        return JSON_ERR_INVAL;
    }
    if (i < p->len && p->s[i] == '.') {
        i++;
        if (i >= p->len) return JSON_ERR_PART;
        if (!is_digit(p->s[i])) return JSON_ERR_INVAL;
        while (i < p->len && is_digit(p->s[i])) i++;
    }
    if (i < p->len && (p->s[i] == 'e' || p->s[i] == 'E')) {
        i++;
        if (i >= p->len) return JSON_ERR_PART;
        if (p->s[i] == '+' || p->s[i] == '-') {
            i++;
            if (i >= p->len) return JSON_ERR_PART;
        }
        if (!is_digit(p->s[i])) return JSON_ERR_INVAL;
        while (i < p->len && is_digit(p->s[i])) i++;
    }
    int idx = tok_new(p, JSON_NUMBER, start);
    if (idx < 0) return idx;
    tok_at(p, idx)->end = i;
    p->pos = i;
    return idx;
}

static int parse_literal(Parser *p, const char *lit, JsonType type)
{
    int n = (int)strlen(lit);
    int avail = p->len - p->pos;
    int cmp = avail < n ? avail : n;
    if (memcmp(p->s + p->pos, lit, (size_t)cmp) != 0) return JSON_ERR_INVAL;
    if (avail < n) return JSON_ERR_PART;
    int idx = tok_new(p, type, p->pos);
    if (idx < 0) return idx;
    tok_at(p, idx)->end = p->pos + n;
    p->pos += n;
    return idx;
}

static int parse_array(Parser *p, int depth)
{
    if (depth >= JSON_MAX_DEPTH) return JSON_ERR_DEPTH;
    int idx = tok_new(p, JSON_ARRAY, p->pos);
    if (idx < 0) return idx;
    int size = 0;
    p->pos++;
    skip_ws(p);
    if (p->pos >= p->len) return JSON_ERR_PART;
    if (p->s[p->pos] == ']') {
        p->pos++;
    } else {
        for (;;) {
            int r = parse_value(p, depth + 1);
            if (r < 0) return r;
            size++;
            skip_ws(p);
            if (p->pos >= p->len) return JSON_ERR_PART;
            char c = p->s[p->pos++];
            if (c == ']') break;
            if (c != ',') return JSON_ERR_INVAL;
            skip_ws(p);
        }
    }
    tok_at(p, idx)->end = p->pos;
    tok_at(p, idx)->size = size;
    tok_at(p, idx)->next = p->count;
    return idx;
}

static int parse_object(Parser *p, int depth)
{
    if (depth >= JSON_MAX_DEPTH) return JSON_ERR_DEPTH;
    int idx = tok_new(p, JSON_OBJECT, p->pos);
    if (idx < 0) return idx;
    int size = 0;
    p->pos++;
    skip_ws(p);
    if (p->pos >= p->len) return JSON_ERR_PART;
    if (p->s[p->pos] == '}') {
        p->pos++;
    } else {
        for (;;) {
            if (p->pos >= p->len) return JSON_ERR_PART;
            if (p->s[p->pos] != '"') return JSON_ERR_INVAL;   /* keys are strings */
            int r = parse_string(p);
            if (r < 0) return r;
            skip_ws(p);
            if (p->pos >= p->len) return JSON_ERR_PART;
            if (p->s[p->pos] != ':') return JSON_ERR_INVAL;
            p->pos++;
            skip_ws(p);
            r = parse_value(p, depth + 1);
            if (r < 0) return r;
            size++;
            skip_ws(p);
            if (p->pos >= p->len) return JSON_ERR_PART;
            char c = p->s[p->pos++];
            if (c == '}') break;
            if (c != ',') return JSON_ERR_INVAL;
            skip_ws(p);
        }
    }
    tok_at(p, idx)->end = p->pos;
    tok_at(p, idx)->size = size;
    tok_at(p, idx)->next = p->count;
    return idx;
}

/* `depth` is the number of containers already open around this value. */
static int parse_value(Parser *p, int depth)
{
    if (p->pos >= p->len) return JSON_ERR_PART;
    char c = p->s[p->pos];
    switch (c) {
    case '{': return parse_object(p, depth);
    case '[': return parse_array(p, depth);
    case '"': return parse_string(p);
    case 't': return parse_literal(p, "true", JSON_BOOL);
    case 'f': return parse_literal(p, "false", JSON_BOOL);
    case 'n': return parse_literal(p, "null", JSON_NULL);
    default:
        if (c == '-' || is_digit(c)) return parse_number(p);
        return JSON_ERR_INVAL;
    }
}

static int parse_doc(const char *s, size_t len, JsonTok *toks, int max_toks, bool count_only)
{
    if (!s || len > (size_t)INT_MAX || max_toks < 0 || (max_toks > 0 && !toks))
        return JSON_ERR_INVAL;
    Parser p = { s, (int)len, 0, toks, max_toks, 0, count_only, { 0 } };
    skip_ws(&p);
    int r = parse_value(&p, 0);
    if (r < 0) return r;
    skip_ws(&p);
    if (p.pos < p.len) return JSON_ERR_INVAL;   /* exactly one top-level value */
    return p.count;
}

int json_parse(const char *s, size_t len, JsonTok *toks, int max_toks)
{
    return parse_doc(s, len, toks, max_toks, false);
}

int json_count(const char *s, size_t len)
{
    return parse_doc(s, len, NULL, 0, true);
}

/* ── lookup and iteration ───────────────────────────────────────────────── */

static bool tok_ok(const JsonTok *t, int n, int i)
{
    return t && i >= 0 && i < n;
}

int json_obj_get(const char *s, const JsonTok *t, int n, int obj, const char *key)
{
    if (!s || !key || !tok_ok(t, n, obj) || t[obj].type != JSON_OBJECT) return -1;
    size_t klen = strlen(key);
    int i = obj + 1;
    for (int k = 0; k < t[obj].size; k++) {
        if (i + 1 >= n) return -1;   /* key and value must both exist */
        if (t[i].type == JSON_STRING && (size_t)(t[i].end - t[i].start) == klen &&
            memcmp(s + t[i].start, key, klen) == 0)
            return i + 1;
        if (t[i + 1].next <= i + 1) return -1;   /* a hand-made array that does not advance */
        i = t[i + 1].next;
    }
    return -1;
}

int json_arr_first(const JsonTok *t, int n, int arr)
{
    if (!tok_ok(t, n, arr) || t[arr].type != JSON_ARRAY) return -1;
    if (t[arr].size <= 0 || arr + 1 >= n) return -1;
    return arr + 1;
}

int json_arr_next(const JsonTok *t, int n, int arr, int elem)
{
    if (!tok_ok(t, n, arr) || t[arr].type != JSON_ARRAY || !tok_ok(t, n, elem)) return -1;
    if (elem <= arr) return -1;
    int nx = t[elem].next;
    if (nx <= elem || nx >= n || nx >= t[arr].next) return -1;
    return nx;
}

int json_arr_get(const JsonTok *t, int n, int arr, int i)
{
    if (i < 0) return -1;
    int e = json_arr_first(t, n, arr);
    while (e >= 0 && i > 0) {
        e = json_arr_next(t, n, arr, e);
        i--;
    }
    return e;
}

/* ── values ─────────────────────────────────────────────────────────────── */

typedef struct {
    char  *out;
    size_t size;      /* capacity including the NUL slot; 0 = measure only */
    size_t written;   /* bytes stored */
    size_t total;     /* bytes the whole string needs */
    bool   stopped;   /* a character did not fit: nothing more is stored */
} Sink;

static void sink_put(Sink *k, const unsigned char *b, size_t n)
{
    k->total += n;
    if (k->stopped) return;
    if (!k->out || k->size == 0 || k->written + n >= k->size) {   /* >= keeps room for the NUL */
        k->stopped = true;
        return;
    }
    memcpy(k->out + k->written, b, n);
    k->written += n;
}

static void sink_cp(Sink *k, unsigned long cp)
{
    unsigned char b[4];
    size_t n;
    if (cp < 0x80) {
        b[0] = (unsigned char)cp; n = 1;
    } else if (cp < 0x800) {
        b[0] = (unsigned char)(0xC0 | (cp >> 6));
        b[1] = (unsigned char)(0x80 | (cp & 0x3F)); n = 2;
    } else if (cp < 0x10000) {
        b[0] = (unsigned char)(0xE0 | (cp >> 12));
        b[1] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
        b[2] = (unsigned char)(0x80 | (cp & 0x3F)); n = 3;
    } else {
        b[0] = (unsigned char)(0xF0 | (cp >> 18));
        b[1] = (unsigned char)(0x80 | ((cp >> 12) & 0x3F));
        b[2] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
        b[3] = (unsigned char)(0x80 | (cp & 0x3F)); n = 4;
    }
    sink_put(k, b, n);
}

/* Four hex digits at s[i..i+3] (inside the span), or -1. */
static long hex4(const char *s, int i, int end)
{
    if (i + 4 > end) return -1;
    long v = 0;
    for (int k = 0; k < 4; k++) {
        int h = hex_val(s[i + k]);
        if (h < 0) return -1;
        v = v * 16 + h;
    }
    return v;
}

int json_str(const char *s, const JsonTok *t, char *out, size_t out_size)
{
    if (out && out_size > 0) out[0] = '\0';
    if (!s || !t || t->type != JSON_STRING || t->start < 0 || t->end < t->start) return -1;
    Sink k = { out, out_size, 0, 0, false };
    int i = t->start, end = t->end;
    while (i < end) {
        unsigned char c = (unsigned char)s[i];
        if (c != '\\') {
            /* A raw multi-byte character is one unit, so a cut never splits it. */
            int want = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
            int n = 1;
            while (n < want && i + n < end && ((unsigned char)s[i + n] & 0xC0) == 0x80) n++;
            sink_put(&k, (const unsigned char *)s + i, (size_t)n);
            i += n;
            continue;
        }
        if (++i >= end) {   /* a lone backslash at the end of the span */
            if (out && out_size > 0) out[0] = '\0';
            return -1;
        }
        char e = s[i++];
        switch (e) {
        case '"':  sink_cp(&k, '"');  break;
        case '\\': sink_cp(&k, '\\'); break;
        case '/':  sink_cp(&k, '/');  break;
        case 'b':  sink_cp(&k, '\b'); break;
        case 'f':  sink_cp(&k, '\f'); break;
        case 'n':  sink_cp(&k, '\n'); break;
        case 'r':  sink_cp(&k, '\r'); break;
        case 't':  sink_cp(&k, '\t'); break;
        case 'u': {
            long cp = hex4(s, i, end);
            if (cp < 0) { if (out && out_size > 0) out[0] = '\0'; return -1; }
            i += 4;
            if (cp >= 0xD800 && cp <= 0xDBFF) {
                long lo = (i + 1 < end && s[i] == '\\' && s[i + 1] == 'u') ? hex4(s, i + 2, end) : -1;
                if (lo >= 0xDC00 && lo <= 0xDFFF) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    i += 6;
                } else {
                    cp = 0xFFFD;   /* a high surrogate with no low one after it */
                }
            } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                cp = 0xFFFD;
            } else if (cp == 0) {
                cp = 0xFFFD;       /* a NUL would end the C string early */
            }
            sink_cp(&k, (unsigned long)cp);
            break;
        }
        default:
            if (out && out_size > 0) out[0] = '\0';
            return -1;
        }
    }
    if (out && out_size > 0) out[k.written] = '\0';
    return k.total > (size_t)INT_MAX ? INT_MAX : (int)k.total;
}

bool json_num(const char *s, const JsonTok *t, double *out)
{
    if (!s || !t || !out || t->type != JSON_NUMBER) return false;
    int n = t->end - t->start;
    if (n <= 0 || n > 63) return false;
    char buf[64];
    memcpy(buf, s + t->start, (size_t)n);
    buf[n] = '\0';
    char *endp = NULL;
    double v = strtod(buf, &endp);
    if (endp != buf + n || !isfinite(v)) return false;   /* 1e999 overflows to inf */
    *out = v;
    return true;
}

bool json_i64(const char *s, const JsonTok *t, long long *out)
{
    if (!s || !t || !out || t->type != JSON_NUMBER) return false;
    int i = t->start, end = t->end;
    if (i >= end) return false;
    bool neg = false;
    if (s[i] == '-') { neg = true; i++; }
    if (i >= end) return false;
    /* One more than LLONG_MAX is a legal magnitude only when negative. */
    unsigned long long limit = neg ? (unsigned long long)LLONG_MAX + 1ULL
                                   : (unsigned long long)LLONG_MAX;
    unsigned long long acc = 0;
    for (; i < end; i++) {
        if (!is_digit(s[i])) return false;   /* '.', 'e' and 'E' land here */
        unsigned d = (unsigned)(s[i] - '0');
        if (acc > (limit - d) / 10ULL) return false;
        acc = acc * 10ULL + d;
    }
    *out = neg ? (long long)(0ULL - acc) : (long long)acc;
    return true;
}

bool json_bool(const char *s, const JsonTok *t, bool *out)
{
    if (!s || !t || !out || t->type != JSON_BOOL || t->end <= t->start) return false;
    *out = s[t->start] == 't';
    return true;
}

bool json_is_null(const JsonTok *t)
{
    return t && t->type == JSON_NULL;
}
