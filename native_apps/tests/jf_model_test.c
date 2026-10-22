/* Host regression for jellyfin/jf_model.c: item pages and the login reply.
 *
 *   cd native_apps && gcc -Wall -Wextra -I common -I jellyfin -fsanitize=undefined \
 *       -o build/jf_model_test tests/jf_model_test.c jellyfin/jf_model.c common/json.c -lm \
 *       && ./build/jf_model_test
 *
 * The fixtures are trimmed, anonymised copies of real server replies (fake ids,
 * fake names, fake token): the SHAPE is what is under test.  Noise fields a real
 * track carries (MediaSources, ImageTags, UserData...) stay in, because skipping
 * them is half of the job.  Not wired into build-and-deploy.sh: host binary.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include "jf_model.h"
#include "jf_core.h"

static int checks, fails;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); \
    printf(__VA_ARGS__); printf("\n"); } } while (0)

#define ID1 "00000000000000000000000000000001"
#define ID2 "00000000000000000000000000000002"
#define ID3 "00000000000000000000000000000003"
#define ID4 "00000000000000000000000000000004"

static JsonTok toks[JF_TOKENS_PAGE];

static int parse(const char *body, JfList *l)
{
    return jf_parse_items(body, strlen(body), toks, JF_TOKENS_PAGE, l);
}

static bool utf8_ok(const char *s)
{
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        int k = *p < 0x80 ? 0 : (*p & 0xE0) == 0xC0 ? 1 : (*p & 0xF0) == 0xE0 ? 2 : (*p & 0xF8) == 0xF0 ? 3 : -1;
        if (k < 0) return false;
        p++;
        while (k--) { if ((*p & 0xC0) != 0x80) return false; p++; }
    }
    return true;
}

static const char ARTISTS[] =
    "{\"Items\":[{\"Name\":\"Artist One\",\"ServerId\":\"" ID2 "\",\"Id\":\"" ID1 "\","
    "\"RunTimeTicks\":0,\"IsFolder\":true,\"Type\":\"MusicArtist\","
    "\"UserData\":{\"PlaybackPositionTicks\":0,\"PlayCount\":0,\"IsFavorite\":false,\"Played\":false},"
    "\"ImageTags\":{\"Primary\":\"abc\"},\"BackdropImageTags\":[],\"LocationType\":\"FileSystem\"},"
    "{\"Name\":\"Artist Two\",\"Id\":\"" ID2 "\",\"Type\":\"MusicArtist\",\"ImageTags\":{}}],"
    "\"TotalRecordCount\":57,\"StartIndex\":0}";

static const char ALBUMS[] =
    "{\"Items\":[{\"Name\":\"First Album\",\"Id\":\"" ID3 "\",\"PremiereDate\":\"2009-01-01T00:00:00.0000000Z\","
    "\"RunTimeTicks\":25000000000,\"ProductionYear\":2009,\"IsFolder\":true,\"Type\":\"MusicAlbum\","
    "\"Artists\":[\"Artist One\"],\"ArtistItems\":[{\"Name\":\"Artist One\",\"Id\":\"" ID1 "\"}],"
    "\"AlbumArtist\":\"Artist One\",\"NormalizationGain\":-7.25},"
    "{\"Name\":\"Second Album\",\"Id\":\"" ID4 "\",\"Type\":\"MusicAlbum\","
    "\"Artists\":[\"Only In Artists\",\"Other\"]}],\"TotalRecordCount\":2,\"StartIndex\":0}";

static const char TRACKS[] =
    "{\"Items\":["
    "{\"Name\":\"Track A\",\"Id\":\"" ID1 "\",\"HasLyrics\":false,\"Container\":\"flac\","
    "\"RunTimeTicks\":2451230000,\"IndexNumber\":1,\"ParentIndexNumber\":1,\"IsFolder\":false,"
    "\"Type\":\"Audio\",\"Artists\":[\"Artist One\"],\"ArtistItems\":[{\"Name\":\"Artist One\",\"Id\":\"" ID2 "\"}],"
    "\"Album\":\"First Album\",\"AlbumArtist\":\"Artist One\",\"NormalizationGain\":-3.6000004,"
    "\"MediaSources\":[{\"Protocol\":\"File\",\"Id\":\"" ID1 "\",\"Path\":\"/x/a.flac\",\"Size\":1234567,"
    "\"MediaStreams\":[{\"Codec\":\"flac\",\"BitRate\":900000,\"IsDefault\":true},{\"Codec\":\"mjpeg\"}]}],"
    "\"ImageTags\":{\"Primary\":\"t1\"}},"
    "{\"Name\":\"Track B\",\"Id\":\"" ID2 "\",\"RunTimeTicks\":1800000000,\"IndexNumber\":2,"
    "\"Type\":\"Audio\",\"Artists\":[\"Solo\"],\"Album\":\"First Album\",\"NormalizationGain\":17.9},"
    "{\"Name\":\"Track C\",\"Id\":\"" ID3 "\",\"Type\":\"Audio\",\"Album\":\"First Album\","
    "\"AlbumArtist\":\"Artist One\"}],\"TotalRecordCount\":3,\"StartIndex\":0}";

static void test_pages(void)
{
    JfItem it[8];
    JfList l;

    jf_list_init(&l, it, 8);
    CHECK(parse(ARTISTS, &l) == JF_PARSE_OK, "artist page parses");
    CHECK(l.n == 2 && l.loaded == 2 && l.total == 57 && !l.full, "artists n=%d loaded=%d total=%d", l.n, l.loaded, l.total);
    CHECK(it[0].type == JF_ITEM_ARTIST && !strcmp(it[0].name, "Artist One") && !strcmp(it[0].id, ID1), "artist 0");
    CHECK(it[0].artist[0] == '\0' && it[0].album[0] == '\0' && it[0].image_tag[0] == '\0', "artist has no artist/album/tag");
    CHECK(it[1].type == JF_ITEM_ARTIST && !strcmp(it[1].name, "Artist Two") && it[1].runtime_s == 0, "artist 1");

    jf_list_init(&l, it, 8);
    CHECK(parse(ALBUMS, &l) == JF_PARSE_OK && l.n == 2, "album page parses");
    CHECK(it[0].type == JF_ITEM_ALBUM && !strcmp(it[0].name, "First Album") && !strcmp(it[0].artist, "Artist One"), "album 0 artist");
    CHECK(it[0].runtime_s == 2500, "album runtime %d", it[0].runtime_s);
    CHECK(it[0].norm_gain_mdb == -7250, "album gain %d", it[0].norm_gain_mdb);
    CHECK(!strcmp(it[1].artist, "Only In Artists"), "album falls back to Artists[0]: '%s'", it[1].artist);
    CHECK(it[1].norm_gain_mdb == 0 && it[1].index == 0, "album absent gain/index are 0");

    jf_list_init(&l, it, 8);
    CHECK(parse(TRACKS, &l) == JF_PARSE_OK && l.n == 3 && l.total == 3, "track page parses");
    CHECK(it[0].type == JF_ITEM_TRACK && !strcmp(it[0].name, "Track A") && !strcmp(it[0].album, "First Album"), "track 0");
    CHECK(it[0].runtime_s == 245 && it[0].index == 1, "track 0 runtime %d index %d", it[0].runtime_s, it[0].index);
    CHECK(it[0].norm_gain_mdb == -3600, "gain -3.6000004 -> %d", it[0].norm_gain_mdb);
    CHECK(it[1].norm_gain_mdb == 17900, "gain 17.9 -> %d", it[1].norm_gain_mdb);
    CHECK(it[2].norm_gain_mdb == 0, "absent gain -> %d", it[2].norm_gain_mdb);
    CHECK(it[1].runtime_s == 180 && it[1].index == 2, "track 1 runtime/index");
    CHECK(!strcmp(it[1].artist, "Solo"), "track 1 artist from Artists[0]");
    CHECK(it[2].runtime_s == 0 && it[2].index == 0, "track 2 absent runtime/index");

    /* two pages append and accumulate */
    jf_list_init(&l, it, 8);
    parse(ARTISTS, &l);
    CHECK(parse(ALBUMS, &l) == JF_PARSE_OK, "second page");
    CHECK(l.n == 4 && l.loaded == 4 && l.total == 2, "accumulated n=%d loaded=%d total=%d", l.n, l.loaded, l.total);
    CHECK(it[2].type == JF_ITEM_ALBUM, "second page lands after the first");
    jf_list_clear(&l);
    CHECK(l.n == 0 && l.loaded == 0 && l.total == 0 && !l.full && l.items == it && l.cap == 8, "clear");
}

static void test_rounding_and_clamp(void)
{
    JfItem it[2];
    JfList l;
    char b[400];
    struct { const char *g; int want; } c[] = {
        { "-3.6000004", -3600 }, { "17.9", 17900 }, { "0.0005", 1 }, { "-0.0005", -1 },
        { "0.0004", 0 }, { "2.5004", 2500 }, { "-2.5006", -2501 }, { "1e9", 100000 }, { "-1e9", -100000 },
    };
    for (size_t i = 0; i < sizeof c / sizeof c[0]; i++) {
        snprintf(b, sizeof b, "{\"Items\":[{\"Id\":\"" ID1 "\",\"Type\":\"Audio\",\"NormalizationGain\":%s}]}", c[i].g);
        jf_list_init(&l, it, 2);
        CHECK(parse(b, &l) == JF_PARSE_OK && l.n == 1 && it[0].norm_gain_mdb == c[i].want,
              "gain %s -> %d want %d", c[i].g, it[0].norm_gain_mdb, c[i].want);
    }
    const char *tk[] = { "9223372036854775807", "99999999999999999999", "-5", "10000000", "9999999" };
    int want[] = { INT_MAX, 0, 0, 1, 0 };
    for (size_t i = 0; i < 5; i++) {
        snprintf(b, sizeof b, "{\"Items\":[{\"Id\":\"" ID1 "\",\"Type\":\"Audio\",\"RunTimeTicks\":%s}]}", tk[i]);
        jf_list_init(&l, it, 2);
        CHECK(parse(b, &l) == JF_PARSE_OK && it[0].runtime_s == want[i], "ticks %s -> %d want %d", tk[i], it[0].runtime_s, want[i]);
    }
    /* a fractional / non-number tick count is "absent", not a crash */
    jf_list_init(&l, it, 2);
    CHECK(parse("{\"Items\":[{\"Id\":\"" ID1 "\",\"Type\":\"Audio\",\"RunTimeTicks\":1.5e3,\"IndexNumber\":\"x\",\"NormalizationGain\":\"y\"}]}", &l) == JF_PARSE_OK
          && it[0].runtime_s == 0 && it[0].index == 0 && it[0].norm_gain_mdb == 0, "wrong-typed numbers read as absent");
}

static void test_many_artists(void)
{
    static char b[8192];
    JfItem it[2];
    JfList l;
    int o = snprintf(b, sizeof b, "{\"Items\":[{\"Id\":\"" ID1 "\",\"Type\":\"Audio\",\"Name\":\"Compilation\",\"Artists\":[");
    for (int i = 0; i < 40; i++) o += snprintf(b + o, sizeof b - o, "%s\"Performer Number %d\"", i ? "," : "", i);
    o += snprintf(b + o, sizeof b - o, "]}]}");
    jf_list_init(&l, it, 2);
    CHECK(parse(b, &l) == JF_PARSE_OK && l.n == 1, "40-artist track parses");
    CHECK(!strcmp(it[0].artist, "Performer Number 0"), "artist is Artists[0]: '%s'", it[0].artist);
    /* null / non-string / empty AlbumArtist falls back */
    jf_list_init(&l, it, 2);
    CHECK(parse("{\"Items\":[{\"Id\":\"" ID1 "\",\"Type\":\"Audio\",\"AlbumArtist\":null,\"Artists\":[\"Fallback\"],\"Name\":null,\"Album\":7}]}", &l) == JF_PARSE_OK, "nulls parse");
    CHECK(!strcmp(it[0].artist, "Fallback") && it[0].name[0] == '\0' && it[0].album[0] == '\0', "null name/album empty, artist falls back");
    jf_list_init(&l, it, 2);
    parse("{\"Items\":[{\"Id\":\"" ID1 "\",\"Type\":\"Audio\",\"Artists\":[]}]}", &l);
    CHECK(it[0].artist[0] == '\0', "empty Artists array -> empty artist");
}

static void test_unicode(void)
{
    static char b[2048];
    static char name[400];
    JfItem it[2];
    JfList l;
    char *p;

    /* escapes decode */
    jf_list_init(&l, it, 2);
    parse("{\"Items\":[{\"Id\":\"" ID1 "\",\"Type\":\"Audio\",\"Name\":\"caf\\u00e9 \\ud83d\\ude00 \xc3\xa9\\n\"}]}", &l);
    CHECK(!strcmp(it[0].name, "caf\xc3\xa9 \xf0\x9f\x98\x80 \xc3\xa9\n"), "escapes and raw UTF-8 decode");

    /* name buffer 96 -> 95 bytes usable; 94 'a' + 2-byte char would be 96 */
    struct { const char *tail; int want_len; } c[] = {
        { "\\u00e9", 94 }, { "\xc3\xa9", 94 }, { "\\ud83d\\ude00", 94 }, { "\xf0\x9f\x98\x80", 94 }, { "\\u20ac", 94 },
    };
    for (size_t i = 0; i < sizeof c / sizeof c[0]; i++) {
        memset(name, 'a', 94);
        name[94] = '\0';
        strcat(name, c[i].tail);
        snprintf(b, sizeof b, "{\"Items\":[{\"Id\":\"" ID1 "\",\"Type\":\"Audio\",\"Name\":\"%s\"}]}", name);
        jf_list_init(&l, it, 2);
        memset(it, 0x55, sizeof it);
        CHECK(parse(b, &l) == JF_PARSE_OK && utf8_ok(it[0].name) && (int)strlen(it[0].name) == c[i].want_len,
              "tail %zu: len %zu valid=%d", i, strlen(it[0].name), utf8_ok(it[0].name));
    }
    /* a 3-byte char that does fit exactly: 92 a + euro = 95 */
    memset(name, 'a', 92); name[92] = '\0'; strcat(name, "\\u20ac");
    snprintf(b, sizeof b, "{\"Items\":[{\"Id\":\"" ID1 "\",\"Type\":\"Audio\",\"Name\":\"%s\"}]}", name);
    jf_list_init(&l, it, 2);
    parse(b, &l);
    CHECK(strlen(it[0].name) == 95 && utf8_ok(it[0].name), "char that fits exactly is kept");

    /* a run of 2-byte chars far longer than any buffer, in name / artist / album */
    p = name;
    for (int i = 0; i < 100; i++) { *p++ = '\xc3'; *p++ = '\xa9'; }
    *p = '\0';
    snprintf(b, sizeof b, "{\"Items\":[{\"Id\":\"" ID1 "\",\"Type\":\"Audio\",\"Name\":\"%s\",\"AlbumArtist\":\"%s\",\"Album\":\"%s\"}]}", name, name, name);
    jf_list_init(&l, it, 2);
    CHECK(parse(b, &l) == JF_PARSE_OK, "long fields parse");
    CHECK(utf8_ok(it[0].name) && strlen(it[0].name) <= 95 && strlen(it[0].name) >= 94, "name valid, len %zu", strlen(it[0].name));
    CHECK(utf8_ok(it[0].artist) && strlen(it[0].artist) <= 63 && strlen(it[0].artist) >= 62, "artist valid, len %zu", strlen(it[0].artist));
    CHECK(utf8_ok(it[0].album) && strlen(it[0].album) <= 63 && strlen(it[0].album) >= 62, "album valid, len %zu", strlen(it[0].album));
    CHECK(it[0].image_tag[0] == '\0', "image_tag stays empty");
}

static void test_prefixes(void)
{
    JfItem it[4];
    JfList l;
    size_t len = strlen(TRACKS);
    int bad = 0, nonpart = 0;
    for (size_t k = 0; k < len; k++) {
        jf_list_init(&l, it, 4);
        int r = jf_parse_items(TRACKS, k, toks, JF_TOKENS_PAGE, &l);
        if (r != JF_PARSE_TRUNC) { nonpart++; if (nonpart < 4) printf("  prefix %zu -> %d\n", k, r); }
        if (l.n || l.loaded || l.total) bad++;
    }
    CHECK(nonpart == 0, "%d of %zu prefixes were not TRUNC", nonpart, len);
    CHECK(bad == 0, "%d prefixes changed the list", bad);
    jf_list_init(&l, it, 4);
    CHECK(jf_parse_items(TRACKS, len, toks, JF_TOKENS_PAGE, &l) == JF_PARSE_OK, "the whole page is OK");
}

static void test_failures(void)
{
    JfItem it[4];
    JfList l;
    jf_list_init(&l, it, 4);
    CHECK(parse(ARTISTS, &l) == JF_PARSE_OK, "seed");
    JfList before = l;

    CHECK(jf_parse_items(TRACKS, strlen(TRACKS), toks, 10, &l) == JF_PARSE_NOMEM, "10 tokens -> NOMEM");
    CHECK(!memcmp(&l, &before, sizeof l) && it[0].type == JF_ITEM_ARTIST, "NOMEM leaves the list unchanged");
    CHECK(jf_parse_items(TRACKS, strlen(TRACKS), NULL, 0, &l) == JF_PARSE_NOMEM, "NULL/0 tokens -> NOMEM");

    const char *bads[] = { "", "not json", "[]", "{}", "{\"Items\":5}", "{\"Items\":{}}", "{\"Items\":null}",
                           "{\"items\":[]}", "{\"Items\":[],}", "{\"Items\":[]} x", "42" };
    for (size_t i = 0; i < sizeof bads / sizeof bads[0]; i++) {
        int r = parse(bads[i], &l);
        /* "" is an empty document: the tokenizer calls that a truncation */
        CHECK(r == (i == 0 ? JF_PARSE_TRUNC : JF_PARSE_BAD), "bad '%s' -> %d", bads[i], r);
        CHECK(!memcmp(&l, &before, sizeof l), "bad '%s' leaves list unchanged", bads[i]);
    }
    /* an empty page is fine and adds nothing; total is read */
    CHECK(parse("{\"Items\":[],\"TotalRecordCount\":9}", &l) == JF_PARSE_OK && l.n == before.n && l.loaded == before.loaded && l.total == 9, "empty page");
    /* total absent: left alone */
    CHECK(parse("{\"Items\":[]}", &l) == JF_PARSE_OK && l.total == 9, "absent total unchanged");
}

static void test_skips_and_cap(void)
{
    JfItem it[4];
    JfList l;

    /* capacity bound: 3 items, cap 2 */
    jf_list_init(&l, it, 2);
    CHECK(parse("{\"Items\":[{\"Id\":\"" ID1 "\",\"Type\":\"Audio\",\"Name\":\"a\"},{\"Id\":\"" ID2 "\",\"Type\":\"Audio\",\"Name\":\"b\"},"
                "{\"Id\":\"" ID3 "\",\"Type\":\"Audio\",\"Name\":\"c\"}],\"TotalRecordCount\":40}", &l) == JF_PARSE_OK, "cap page");
    CHECK(l.n == 2 && l.full && l.loaded == 3 && l.total == 40, "cap n=%d full=%d loaded=%d total=%d", l.n, l.full, l.loaded, l.total);
    CHECK(!strcmp(it[1].name, "b"), "stored the first two");
    /* exactly filling the cap drops nothing */
    jf_list_init(&l, it, 2);
    parse("{\"Items\":[{\"Id\":\"" ID1 "\",\"Type\":\"Audio\"},{\"Id\":\"" ID2 "\",\"Type\":\"Audio\"}]}", &l);
    CHECK(l.n == 2 && !l.full && l.loaded == 2, "exact fit is not full");
    /* once full, later pages advance loaded but store nothing */
    parse("{\"Items\":[{\"Id\":\"" ID3 "\",\"Type\":\"Audio\"},{\"Id\":\"" ID4 "\",\"Type\":\"Audio\"}]}", &l);
    CHECK(l.n == 2 && l.full && l.loaded == 4, "later page: n=%d loaded=%d", l.n, l.loaded);
    CHECK(it[0].id[31] == '1', "stored items untouched");
    jf_list_init(&l, it, 0);
    parse("{\"Items\":[{\"Id\":\"" ID1 "\",\"Type\":\"Audio\"}]}", &l);
    CHECK(l.n == 0 && l.full && l.loaded == 1, "cap 0");

    /* skipped but counted: unknown Type, non-hex Id, short Id, no Id, no Type, non-object */
    jf_list_init(&l, it, 4);
    CHECK(parse("{\"Items\":[{\"Id\":\"" ID1 "\",\"Type\":\"Folder\",\"Name\":\"p\"},"
                "{\"Id\":\"zz000000000000000000000000000001\",\"Type\":\"Audio\"},"
                "{\"Id\":\"0000000000000000000000000000001\",\"Type\":\"Audio\"},"
                "{\"Id\":\"" ID1 "0\",\"Type\":\"Audio\"},"
                "{\"Type\":\"Audio\"},{\"Id\":\"" ID2 "\"},7,\"s\",null,"
                "{\"Id\":\"" ID3 "\",\"Type\":\"MusicAlbum\",\"Name\":\"keep\"}],\"TotalRecordCount\":10}", &l) == JF_PARSE_OK, "skip page");
    CHECK(l.n == 1 && l.loaded == 10 && !l.full, "skips: n=%d loaded=%d", l.n, l.loaded);
    CHECK(!strcmp(it[0].name, "keep") && it[0].type == JF_ITEM_ALBUM, "the valid one is kept");
    /* skipped items never mark the list full */
    jf_list_init(&l, it, 1);
    parse("{\"Items\":[{\"Id\":\"" ID1 "\",\"Type\":\"Audio\"},{\"Id\":\"" ID2 "\",\"Type\":\"Folder\"}]}", &l);
    CHECK(l.n == 1 && !l.full && l.loaded == 2, "a skipped type is not a dropped item");
    /* uppercase hex is accepted */
    jf_list_init(&l, it, 1);
    parse("{\"Items\":[{\"Id\":\"ABCDEF00000000000000000000000001\",\"Type\":\"Audio\"}]}", &l);
    CHECK(l.n == 1, "uppercase hex id");
}

/* Playlists and genres are kept: name and id only, and the id is checked like any other. */
static void test_root_types(void)
{
    JfItem it[4];
    JfList l;
    jf_list_init(&l, it, 4);
    CHECK(parse("{\"Items\":[{\"Name\":\"Mix\",\"Id\":\"" ID1 "\",\"Type\":\"Playlist\",\"Album\":\"x\",\"AlbumArtist\":\"y\"},"
                "{\"Name\":\"Jazz\",\"Id\":\"" ID2 "\",\"Type\":\"MusicGenre\"},"
                "{\"Name\":\"Bad\",\"Id\":\"nothex\",\"Type\":\"MusicGenre\"}],\"TotalRecordCount\":1120}", &l) == JF_PARSE_OK, "root types page");
    CHECK(l.n == 2 && l.loaded == 3 && l.total == 1120, "n=%d loaded=%d total=%d", l.n, l.loaded, l.total);
    CHECK(it[0].type == JF_ITEM_PLAYLIST && !strcmp(it[0].name, "Mix") && !it[0].album[0] && !it[0].artist[0], "playlist");
    CHECK(it[1].type == JF_ITEM_GENRE && !strcmp(it[1].name, "Jazz"), "genre");
}

static const char AUTH[] =
    "{\"User\":{\"Name\":\"user1\",\"ServerId\":\"" ID4 "\",\"Id\":\"" ID2 "\",\"HasPassword\":true,"
    "\"Configuration\":{\"PlayDefaultAudioTrack\":true,\"SubtitleMode\":\"Default\"},"
    "\"Policy\":{\"IsAdministrator\":false,\"EnabledFolders\":[]}},"
    "\"SessionInfo\":{\"Id\":\"s\",\"UserId\":\"" ID2 "\"},\"AccessToken\":\"fedcba9876543210fedcba9876543210\","
    "\"ServerId\":\"" ID4 "\"}";

static int auth(const char *b, size_t tokcap, size_t uidcap, char *t, char *u)
{
    return jf_parse_auth(b, strlen(b), toks, JF_TOKENS_PAGE, t, tokcap, u, uidcap);
}

static void test_auth(void)
{
    char t[64], u[33];
    memset(t, 'x', sizeof t); memset(u, 'x', sizeof u);
    CHECK(auth(AUTH, sizeof t, sizeof u, t, u) == JF_PARSE_OK, "auth parses");
    CHECK(!strcmp(t, "fedcba9876543210fedcba9876543210") && !strcmp(u, ID2), "token and user id");

    /* token needs 33 bytes: cap 32 is too small */
    CHECK(auth(AUTH, 32, sizeof u, t, u) == JF_PARSE_BAD && !t[0] && !u[0], "token too long for cap -> BAD, emptied");
    CHECK(auth(AUTH, 33, sizeof u, t, u) == JF_PARSE_OK, "token exactly fits cap 33");

    const char *bad[] = {
        "{\"User\":{\"Id\":\"" ID2 "\"},\"AccessToken\":\"\"}",
        "{\"User\":{\"Id\":\"" ID2 "\"},\"AccessToken\":null}",
        "{\"User\":{\"Id\":\"" ID2 "\"}}",
        "{\"AccessToken\":\"tok\"}",
        "{\"User\":{},\"AccessToken\":\"tok\"}",
        "{\"User\":5,\"AccessToken\":\"tok\"}",
        "{\"User\":{\"Id\":\"zz000000000000000000000000000002\"},\"AccessToken\":\"tok\"}",
        "{\"User\":{\"Id\":\"0000000000000000000000000000002\"},\"AccessToken\":\"tok\"}",
        "{\"User\":{\"Id\":\"" ID2 "0\"},\"AccessToken\":\"tok\"}",
        "[]", "nonsense",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        memset(t, 'x', sizeof t); memset(u, 'x', sizeof u); u[32] = '\0';
        int r = auth(bad[i], sizeof t, sizeof u, t, u);
        CHECK(r == JF_PARSE_BAD && !t[0] && !u[0], "bad auth %zu -> %d, emptied", i, r);
    }
    size_t len = strlen(AUTH);
    int nonpart = 0;
    for (size_t k = 0; k < len; k++)
        if (jf_parse_auth(AUTH, k, toks, JF_TOKENS_PAGE, t, sizeof t, u, sizeof u) != JF_PARSE_TRUNC || t[0] || u[0]) nonpart++;
    CHECK(nonpart == 0, "%d auth prefixes not TRUNC/emptied", nonpart);
    CHECK(jf_parse_auth(AUTH, len, toks, 8, t, sizeof t, u, sizeof u) == JF_PARSE_NOMEM && !t[0], "auth NOMEM");
    CHECK(jf_parse_auth(AUTH, len, toks, JF_TOKENS_PAGE, t, 0, u, sizeof u) == JF_PARSE_BAD, "zero token cap");
    CHECK(jf_parse_auth(AUTH, len, toks, JF_TOKENS_PAGE, t, sizeof t, u, 10) == JF_PARSE_BAD, "user id cap too small");
}

/* album_id and image_tag: the cover source of a track (its album) and of an album (itself) */
static void test_art_fields(void)
{
    JfItem it[6];
    JfList l;
    jf_list_init(&l, it, 6);
    const char *body =
        "{\"Items\":["
        "{\"Name\":\"T\",\"Id\":\"" ID1 "\",\"Type\":\"Audio\",\"AlbumId\":\"" ID2 "\",\"AlbumPrimaryImageTag\":\"abc123\"},"
        "{\"Name\":\"A\",\"Id\":\"" ID3 "\",\"Type\":\"MusicAlbum\",\"ImageTags\":{\"Backdrop\":\"no\",\"Primary\":\"tag9\"}},"
        "{\"Name\":\"Bare\",\"Id\":\"" ID4 "\",\"Type\":\"Audio\"},"
        "{\"Name\":\"BadAlb\",\"Id\":\"" ID4 "\",\"Type\":\"Audio\",\"AlbumId\":\"nothex\",\"AlbumPrimaryImageTag\":\"t\"},"
        "{\"Name\":\"NoTag\",\"Id\":\"" ID2 "\",\"Type\":\"MusicAlbum\",\"ImageTags\":{}},"
        "{\"Name\":\"Art\",\"Id\":\"" ID1 "\",\"Type\":\"MusicArtist\",\"ImageTags\":{\"Primary\":\"x\"}}"
        "],\"TotalRecordCount\":6}";
    CHECK(parse(body, &l) == JF_PARSE_OK && l.n == 6, "art fields page n=%d", l.n);
    CHECK(strcmp(it[0].album_id, ID2) == 0 && strcmp(it[0].image_tag, "abc123") == 0,
          "track: AlbumId + AlbumPrimaryImageTag ('%s','%s')", it[0].album_id, it[0].image_tag);
    CHECK(strcmp(it[1].album_id, ID3) == 0 && strcmp(it[1].image_tag, "tag9") == 0,
          "album: its own Id + ImageTags.Primary ('%s','%s')", it[1].album_id, it[1].image_tag);
    CHECK(!it[2].album_id[0] && !it[2].image_tag[0], "absent stays empty");
    CHECK(!it[3].album_id[0], "a non-hex AlbumId is dropped ('%s')", it[3].album_id);
    CHECK(strcmp(it[4].album_id, ID2) == 0 && !it[4].image_tag[0], "album without a Primary tag: id set, tag empty");
    CHECK(!it[5].album_id[0] && !it[5].image_tag[0], "an artist item takes no art fields");
}

/* year, count, artist_id and genres: the fields MIX reads off a track and off the genre index */
#define A60 "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
static void test_mix_fields(void)
{
    JfItem it[6];
    JfList l;
    jf_list_init(&l, it, 6);
    const char *body =
        "{\"Items\":["
        "{\"Name\":\"Song\",\"Id\":\"" ID1 "\",\"Type\":\"Audio\",\"ProductionYear\":1984,"
        "\"ArtistItems\":[{\"Name\":\"Ann\",\"Id\":\"" ID3 "\"},{\"Name\":\"Bob\",\"Id\":\"" ID4 "\"}],"
        "\"Genres\":[\"Rock\",\"Hip-Hop\",\"New Wave\"],\"Artists\":[\"Ann\"]},"
        "{\"Name\":\"Bare\",\"Id\":\"" ID2 "\",\"Type\":\"Audio\"},"
        "{\"Name\":\"Pop Music\",\"Id\":\"" ID3 "\",\"Type\":\"MusicGenre\",\"SongCount\":4321,\"ProductionYear\":1999},"
        "{\"Name\":\"Odd\",\"Id\":\"" ID4 "\",\"Type\":\"Audio\",\"ProductionYear\":12345678,"
        "\"ArtistItems\":[{\"Name\":\"Bad\",\"Id\":\"nothex\"}],\"Genres\":[]},"
        "{\"Name\":\"Long\",\"Id\":\"" ID1 "\",\"Type\":\"Audio\",\"Genres\":[\"" A60 "\",\"" A60 "\",\"" A60 "\",\"Pop\",\"x\134ny\",7]},"
        "{\"Name\":\"Tag\",\"Id\":\"" ID2 "\",\"Type\":\"MusicArtist\",\"SongCount\":-5}"
        "],\"TotalRecordCount\":6}";
    int pr = parse(body, &l);
    CHECK(pr == JF_PARSE_OK, "mix fields page (%d)", pr);
    CHECK(l.n == 6, "n=%d", l.n);

    CHECK(it[0].year == 1984, "year %d", it[0].year);
    CHECK(strcmp(it[0].artist_id, ID3) == 0, "artist_id is ArtistItems[0] (got '%s')", it[0].artist_id);
    CHECK(strcmp(it[0].genres, "Rock\nHip-Hop\nNew Wave\n") == 0, "genres newline-ended: '%s'", it[0].genres);
    CHECK(strcmp(it[0].artist, "Ann") == 0, "artist still read (%s)", it[0].artist);

    CHECK(it[1].year == 0 && !it[1].artist_id[0] && !it[1].genres[0] && it[1].count == 0, "absent fields stay zero");

    CHECK(it[2].type == JF_ITEM_GENRE && it[2].count == 4321, "genre SongCount %d", it[2].count);
    CHECK(it[2].year == 0 && !it[2].genres[0] && !it[2].artist_id[0], "a genre item takes no track fields");

    CHECK(it[3].year == 9999, "absurd year clamped (%d)", it[3].year);
    CHECK(!it[3].artist_id[0], "a non-hex artist id is dropped ('%s')", it[3].artist_id);
    CHECK(!it[3].genres[0], "empty Genres array");

    /* 3 x 60 chars cannot all fit in JF_GENRES_BYTES: whole names or nothing, later short ones still fit */
    size_t gl = strlen(it[4].genres);
    CHECK(gl < JF_GENRES_BYTES, "genres within the field (%zu)", gl);
    CHECK(strcmp(it[4].genres, A60 "\n" A60 "\nPop\n") == 0, "third long name dropped whole, 'Pop' kept, name with newline and number skipped: '%s'", it[4].genres);
    CHECK(it[4].genres[gl - 1] == '\n', "ends on a newline, never on half a name");

    CHECK(it[5].type == JF_ITEM_ARTIST && it[5].count == 0, "negative SongCount clamps to 0 (%d)", it[5].count);
}

/* A MIX pool page must parse with the real token budget and arrive inside the body buffer, for the worst track a
 * real server sends: Fields=Genres makes it list every genre twice (Genres and GenreItems) plus ArtistItems,
 * AlbumArtists, ImageBlurHashes.  Measured on the live server, 50 random pool pages of 40: up to 5259 tokens and
 * 83670 bytes, and 4 of the 50 failed json_parse at 5000 tokens (UNEXPECTED REPLY on the device).  The shape here is
 * a 12-genre track (~135 tokens, ~1.9 KB, the average of that sample's worst pages).  The page must use at most
 * three quarters of either budget, so a heavier page than the sample still fits. */
static size_t put_pool_item(char *b, size_t cap, size_t w, int k)
{
    w += (size_t)snprintf(b + w, cap - w, "%s{\"Name\":\"Track %d\",\"ServerId\":\"%s\",\"Id\":\"%031d1\","
        "\"HasLyrics\":false,\"Container\":\"flac\",\"PremiereDate\":\"2000-08-15T04:00:00.0000000Z\",\"ChannelId\":null,"
        "\"Genres\":[", k ? "," : "", k, ID1, k);
    for (int g = 0; g < 12; g++)
        w += (size_t)snprintf(b + w, cap - w, "%s\"Some Genre Name %d\"", g ? "," : "", g);
    w += (size_t)snprintf(b + w, cap - w, "],\"RunTimeTicks\":2746933330,\"ProductionYear\":2000,\"IndexNumber\":5,"
        "\"ParentIndexNumber\":1,\"IsFolder\":false,\"Type\":\"Audio\",\"GenreItems\":[");
    for (int g = 0; g < 12; g++)
        w += (size_t)snprintf(b + w, cap - w, "%s{\"Name\":\"Some Genre Name %d\",\"Id\":\"%s\"}", g ? "," : "", g, ID2);
    w += (size_t)snprintf(b + w, cap - w, "],\"Artists\":[\"Mike Oldfield\"],\"ArtistItems\":[{\"Name\":\"Mike Oldfield\","
        "\"Id\":\"%s\"}],\"Album\":\"Discovery\",\"AlbumId\":\"%s\",\"AlbumPrimaryImageTag\":\"%s\","
        "\"AlbumArtist\":\"Mike Oldfield\",\"AlbumArtists\":[{\"Name\":\"Mike Oldfield\",\"Id\":\"%s\"}],"
        "\"ImageBlurHashes\":{\"Primary\":{\"%s\":\"eIAS+6sk14S%%$gIqRis.xBWqJAoI$%%S5R.Sin$jaWrsloefkayoJWV\"}},"
        "\"LocationType\":\"FileSystem\",\"MediaType\":\"Audio\"}", ID3, ID3, ID4, ID3, ID4);
    return w;
}

static void test_pool_page_budget(void)
{
    static char body[160 * 1024];
    static JsonTok big[40000];
    static JfItem st[JF_MIX_PAGE];
    size_t w = (size_t)snprintf(body, sizeof body, "{\"Items\":[");
    for (int k = 0; k < JF_MIX_PAGE; k++) w = put_pool_item(body, sizeof body, w, k);
    w += (size_t)snprintf(body + w, sizeof body - w, "],\"TotalRecordCount\":5000,\"StartIndex\":0}");
    int need = json_parse(body, w, big, 40000);
    CHECK(need > 0, "pool page json_parse (%d)", need);
    CHECK(need * 4 <= JF_TOKENS_PAGE * 3, "a pool page of %d needs %d tokens, budget %d (75%% = %d)",
          JF_MIX_PAGE, need, JF_TOKENS_PAGE, JF_TOKENS_PAGE * 3 / 4);
    CHECK((long)w * 4 <= (long)JF_API_BODY_BYTES * 3, "a pool page is %zu bytes, body buffer %d", w, JF_API_BODY_BYTES);
    JfList l;
    jf_list_init(&l, st, JF_MIX_PAGE);
    CHECK(jf_parse_items(body, w, toks, JF_TOKENS_PAGE, &l) == JF_PARSE_OK && l.n == JF_MIX_PAGE,
          "the pool page parses with the real budget (n %d)", l.n);

    /* an instant mix: JF_MIX_LIMIT plain items, up to 88 tokens each (4399 for 50, measured) */
    int plain = JF_MIX_LIMIT * 88;
    CHECK(plain * 4 <= JF_TOKENS_PAGE * 3, "an instant mix of %d needs ~%d tokens, budget %d", JF_MIX_LIMIT, plain, JF_TOKENS_PAGE);
}

/* Operator rule: a track shows its ARTIST (Artists[0]); AlbumArtist only when Artists is empty.  A compilation track
 * used to read "Various Artists" because AlbumArtist won.  Both are kept; an ALBUM row names its album artist. */
static void test_artist_rule(void)
{
    static const char B[] = "{\"Items\":["
        "{\"Id\":\"" ID1 "\",\"Type\":\"Audio\",\"Name\":\"Comp\",\"AlbumArtist\":\"Various Artists\",\"Artists\":[\"Real Band\",\"Guest\"]},"
        "{\"Id\":\"" ID2 "\",\"Type\":\"Audio\",\"Name\":\"NoArtists\",\"AlbumArtist\":\"Album Person\",\"Artists\":[]},"
        "{\"Id\":\"" ID3 "\",\"Type\":\"Audio\",\"Name\":\"OnlyArtists\",\"Artists\":[\"Solo\"]},"
        "{\"Id\":\"" ID4 "\",\"Type\":\"MusicAlbum\",\"Name\":\"Album\",\"AlbumArtist\":\"Various Artists\",\"Artists\":[\"Real Band\"]}"
        "],\"TotalRecordCount\":4}";
    JfItem it[6];
    JfList l;
    jf_list_init(&l, it, 6);
    CHECK(parse(B, &l) == JF_PARSE_OK && l.n == 4, "artist rule page parses");
    CHECK(!strcmp(it[0].artist, "Real Band") && !strcmp(it[0].album_artist, "Various Artists"), "track: artist '%s', album artist '%s'", it[0].artist, it[0].album_artist);
    CHECK(!strcmp(it[1].artist, "Album Person") && !strcmp(it[1].album_artist, "Album Person"), "empty Artists falls back to AlbumArtist: '%s'", it[1].artist);
    CHECK(!strcmp(it[2].artist, "Solo") && !it[2].album_artist[0], "no AlbumArtist: '%s' / '%s'", it[2].artist, it[2].album_artist);
    CHECK(!strcmp(it[3].artist, "Various Artists"), "an album row names its album artist: '%s'", it[3].artist);

    CHECK(jf_row_shows_artist(false, &it[0]) && jf_row_shows_artist(false, &it[2]), "queue/search/mix/playlist rows: always for a track");
    CHECK(jf_row_shows_artist(true, &it[0]), "album list: the compilation track differs from its album artist");
    CHECK(!jf_row_shows_artist(true, &it[1]), "album list: artist == album artist hides it");
    JfItem same = it[0];
    snprintf(same.artist, sizeof same.artist, "VARIOUS artists");
    CHECK(!jf_row_shows_artist(true, &same), "album list: the comparison ignores case");
    CHECK(!jf_row_shows_artist(false, &it[3]) && !jf_row_shows_artist(true, &it[3]), "a non-track never shows an artist");
}

static void test_row_detail(void)
{
    char o[64];
    JfItem t;
    memset(&t, 0, sizeof t);
    t.type = JF_ITEM_TRACK;
    t.runtime_s = 245;
    snprintf(t.artist, sizeof t.artist, "Short Band");
    jf_row_detail(&t, true, o, sizeof o);
    CHECK(!strcmp(o, "Short Band  4:05"), "artist and time: '%s'", o);
    jf_row_detail(&t, false, o, sizeof o);
    CHECK(!strcmp(o, "4:05"), "time only: '%s'", o);
    snprintf(t.artist, sizeof t.artist, "ABCDEFGHIJKLMNOPQRSTUV");   /* exactly 22 */
    jf_row_detail(&t, true, o, sizeof o);
    CHECK(!strcmp(o, "ABCDEFGHIJKLMNOPQRSTUV  4:05"), "22 characters stay whole: '%s'", o);
    snprintf(t.artist, sizeof t.artist, "ABCDEFGHIJKLMNOPQRSTUVW");  /* 23 */
    jf_row_detail(&t, true, o, sizeof o);
    CHECK(!strcmp(o, "ABCDEFGHIJKLMNOPQRST..  4:05"), "23 characters cut to 20 + '..': '%s'", o);
    /* 30 two-byte characters: cut on a character boundary */
    char u[64] = "";
    for (int i = 0; i < 30; i++) strcat(u, "\xC3\xA9");
    snprintf(t.artist, sizeof t.artist, "%s", u);
    jf_row_detail(&t, true, o, sizeof o);
    size_t ol = strlen(o);
    CHECK(utf8_ok(o) && ol == 20 * 2 + 2 + 2 + 4 && !strncmp(o + 40, "..", 2), "UTF-8 cut on a boundary (len %zu '%s')", ol, o);
    t.runtime_s = 0;
    snprintf(t.artist, sizeof t.artist, "Band");
    jf_row_detail(&t, true, o, sizeof o);
    CHECK(!strcmp(o, "Band"), "no runtime: no time part: '%s'", o);
    t.runtime_s = 3600 + 61;
    t.artist[0] = '\0';
    jf_row_detail(&t, true, o, sizeof o);
    CHECK(!strcmp(o, "61:01"), "no artist: time only, minutes not wrapped: '%s'", o);
    char tiny[6];
    snprintf(t.artist, sizeof t.artist, "Some Artist");
    jf_row_detail(&t, true, tiny, sizeof tiny);
    CHECK(strlen(tiny) < sizeof tiny, "a small buffer is never overrun ('%s')", tiny);
    jf_row_detail(&t, true, tiny, 1);
    CHECK(tiny[0] == '\0', "n == 1 gives the empty string");
}

/* A Views reply: only CollectionType "music" entries are kept, as JF_ITEM_LIBRARY.  Sabotage: keep every collection. */
static void test_views(void)
{
    static const char V[] = "{\"Items\":[{\"Name\":\"Music\",\"Id\":\"" ID1 "\",\"Type\":\"CollectionFolder\",\"CollectionType\":\"music\"},"
        "{\"Name\":\"Movies\",\"Id\":\"" ID2 "\",\"Type\":\"CollectionFolder\",\"CollectionType\":\"movies\"},"
        "{\"Name\":\"Classical\",\"Id\":\"" ID3 "\",\"Type\":\"CollectionFolder\",\"CollectionType\":\"music\"},"
        "{\"Name\":\"Lists\",\"Id\":\"" ID4 "\",\"Type\":\"ManualPlaylistsFolder\",\"CollectionType\":\"playlists\"}],\"TotalRecordCount\":4}";
    JfItem st[8]; JfList l;
    jf_list_init(&l, st, 8);
    CHECK(parse(V, &l) == JF_PARSE_OK, "views parse");
    CHECK(l.n == 2 && st[0].type == JF_ITEM_LIBRARY && st[1].type == JF_ITEM_LIBRARY && !strcmp(st[0].id, ID1) &&
          !strcmp(st[1].name, "Classical"), "only the 2 music libraries kept (n=%d)", l.n);
}

int main(void)
{
    test_pool_page_budget();
    test_views();
    test_artist_rule();
    test_row_detail();
    test_pages();
    test_rounding_and_clamp();
    test_many_artists();
    test_unicode();
    test_prefixes();
    test_failures();
    test_skips_and_cap();
    test_root_types();
    test_auth();
    test_mix_fields();
    test_art_fields();
    printf("jf_model_test: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
