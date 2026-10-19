/*
 * tests/json_test.c — the JSON tokenizer: a Jellyfin-shaped document end to end,
 * string unescaping and truncation, every malformed shape with its exact error,
 * the depth and token limits, and every prefix of the document under ASan.
 *
 * The prefix sweep parses a heap copy of EXACTLY n bytes for every n, so a read
 * one byte past `len` is a heap-buffer-overflow report rather than a lucky NUL
 * from a string literal.  Every strict prefix of a complete document must come
 * back JSON_ERR_PART — truncated, never "malformed".
 *
 * Build and run (host gcc, from native_apps/):
 *   gcc -Wall -Wextra -Wno-unused-parameter -I common -fsanitize=address,undefined \
 *       -o build/json_test tests/json_test.c common/json.c -lm && ./build/json_test
 */

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "json.h"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); \
    printf(__VA_ARGS__); printf("\n"); } } while (0)

#define MAXT 256

/* A realistic /Items reply: nested arrays of objects, negative and long numbers,
 * a null, bools, an escaped name, an empty array, and assorted whitespace. */
static const char DOC[] =
    "{\n"
    "  \"Items\": [\n"
    "    {\"Name\":\"Sehnsucht\",\"Id\":\"a1b2c3d4\",\"Album\":\"Herzeleid\","
    "\"AlbumArtist\":\"Rammstein\",\"NormalizationGain\":-8.3,"
    "\"RunTimeTicks\":1234567890123,\"IsFolder\":false,\"Overview\":null,\n"
    "     \"MediaSources\":[{\"Id\":\"ms1\",\"Container\":\"flac\",\"Size\":12345678},"
    "{\"Id\":\"ms2\",\"Container\":\"mp3\",\"Size\":3}]},\r\n"
    "    {\"Name\":\"Bj\\u00F6rk \\\"Live\\\"\",\"Id\":\"c3\",\"Album\":\"A\\u0027s\","
    "\"AlbumArtist\":\"B\",\"NormalizationGain\":2.1000004,"
    "\"RunTimeTicks\":9000000000,\"IsFolder\":true,\"Overview\":null,"
    "\"MediaSources\":[]}\t],\n"
    "  \"TotalRecordCount\": 2,\n"
    "  \"StartIndex\": 0\n"
    "}\n";
#define DOC_LEN (sizeof(DOC) - 1)

static int parse_lit(const char *s, size_t len, JsonTok *t, int max)
{
    /* Exact-size heap copy so ASan sees an over-read. */
    char *c = malloc(len ? len : 1);
    memcpy(c, s, len);
    int r = json_parse(c, len, t, max);
    free(c);
    return r;
}
#define PARSE(lit, t, max) parse_lit((lit), sizeof(lit) - 1, (t), (max))

/* Parse a one-string document and unescape its token; returns json_str's value. */
static int str_of(const char *lit, size_t len, char *out, size_t cap)
{
    JsonTok t[4];
    char *c = malloc(len);
    memcpy(c, lit, len);
    int n = json_parse(c, len, t, 4);
    int r = n == 1 ? json_str(c, &t[0], out, cap) : -99;
    free(c);
    return r;
}
#define STR_OF(lit, out, cap) str_of((lit), sizeof(lit) - 1, (out), (cap))

static int keyidx(const char *s, const JsonTok *t, int n, int obj, const char *key)
{
    return json_obj_get(s, t, n, obj, key);
}

static void test_document(void)
{
    JsonTok t[MAXT];
    int n = json_parse(DOC, DOC_LEN, t, MAXT);
    CHECK(n > 0, "document parses (got %d)", n);
    if (n <= 0) return;
    CHECK(t[0].type == JSON_OBJECT, "root is an object");
    CHECK(t[0].size == 3, "root has 3 pairs, got %d", t[0].size);
    CHECK(t[0].next == n, "root next == count (%d vs %d)", t[0].next, n);
    CHECK(t[0].start == 0 && t[0].end == (int)DOC_LEN - 1, "root span %d..%d", t[0].start, t[0].end);

    /* invariants for every token */
    for (int i = 0; i < n; i++) {
        CHECK(t[i].type != JSON_UNDEF, "tok %d has a type", i);
        CHECK(t[i].start <= t[i].end, "tok %d start<=end", i);
        CHECK(t[i].next > i && t[i].next <= n, "tok %d next %d in (i, n]", i, t[i].next);
        if (t[i].type != JSON_OBJECT && t[i].type != JSON_ARRAY)
            CHECK(t[i].next == i + 1 && t[i].size == 0, "tok %d scalar next/size", i);
        for (int j = i + 1; j < t[i].next && j < n; j++) {
            CHECK(t[j].start >= t[i].start && t[j].end <= t[i].end,
                  "tok %d inside tok %d span", j, i);
            CHECK(t[j].next <= t[i].next, "tok %d subtree inside tok %d subtree", j, i);
        }
    }

    int items = keyidx(DOC, t, n, 0, "Items");
    CHECK(items > 0 && t[items].type == JSON_ARRAY && t[items].size == 2, "Items is an array of 2");
    CHECK(keyidx(DOC, t, n, 0, "Nope") == -1, "absent key -> -1");
    CHECK(keyidx(DOC, t, n, 0, "Item") == -1, "key prefix does not match");
    CHECK(keyidx(DOC, t, n, 0, "Itemsx") == -1, "key extension does not match");
    CHECK(keyidx(DOC, t, n, 0, "") == -1, "empty key absent");
    CHECK(keyidx(DOC, t, n, items, "Name") == -1, "obj_get on an array -> -1");
    CHECK(keyidx(DOC, t, n, -1, "Items") == -1 && keyidx(DOC, t, n, n, "Items") == -1 &&
          keyidx(DOC, t, n, 1000, "Items") == -1, "obj_get bad index -> -1");
    CHECK(keyidx(NULL, t, n, 0, "Items") == -1 && keyidx(DOC, t, n, 0, NULL) == -1 &&
          keyidx(DOC, NULL, n, 0, "Items") == -1, "obj_get NULL args -> -1");

    long long cnt = -1;
    int tot = keyidx(DOC, t, n, 0, "TotalRecordCount");
    CHECK(tot > 0 && json_i64(DOC, &t[tot], &cnt) && cnt == 2, "TotalRecordCount == 2");
    int sti = keyidx(DOC, t, n, 0, "StartIndex");
    CHECK(sti > 0 && json_i64(DOC, &t[sti], &cnt) && cnt == 0, "StartIndex == 0");

    /* iterate the array */
    int seen = 0;
    int e = json_arr_first(t, n, items);
    char buf[64];
    while (e >= 0) {
        CHECK(t[e].type == JSON_OBJECT, "item %d is an object", seen);
        int nm = keyidx(DOC, t, n, e, "Name");
        int rc = nm > 0 ? json_str(DOC, &t[nm], buf, sizeof buf) : -1;
        if (seen == 0) {
            CHECK(rc == 9 && strcmp(buf, "Sehnsucht") == 0, "item0 Name '%s' rc=%d", buf, rc);
            int al = keyidx(DOC, t, n, e, "Album");
            CHECK(al > 0 && json_str(DOC, &t[al], buf, sizeof buf) == 9 && strcmp(buf, "Herzeleid") == 0, "item0 Album");
            int aa = keyidx(DOC, t, n, e, "AlbumArtist");
            CHECK(aa > 0 && json_str(DOC, &t[aa], buf, sizeof buf) == 9 && strcmp(buf, "Rammstein") == 0, "item0 AlbumArtist");
            int id = keyidx(DOC, t, n, e, "Id");
            CHECK(id > 0 && json_str(DOC, &t[id], buf, sizeof buf) == 8 && strcmp(buf, "a1b2c3d4") == 0, "item0 Id");
            double g = 0;
            int ng = keyidx(DOC, t, n, e, "NormalizationGain");
            CHECK(ng > 0 && json_num(DOC, &t[ng], &g) && fabs(g - -8.3) < 1e-12, "item0 gain %g", g);
            long long tk = 0;
            CHECK(ng > 0 && !json_i64(DOC, &t[ng], &tk), "gain is not an integer");
            int rt = keyidx(DOC, t, n, e, "RunTimeTicks");
            CHECK(rt > 0 && json_i64(DOC, &t[rt], &tk) && tk == 1234567890123LL, "ticks exact %lld", tk);
            double td = 0;
            CHECK(rt > 0 && json_num(DOC, &t[rt], &td) && td == 1234567890123.0, "ticks as double");
            bool b = true;
            int fo = keyidx(DOC, t, n, e, "IsFolder");
            CHECK(fo > 0 && json_bool(DOC, &t[fo], &b) && b == false, "IsFolder false");
            int ov = keyidx(DOC, t, n, e, "Overview");
            CHECK(ov > 0 && json_is_null(&t[ov]), "Overview null");
            CHECK(ov > 0 && !json_bool(DOC, &t[ov], &b), "null is not a bool");
            CHECK(ov > 0 && json_str(DOC, &t[ov], buf, sizeof buf) == -1, "null is not a string");
            int ms = keyidx(DOC, t, n, e, "MediaSources");
            CHECK(ms > 0 && t[ms].type == JSON_ARRAY && t[ms].size == 2, "MediaSources of 2");
            int m1 = json_arr_get(t, n, ms, 1);
            int ct = m1 > 0 ? keyidx(DOC, t, n, m1, "Container") : -1;
            CHECK(ct > 0 && json_str(DOC, &t[ct], buf, sizeof buf) == 3 && strcmp(buf, "mp3") == 0, "ms[1] Container mp3");
            int sz = m1 > 0 ? keyidx(DOC, t, n, m1, "Size") : -1;
            CHECK(sz > 0 && json_i64(DOC, &t[sz], &tk) && tk == 3, "ms[1] Size 3");
            CHECK(json_arr_get(t, n, ms, 2) == -1 && json_arr_get(t, n, ms, -1) == -1, "arr_get out of range");
            int m0 = json_arr_get(t, n, ms, 0);
            CHECK(m0 == json_arr_first(t, n, ms), "arr_get 0 == first");
            CHECK(json_arr_next(t, n, ms, m1) == -1, "no sibling after the last element");
            CHECK(json_arr_next(t, n, ms, m0) == m1, "next of ms[0] is ms[1]");
            /* skip via next: the item's subtree ends exactly where item 1 starts */
            CHECK(t[e].next == json_arr_next(t, n, items, e), "item0 next is item1");
            /* iterate the pairs of item 0 by hand */
            int pairs = 0, i = e + 1;
            while (pairs < t[e].size) { CHECK(t[i].type == JSON_STRING, "key %d is a string", pairs); i = t[i + 1].next; pairs++; }
            CHECK(i == t[e].next, "pair walk lands on next (%d vs %d)", i, t[e].next);
            CHECK(pairs == 9, "item0 has 9 pairs, got %d", pairs);
        } else {
            CHECK(rc == 13 && strcmp(buf, "Bj\xC3\xB6rk \"Live\"") == 0, "item1 Name '%s' rc=%d", buf, rc);
            int al = keyidx(DOC, t, n, e, "Album");
            CHECK(al > 0 && json_str(DOC, &t[al], buf, sizeof buf) == 3 && strcmp(buf, "A's") == 0, "item1 Album A's");
            double g = 0;
            int ng = keyidx(DOC, t, n, e, "NormalizationGain");
            CHECK(ng > 0 && json_num(DOC, &t[ng], &g) && fabs(g - 2.1000004) < 1e-12, "item1 gain %.9g", g);
            bool b = false;
            int fo = keyidx(DOC, t, n, e, "IsFolder");
            CHECK(fo > 0 && json_bool(DOC, &t[fo], &b) && b == true, "IsFolder true");
            int ms = keyidx(DOC, t, n, e, "MediaSources");
            CHECK(ms > 0 && t[ms].size == 0 && json_arr_first(t, n, ms) == -1, "empty MediaSources");
            CHECK(t[ms].next == ms + 1, "empty array next");
            CHECK(json_arr_next(t, n, items, e) == -1, "item1 is the last");
        }
        seen++;
        e = json_arr_next(t, n, items, e);
    }
    CHECK(seen == 2, "iterated %d items", seen);

    /* root pair walk and skip */
    int pairs = 0, i = 1;
    while (pairs < t[0].size) { i = t[i + 1].next; pairs++; }
    CHECK(i == n, "root pair walk lands on count");
    CHECK(t[items].next == tot - 1, "Items subtree ends at the next key (%d vs %d)", t[items].next, tot - 1);

    /* helpers on wrong types */
    long long ll; double dd; bool bb;
    CHECK(!json_i64(DOC, &t[0], &ll) && !json_num(DOC, &t[0], &dd) && !json_bool(DOC, &t[0], &bb),
          "scalar getters refuse an object");
    CHECK(!json_is_null(NULL) && !json_is_null(&t[0]), "is_null false for object/NULL");
    CHECK(json_arr_first(t, n, 0) == -1 && json_arr_first(t, n, -1) == -1 && json_arr_first(t, n, n) == -1,
          "arr_first on object/bad index");
    CHECK(json_arr_next(t, n, items, items) == -1 && json_arr_next(t, n, items, n) == -1,
          "arr_next with elem==arr or bad index");
}

static void test_prefixes(void)
{
    JsonTok t[MAXT];
    for (size_t k = 0; k < DOC_LEN - 1; k++) {  /* the last byte is a newline after the value */
        int r = parse_lit(DOC, k, t, MAXT);
        CHECK(r == JSON_ERR_PART, "prefix %zu returned %d, want PART", k, r);
    }
    int r = parse_lit(DOC, DOC_LEN, t, MAXT);
    CHECK(r > 0, "full document parses");
    /* A tiny token budget: never a crash, never a count, whatever the cut. */
    for (size_t k = 0; k <= DOC_LEN; k++) {
        r = parse_lit(DOC, k, t, 4);
        CHECK(r < 0, "prefix %zu with 4 tokens returned %d", k, r);
    }
    for (size_t k = 0; k <= DOC_LEN; k++) parse_lit(DOC, k, NULL, 0);
}

static void test_strings(void)
{
    char o[64];
    int r;
    r = STR_OF("\"\\u00F6\"", o, sizeof o);
    CHECK(r == 2 && memcmp(o, "\xC3\xB6", 3) == 0, "u00F6 -> C3 B6 (r=%d)", r);
    r = STR_OF("\"\\u0027\"", o, sizeof o);
    CHECK(r == 1 && strcmp(o, "'") == 0, "u0027 -> apostrophe");
    r = STR_OF("\"\\u20AC\"", o, sizeof o);
    CHECK(r == 3 && memcmp(o, "\xE2\x82\xAC", 4) == 0, "u20AC -> E2 82 AC");
    r = STR_OF("\"\\u00f6\"", o, sizeof o);
    CHECK(r == 2 && memcmp(o, "\xC3\xB6", 3) == 0, "lowercase hex");
    r = STR_OF("\"\\uD83D\\uDE00\"", o, sizeof o);
    CHECK(r == 4 && memcmp(o, "\xF0\x9F\x98\x80", 5) == 0, "surrogate pair -> F0 9F 98 80 (r=%d)", r);
    r = STR_OF("\"\\uD83D\"", o, sizeof o);
    CHECK(r == 3 && memcmp(o, "\xEF\xBF\xBD", 4) == 0, "lone high surrogate -> FFFD");
    r = STR_OF("\"\\uDE00\"", o, sizeof o);
    CHECK(r == 3 && memcmp(o, "\xEF\xBF\xBD", 4) == 0, "lone low surrogate -> FFFD");
    r = STR_OF("\"\\uDE00\\uD83D\"", o, sizeof o);
    CHECK(r == 6 && memcmp(o, "\xEF\xBF\xBD\xEF\xBF\xBD", 7) == 0, "misordered pair -> two FFFD (r=%d)", r);
    r = STR_OF("\"\\uD83Dx\"", o, sizeof o);
    CHECK(r == 4 && memcmp(o, "\xEF\xBF\xBDx", 5) == 0, "high surrogate then plain char");
    r = STR_OF("\"\\uD83D\\u0041\"", o, sizeof o);
    CHECK(r == 4 && memcmp(o, "\xEF\xBF\xBD" "A", 5) == 0,
          "high surrogate then \\u0041 -> FFFD, A (r=%d)", r);
    r = STR_OF("\"\\uD83D\\uD83D\\uDE00\"", o, sizeof o);
    CHECK(r == 7 && memcmp(o, "\xEF\xBF\xBD\xF0\x9F\x98\x80", 8) == 0, "high, then a valid pair");
    r = STR_OF("\"a\\u0000b\"", o, sizeof o);
    CHECK(r == 5 && memcmp(o, "a\xEF\xBF\xBD" "b", 6) == 0, "u0000 -> FFFD, string stays whole");
    r = STR_OF("\"a\\n\\t\\\"\\\\\\/\\b\\f\\rz\"", o, sizeof o);
    CHECK(r == 10 && memcmp(o, "a\n\t\"\\/\b\f\rz", 11) == 0, "simple escapes (r=%d)", r);
    r = STR_OF("\"\xC3\xA9t\xC3\xA9\"", o, sizeof o);
    CHECK(r == 5 && memcmp(o, "\xC3\xA9t\xC3\xA9", 6) == 0, "raw UTF-8 passes through");
    r = STR_OF("\"\"", o, sizeof o);
    CHECK(r == 0 && o[0] == '\0', "empty string");

    /* truncation */
    char big[32];
    r = STR_OF("\"ab\\u00F6cd\"", o, 4);               /* needs 6 */
    CHECK(r == 6 && strcmp(o, "ab") == 0, "cut before a 2-byte char: '%s' r=%d", o, r);
    r = STR_OF("\"ab\\u00F6cd\"", o, 5);
    CHECK(r == 6 && strcmp(o, "ab\xC3\xB6") == 0, "cut after it, 'c' not written (r=%d)", r);
    r = STR_OF("\"ab\\u00F6cd\"", o, 6);
    CHECK(r == 6 && strcmp(o, "ab\xC3\xB6" "c") == 0, "size 6 holds 5 + NUL");
    r = STR_OF("\"ab\\u00F6cd\"", o, 7);
    CHECK(r == 6 && strcmp(o, "ab\xC3\xB6" "cd") == 0, "size 7 holds it all, r < size");
    r = STR_OF("\"ab\\u00F6cd\"", o, 3);
    CHECK(r == 6 && strcmp(o, "ab") == 0, "size 3");
    r = STR_OF("\"\\u00F6a\"", o, 2);
    CHECK(r == 3 && o[0] == '\0', "later short char does not leapfrog a long one (r=%d)", r);
    r = STR_OF("\"x\\uD83D\\uDE00\"", o, 4);
    CHECK(r == 5 && strcmp(o, "x") == 0, "4-byte char dropped whole at size 4");
    r = STR_OF("\"x\\uD83D\\uDE00\"", o, 5);
    CHECK(r == 5 && strcmp(o, "x") == 0, "4-byte char dropped whole at size 5 (no room for NUL)");
    r = STR_OF("\"x\\uD83D\\uDE00\"", o, 6);
    CHECK(r == 5 && strcmp(o, "x\xF0\x9F\x98\x80") == 0, "4-byte char fits at size 6");
    r = STR_OF("\"\xC3\xA9\xC3\xA9\"", o, 4);          /* raw: two chars, 4 bytes */
    CHECK(r == 4 && strcmp(o, "\xC3\xA9") == 0, "raw 2-byte char is not split (r=%d)", r);
    memset(o, 'Z', sizeof o);
    r = STR_OF("\"abc\"", o, 1);
    CHECK(r == 3 && o[0] == '\0' && o[1] == 'Z', "size 1: just the NUL, nothing else touched");
    memset(o, 'Z', sizeof o);
    r = STR_OF("\"abc\"", o, 0);
    CHECK(r == 3 && o[0] == 'Z', "size 0 with a buffer: untouched");
    r = STR_OF("\"abc\"", NULL, 0);
    CHECK(r == 3, "NULL/0 measures");
    r = STR_OF("\"a\\u00F6\\uD83D\\uDE00\"", NULL, 0);
    CHECK(r == 7, "measure with escapes: %d", r);
    r = STR_OF("\"a\\u00F6\\uD83D\\uDE00\"", big, sizeof big);
    CHECK(r == 7 && (int)strlen(big) == r, "measure equals what an ample buffer holds");
    /* every size 0..8: NUL-terminated, valid prefix, never partial UTF-8 */
    for (size_t cap = 1; cap <= 9; cap++) {
        memset(big, 'Z', sizeof big);
        r = STR_OF("\"a\\u00F6\\uD83D\\uDE00\"", big, cap);
        size_t L = strlen(big);
        CHECK(r == 7 && L < cap, "cap %zu: strlen %zu", cap, L);
        int ok = 1;
        for (size_t i = 0; i < L;) {
            unsigned char c = (unsigned char)big[i];
            size_t need = c < 0x80 ? 1 : c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : 2;
            if (i + need > L) ok = 0;
            i += need;
        }
        CHECK(ok, "cap %zu: no partial UTF-8 sequence", cap);
        CHECK(memcmp(big, "a\xC3\xB6\xF0\x9F\x98\x80", L) == 0, "cap %zu: output is a prefix", cap);
    }

    /* errors from json_str */
    JsonTok num = { JSON_NUMBER, 0, 1, 0, 1 };
    memset(o, 'Z', sizeof o);
    CHECK(json_str("1", &num, o, sizeof o) == -1 && o[0] == '\0', "not a string -> -1, empty out");
    CHECK(json_str("1", NULL, o, sizeof o) == -1 && json_str(NULL, &num, o, sizeof o) == -1, "NULL args");
    JsonTok bad = { JSON_STRING, 0, 2, 0, 1 };
    CHECK(json_str("\\q", &bad, o, sizeof o) == -1 && o[0] == '\0', "invalid escape -> -1");
    JsonTok bad2 = { JSON_STRING, 0, 5, 0, 1 };
    CHECK(json_str("\\u12G4", &bad2, o, sizeof o) == -1, "bad hex -> -1");
    JsonTok bad3 = { JSON_STRING, 0, 4, 0, 1 };
    CHECK(json_str("\\u12", &bad3, o, sizeof o) == -1, "short \\u at span end -> -1");
    JsonTok bad4 = { JSON_STRING, 0, 1, 0, 1 };
    CHECK(json_str("\\", &bad4, o, sizeof o) == -1, "lone backslash -> -1");
    memcpy(o, "ab\\q", 5);
    JsonTok bad5 = { JSON_STRING, 0, 4, 0, 1 };
    CHECK(json_str(o, &bad5, o, sizeof o) == -1 && o[0] == '\0', "invalid escape after output -> out emptied");
}

static void test_numbers(void)
{
    JsonTok t[4];
    long long v;
    double d;
    CHECK(PARSE("12345678901234", t, 4) == 1 && json_i64("12345678901234", &t[0], &v) && v == 12345678901234LL, "i64 14 digits");
    CHECK(PARSE("9223372036854775807", t, 4) == 1 && json_i64("9223372036854775807", &t[0], &v) && v == LLONG_MAX, "i64 max");
    CHECK(PARSE("9223372036854775808", t, 4) == 1 && !json_i64("9223372036854775808", &t[0], &v), "i64 max+1 refused");
    CHECK(PARSE("-9223372036854775808", t, 4) == 1 && json_i64("-9223372036854775808", &t[0], &v) && v == LLONG_MIN, "i64 min");
    CHECK(PARSE("-9223372036854775809", t, 4) == 1 && !json_i64("-9223372036854775809", &t[0], &v), "i64 min-1 refused");
    CHECK(PARSE("99999999999999999999999", t, 4) == 1 && !json_i64("99999999999999999999999", &t[0], &v), "i64 huge refused");
    CHECK(PARSE("-0", t, 4) == 1 && json_i64("-0", &t[0], &v) && v == 0, "-0");
    CHECK(PARSE("12.0", t, 4) == 1 && !json_i64("12.0", &t[0], &v) && json_num("12.0", &t[0], &d) && d == 12.0, "12.0 is no integer");
    CHECK(PARSE("1e3", t, 4) == 1 && !json_i64("1e3", &t[0], &v) && json_num("1e3", &t[0], &d) && d == 1000.0, "1e3 is no integer");
    CHECK(PARSE("1E+2", t, 4) == 1 && json_num("1E+2", &t[0], &d) && d == 100.0, "1E+2");
    CHECK(PARSE("0.5e-3", t, 4) == 1 && json_num("0.5e-3", &t[0], &d) && fabs(d - 0.0005) < 1e-15, "0.5e-3");
    CHECK(PARSE("-0.0", t, 4) == 1 && json_num("-0.0", &t[0], &d) && d == 0.0, "-0.0");
    /* the parser accepts an out-of-range exponent; json_num must not hand back inf */
    CHECK(PARSE("1e999", t, 4) == 1 && !json_num("1e999", &t[0], &d), "1e999 is not finite, refused");
    CHECK(PARSE("-1e999", t, 4) == 1 && !json_num("-1e999", &t[0], &d), "-1e999 is not finite, refused");
    CHECK(PARSE("1.7976931348623157e308", t, 4) == 1 && json_num("1.7976931348623157e308", &t[0], &d) && isfinite(d),
          "DBL_MAX still accepted");
    CHECK(PARSE("  7  ", t, 4) == 1 && t[0].start == 2 && t[0].end == 3, "number span inside whitespace");
    /* a 64-char token is refused by json_num, accepted by the parser */
    char longnum[70];
    memset(longnum, '1', 64); longnum[64] = '\0';
    CHECK(parse_lit(longnum, 64, t, 4) == 1 && !json_num(longnum, &t[0], &d), "64-byte number refused by json_num");
    CHECK(parse_lit(longnum, 63, t, 4) == 1 && json_num(longnum, &t[0], &d), "63-byte number accepted");
    bool b;
    CHECK(PARSE("true", t, 4) == 1 && json_bool("true", &t[0], &b) && b, "true");
    CHECK(PARSE("false", t, 4) == 1 && json_bool("false", &t[0], &b) && !b, "false");
    CHECK(PARSE("null", t, 4) == 1 && json_is_null(&t[0]) && t[0].type == JSON_NULL, "null");
    CHECK(PARSE("\"\"", t, 4) == 1 && t[0].type == JSON_STRING && t[0].start == 1 && t[0].end == 1, "empty string span");
}

typedef struct { const char *text; size_t len; int want; } Case;
#define C(lit, want) { (lit), sizeof(lit) - 1, (want) }

static void test_grammar(void)
{
    static const Case cases[] = {
        /* valid */
        C("{}", 1), C("[]", 1), C("{ }", 1), C("[ ]", 1), C(" \t\r\n[1]\n", 2), C("\"\\/\"", 1),
        C("[true,false,null,0,-1,1.5,\"x\",[],{}]", 10), C("{\"a\":{\"b\":[1,2]}}", 7),
        C("1E5", 1), C("[\"\\u0041\"]", 2),
        /* truncated: always PART */
        C("", JSON_ERR_PART), C("   \n\t", JSON_ERR_PART),
        C("\"abc", JSON_ERR_PART), C("[\"a", JSON_ERR_PART), C("\"\\", JSON_ERR_PART),
        C("\"\\u12", JSON_ERR_PART), C("\"\\u", JSON_ERR_PART),
        C("[", JSON_ERR_PART), C("{", JSON_ERR_PART), C("[1", JSON_ERR_PART), C("[1,", JSON_ERR_PART),
        C("{\"a\"", JSON_ERR_PART), C("{\"a\":", JSON_ERR_PART), C("{\"a\":1", JSON_ERR_PART),
        C("{\"a\":1,", JSON_ERR_PART), C("[[[]]", JSON_ERR_PART), C("{\"a\":[1,2", JSON_ERR_PART),
        C("tru", JSON_ERR_PART), C("fals", JSON_ERR_PART), C("nul", JSON_ERR_PART), C("t", JSON_ERR_PART),
        C("-", JSON_ERR_PART), C("1.", JSON_ERR_PART), C("1e", JSON_ERR_PART), C("1e+", JSON_ERR_PART),
        C("[1.", JSON_ERR_PART), C("[-", JSON_ERR_PART),
        /* malformed */
        C("[1,]", JSON_ERR_INVAL), C("{\"a\":1,}", JSON_ERR_INVAL), C("[,1]", JSON_ERR_INVAL),
        C("[1,,2]", JSON_ERR_INVAL), C("{,}", JSON_ERR_INVAL), C("{\"a\" 1}", JSON_ERR_INVAL),
        C("{\"a\"}", JSON_ERR_INVAL), C("{\"a\":}", JSON_ERR_INVAL), C("{1:2}", JSON_ERR_INVAL),
        C("{true:1}", JSON_ERR_INVAL), C("{\"a\":1 \"b\":2}", JSON_ERR_INVAL), C("[1 2]", JSON_ERR_INVAL),
        C("{\"a\"::1}", JSON_ERR_INVAL), C("{'a':1}", JSON_ERR_INVAL), C("'a'", JSON_ERR_INVAL),
        C("[\"\\q\"]", JSON_ERR_INVAL), C("\"\\x41\"", JSON_ERR_INVAL), C("\"\\U0041\"", JSON_ERR_INVAL),
        C("\"\\u12G4\"", JSON_ERR_INVAL), C("\"\\u12\"", JSON_ERR_INVAL), C("\"\\uZZZZ\"", JSON_ERR_INVAL),
        C("\"a\nb\"", JSON_ERR_INVAL), C("\"a\tb\"", JSON_ERR_INVAL), C("\"a\x01" "b\"", JSON_ERR_INVAL),
        C("\"a\x1F\"", JSON_ERR_INVAL), C("\"a\0b\"", JSON_ERR_INVAL),
        C("01", JSON_ERR_INVAL), C("-01", JSON_ERR_INVAL), C("[00]", JSON_ERR_INVAL), C("+1", JSON_ERR_INVAL),
        C(".5", JSON_ERR_INVAL), C("1.x", JSON_ERR_INVAL), C("1.e5", JSON_ERR_INVAL), C("-x", JSON_ERR_INVAL),
        C("0x1", JSON_ERR_INVAL), C("1e+x", JSON_ERR_INVAL), C("1ex", JSON_ERR_INVAL), C("--1", JSON_ERR_INVAL),
        C("NaN", JSON_ERR_INVAL), C("Infinity", JSON_ERR_INVAL), C("-Infinity", JSON_ERR_INVAL),
        C("True", JSON_ERR_INVAL), C("nulL", JSON_ERR_INVAL), C("trux", JSON_ERR_INVAL),
        C("[tru,1]", JSON_ERR_INVAL), C("abc", JSON_ERR_INVAL), C("undefined", JSON_ERR_INVAL),
        C("1 2", JSON_ERR_INVAL), C("{} x", JSON_ERR_INVAL), C("[]]", JSON_ERR_INVAL), C("{}}", JSON_ERR_INVAL),
        C("[] []", JSON_ERR_INVAL), C("truefalse", JSON_ERR_INVAL), C("1\"a\"", JSON_ERR_INVAL),
        C("[}", JSON_ERR_INVAL), C("{]", JSON_ERR_INVAL), C("[1}", JSON_ERR_INVAL), C("{\"a\":1]", JSON_ERR_INVAL),
        C("\f1", JSON_ERR_INVAL), C("\v1", JSON_ERR_INVAL), C("\xEF\xBB\xBF[]", JSON_ERR_INVAL),
        C("//c\n1", JSON_ERR_INVAL), C(")", JSON_ERR_INVAL),
    };
    JsonTok t[MAXT];
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        int r = parse_lit(cases[i].text, cases[i].len, t, MAXT);
        CHECK(r == cases[i].want, "case %zu <%.*s>: got %d want %d", i, (int)cases[i].len, cases[i].text, r, cases[i].want);
    }
    /* trailing-garbage control: the same valid text parses with only whitespace after it */
    CHECK(PARSE("[1] \r\n\t ", t, MAXT) == 2, "trailing whitespace is fine");
    /* bad arguments */
    CHECK(json_parse(NULL, 0, t, MAXT) == JSON_ERR_INVAL, "NULL text");
    CHECK(json_parse("[]", (size_t)INT_MAX + 1u, t, MAXT) == JSON_ERR_INVAL, "len > INT_MAX");
    CHECK(json_parse("[]", 2, NULL, 5) == JSON_ERR_INVAL, "NULL toks with max>0");
    CHECK(json_parse("[]", 2, t, -1) == JSON_ERR_INVAL, "negative max");
    CHECK(json_parse("[]", 2, NULL, 0) == JSON_ERR_NOMEM, "valid doc, no tokens -> NOMEM");
    CHECK(json_parse("[", 1, NULL, 0) == JSON_ERR_NOMEM || json_parse("[", 1, NULL, 0) == JSON_ERR_PART,
          "truncated doc with no tokens is an error");
}

static void test_limits(void)
{
    JsonTok t[MAXT];
    char d[200];
    for (int depth = 1; depth <= JSON_MAX_DEPTH + 1; depth++) {
        int k = 0;
        for (int i = 0; i < depth; i++) d[k++] = '[';
        for (int i = 0; i < depth; i++) d[k++] = ']';
        int r = parse_lit(d, (size_t)k, t, MAXT);
        if (depth <= JSON_MAX_DEPTH) CHECK(r == depth, "depth %d parses to %d tokens (got %d)", depth, depth, r);
        else CHECK(r == JSON_ERR_DEPTH, "depth %d -> DEPTH (got %d)", depth, r);
    }
    /* 33 unterminated opens: the limit is reported, not PART */
    memset(d, '[', 33);
    CHECK(parse_lit(d, 33, t, MAXT) == JSON_ERR_DEPTH, "33 opens -> DEPTH");
    CHECK(parse_lit(d, 32, t, MAXT) == JSON_ERR_PART, "32 opens -> PART");
    /* objects count too, and mixed nesting */
    char o[400];
    int k = 0;
    for (int i = 0; i < 16; i++) { memcpy(o + k, "{\"a\":[", 6); k += 6; }
    CHECK(parse_lit(o, (size_t)k, t, MAXT) == JSON_ERR_PART, "32 mixed opens -> PART");
    memcpy(o + k, "{\"a\":1}", 7);
    CHECK(parse_lit(o, (size_t)k + 7, t, MAXT) == JSON_ERR_DEPTH, "33rd mixed container -> DEPTH");
    /* a deep bomb must not blow the stack */
    char *bomb = malloc(200000);
    memset(bomb, '[', 200000);
    CHECK(parse_lit(bomb, 200000, t, MAXT) == JSON_ERR_DEPTH, "200000 opens -> DEPTH");
    free(bomb);

    /* token budget */
    int n = json_parse(DOC, DOC_LEN, t, MAXT);
    CHECK(n > 0, "doc parses for the budget test");
    JsonTok *exact = malloc((size_t)n * sizeof *exact);
    JsonTok *under = malloc((size_t)(n - 1) * sizeof *under);
    CHECK(json_parse(DOC, DOC_LEN, exact, n) == n, "max_toks == count -> ok");
    CHECK(memcmp(exact, t, (size_t)n * sizeof *exact) == 0, "exact-fit tokens equal the roomy parse");
    CHECK(json_parse(DOC, DOC_LEN, under, n - 1) == JSON_ERR_NOMEM, "max_toks == count-1 -> NOMEM");
    CHECK(json_parse(DOC, DOC_LEN, exact, 1) == JSON_ERR_NOMEM, "max_toks 1 -> NOMEM");
    CHECK(json_parse(DOC, DOC_LEN, t, 0) == JSON_ERR_NOMEM, "max_toks 0 -> NOMEM");
    for (int m = 0; m < n; m++) {
        JsonTok *tt = malloc((m ? (size_t)m : 1) * sizeof *tt);
        CHECK(json_parse(DOC, DOC_LEN, tt, m) == JSON_ERR_NOMEM, "budget %d -> NOMEM", m);
        free(tt);
    }
    free(exact);
    free(under);
}

/* json_count() must agree with json_parse() on every document: the count when
 * valid, the same negative code when not, and the count is exactly the token
 * budget json_parse() needs (count-1 is NOMEM, count is enough). */
static int count_lit(const char *s, size_t len)
{
    char *c = malloc(len ? len : 1);
    memcpy(c, s, len);
    int r = json_count(c, len);
    free(c);
    return r;
}

static void test_count(void)
{
    static const char *docs[] = {
        "{}",
        "[]",
        "\"s\"",
        "  -12.5e3  ",
        "[[],{},[1,[2]]]",
        "{\"a\":{\"b\":[true,null,-1.5e3]},\"c\":[]}",
        NULL
    };
    JsonTok t[MAXT];
    for (int i = 0; docs[i]; i++) {
        size_t len = strlen(docs[i]);
        int want = parse_lit(docs[i], len, t, MAXT);
        int got = count_lit(docs[i], len);
        CHECK(want > 0, "doc %d parses (%d)", i, want);
        CHECK(got == want, "doc %d: count %d, parse %d", i, got, want);
        CHECK(parse_lit(docs[i], len, t, got) == got, "doc %d: count tokens are enough", i);
        if (got > 1)
            CHECK(parse_lit(docs[i], len, t, got - 1) == JSON_ERR_NOMEM, "doc %d: count-1 is NOMEM", i);
    }
    int big = json_count(DOC, DOC_LEN);
    CHECK(big == json_parse(DOC, DOC_LEN, t, MAXT), "Jellyfin document: count == parse (%d)", big);
    CHECK(count_lit("[1,]", 4) == JSON_ERR_INVAL, "[1,] -> INVAL");
    CHECK(count_lit("{\"a\" 1}", 8) == JSON_ERR_INVAL, "missing colon -> INVAL");
    CHECK(count_lit("[1,2", 4) == JSON_ERR_PART, "truncated -> PART");
    CHECK(count_lit("", 0) == JSON_ERR_PART, "empty -> PART");
    CHECK(json_count(NULL, 1) == JSON_ERR_INVAL, "NULL text -> INVAL");
    CHECK(json_count("1", (size_t)INT_MAX + 1) == JSON_ERR_INVAL, "len above INT_MAX -> INVAL");
    char *o = malloc(80);
    int k = 0;
    for (int i = 0; i < 32; i++) { o[k++] = '['; }
    o[k++] = '1';
    for (int i = 0; i < 32; i++) { o[k++] = ']'; }
    CHECK(count_lit(o, (size_t)k) == 33, "32 nested arrays -> 33 tokens");
    CHECK(json_count(o, (size_t)k - 1) == JSON_ERR_PART, "truncated nesting -> PART");
    free(o);
    char *bomb = malloc(200000);
    memset(bomb, '[', 200000);
    CHECK(count_lit(bomb, 200000) == JSON_ERR_DEPTH, "depth bomb -> DEPTH");
    free(bomb);
}

int main(void)
{
    test_document();
    test_prefixes();
    test_strings();
    test_numbers();
    test_grammar();
    test_limits();
    test_count();
    printf("json_test: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
