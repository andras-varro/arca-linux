/*
 * vid_srt.c — see vid_srt.h.
 *
 * One pass over the bytes: split into lines (CRLF, LF or lone CR), and a three-state
 * machine per line: IDLE (between cues, index lines and garbage fall on the floor),
 * TEXT (inside a cue, lines are cleaned and appended to the arena) and SKIP (a header
 * that did not parse: ignore everything up to the next blank line).  A cue's text is
 * built straight in the arena and rewound if the cue turns out to be useless, so a
 * rejected cue costs no memory.
 */
#include "vid_srt.h"

#include "utf8.h"

#include <stdlib.h>
#include <string.h>

#define TAG_REACH 40      /* a '>' must follow a '<' within this many bytes          */
#define ASS_REACH 120     /* same for '}' after '{'                                  */

enum { ST_IDLE, ST_TEXT, ST_SKIP };

typedef struct {
    VidSrt *s;
    int     state;
    int     cstart, cend;     /**< times of the cue being built                      */
    size_t  coff;             /**< arena offset where its text starts                */
    size_t  mark;             /**< arena length before the last line appended        */
    int     last_digits;      /**< the last appended line was digits only            */
} Ctx;

static int is_ws(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f';
}

static int is_alpha(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

/* ---- arena and cue array ----------------------------------------------------- */

static int reserve_text(VidSrt *s, size_t extra)
{
    if (s->tlen + extra <= s->tcap) return 0;
    size_t nc = s->tcap ? s->tcap * 2 : 4096;
    if (nc < s->tlen + extra) nc = s->tlen + extra + 256;
    char *t = realloc(s->text, nc);
    if (!t) return -1;
    s->text = t;
    s->tcap = nc;
    return 0;
}

/* Close the cue being built: keep it if it has text and a positive duration, else
 * rewind the arena.  Always leaves the machine IDLE. */
static int finish(Ctx *x)
{
    VidSrt *s = x->s;
    if (x->state == ST_TEXT) {
        if (s->tlen > x->coff && x->cend > x->cstart && s->count < VID_SRT_MAX_CUES) {
            if (s->count == s->cap) {
                int nc = s->cap ? s->cap * 2 : 256;
                VidSrtCue *c = realloc(s->cue, (size_t)nc * sizeof *c);
                if (!c) return -1;
                s->cue = c;
                s->cap = nc;
            }
            s->text[s->tlen++] = '\0';          /* room was reserved with the line */
            s->cue[s->count].start = x->cstart;
            s->cue[s->count].end   = x->cend;
            s->cue[s->count].off   = (uint32_t)x->coff;
            s->count++;
        } else {
            s->tlen = x->coff;
        }
    }
    x->state = ST_IDLE;
    return 0;
}

/* ---- times --------------------------------------------------------------------- */

/* Up to maxd digits; returns how many were read (0 = none). */
static int num(const char **pp, const char *end, int maxd, int *v)
{
    int n = 0, acc = 0;
    const char *p = *pp;
    while (p < end && *p >= '0' && *p <= '9' && n < maxd) {
        acc = acc * 10 + (*p - '0');
        p++; n++;
    }
    *pp = p;
    *v = acc;
    return n;
}

/* "H:MM:SS,mmm" (',' or '.'), leading blanks allowed.  Advances *pp past it. */
static int parse_time(const char **pp, const char *end, int *ms)
{
    int h, m, sec, f;
    const char *p = *pp;
    while (p < end && (*p == ' ' || *p == '\t')) p++;
    if (!num(&p, end, 2, &h)) return 0;
    if (p >= end || *p != ':') return 0;
    p++;
    if (!num(&p, end, 2, &m)) return 0;
    if (p >= end || *p != ':') return 0;
    p++;
    if (!num(&p, end, 2, &sec)) return 0;
    if (p >= end || (*p != ',' && *p != '.')) return 0;
    p++;
    if (!num(&p, end, 3, &f)) return 0;
    *ms = ((h * 60 + m) * 60 + sec) * 1000 + f;
    *pp = p;
    return 1;
}

static const char *find_arrow(const char *a, const char *z)
{
    for (; a + 3 <= z; a++)
        if (a[0] == '-' && a[1] == '-' && a[2] == '>') return a;
    return NULL;
}

/* ---- text lines ---------------------------------------------------------------- */

static const struct { const char *name; size_t len; char ch; } ents[] = {
    { "&amp;", 5, '&' }, { "&lt;", 4, '<' }, { "&gt;", 4, '>' },
    { "&quot;", 6, '"' }, { "&nbsp;", 6, ' ' },
};

/* Clean one raw line into the arena.  Returns 1 if text was added, 0 if the line came
 * out empty (nothing is left in the arena then), -1 on allocation failure.  Output is
 * never longer than the input, so n + 2 bytes (separator, NUL) are reserved. */
static int append_line(Ctx *x, const char *ls, size_t n)
{
    VidSrt *s = x->s;
    if (reserve_text(s, n + 2) < 0) return -1;
    size_t m = s->tlen;
    if (s->tlen > x->coff) s->text[s->tlen++] = '\n';
    size_t lstart = s->tlen;

    for (size_t i = 0; i < n; i++) {
        char c = ls[i];
        if (c == '<' && i + 1 < n && (is_alpha(ls[i + 1]) || ls[i + 1] == '/')) {
            size_t lim = i + TAG_REACH, j = i + 1;
            if (lim >= n) lim = n - 1;
            while (j <= lim && ls[j] != '>') j++;
            if (j <= lim) { i = j; continue; }            /* drop <...> */
        } else if (c == '{') {
            size_t lim = i + ASS_REACH, j = i + 1;
            if (lim >= n) lim = n - 1;
            while (j <= lim && ls[j] != '}') j++;
            if (j <= lim) { i = j; continue; }            /* drop {...} */
        } else if (c == '&') {
            size_t k;
            for (k = 0; k < sizeof ents / sizeof ents[0]; k++)
                if (i + ents[k].len <= n && memcmp(ls + i, ents[k].name, ents[k].len) == 0)
                    break;
            if (k < sizeof ents / sizeof ents[0]) {
                c = ents[k].ch;
                i += ents[k].len - 1;
            }
        } else if (c == '\0') {
            continue;
        }
        if (s->tlen == lstart && is_ws(c)) continue;     /* trim the left edge */
        s->text[s->tlen++] = c;
    }
    while (s->tlen > lstart && is_ws(s->text[s->tlen - 1])) s->tlen--;
    if (s->tlen == lstart) { s->tlen = m; return 0; }
    x->mark = m;
    return 1;
}

static int do_line(Ctx *x, const char *ls, size_t n)
{
    const char *a = ls, *z = ls + n;
    while (a < z && is_ws(*a)) a++;
    while (z > a && is_ws(z[-1])) z--;
    if (a == z) return finish(x);                         /* blank line */

    const char *arrow = find_arrow(a, z);
    if (arrow) {
        /* the previous cue's last line was really this cue's index */
        if (x->state == ST_TEXT && x->last_digits) x->s->tlen = x->mark;
        x->last_digits = 0;
        if (finish(x) < 0) return -1;
        int t0, t1;
        const char *p = a;
        if (parse_time(&p, arrow, &t0)) {
            while (p < arrow && (*p == ' ' || *p == '\t')) p++;
            const char *q = arrow + 3;
            if (p == arrow && parse_time(&q, z, &t1)) {
                x->state = ST_TEXT;
                x->cstart = t0; x->cend = t1;
                x->coff = x->s->tlen;
                return 0;
            }
        }
        x->state = ST_SKIP;                               /* malformed header */
        return 0;
    }
    if (x->state != ST_TEXT) return 0;                    /* index line or garbage */

    int digits = 1;
    for (const char *d = a; d < z; d++)
        if (*d < '0' || *d > '9') { digits = 0; break; }
    int r = append_line(x, ls, n);
    if (r < 0) return -1;
    x->last_digits = (r > 0 && digits);
    return 0;
}

/* ---- sorting ------------------------------------------------------------------- */

/* Stable bottom-up merge sort by start; only runs when the file is out of order. */
static int sort_cues(VidSrt *s)
{
    int n = s->count, i;
    for (i = 1; i < n; i++)
        if (s->cue[i].start < s->cue[i - 1].start) break;
    if (i >= n) return 0;
    VidSrtCue *tmp = malloc((size_t)n * sizeof *tmp);
    if (!tmp) return -1;
    VidSrtCue *src = s->cue, *dst = tmp;
    for (int w = 1; w < n; w <<= 1) {
        for (int lo = 0; lo < n; lo += 2 * w) {
            int mid = lo + w < n ? lo + w : n;
            int hi  = lo + 2 * w < n ? lo + 2 * w : n;
            int l = lo, r = mid, o = lo;
            while (l < mid && r < hi)
                dst[o++] = (src[l].start <= src[r].start) ? src[l++] : src[r++];
            while (l < mid) dst[o++] = src[l++];
            while (r < hi)  dst[o++] = src[r++];
        }
        VidSrtCue *t = src; src = dst; dst = t;
    }
    if (src != s->cue) memcpy(s->cue, src, (size_t)n * sizeof *src);
    free(tmp);
    return 0;
}

/* ---- public API ---------------------------------------------------------------- */

int vid_srt_parse(VidSrt *s, const char *data, size_t len)
{
    memset(s, 0, sizeof *s);
    if (!data) len = 0;
    size_t i = 0;
    if (len >= 3 && (unsigned char)data[0] == 0xEF && (unsigned char)data[1] == 0xBB &&
        (unsigned char)data[2] == 0xBF)
        i = 3;

    Ctx x;
    memset(&x, 0, sizeof x);
    x.s = s;
    while (i < len) {
        size_t b = i;
        while (i < len && data[i] != '\n' && data[i] != '\r') i++;
        size_t n = i - b;
        if (i < len) {
            if (data[i] == '\r' && i + 1 < len && data[i + 1] == '\n') i += 2;
            else i++;
        }
        if (do_line(&x, data + b, n) < 0) { vid_srt_free(s); return -1; }
    }
    if (finish(&x) < 0 || sort_cues(s) < 0) { vid_srt_free(s); return -1; }
    return s->count;
}

void vid_srt_free(VidSrt *s)
{
    free(s->cue);
    free(s->text);
    memset(s, 0, sizeof *s);
}

int vid_srt_count(const VidSrt *s) { return s->count; }

const char *vid_srt_text(const VidSrt *s, int idx)
{
    if (idx < 0 || idx >= s->count) return "";
    return s->text + s->cue[idx].off;
}

int vid_srt_start_ms(const VidSrt *s, int idx)
{
    return (idx < 0 || idx >= s->count) ? 0 : s->cue[idx].start;
}

int vid_srt_end_ms(const VidSrt *s, int idx)
{
    return (idx < 0 || idx >= s->count) ? 0 : s->cue[idx].end;
}

int vid_srt_find(VidSrt *s, int ms)
{
    int n = s->count, i = -1;
    if (n <= 0 || ms < s->cue[0].start) return -1;

    /* i = the last cue that has started by ms.  Playing forwards it is the cursor
     * or the next one; after a seek it is a binary search. */
    int c = s->cursor;
    if (c < 0 || c >= n) c = 0;
    if (s->cue[c].start <= ms) {
        if (c + 1 >= n || s->cue[c + 1].start > ms) i = c;
        else if (c + 2 >= n || s->cue[c + 2].start > ms) i = c + 1;
    }
    if (i < 0) {
        int lo = 0, hi = n - 1;                           /* cue[0].start <= ms holds */
        while (lo < hi) {
            int mid = (lo + hi + 1) >> 1;
            if (s->cue[mid].start <= ms) lo = mid; else hi = mid - 1;
        }
        i = lo;
    }
    s->cursor = i;
    /* overlap: the latest-starting cue still showing */
    for (int j = i; j >= 0 && j >= i - 8; j--)
        if (s->cue[j].end > ms) return j;
    return -1;
}

/* ---- wrapping ------------------------------------------------------------------ */

int vid_srt_wrap(const char *text, int max_px, VidSrtGlyphW gw, void *user,
                 VidSrtLine *lines, int max_lines)
{
    int n = 0;
    if (!text || !gw || !lines || max_lines <= 0) return 0;
    if (max_px < 1) max_px = 1;

    const char *seg = text;
    for (;;) {
        const char *le = seg;
        while (*le && *le != '\n') le++;                  /* end of this hard line */
        const char *pos = seg;
        while (pos < le) {
            const char *start = pos, *q = pos, *brk = NULL, *end = NULL;
            int w = 0, prev_space = 0;
            while (q < le) {
                const char *g = q;
                uint32_t cp = utf8_next(&g);
                int cw = gw(cp, user);
                if (cw < 0) cw = 0;
                if (cp == ' ') {
                    if (w + cw > max_px && q > start) { end = q; break; }
                    if (!prev_space) brk = q;             /* start of this run of spaces */
                    prev_space = 1;
                } else {
                    if (w + cw > max_px && q > start) {
                        end = (brk && brk > start) ? brk : q;   /* word wrap, else cut */
                        break;
                    }
                    prev_space = 0;
                }
                w += cw;
                q = g;
            }
            if (!end) end = le;
            const char *next = end;
            while (next < le && *next == ' ') next++;     /* spaces at a break vanish */
            int len = (int)(end - start);
            while (len > 0 && start[len - 1] == ' ') len--;
            if (len > 0) {
                if (n >= max_lines) return n;
                lines[n].p = start;
                lines[n].len = len;
                n++;
            }
            pos = next;
        }
        if (!*le) break;
        seg = le + 1;
    }
    return n;
}
