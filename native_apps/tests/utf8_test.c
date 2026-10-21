/* Host regression for common/utf8.h (header-only; no objects to link).
 *
 *   cd native_apps && gcc -Wall -Wextra -Wno-unused-parameter -I common \
 *       -o build/utf8_test tests/utf8_test.c && ./build/utf8_test
 *
 * Valid 1-4 byte forms decode; every malformed class returns U+FFFD, consumes
 * exactly one byte and never reads past the NUL (a poison continuation byte is
 * placed after the terminator); the cut/prefix/prev helpers never land inside
 * a sequence for any offset of a mixed string.  Negative control: the same
 * malformed-input checks run against a naive decoder that accepts anything
 * above 0x7F as one byte, and must FAIL there.
 */
#include <stdio.h>
#include <string.h>
#include "utf8.h"

typedef uint32_t (*next_fn)(const char **);

static uint32_t naive_next(const char **pp) {      /* the wrong decoder */
    unsigned char c = (unsigned char)**pp;
    if (!c) return 0;
    (*pp)++;
    return c;
}

static uint32_t real_next(const char **pp) { return utf8_next(pp); }

/* returns the number of failed checks; quiet when verbose == 0 */
static int check_decoder(next_fn nx, int verbose) {
    int fails = 0;
#define EXPECT(cond, what) do { if (!(cond)) { fails++; if (verbose) printf("  FAIL %s\n", what); } } while (0)
    struct { const char *s; uint32_t cp; int len; const char *name; } good[] = {
        { "A", 0x41, 1, "ascii" }, { "\xC3\xA9", 0xE9, 2, "e-acute" },
        { "\xC5\x91", 0x151, 2, "o double acute" }, { "\xE2\x82\xAC", 0x20AC, 3, "euro" },
        { "\xE2\x80\xA6", 0x2026, 3, "ellipsis" }, { "\xF0\x9F\x98\x80", 0x1F600, 4, "emoji" },
        { "\xF0\x90\x80\x80", 0x10000, 4, "U+10000" }, { "\xF4\x8F\xBF\xBF", 0x10FFFF, 4, "U+10FFFF" },
        { "\xED\x9F\xBF", 0xD7FF, 3, "last before surrogates" }, { "\xEE\x80\x80", 0xE000, 3, "first after surrogates" },
    };
    for (unsigned i = 0; i < sizeof good / sizeof good[0]; i++) {
        const char *p = good[i].s;
        uint32_t cp = nx(&p);
        EXPECT(cp == good[i].cp && p - good[i].s == good[i].len, good[i].name);
    }
    const char *bad[] = {
        "\x80", "\xBF", "\xC0\x80", "\xC1\xBF", "\xE0\x80\x80", "\xE0\x9F\xBF", "\xED\xA0\x80",
        "\xF0\x80\x80\x80", "\xF4\x90\x80\x80", "\xF5\x80\x80\x80", "\xF8\x88\x80\x80\x80", "\xFF",
        "\xC3",            /* truncated by NUL */
        "\xE2\x82",        /* truncated by NUL */
        "\xF0\x9F\x98",    /* truncated by NUL */
        "\xC3\x41",        /* lead followed by ASCII */
        "\xE2\x82\x41",
    };
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        char buf[16];                                   /* poison AFTER the NUL */
        size_t n = strlen(bad[i]);
        memcpy(buf, bad[i], n); buf[n] = 0;
        memset(buf + n + 1, 0xA9, sizeof buf - n - 1);
        const char *p = buf;
        uint32_t cp = nx(&p);
        EXPECT(cp == 0xFFFD && p == buf + 1, "malformed lead: U+FFFD, exactly one byte");
        /* walking the whole string terminates, never passes the NUL, and every step is >= 1 byte */
        const char *q = buf; int steps = 0;
        while (*q && steps < 32) { const char *before = q; nx(&q); if (q <= before) break; steps++; }
        EXPECT(*q == 0 && q == buf + n, "walk ends exactly at the NUL");
    }
    {   /* NUL: returns 0, does not advance */
        const char *p = ""; const char *s = p;
        EXPECT(nx(&p) == 0 && p == s, "NUL is not consumed");
    }
    {   /* resync: a bad lead then a good letter */
        const char *p = "\xE2\xC3\xA9";
        uint32_t a = nx(&p), b = nx(&p);
        EXPECT(a == 0xFFFD && b == 0xE9 && *p == 0, "resync after a bad lead");
    }
#undef EXPECT
    return fails;
}

static int fails = 0;
#define CHECK(cond, ...) do { if (!(cond)) { fails++; printf("  FAIL " __VA_ARGS__); printf("\n"); } } while (0)

int main(void) {
    int real = check_decoder(real_next, 1);
    int naive = check_decoder(naive_next, 0);
    printf("decoder checks: real decoder failed %d, naive decoder (control) failed %d\n", real, naive);
    fails += real;
    CHECK(naive > 0, "control: the naive decoder must fail the malformed-input checks");

    /* helpers over a mixed string: 'a' (1) e-acute (2) euro (3) emoji (4) 'b' (1) = 11 bytes, 5 code points */
    const char *m = "a\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80" "b";
    CHECK(strlen(m) == 11, "fixture length");
    CHECK(utf8_len_cp(m) == 5, "utf8_len_cp mixed = %zu", utf8_len_cp(m));
    CHECK(utf8_len_cp("") == 0, "utf8_len_cp empty");
    CHECK(utf8_len_cp("\xFF\xFFx") == 3, "utf8_len_cp counts each bad byte");
    static const size_t bound[] = { 0, 1, 3, 6, 10, 11 };          /* the sequence starts + end */
    for (size_t n = 0; n <= 14; n++) {
        size_t want = 0;
        for (unsigned i = 0; i < sizeof bound / sizeof bound[0]; i++) if (bound[i] <= n) want = bound[i];
        CHECK(utf8_cut_boundary(m, n) == want, "utf8_cut_boundary(mixed, %zu) = %zu want %zu", n, utf8_cut_boundary(m, n), want);
    }
    CHECK(utf8_prefix_bytes(m, 0) == 0 && utf8_prefix_bytes(m, 1) == 1 && utf8_prefix_bytes(m, 2) == 3 &&
          utf8_prefix_bytes(m, 3) == 6 && utf8_prefix_bytes(m, 4) == 10 && utf8_prefix_bytes(m, 5) == 11 &&
          utf8_prefix_bytes(m, 9) == 11, "utf8_prefix_bytes");
    CHECK(utf8_prev(m, 11) == 10 && utf8_prev(m, 10) == 6 && utf8_prev(m, 6) == 3 && utf8_prev(m, 3) == 1 &&
          utf8_prev(m, 1) == 0, "utf8_prev walks back one code point at a time");
    CHECK(utf8_prev("a\xC3", 2) == 1, "utf8_prev over a truncated tail deletes the stray byte");

    printf("%s (%d failed)\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
