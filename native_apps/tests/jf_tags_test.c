/*
 * tests/jf_tags_test.c — the file-tag parser (jf_tags.c): synthetic ID3v2.3/2.4 and FLAC byte blobs built here.
 *
 * Build and run (host gcc, from native_apps/):
 *   gcc -Wall -Wextra -I common -I jellyfin -fsanitize=undefined tests/jf_tags_test.c jellyfin/jf_tags.c \
 *       -o build/jf_tags_test && ./build/jf_tags_test
 */

#include <stdio.h>
#include <string.h>

#include "jf_tags.h"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); \
    printf(__VA_ARGS__); printf("\n"); } } while (0)
#define STREQ(a, b) (strcmp((a), (b)) == 0)

typedef unsigned char u8;
static u8 blob[300000];
static size_t bl;

static void put(const void *p, size_t n) { memcpy(blob + bl, p, n); bl += n; }
static void put32be(unsigned v) { u8 b[4] = { (u8)(v >> 24), (u8)(v >> 16), (u8)(v >> 8), (u8)v }; put(b, 4); }
static void put32ss(unsigned v) { u8 b[4] = { (u8)((v >> 21) & 0x7F), (u8)((v >> 14) & 0x7F), (u8)((v >> 7) & 0x7F), (u8)(v & 0x7F) }; put(b, 4); }

/* one ID3 frame: v is 3 or 4; body is raw bytes */
static void frame(int v, const char *id, const u8 *body, size_t n)
{
    put(id, 4);
    if (v == 4) put32ss((unsigned)n); else put32be((unsigned)n);
    u8 fl[2] = { 0, 0 };
    put(fl, 2);
    put(body, n);
}
/* text frame, encoding 3 (UTF-8) with values separated by NUL */
static void tf8(int v, const char *id, const char *vals_nul_separated, size_t n)
{
    u8 b[512];
    b[0] = 3;
    memcpy(b + 1, vals_nul_separated, n);
    frame(v, id, b, n + 1);
}
#define TF8(v, id, lit) tf8(v, id, lit, sizeof(lit) - 1)

static void id3_begin(int v) { bl = 0; memcpy(blob, "ID3", 3); blob[3] = (u8)v; blob[4] = 0; blob[5] = 0; bl = 6; put32ss(0); }
static void id3_end(void) { unsigned sz = (unsigned)(bl - 10); blob[6] = (u8)((sz >> 21) & 0x7F); blob[7] = (u8)((sz >> 14) & 0x7F); blob[8] = (u8)((sz >> 7) & 0x7F); blob[9] = (u8)(sz & 0x7F); }

static void test_id3(int v)
{
    JfTags t;
    id3_begin(v);
    TF8(v, "TPE1", "Lang Lang\0Daniel Barenboim");
    TF8(v, "TPE2", "Berliner Philharmoniker");
    TF8(v, "TPE3", "Simon Rattle");
    TF8(v, "TIT1", "Piano Concerto No. 1");
    TF8(v, v == 4 ? "TDRC" : "TYER", "2011-05-06");
    TF8(v, "TCON", "(17)Classical\0Piano\0(13)\0Classical\0A\0B\0C");
    TF8(v, "TMCL", "violin\0Anne-Sophie Mutter\0cello\0Yo-Yo Ma");
    TF8(v, "TXXX", "WORK\0Symphony 5");
    id3_end();
    bool ok = jf_tags_parse(blob, bl, &t);
    CHECK(ok && t.have && !t.truncated, "v2.%d parsed", v);
    CHECK(STREQ(t.artist, "Lang Lang, Daniel Barenboim"), "v%d artist '%s'", v, t.artist);
    CHECK(STREQ(t.album_artist, "Berliner Philharmoniker"), "album artist '%s'", t.album_artist);
    CHECK(STREQ(t.conductor, "Simon Rattle"), "conductor '%s'", t.conductor);
    CHECK(STREQ(t.work, "Symphony 5"), "TXXX WORK overrides TIT1: '%s'", t.work);
    CHECK(t.year == 2011, "year %d", t.year);
    CHECK(t.genre_n == 5 && STREQ(t.genre[0], "Classical") && STREQ(t.genre[1], "Piano") && STREQ(t.genre[2], "A") && STREQ(t.genre[4], "C"),
          "genres %d: '%s' '%s' (numeric ids dropped, duplicates dropped, five at most)", t.genre_n, t.genre[0], t.genre[1]);
    CHECK(t.perf_n == 2 && STREQ(t.perf[0].name, "Anne-Sophie Mutter") && STREQ(t.perf[0].role, "violin") && STREQ(t.perf[1].role, "cello"),
          "performers %d", t.perf_n);
}

static void test_id3_enc_and_edges(void)
{
    JfTags t;
    /* UTF-16 with BOM (little endian) and Latin-1 */
    id3_begin(3);
    {
        u8 b[64];
        int n = 0;
        b[n++] = 1; b[n++] = 0xFF; b[n++] = 0xFE;
        const char *s = "Bj\xC3\xB6rk";                       /* UTF-8 source: Björk */
        u8 u16[] = { 'B', 0, 'j', 0, 0xF6, 0 , 'r', 0, 'k', 0 };
        (void)s;
        memcpy(b + n, u16, sizeof u16); n += (int)sizeof u16;
        frame(3, "TPE1", b, (size_t)n);
        u8 l1[] = { 0, 'C', 'a', 'f', 0xE9 };                  /* Latin-1 "Café" */
        frame(3, "TPE2", l1, sizeof l1);
        u8 be[] = { 2, 0, 'M', 0, 0xE9 };                      /* UTF-16BE "Mé" */
        frame(3, "TPE3", be, sizeof be);
    }
    id3_end();
    jf_tags_parse(blob, bl, &t);
    CHECK(STREQ(t.artist, "Bj\xC3\xB6rk"), "UTF-16 LE+BOM -> UTF-8 '%s'", t.artist);
    CHECK(STREQ(t.album_artist, "Caf\xC3\xA9"), "Latin-1 -> UTF-8 '%s'", t.album_artist);
    CHECK(STREQ(t.conductor, "M\xC3\xA9"), "UTF-16BE -> UTF-8 '%s'", t.conductor);

    /* padding ends the walk; TIPL gives the conductor; TXXX CONDUCTOR does not override a TPE3 */
    id3_begin(4);
    TF8(4, "TIPL", "producer\0Someone\0conductor\0Karajan");
    TF8(4, "TXXX", "CONDUCTOR\0Other");
    { u8 z[40] = { 0 }; put(z, sizeof z); }
    TF8(4, "TPE1", "After Padding");
    id3_end();
    jf_tags_parse(blob, bl, &t);
    CHECK(STREQ(t.conductor, "Karajan"), "TIPL conductor '%s'", t.conductor);
    CHECK(t.artist[0] == 0, "nothing is read past padding: '%s'", t.artist);

    /* a v2.2 tag is not covered */
    id3_begin(2);
    id3_end();
    CHECK(!jf_tags_parse(blob, bl, &t) && !t.have, "ID3v2.2 reports have=false");
}

static void test_id3_truncation(void)
{
    JfTags t;
    id3_begin(4);
    TF8(4, "TPE1", "Early");
    { u8 pic[40000]; memset(pic, 'x', sizeof pic); pic[0] = 0; frame(4, "APIC", pic, sizeof pic); }
    TF8(4, "TPE3", "Late Conductor");
    id3_end();
    size_t total = bl;

    jf_tags_parse(blob, 20000, &t);                           /* the range ends inside the picture */
    CHECK(t.have && t.truncated && t.need == total, "truncated, need %zu (full %zu)", t.need, total);
    CHECK(STREQ(t.artist, "Early") && t.conductor[0] == 0, "what fit is read, nothing past the cut: '%s' '%s'", t.artist, t.conductor);

    jf_tags_parse(blob, total, &t);                           /* the extension */
    CHECK(t.have && !t.truncated && STREQ(t.conductor, "Late Conductor"), "extension reads the rest: '%s'", t.conductor);

    /* a size field larger than the buffer in a frame header must not read out of bounds (ubsan/asan watch this) */
    jf_tags_parse(blob, 14, &t);
    CHECK(t.truncated, "cut inside a frame header is truncated");
    for (size_t n = 0; n < 60; n++) jf_tags_parse(blob, n, &t);   /* every short cut parses without a fault */
    CHECK(!jf_tags_parse(blob, 0, &t), "empty input");
    CHECK(!jf_tags_parse(NULL, 100, &t), "NULL input");
}

static void flac_begin(void) { bl = 0; put("fLaC", 4); }
static void flac_block(int type, bool last, const u8 *body, size_t n)
{
    u8 h[4] = { (u8)((last ? 0x80 : 0) | type), (u8)(n >> 16), (u8)(n >> 8), (u8)n };
    put(h, 4);
    put(body, n);
}
static size_t vc_entry(u8 *p, const char *kv)
{
    size_t n = strlen(kv);
    u8 l[4] = { (u8)n, (u8)(n >> 8), (u8)(n >> 16), (u8)(n >> 24) };
    memcpy(p, l, 4);
    memcpy(p + 4, kv, n);
    return 4 + n;
}

static size_t vorbis_body(u8 *p, const char *const *kv, int n)
{
    size_t o = 0;
    const char *vendor = "reference libFLAC";
    size_t vl = strlen(vendor);
    u8 l[4] = { (u8)vl, 0, 0, 0 };
    memcpy(p + o, l, 4); o += 4;
    memcpy(p + o, vendor, vl); o += vl;
    u8 c[4] = { (u8)n, 0, 0, 0 };
    memcpy(p + o, c, 4); o += 4;
    for (int i = 0; i < n; i++) o += vc_entry(p + o, kv[i]);
    return o;
}

static void test_flac(void)
{
    JfTags t;
    static u8 vb[4096];
    const char *kv[] = { "TITLE=x", "artist=A.S. Kang", "ARTIST=Second", "ALBUMARTIST=Various Artists", "CONDUCTOR=Gergiev",
                         "PERFORMER=Sukshinder Shinda", "PERFORMER=Jane Roe (viola)", "WORK=Opus 9", "DATE=2002-03-04",
                         "GENRE=Bhangra", "GENRE=bhangra", "GENRE=Pop", "BROKEN" };
    size_t vn = vorbis_body(vb, kv, 13);
    flac_begin();
    { u8 si[34] = { 0 }; flac_block(0, false, si, sizeof si); }
    { static u8 pic[5000]; memset(pic, 1, sizeof pic); flac_block(6, false, pic, sizeof pic); }
    flac_block(4, true, vb, vn);
    size_t total = bl;
    bool ok = jf_tags_parse(blob, total, &t);
    CHECK(ok && !t.truncated, "flac parsed");
    CHECK(STREQ(t.artist, "A.S. Kang, Second"), "artist '%s' (key case-insensitive, repeated key joined)", t.artist);
    CHECK(STREQ(t.album_artist, "Various Artists") && !jf_tags_show_album_artist(t.artist, t.album_artist), "album artist rule hides Various Artists");
    CHECK(STREQ(t.conductor, "Gergiev") && STREQ(t.work, "Opus 9") && t.year == 2002, "conductor '%s' work '%s' year %d", t.conductor, t.work, t.year);
    CHECK(t.perf_n == 2 && STREQ(t.perf[0].name, "Sukshinder Shinda") && t.perf[0].role[0] == 0 &&
          STREQ(t.perf[1].name, "Jane Roe") && STREQ(t.perf[1].role, "viola"), "performers %d: '%s' / '%s'", t.perf_n, t.perf[1].name, t.perf[1].role);
    CHECK(t.genre_n == 2 && STREQ(t.genre[0], "Bhangra") && STREQ(t.genre[1], "Pop"), "genres %d (case-insensitive duplicate dropped)", t.genre_n);

    /* range ends inside the comment block: need says where it ends; the extension reads it */
    jf_tags_parse(blob, total - 30, &t);
    CHECK(t.have && t.truncated && t.need == total, "flac truncated, need %zu (full %zu)", t.need, total);
    /* range ends inside the picture before the comment block: need points past the picture's header */
    jf_tags_parse(blob, 60, &t);
    CHECK(t.have && t.truncated && t.need > 5000, "flac cut in a picture block, need %zu", t.need);
    for (size_t n = 0; n < 80; n++) jf_tags_parse(blob, n, &t);
    /* no comment block at all */
    flac_begin();
    { u8 si[34] = { 0 }; flac_block(0, true, si, sizeof si); }
    CHECK(jf_tags_parse(blob, bl, &t) && !t.truncated && t.artist[0] == 0, "flac without comments is have, empty");
}

static void test_other(void)
{
    JfTags t;
    memcpy(blob, "OggS\0\2\0\0\0\0\0\0\0\0", 14);
    CHECK(!jf_tags_parse(blob, 14, &t) && !t.have, "Ogg is not covered");
    memcpy(blob + 4, "ftypM4A ", 8);
    CHECK(!jf_tags_parse(blob, 14, &t) && !t.have, "MP4 is not covered");
    CHECK(!jf_tags_show_album_artist("X", "") && !jf_tags_show_album_artist("x", "X") && jf_tags_show_album_artist("X", "Y") &&
          !jf_tags_show_album_artist("X", "various artists"), "album-artist display rule");
}

int main(void)
{
    test_id3(3);
    test_id3(4);
    test_id3_enc_and_edges();
    test_id3_truncation();
    test_flac();
    test_other();
    printf("jf_tags_test: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
