/*
 * vid_srt_test.c — host regression for jfvideo/vid_srt.{c,h}: the SubRip parser,
 * the cue lookup and the line wrapper.  All fixtures are synthetic.
 *
 * Build (from native_apps):
 *   gcc -Wall -Wextra -O1 -fsanitize=address,undefined -I common -I jfvideo \
 *       tests/vid_srt_test.c jfvideo/vid_srt.c -o /tmp/vst && /tmp/vst
 * Prints one FAIL line per failed check; exit status 1 if any failed.
 */
#include "vid_srt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_checks, g_fail;

#define CHECK(cond) do { g_checks++; if (!(cond)) { g_fail++; \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

/* ---- (1) the synthetic file ------------------------------------------------------ */

/* Line endings are mixed on purpose: CRLF, LF and lone CR all appear. */
static const char SRT[] =
    "\xEF\xBB\xBF"                  /* right against the first time line: no index to hide it */
    "00:00:01,000 --> 00:00:03,500\r\n"
    "Hello, world.\r\n"
    "\r\n"
    "2\n"
    "00:00:04,000 --> 00:00:06,000\n"
    "<i>Italic</i> and <b>bold</b>\n"
    "<font color=\"#ff0000\">red</font> <u>u</u>\n"
    "\n"
    "3\r"
    "00:00:07.250 --> 00:00:09,5\r"
    "{\\an8}Top &amp; bottom &lt;ok&gt; &quot;q&quot;&nbsp;x\r"
    "\r"
    "00:00:10,000 --> 00:00:12,000 X1:100 X2:200 Y1:50 Y2:80\n"
    "No index line\n"
    "   Pad   \n"
    "<i></i>\n"
    "{\\i1}second line{\\i0}\n"
    "\n"
    "4\n"
    "00:00:13,000 --> 00:00:14,000\n"
    "First\n"
    "5\n"
    "00:00:14,500 --> 00:00:15,500\n"
    "Second\n"
    "\n"
    "6\r\n"
    "00:00:16,000 --> 00:00:18,000\r\n"
    "T\xC5\x91" "zsg\xC3\xA9r \xC5\xB1" "rhaj\xC3\xB3\r\n"
    "\r\n"
    "00:00:20,000 --> 00:00:20,000\n"
    "Zero length\n"
    "\n"
    "00:00:25,000 --> 00:00:21,000\n"
    "Negative length\n"
    "\n"
    "this is a garbage block\n"
    "that has no times\n"
    "\n"
    "7\n"
    "00:00:xx --> 00:00:30,000\n"
    "Bad header text\n"
    "more of it\n"
    "\n"
    "8\n"
    "00:00:40,000 --> 00:00:42,000\n"
    "Late\n"
    "\n"
    "9\n"
    "00:00:30,000 --> 00:00:32,000\n"
    "Early\n"
    "\n"
    "00:00:50,000 --> 00:00:51,000\n"
    "Tie1\n"
    "\n"
    "00:00:50,000 --> 00:00:52,000\n"
    "Tie2\n"
    "\n"
    "00:01:00,000 --> 00:01:10,000\n"
    "Long\n"
    "\n"
    "00:01:02,000 --> 00:01:04,000\n"
    "Short\n"
    "\n"
    "00:01:10,000 --> 00:01:12,000\r\n"
    "Line one\r\n"
    "  <b> two </b>  \r\n"
    "\r\n"
    "00:01:20,000 --> 00:01:21,000\n"
    "Last, no newline";

static const struct { int start, end; const char *text; } EXP[] = {
    {  1000,  3500, "Hello, world." },
    {  4000,  6000, "Italic and bold\nred u" },
    {  7250,  9005, "Top & bottom <ok> \"q\" x" },
    { 10000, 12000, "No index line\nPad\nsecond line" },
    { 13000, 14000, "First" },
    { 14500, 15500, "Second" },
    { 16000, 18000, "T\xC5\x91" "zsg\xC3\xA9r \xC5\xB1" "rhaj\xC3\xB3" },
    { 30000, 32000, "Early" },
    { 40000, 42000, "Late" },
    { 50000, 51000, "Tie1" },
    { 50000, 52000, "Tie2" },
    { 60000, 70000, "Long" },
    { 62000, 64000, "Short" },
    { 70000, 72000, "Line one\ntwo" },
    { 80000, 81000, "Last, no newline" },
};
#define NEXP ((int)(sizeof EXP / sizeof EXP[0]))

static void test_synthetic(void)
{
    VidSrt s;
    int n = vid_srt_parse(&s, SRT, sizeof SRT - 1);
    CHECK(n == NEXP);
    CHECK(vid_srt_count(&s) == n);
    for (int i = 0; i < NEXP && i < n; i++) {
        CHECK(vid_srt_start_ms(&s, i) == EXP[i].start);
        CHECK(vid_srt_end_ms(&s, i) == EXP[i].end);
        CHECK(strcmp(vid_srt_text(&s, i), EXP[i].text) == 0);
        if (strcmp(vid_srt_text(&s, i), EXP[i].text) != 0)
            printf("  cue %d got \"%s\"\n", i, vid_srt_text(&s, i));
    }
    /* out-of-range accessors */
    CHECK(vid_srt_text(&s, -1)[0] == 0 && vid_srt_text(&s, n)[0] == 0);
    CHECK(vid_srt_start_ms(&s, n) == 0 && vid_srt_end_ms(&s, -1) == 0);

    /* find: end exclusive, before the first, after the last, gaps */
    CHECK(vid_srt_find(&s, 0) == -1);
    CHECK(vid_srt_find(&s, 999) == -1);
    CHECK(vid_srt_find(&s, 1000) == 0);
    CHECK(vid_srt_find(&s, 3499) == 0);
    CHECK(vid_srt_find(&s, 3500) == -1);
    CHECK(vid_srt_find(&s, 20000) == -1);
    /* overlap: Long 60-70 s with Short 62-64 s inside it */
    CHECK(vid_srt_find(&s, 61999) == 11);
    CHECK(vid_srt_find(&s, 62000) == 12);
    CHECK(vid_srt_find(&s, 63999) == 12);
    CHECK(vid_srt_find(&s, 64000) == 11);       /* Short is over, Long still on */
    CHECK(vid_srt_find(&s, 70000) == 13);       /* Long ended exactly here */
    /* same start: the later one in the file wins while both show */
    CHECK(vid_srt_find(&s, 50500) == 10);
    CHECK(vid_srt_find(&s, 51000) == 10);       /* Tie1 over, Tie2 still on */
    CHECK(vid_srt_find(&s, 52000) == -1);
    CHECK(vid_srt_find(&s, 80000) == 14);
    CHECK(vid_srt_find(&s, 81000) == -1);
    CHECK(vid_srt_find(&s, 1000000) == -1);
    CHECK(vid_srt_find(&s, 2000) == 0);         /* a jump back */
    vid_srt_free(&s);
    CHECK(vid_srt_count(&s) == 0);
}

/* ---- (2) a large file --------------------------------------------------------------- */

#define BIG 2000
static int bs[BIG], be[BIG];

static void fmt_time(char *o, int ms)
{
    int h = ms / 3600000, m = (ms / 60000) % 60, sec = (ms / 1000) % 60, f = ms % 1000;
    sprintf(o, "%02d:%02d:%02d,%03d", h, m, sec, f);
}

static void test_large(void)
{
    size_t cap = (size_t)BIG * 100 + 64;
    char *buf = malloc(cap), *p = buf;
    for (int i = 0; i < BIG; i++) {
        bs[i] = i * 3000 + 100;
        be[i] = bs[i] + 2000 - (i & 15) * 10;
        const char *eol = (i & 1) ? "\r\n" : "\n";
        char t0[16], t1[16];
        fmt_time(t0, bs[i]);
        fmt_time(t1, be[i]);
        p += sprintf(p, "%d%s%s --> %s%s", i + 1, eol, t0, t1, eol);
        if (i % 3 == 0) p += sprintf(p, "<i>Line %d</i>%s", i, eol);
        else            p += sprintf(p, "Line %d%s", i, eol);
        p += sprintf(p, "%s", eol);
    }
    VidSrt s;
    int n = vid_srt_parse(&s, buf, (size_t)(p - buf));
    CHECK(n == BIG);
    if (n == BIG) {
        char want[32];
        int bad = 0;
        for (int i = 0; i < BIG; i++) {
            sprintf(want, "Line %d", i);
            if (vid_srt_start_ms(&s, i) != bs[i] || vid_srt_end_ms(&s, i) != be[i] ||
                strcmp(vid_srt_text(&s, i), want) != 0) bad++;
        }
        CHECK(bad == 0);

        /* sequential sweep against a moving reference */
        int j = 0, wrong = 0;
        for (int ms = 0; ms < be[BIG - 1] + 3000; ms += 7) {
            while (j < BIG && be[j] <= ms) j++;
            int exp = (j < BIG && bs[j] <= ms) ? j : -1;
            if (vid_srt_find(&s, ms) != exp) wrong++;
        }
        CHECK(wrong == 0);

        /* the four boundary probes of every cue, in random order (binary search) */
        unsigned seed = 12345;
        wrong = 0;
        for (int k = 0; k < BIG * 3; k++) {
            seed = seed * 1103515245u + 12345u;
            int i = (int)((seed >> 8) % BIG);
            if (vid_srt_find(&s, bs[i] - 1) != -1) wrong++;
            if (vid_srt_find(&s, bs[i]) != i) wrong++;
            if (vid_srt_find(&s, be[i] - 1) != i) wrong++;
            if (vid_srt_find(&s, be[i]) != -1) wrong++;
        }
        CHECK(wrong == 0);
        /* and in order, every cue once, the cursor fast path */
        wrong = 0;
        for (int i = 0; i < BIG; i++) {
            if (vid_srt_find(&s, bs[i] - 1) != -1) wrong++;
            if (vid_srt_find(&s, bs[i]) != i) wrong++;
            if (vid_srt_find(&s, be[i] - 1) != i) wrong++;
            if (vid_srt_find(&s, be[i]) != -1) wrong++;
        }
        CHECK(wrong == 0);
    }
    vid_srt_free(&s);

    /* the cap: 20005 valid cues keep exactly VID_SRT_MAX_CUES */
    size_t cap2 = (size_t)(VID_SRT_MAX_CUES + 5) * 48 + 16;
    char *b2 = malloc(cap2), *q = b2;
    for (int i = 0; i < VID_SRT_MAX_CUES + 5; i++) {
        char t0[16], t1[16];
        fmt_time(t0, i * 10);
        fmt_time(t1, i * 10 + 5);
        q += sprintf(q, "%s --> %s\nx\n\n", t0, t1);
    }
    CHECK(vid_srt_parse(&s, b2, (size_t)(q - b2)) == VID_SRT_MAX_CUES);
    vid_srt_free(&s);
    free(b2);
    free(buf);
}

/* ---- (3) wrapping --------------------------------------------------------------------- */

static int g_bad_cp;
static int gw6(uint32_t cp, void *user)
{
    (void)user;
    if (cp == 0xFFFD) g_bad_cp++;              /* a sequence was split or mangled */
    return cp == ' ' ? 3 : 6;
}

static int line_is(const VidSrtLine *l, const char *s)
{
    return l->len == (int)strlen(s) && memcmp(l->p, s, (size_t)l->len) == 0;
}

static void test_wrap(void)
{
    VidSrtLine ln[8];
    int n;
    g_bad_cp = 0;

    const char *t = "hello world foo";
    n = vid_srt_wrap(t, 60, gw6, NULL, ln, 8);
    CHECK(n == 2 && line_is(&ln[0], "hello") && line_is(&ln[1], "world foo"));
    CHECK(ln[0].p == t && ln[1].p == t + 6);              /* views, not copies */

    n = vid_srt_wrap("abcdefghijkl", 30, gw6, NULL, ln, 8);
    CHECK(n == 3 && line_is(&ln[0], "abcde") && line_is(&ln[1], "fghij") && line_is(&ln[2], "kl"));

    n = vid_srt_wrap("ab cdefghijkl", 30, gw6, NULL, ln, 8);
    CHECK(n == 3 && line_is(&ln[0], "ab") && line_is(&ln[1], "cdefg") && line_is(&ln[2], "hijkl"));

    n = vid_srt_wrap("one\ntwo three", 60, gw6, NULL, ln, 8);
    CHECK(n == 2 && line_is(&ln[0], "one") && line_is(&ln[1], "two three"));

    /* trailing spaces at a break are not part of the line */
    n = vid_srt_wrap("hello   world", 36, gw6, NULL, ln, 8);
    CHECK(n == 2 && line_is(&ln[0], "hello") && line_is(&ln[1], "world"));

    /* seven 2-byte glyphs, five per line: cut between sequences */
    const char *u = "\xC5\x91\xC5\x91\xC5\x91\xC5\x91\xC5\x91\xC5\x91\xC5\x91";
    n = vid_srt_wrap(u, 30, gw6, NULL, ln, 8);
    CHECK(n == 2 && ln[0].len == 10 && ln[1].len == 4 && ln[1].p == u + 10);
    CHECK(g_bad_cp == 0);

    /* a glyph wider than the line still gets a line of its own (terminates) */
    n = vid_srt_wrap("abc", 2, gw6, NULL, ln, 8);
    CHECK(n == 3 && line_is(&ln[0], "a") && line_is(&ln[2], "c"));

    /* max_lines drops the rest */
    n = vid_srt_wrap("a\nb\nc\nd\ne", 60, gw6, NULL, ln, 3);
    CHECK(n == 3 && line_is(&ln[2], "c"));
    n = vid_srt_wrap("aaaaaaaaaaaaaaaaaaaaaaaa", 30, gw6, NULL, ln, 2);
    CHECK(n == 2);

    CHECK(vid_srt_wrap("", 60, gw6, NULL, ln, 8) == 0);
}

/* ---- (4) tiny and hostile input --------------------------------------------------------- */

static void test_tiny(void)
{
    VidSrt s;
    static const char bom[] = "\xEF\xBB\xBF";
    CHECK(vid_srt_parse(&s, "", 0) == 0);
    CHECK(vid_srt_find(&s, 0) == -1 && vid_srt_text(&s, 0)[0] == 0);
    vid_srt_free(&s);
    CHECK(vid_srt_parse(&s, NULL, 0) == 0);
    vid_srt_free(&s);
    CHECK(vid_srt_parse(&s, bom, 3) == 0);
    CHECK(vid_srt_find(&s, 5) == -1);
    vid_srt_free(&s);
    CHECK(vid_srt_parse(&s, bom, 2) == 0);      /* truncated BOM is just junk */
    vid_srt_free(&s);
    CHECK(vid_srt_parse(&s, "\r\n\r\n\r", 5) == 0);
    vid_srt_free(&s);
    static const char hdr[] = "00:00:01,000 --> 00:00:02,000\n";
    CHECK(vid_srt_parse(&s, hdr, sizeof hdr - 1) == 0);   /* header, no text */
    vid_srt_free(&s);
    static const char cut[] = "00:00:01,000 --> 00:00:0";
    CHECK(vid_srt_parse(&s, cut, sizeof cut - 1) == 0);
    vid_srt_free(&s);
    static const char arrow[] = "-->";
    CHECK(vid_srt_parse(&s, arrow, 3) == 0);
    vid_srt_free(&s);
    static const char ok1[] = "0:0:1,0 --> 0:0:2,5\nx";   /* shortest legal form */
    CHECK(vid_srt_parse(&s, ok1, sizeof ok1 - 1) == 1);
    CHECK(vid_srt_start_ms(&s, 0) == 1000 && vid_srt_end_ms(&s, 0) == 2005);
    vid_srt_free(&s);
    /* an unterminated tag and brace stay literal; a digits-only cue text survives a blank */
    static const char lit[] = "00:00:01,000 --> 00:00:02,000\na < b { c\n42\n\n";
    CHECK(vid_srt_parse(&s, lit, sizeof lit - 1) == 1);
    CHECK(strcmp(vid_srt_text(&s, 0), "a < b { c\n42") == 0);
    vid_srt_free(&s);
}

int main(void)
{
    test_synthetic();
    test_large();
    test_wrap();
    test_tiny();
    printf("vid_srt_test: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
