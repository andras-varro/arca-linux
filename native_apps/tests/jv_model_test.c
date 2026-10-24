/* Host regression for jfvideo/jv_model.c: video item pages and one item's detail.
 *
 *   cd native_apps && gcc -Wall -Wextra -I jf_shared -I common -I jfvideo -fsanitize=undefined,address \
 *       -o build/jv_model_test tests/jv_model_test.c jfvideo/jv_model.c jfvideo/jv_tracks.c \
 *       jf_shared/jf_model.c common/json.c -lm && setarch $(uname -m) -R ./build/jv_model_test
 *
 * Run from native_apps/ (the gate does): the fixtures are read from tests/fixtures/jv/.  They are
 * trimmed, anonymised copies of replies from a real server (fake ids, fake names, no paths): the SHAPE
 * is what is under test, so the noise fields a real reply carries (MediaStreams on a list row,
 * ImageBlurHashes, Trickplay, Chapters) stay in.  A case built here from a fixture or from a literal is
 * labelled SYNTHETIC: the real server's data did not contain it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "jv_model.h"
#include "jv_tracks.h"

static int checks, fails;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); \
    printf(__VA_ARGS__); printf("\n"); } } while (0)
#define STREQ(a, b) (strcmp((a), (b)) == 0)

static JsonTok toks[JF_TOKENS_PAGE];
static JvItem rows[64];
static JvDetail det;

/* ids in the fixtures */
#define MOVIE1   "5eed0000000000000000000000000002"
#define SERIES1  "5eed000000000000000000000000000d"
#define SERIES_TAG "5eed000000000000000000000000000e"
#define SEASON1  "5eed000000000000000000000000001a"
#define EP1      "5eed000000000000000000000000001b"
#define DETAIL_ID "5eed0000000000000000000000000030"

static char *slurp(const char *name)
{
    static const char *const dirs[] = { "tests/fixtures/jv/", "fixtures/jv/" };
    for (size_t d = 0; d < sizeof dirs / sizeof dirs[0]; d++) {
        char path[160];
        snprintf(path, sizeof path, "%s%s", dirs[d], name);
        FILE *f = fopen(path, "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        rewind(f);
        char *b = malloc((size_t)n + 1);
        if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(b); return NULL; }
        b[n] = '\0';
        fclose(f);
        return b;
    }
    return NULL;
}

static JvList mklist(int cap)
{
    JvList l;
    jv_list_init(&l, rows, cap);
    return l;
}

static int parse_list(const char *body, JvList *l)
{
    return jv_parse_items(body, strlen(body), toks, JF_TOKENS_PAGE, l);
}

static int parse_det(const char *body)
{
    return jv_parse_detail(body, strlen(body), toks, JV_TOKENS_DETAIL, &det);
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

/* Insert `ins` right after the first occurrence of `marker` in `text`; a new buffer. */
static char *splice_after(const char *text, const char *marker, const char *ins)
{
    const char *at = strstr(text, marker);
    if (!at) return NULL;
    at += strlen(marker);
    size_t head = (size_t)(at - text);
    char *out = malloc(strlen(text) + strlen(ins) + 1);
    memcpy(out, text, head);
    strcpy(out + head, ins);
    strcpy(out + head + strlen(ins), at);
    return out;
}

/* ── lists from the real fixtures ───────────────────────────────────────── */

static void test_movies(void)
{
    char *b = slurp("movies.json");
    CHECK(b != NULL, "movies.json not found (run from native_apps/)");
    if (!b) return;
    JvList l = mklist(64);
    CHECK(parse_list(b, &l) == JF_PARSE_OK, "movies parse");
    CHECK(l.n == 4 && l.loaded == 4 && l.total == 748 && !l.full, "movies counts n=%d loaded=%d total=%d", l.n, l.loaded, l.total);
    CHECK(l.items[0].type == JV_MOVIE && STREQ(l.items[0].b.id, MOVIE1), "movie 0 id/type");
    CHECK(STREQ(l.items[0].b.name, "Movie 01"), "name '%s'", l.items[0].b.name);
    CHECK(l.items[0].b.year == 2004 && l.items[0].b.runtime_s == 1199, "year %d runtime %d", l.items[0].b.year, l.items[0].b.runtime_s);
    CHECK(STREQ(l.items[0].b.image_tag, "5eed0000000000000000000000000003"), "image tag '%s'", l.items[0].b.image_tag);
    CHECK(!l.items[0].b.played && l.items[0].b.position_s == 0, "unwatched movie");
    CHECK(l.items[1].b.year == 1957 && l.items[1].b.runtime_s == 5768, "movie 1 year %d runtime %d", l.items[1].b.year, l.items[1].b.runtime_s);
    CHECK(l.items[2].b.year == 0 && l.items[2].b.runtime_s == 46, "movie without ProductionYear: year %d runtime %d", l.items[2].b.year, l.items[2].b.runtime_s);
    CHECK(STREQ(l.items[3].b.name, "Movie 04") && l.items[3].b.runtime_s == 4435, "movie 3");
    CHECK(l.items[0].series_id[0] == '\0' && l.items[0].season_no == 0 && l.items[0].episode_no == 0, "a movie has no series fields");

    /* a second page appends and moves the next StartIndex */
    CHECK(parse_list(b, &l) == JF_PARSE_OK && l.n == 8 && l.loaded == 8, "second page n=%d loaded=%d", l.n, l.loaded);

    /* storage cap: the overflow is dropped, flagged, and still consumed */
    JvList small = mklist(2);
    CHECK(parse_list(b, &small) == JF_PARSE_OK, "cap parse");
    CHECK(small.n == 2 && small.full && small.loaded == 4, "cap n=%d full=%d loaded=%d", small.n, small.full, small.loaded);
    jv_list_clear(&small);
    CHECK(small.n == 0 && small.loaded == 0 && small.total == 0 && !small.full && small.cap == 2, "clear");

    /* a body cut short changes nothing */
    JvList keep = mklist(64);
    parse_list(b, &keep);
    size_t half = strlen(b) / 2;
    CHECK(jv_parse_items(b, half, toks, JF_TOKENS_PAGE, &keep) == JF_PARSE_TRUNC, "truncated body");
    CHECK(keep.n == 4 && keep.loaded == 4 && keep.total == 748, "truncated parse left the list alone");
    CHECK(jv_parse_items(b, strlen(b), toks, 50, &keep) == JF_PARSE_NOMEM && keep.n == 4, "token-starved parse");
    CHECK(parse_list("<html>502</html>", &keep) == JF_PARSE_BAD && keep.n == 4, "not JSON");
    CHECK(parse_list("{\"Name\":\"x\"}", &keep) == JF_PARSE_BAD && keep.n == 4, "no Items array");
    free(b);
}

static void test_series_seasons_episodes(void)
{
    char *b = slurp("series.json");
    CHECK(b != NULL, "series.json not found");
    if (!b) return;
    JvList l = mklist(64);
    CHECK(parse_list(b, &l) == JF_PARSE_OK && l.n == 3 && l.total == 114, "series n=%d total=%d", l.n, l.total);
    CHECK(l.items[0].type == JV_SERIES && STREQ(l.items[0].b.id, SERIES1) && STREQ(l.items[0].b.name, "Series 01"), "series 0");
    CHECK(l.items[0].b.unplayed == 2 && l.items[1].b.unplayed == 35 && l.items[2].b.unplayed == 9, "unplayed %d %d %d",
          l.items[0].b.unplayed, l.items[1].b.unplayed, l.items[2].b.unplayed);
    CHECK(l.items[0].b.runtime_s == 4200 && l.items[2].b.runtime_s == 0, "series runtime %d / %d", l.items[0].b.runtime_s, l.items[2].b.runtime_s);
    CHECK(l.items[1].b.year == 1957 && STREQ(l.items[0].b.image_tag, SERIES_TAG), "series year/tag");
    free(b);

    b = slurp("seasons.json");
    CHECK(b != NULL, "seasons.json not found");
    if (!b) return;
    l = mklist(64);
    CHECK(parse_list(b, &l) == JF_PARSE_OK && l.n == 1 && l.total == 1, "seasons n=%d", l.n);
    JvItem *s = &l.items[0];
    CHECK(s->type == JV_SEASON && STREQ(s->b.id, SEASON1) && STREQ(s->b.name, "Season 1"), "season row");
    CHECK(s->season_no == 1 && s->episode_no == 0, "season_no %d episode_no %d", s->season_no, s->episode_no);
    CHECK(STREQ(s->series_id, SERIES1) && STREQ(s->series_name, "Series 01") && s->b.unplayed == 2, "season's series");
    CHECK(s->b.image_tag[0] == '\0' && STREQ(s->series_image_tag, SERIES_TAG), "season has no poster of its own");
    const char *aid, *atag;
    jv_item_art(s, &aid, &atag);
    CHECK(STREQ(aid, SERIES1) && STREQ(atag, SERIES_TAG), "season art falls back to the series poster: %s %s", aid, atag);
    free(b);

    b = slurp("episodes.json");
    CHECK(b != NULL, "episodes.json not found");
    if (!b) return;
    l = mklist(64);
    CHECK(parse_list(b, &l) == JF_PARSE_OK && l.n == 2 && l.total == 2, "episodes n=%d", l.n);
    JvItem *e = &l.items[0];
    CHECK(e->type == JV_EPISODE && STREQ(e->b.id, EP1) && STREQ(e->b.name, "Episode 01"), "episode row");
    CHECK(e->season_no == 1 && e->episode_no == 1 && e->episode_end == 2, "double episode %d %d %d", e->season_no, e->episode_no, e->episode_end);
    CHECK(STREQ(e->series_id, SERIES1) && STREQ(e->season_id, SEASON1) && STREQ(e->series_name, "Series 01"), "episode parents");
    CHECK(e->b.runtime_s == 4108, "episode runtime %d", e->b.runtime_s);
    CHECK(l.items[1].episode_no == 2 && l.items[1].episode_end == 3, "second episode %d %d", l.items[1].episode_no, l.items[1].episode_end);
    jv_item_art(e, &aid, &atag);
    CHECK(STREQ(aid, EP1) && STREQ(atag, e->b.image_tag) && atag[0], "episode art is its own");
    free(b);
}

static void test_views_and_empty(void)
{
    char *b = slurp("views.json");
    CHECK(b != NULL, "views.json not found");
    if (b) {
        JvList l = mklist(64);
        CHECK(parse_list(b, &l) == JF_PARSE_OK, "views parse");
        /* two music libraries and a playlists view are skipped, but consumed */
        CHECK(l.n == 2 && l.loaded == 5 && l.total == 5, "views n=%d loaded=%d total=%d", l.n, l.loaded, l.total);
        CHECK(l.items[0].type == JV_LIB_MOVIES && l.items[1].type == JV_LIB_SHOWS, "library kinds");
        free(b);
    }
    const char *names[] = { "resume.json", "nextup.json" };
    for (int i = 0; i < 2; i++) {
        b = slurp(names[i]);
        CHECK(b != NULL, "%s not found", names[i]);
        if (!b) continue;
        JvList l = mklist(64);
        CHECK(parse_list(b, &l) == JF_PARSE_OK && l.n == 0 && l.loaded == 0 && l.total == 0, "%s is an empty list", names[i]);
        free(b);
    }
}

/* ── lists built here (SYNTHETIC) ───────────────────────────────────────── */

static void test_synthetic_lists(void)
{
    /* SYNTHETIC: a resume list with a position, a played movie, an item without any image, a long name,
     * a bad id and a type the client does not show. */
    char longname[200];
    memset(longname, 'a', sizeof longname - 1);
    longname[sizeof longname - 1] = '\0';
    static char body[2048];
    snprintf(body, sizeof body,
        "{\"Items\":["
        "{\"Name\":\"Half\",\"Id\":\"" MOVIE1 "\",\"Type\":\"Movie\",\"ProductionYear\":1999,\"RunTimeTicks\":72000000000,"
        "\"UserData\":{\"PlaybackPositionTicks\":36000000000,\"Played\":false,\"PlayCount\":0,\"IsFavorite\":true},"
        "\"ImageTags\":{\"Primary\":\"abc\"}},"
        "{\"Name\":\"Done\",\"Id\":\"5eed0000000000000000000000000099\",\"Type\":\"Movie\","
        "\"UserData\":{\"PlaybackPositionTicks\":0,\"Played\":true}},"
        "{\"Name\":\"NoImage\",\"Id\":\"5eed000000000000000000000000009a\",\"Type\":\"Movie\",\"ImageTags\":{}},"
        "{\"Name\":\"BadId\",\"Id\":\"nothex\",\"Type\":\"Movie\"},"
        "{\"Name\":\"Song\",\"Id\":\"5eed000000000000000000000000009b\",\"Type\":\"Audio\"},"
        "5,"
        "{\"Name\":\"%s\",\"Id\":\"5eed000000000000000000000000009c\",\"Type\":\"BoxSet\"}],"
        "\"TotalRecordCount\":7,\"StartIndex\":0}", longname);
    JvList l = mklist(64);
    CHECK(parse_list(body, &l) == JF_PARSE_OK, "synthetic list");
    CHECK(l.n == 4 && l.loaded == 7 && l.total == 7, "n=%d loaded=%d (skipped ones count)", l.n, l.loaded);
    CHECK(l.items[0].b.position_s == 3600 && !l.items[0].b.played && l.items[0].b.favorite && l.items[0].b.runtime_s == 7200,
          "resume position %d runtime %d", l.items[0].b.position_s, l.items[0].b.runtime_s);
    CHECK(l.items[1].b.played && l.items[1].b.position_s == 0 && l.items[1].b.runtime_s == 0, "played movie");
    const char *aid, *atag;
    jv_item_art(&l.items[2], &aid, &atag);
    CHECK(l.items[2].b.image_tag[0] == '\0' && aid[0] == '\0' && atag[0] == '\0', "item without images has no art");
    jv_item_art(&l.items[0], &aid, &atag);
    CHECK(STREQ(aid, MOVIE1) && STREQ(atag, "abc"), "art of an item with one");
    CHECK(l.items[3].type == JV_BOXSET && strlen(l.items[3].b.name) == sizeof l.items[3].b.name - 1, "long name cut to %zu", strlen(l.items[3].b.name));
    CHECK(parse_list(body, &l) == JF_PARSE_OK && l.loaded == 14, "loaded accumulates");
}

/* ── detail ─────────────────────────────────────────────────────────────── */

static void test_detail_real(void)
{
    char *b = slurp("detail.json");
    CHECK(b != NULL, "detail.json not found");
    if (!b) return;
    memset(&det, 0xAA, sizeof det);
    CHECK(parse_det(b) == JF_PARSE_OK, "detail parse");
    CHECK(det.it.type == JV_MOVIE && STREQ(det.it.b.id, DETAIL_ID) && STREQ(det.it.b.name, "Movie 05"), "detail row");
    CHECK(det.it.b.year == 1997 && det.it.b.runtime_s == 9262 && !det.it.b.played && det.it.b.position_s == 0,
          "year %d runtime %d", det.it.b.year, det.it.b.runtime_s);
    CHECK(STREQ(det.overview, "Overview text."), "overview");
    CHECK(STREQ(det.genres, "Crime, Drama, Thriller"), "genres '%s'", det.genres);
    CHECK(STREQ(det.rating, "R") && det.community_x10 == 74, "rating '%s' community %d", det.rating, det.community_x10);
    CHECK(det.width == 1920 && det.height == 1040, "picture %dx%d", det.width, det.height);
    CHECK(STREQ(det.msid, DETAIL_ID) && STREQ(det.container, "mkv"), "source '%s' '%s'", det.msid, det.container);

    /* only MediaSources[0].MediaStreams: the video stream is not an entry, and the copy at the top is not read twice */
    CHECK(det.n_audio == 2 && det.n_subs == 2 && det.audio_dropped == 0 && det.subs_dropped == 0,
          "streams audio %d subs %d", det.n_audio, det.n_subs);
    CHECK(det.audio[0].index == 1 && STREQ(det.audio[0].codec, "ac3") && STREQ(det.audio[0].lang, "hun") &&
          det.audio[0].channels == 6 && det.audio[0].is_default && !det.audio[0].is_forced && !det.audio[0].is_external,
          "audio 0");
    CHECK(STREQ(det.audio[0].title, "Magyar - Grp"), "audio title '%s'", det.audio[0].title);
    CHECK(det.audio[1].index == 2 && STREQ(det.audio[1].codec, "dts") && STREQ(det.audio[1].lang, "eng") &&
          det.audio[1].channels == 6 && !det.audio[1].is_default, "audio 1");
    CHECK(det.subs[0].index == 3 && STREQ(det.subs[0].codec, "subrip") && STREQ(det.subs[0].lang, "hun") &&
          det.subs[0].is_forced && det.subs[0].is_default && det.subs[0].channels == 0, "sub 0 (forced)");
    CHECK(det.subs[1].index == 4 && !det.subs[1].is_forced && !det.subs[1].is_default && !det.subs[1].is_external, "sub 1");

    /* people: the director leads, writers and producers are left out, the 15 actors follow */
    CHECK(det.n_people == 16, "people %d", det.n_people);
    CHECK(det.people[0].director && STREQ(det.people[0].name, "Person 16") && STREQ(det.people[0].role, "Role 16"), "director first");
    CHECK(!det.people[1].director && STREQ(det.people[1].name, "Person 01") && det.people[1].id[0] != '\0', "first actor");
    CHECK(STREQ(det.people[15].name, "Person 15"), "last actor '%s'", det.people[15].name);

    /* the parsed arrays drive the selection in jv_tracks */
    int a = jv_pick_audio(det.audio, det.n_audio, "eng,hun");
    CHECK(a == 1, "english preferred: %d", a);
    a = jv_pick_audio(det.audio, det.n_audio, "default");
    CHECK(a == 0, "default track: %d", a);
    int s = jv_pick_sub(det.subs, det.n_subs, JV_SUB_LANG, "hun", "hun", &det.audio[0]);
    CHECK(s == 1, "full track before the forced one: %d", s);
    s = jv_pick_sub(det.subs, det.n_subs, JV_SUB_FORCED, "hun", "hun", &det.audio[0]);
    CHECK(s == 0, "forced track: %d", s);

    /* the same bytes cut short: zeroed, not half filled */
    memset(&det, 0xAA, sizeof det);
    CHECK(jv_parse_detail(b, strlen(b) / 2, toks, JV_TOKENS_DETAIL, &det) == JF_PARSE_TRUNC, "truncated detail");
    CHECK(det.n_audio == 0 && det.n_subs == 0 && det.it.b.id[0] == '\0' && det.overview[0] == '\0', "truncated detail is zeroed");
    memset(&det, 0xAA, sizeof det);
    CHECK(jv_parse_detail(b, strlen(b), toks, 100, &det) == JF_PARSE_NOMEM && det.n_audio == 0 && det.msid[0] == '\0', "token-starved detail");
    free(b);
}

static void test_detail_synthetic(void)
{
    char *b = slurp("detail.json");
    if (!b) { CHECK(0, "detail.json not found"); return; }

    /* SYNTHETIC: the real detail plus a PGS subtitle, an audio stream without a language and an external
     * text subtitle, all inserted at the start of MediaSources[0].MediaStreams. */
    char *c = splice_after(b, "\"MediaStreams\":[",
        "{\"Codec\":\"hdmv_pgs_subtitle\",\"Language\":\"eng\",\"Title\":\"PGS\",\"IsDefault\":false,\"IsForced\":false,"
        "\"IsExternal\":false,\"Index\":9,\"Type\":\"Subtitle\"},"
        "{\"Codec\":\"aac\",\"Title\":\"No language\",\"IsDefault\":false,\"IsForced\":false,\"IsExternal\":false,"
        "\"Channels\":2,\"Index\":8,\"Type\":\"Audio\"},"
        "{\"Codec\":\"srt\",\"Language\":\"ger\",\"IsDefault\":false,\"IsForced\":false,\"IsExternal\":true,"
        "\"Index\":10,\"Type\":\"Subtitle\"},");
    CHECK(c != NULL, "splice marker");
    if (c) {
        CHECK(parse_det(c) == JF_PARSE_OK, "synthetic detail");
        CHECK(det.n_audio == 3 && det.n_subs == 4, "audio %d subs %d", det.n_audio, det.n_subs);
        CHECK(det.subs[0].index == 9 && strncmp(det.subs[0].codec, "hdmv_pgs", 8) == 0 && !jv_sub_codec_text(det.subs[0].codec),
              "the image subtitle is stored, and jv_tracks knows it is not text");
        CHECK(det.audio[0].index == 8 && det.audio[0].lang[0] == '\0' && det.audio[0].channels == 2, "audio without language");
        CHECK(det.subs[1].index == 10 && det.subs[1].is_external && STREQ(det.subs[1].lang, "ger"), "external subtitle");
        int s = jv_pick_sub(det.subs, det.n_subs, JV_SUB_LANG, "eng", "hun", &det.audio[1]);
        CHECK(s == -1, "english subs exist only as PGS, so none is picked (got %d)", s);
        int a = jv_pick_audio(det.audio, det.n_audio, "hun");
        CHECK(a == 1, "a stream without a language never wins a language list: %d", a);
        free(c);
    }

    /* SYNTHETIC: UserData of a half-watched movie, no ImageTags, no MediaSources, no People */
    static const char bare[] =
        "{\"Name\":\"Bare\",\"Id\":\"" DETAIL_ID "\",\"Type\":\"Movie\",\"ProductionYear\":2001,\"RunTimeTicks\":36000000000,"
        "\"UserData\":{\"PlaybackPositionTicks\":9000000000,\"Played\":true,\"PlayCount\":2},\"CommunityRating\":6}";
    CHECK(parse_det(bare) == JF_PARSE_OK, "bare detail");
    CHECK(det.it.b.position_s == 900 && det.it.b.played && det.it.b.runtime_s == 3600 && det.community_x10 == 60,
          "resume %d runtime %d community %d", det.it.b.position_s, det.it.b.runtime_s, det.community_x10);
    CHECK(det.msid[0] == '\0' && det.n_audio == 0 && det.n_subs == 0 && det.n_people == 0 && det.it.b.image_tag[0] == '\0' &&
          det.width == 0, "nothing playable");

    /* SYNTHETIC: the item has no runtime but its source has; a comma list of containers */
    static const char src[] =
        "{\"Name\":\"S\",\"Id\":\"" DETAIL_ID "\",\"Type\":\"Episode\",\"IndexNumber\":4,\"ParentIndexNumber\":2,"
        "\"MediaSources\":[{\"Id\":\"5eed000000000000000000000000aaaa\",\"Container\":\"mov,mp4,m4a\",\"RunTimeTicks\":18000000000,"
        "\"MediaStreams\":[{\"Type\":\"Video\",\"Codec\":\"h264\",\"Index\":0,\"Width\":1280,\"Height\":720},"
        "{\"Type\":\"Audio\",\"Codec\":\"aac\",\"Index\":-1,\"Channels\":2},"
        "{\"Type\":\"Audio\",\"Codec\":\"aac\",\"Index\":1,\"Channels\":2,\"Language\":\"und\"}]},"
        "{\"Id\":\"5eed000000000000000000000000bbbb\",\"MediaStreams\":[{\"Type\":\"Audio\",\"Index\":7}]}]}";
    CHECK(parse_det(src) == JF_PARSE_OK, "source detail");
    CHECK(det.it.type == JV_EPISODE && det.it.episode_no == 4 && det.it.season_no == 2, "episode numbers in detail");
    CHECK(det.it.b.runtime_s == 1800, "runtime from the source: %d", det.it.b.runtime_s);
    CHECK(STREQ(det.msid, "5eed000000000000000000000000aaaa") && STREQ(det.container, "mov"), "first source only: '%s' '%s'", det.msid, det.container);
    CHECK(det.width == 1280 && det.height == 720, "picture from the video stream %dx%d", det.width, det.height);
    CHECK(det.n_audio == 1 && det.audio[0].index == 1 && STREQ(det.audio[0].lang, "und"), "a stream without an index is skipped; the second source is ignored (n=%d)", det.n_audio);

    /* SYNTHETIC: a bad id and a non-object */
    memset(&det, 0xAA, sizeof det);
    CHECK(parse_det("{\"Name\":\"x\",\"Id\":\"short\"}") == JF_PARSE_BAD && det.it.b.id[0] == '\0' && det.overview[0] == '\0', "bad id: zeroed");
    CHECK(parse_det("[1,2]") == JF_PARSE_BAD, "array root");
    CHECK(parse_det("") != JF_PARSE_OK, "empty body");

    /* SYNTHETIC: more streams than fit, a long multi-byte overview, a long genre list */
    static char big[16384];
    size_t o = 0;
    o += (size_t)snprintf(big + o, sizeof big - o, "{\"Name\":\"Big\",\"Id\":\"" DETAIL_ID "\",\"Type\":\"Movie\",\"Overview\":\"");
    for (int i = 0; i < 700; i++) o += (size_t)snprintf(big + o, sizeof big - o, "\xC5\x91");   /* o-double-acute, 2 bytes */
    o += (size_t)snprintf(big + o, sizeof big - o, "\",\"Genres\":[");
    for (int i = 0; i < 20; i++) o += (size_t)snprintf(big + o, sizeof big - o, "%s\"Genre%02d\"", i ? "," : "", i);
    o += (size_t)snprintf(big + o, sizeof big - o, "],\"MediaSources\":[{\"Id\":\"" DETAIL_ID "\",\"MediaStreams\":[");
    for (int i = 0; i < 20; i++) o += (size_t)snprintf(big + o, sizeof big - o, "%s{\"Type\":\"Audio\",\"Index\":%d,\"Codec\":\"ac3\"}", i ? "," : "", i);
    for (int i = 0; i < 30; i++) o += (size_t)snprintf(big + o, sizeof big - o, ",{\"Type\":\"Subtitle\",\"Index\":%d,\"Codec\":\"subrip\"}", 20 + i);
    snprintf(big + o, sizeof big - o, "]}]}");
    CHECK(parse_det(big) == JF_PARSE_OK, "big detail");
    CHECK(det.n_audio == JV_MAX_AUDIO && det.audio_dropped == 20 - JV_MAX_AUDIO && det.audio[JV_MAX_AUDIO - 1].index == JV_MAX_AUDIO - 1,
          "audio cap: %d kept, %d dropped", det.n_audio, det.audio_dropped);
    CHECK(det.n_subs == JV_MAX_SUBS && det.subs_dropped == 30 - JV_MAX_SUBS && det.subs[JV_MAX_SUBS - 1].index == 20 + JV_MAX_SUBS - 1,
          "subs cap: %d kept, %d dropped", det.n_subs, det.subs_dropped);
    CHECK(strlen(det.overview) > 1000 && strlen(det.overview) < sizeof det.overview && utf8_ok(det.overview),
          "overview cut on a character boundary (%zu bytes)", strlen(det.overview));
    CHECK(strlen(det.genres) < sizeof det.genres && det.genres[0] == 'G' && strstr(det.genres, ", Genre01") && !strstr(det.genres, "Genre19"),
          "genres cut at a whole name: '%s'", det.genres);
    free(b);
}

int main(void)
{
    test_movies();
    test_series_seasons_episodes();
    test_views_and_empty();
    test_synthetic_lists();
    test_detail_real();
    test_detail_synthetic();
    printf("jv_model_test: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
