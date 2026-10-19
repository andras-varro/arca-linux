/* Host regression for jf_genre_key (jellyfin/jf_model.c): the key two spellings of a genre share.
 *
 *   cd native_apps && gcc -Wall -Wextra -I common -I jellyfin -fsanitize=undefined \
 *       -o build/jf_genre_test tests/jf_genre_test.c jellyfin/jf_model.c common/json.c -lm \
 *       && ./build/jf_genre_test
 *
 * A MIX matches the ticked genres against the server's genre index by this key, so a mapping that is too
 * loose merges two genres and one too strict loses the pairs ("Hip-Hop" / "Hip Hop Music").
 */
#include <stdio.h>
#include <string.h>
#include "jf_model.h"

static int checks, fails;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); \
    printf(__VA_ARGS__); printf("\n"); } } while (0)

static void key_is(int line, const char *in, const char *want)
{
    char k[64];
    memset(k, 'X', sizeof k);
    int n = jf_genre_key(k, sizeof k, in);
    CHECK(strcmp(k, want) == 0, "line %d: key(\"%s\") = \"%s\", want \"%s\"", line, in ? in : "(null)", k, want);
    CHECK(n == (int)strlen(want), "line %d: key(\"%s\") returned %d, want %d", line, in ? in : "(null)", n, (int)strlen(want));
}
#define KEY(in, want) key_is(__LINE__, in, want)

int main(void)
{
    KEY("Rock", "rock");
    KEY("ROCK", "rock");
    KEY("  Rock  ", "rock");
    KEY("Hip-Hop", "hip hop");
    KEY("Hip Hop Music", "hip hop");
    KEY("hip_hop", "hip hop");
    KEY("Hip -_ \t Hop", "hip hop");
    KEY("Hip\t\tHop", "hip hop");
    KEY("\nHip\r\nHop\f\v", "hip hop");
    KEY("--Rock--", "rock");
    KEY("Pop Music", "pop");
    KEY("POP  MUSIC", "pop");
    KEY("Pop-Music", "pop");
    KEY("pop music ", "pop");
    KEY("Music", "music");              /* nothing before it: the word is the genre */
    KEY(" Music ", "music");
    KEY("Smusic", "smusic");
    KEY("Folk Musicals", "folk musicals");
    KEY("Music Music", "music");        /* only one trailing " music" is cut */
    KEY("Music Music Music", "music music");
    KEY("", "");
    KEY(NULL, "");
    KEY("   ", "");
    KEY("-_-\t", "");
    KEY("\xC3\x89lectro", "\xC3\x89lectro");   /* non-ASCII bytes pass through untouched (only A-Z fold) */
    KEY("\xC3\x89LECTRO Music", "\xC3\x89lectro");
    KEY("Caf\xC3\xA9-Jazz", "caf\xC3\xA9 jazz");

    /* idempotent: the key of a key is the key */
    const char *in[] = { "Hip-Hop", "Hip Hop Music", "POP  MUSIC", "Music", "  a_b-c ", "\xC3\x89lectro", "Rock", "" };
    for (size_t i = 0; i < sizeof in / sizeof in[0]; i++) {
        char k1[64], k2[64];
        jf_genre_key(k1, sizeof k1, in[i]);
        jf_genre_key(k2, sizeof k2, k1);
        CHECK(strcmp(k1, k2) == 0, "not idempotent on \"%s\": \"%s\" -> \"%s\"", in[i], k1, k2);
    }

    /* small caps: at most cap-1 bytes, always terminated, never past the buffer */
    for (size_t cap = 1; cap <= 12; cap++) {
        char guard[32];
        memset(guard, 'G', sizeof guard);
        int n = jf_genre_key(guard, cap, "Alternative Rock Music");
        CHECK(n >= 0 && (size_t)n < cap, "cap %zu: returned %d", cap, n);
        CHECK(memchr(guard, '\0', cap) != NULL && strlen(guard) == (size_t)n, "cap %zu: not terminated at %d", cap, n);
        int over = 0;
        for (size_t k = cap; k < sizeof guard; k++) if (guard[k] != 'G') over++;
        CHECK(over == 0, "cap %zu: wrote %d bytes past the buffer", cap, over);
    }
    {
        char k[8];
        int n = jf_genre_key(k, sizeof k, "Alternative");
        CHECK(n == 7 && strcmp(k, "alterna") == 0, "cap 8 truncates to 7 (got %d \"%s\")", n, k);
        n = jf_genre_key(k, sizeof k, "ab cd ef gh");
        CHECK(n <= 7 && (n == 0 || k[n - 1] != ' '), "truncated key ends on a space? \"%s\"", k);
    }
    {
        char k[4] = "xyz";
        CHECK(jf_genre_key(NULL, 4, "Rock") == 0, "NULL out");
        CHECK(jf_genre_key(k, 0, "Rock") == 0 && strcmp(k, "xyz") == 0, "cap 0 writes nothing");
    }
    printf("jf_genre_test: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
