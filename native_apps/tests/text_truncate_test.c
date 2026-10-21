/*
 * text_truncate_test.c — text_truncate() must stay inside the buffer it is given.
 *
 * text_truncate() used to take no destination size: it strcpy'd the
 * uppercased source (up to 255 chars) into dest and strcat'd "..." after a
 * width cut.  Every caller survived by arithmetic luck — usb_page.c passed a
 * 48-byte buffer for a 48-byte product name, and button_set_text() passed the
 * 128-byte Button.text for a source that may be 255 long.  The size is now a
 * parameter, and the contract this file pins is:
 *
 *   for every dest_size >= 1 the result is NUL-terminated within dest_size,
 *   nothing past dest[dest_size-1] is written, and a result cut for EITHER
 *   reason (pixel width or buffer size) ends in "..." when there is room.
 *
 * Every destination sits in a struct with a sentinel-filled guard behind it,
 * so an overflow is a deterministic failed check without a sanitizer; the one
 * malloc'd case is there for -fsanitize=address, which cannot see an overrun
 * inside a struct.  Widths are asserted as "fits max_width" and "one more
 * character would not", both measured with text_measure_width(), so this file
 * pins the cut to the same advance the drawing uses without naming it.
 *
 * Build (host gcc, from native_apps/):
 *   gcc -Wall -Wextra -Wno-unused-parameter -I common -o build/text_truncate_test \
 *       tests/text_truncate_test.c common/common.c common/framebuffer.c common/font.c \
 *       common/touch_input.c common/hardware.c common/config.c \
 *       common/highscore.c common/keyboard.c common/audio.c common/audio_gen.c \
 *       common/audio_out.c common/audio_wav.c -lm && \
 *   ./build/text_truncate_test
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../common/common.h"
#include "../common/utf8.h"

static int checks = 0, failures = 0;

static void check(int ok, const char *what) {
    checks++;
    if (ok) printf("  ok    %s\n", what);
    else  { failures++; printf("  FAIL  %s\n", what); }
}

#define GUARD_LEN 32
#define SENTINEL  ((char)0x5A)

typedef struct { char dst[64]; char guard[GUARD_LEN]; } Slot;

static void slot_reset(Slot *s) { memset(s, SENTINEL, sizeof(*s)); }

/* True when nothing at or past dst[n] was written: the rest of dst (when n <
 * 64) and the whole guard still hold the sentinel. */
static int untouched_from(const Slot *s, size_t n) {
    const char *p = (const char *)s;
    for (size_t i = n; i < sizeof(*s); i++) if (p[i] != SENTINEL) return 0;
    return 1;
}

static int terminated_within(const char *d, size_t n) {
    return memchr(d, '\0', n) != NULL;
}

static int ends_with_ellipsis(const char *d) {
    size_t l = strlen(d);
    return l >= 3 && strcmp(d + l - 3, "...") == 0;
}

static int starts_with(const char *d, const char *prefix, size_t n) {
    return strncmp(d, prefix, n) == 0;
}

int main(void) {
    Slot s;
    char what[160];

    printf("text_truncate: destination size is honoured\n");

    /* A — fits both ways: a plain uppercase copy. */
    slot_reset(&s);
    text_truncate(s.dst, 32, "abc", 0, 1);
    check(strcmp(s.dst, "ABC") == 0, "A  short source, no width limit -> \"ABC\"");
    check(untouched_from(&s, 32), "A  nothing written past dest_size");

    /* B — pixel-width cut into a roomy buffer. */
    slot_reset(&s);
    text_truncate(s.dst, 64, "abcdefghijklmnopqrstuvwxyz", 60, 1);
    check(ends_with_ellipsis(s.dst), "B  width cut ends in \"...\"");
    check(text_measure_width(s.dst, 1) <= 60, "B  width cut fits max_width");
    check(text_measure_width(s.dst, 1) + text_measure_width("M", 1) > 60,
          "B  width cut uses the room: one more character would not fit");
    check(starts_with(s.dst, "ABCDEFGHIJKLMNOPQRSTUVWXYZ", strlen(s.dst) - 3),
          "B  width cut keeps a prefix of the uppercased source");

    /* C — the destination is smaller than the source, no width limit: the
     * buffer is now the only bound, and it must be honoured. */
    slot_reset(&s);
    text_truncate(s.dst, 8, "usb product name", 0, 1);
    check(terminated_within(s.dst, 8), "C  dst[8], 16-char source: NUL within 8 bytes");
    check(untouched_from(&s, 8), "C  dst[8], 16-char source: nothing written past byte 8");
    check(strcmp(s.dst, "USB ...") == 0, "C  dst[8], 16-char source -> \"USB ...\"");

    /* D — a width cut whose result is still longer than the buffer. */
    slot_reset(&s);
    text_truncate(s.dst, 6, "a very long device name indeed", 400, 2);
    check(terminated_within(s.dst, 6), "D  dst[6] under a wide width cut: NUL within 6");
    check(untouched_from(&s, 6), "D  dst[6] under a wide width cut: nothing past byte 6");
    check(strcmp(s.dst, "A ...") == 0, "D  dst[6] under a wide width cut -> \"A ...\"");

    /* E — the degenerate sizes: always terminated, never overrun. */
    for (size_t n = 1; n <= 4; n++) {
        slot_reset(&s);
        text_truncate(s.dst, n, "abcdefgh", 0, 1);
        snprintf(what, sizeof(what), "E  dest_size %zu: NUL within, nothing past", n);
        check(terminated_within(s.dst, n) && untouched_from(&s, n), what);
    }
    slot_reset(&s);
    text_truncate(s.dst, 1, "abcdefgh", 0, 1);
    check(s.dst[0] == '\0', "E  dest_size 1 -> empty string");

    /* F — the usb_page case: a 47-char name into a 48-byte buffer fits
     * exactly and must come back whole, not ellipsised. */
    const char *name47 = "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstu";
    slot_reset(&s);
    text_truncate(s.dst, 48, name47, 0, 2);
    check(strlen(s.dst) == 47 && !ends_with_ellipsis(s.dst),
          "F  47-char name in dst[48] is copied whole");
    check(untouched_from(&s, 48), "F  nothing written past byte 48");

    /* G — a width too narrow for even the ellipsis, into a 2-byte buffer. */
    slot_reset(&s);
    text_truncate(s.dst, 2, "abcdefgh", 4, 1);
    check(terminated_within(s.dst, 2) && untouched_from(&s, 2),
          "G  narrow width into dst[2]: NUL within, nothing past");

    /* H — an exact-size heap buffer, for -fsanitize=address. */
    char *heap = malloc(5);
    if (!heap) { printf("  FAIL  malloc\n"); return 1; }
    text_truncate(heap, 5, "a forty character source string, roughly", 0, 1);
    check(terminated_within(heap, 5) && strlen(heap) <= 4,
          "H  malloc(5), 40-char source: terminated within 5");
    free(heap);

    /* I -- UTF-8: the all-caps UI upcases accented, Greek and Cyrillic letters,
     * and no cut, by width or by buffer, lands inside a sequence. */
    slot_reset(&s);
    text_truncate(s.dst, 32, "\xC3\xBC" "ber", 0, 1);            /* uber with u-diaeresis */
    check(strcmp(s.dst, "\xC3\x9C" "BER") == 0, "I  u-diaeresis -> U-diaeresis");
    slot_reset(&s);
    text_truncate(s.dst, 32, "\xC5\x91z", 0, 1);                 /* o-double-acute z */
    check(strcmp(s.dst, "\xC5\x90Z") == 0, "I  o-double-acute -> O-double-acute");
    slot_reset(&s);
    text_truncate(s.dst, 32, "stra\xC3\x9F" "e", 0, 1);          /* sharp s stays */
    check(strcmp(s.dst, "STRA\xC3\x9F" "E") == 0, "I  sharp s kept as is");
    slot_reset(&s);
    text_truncate(s.dst, 32, "\xC3\xBF", 0, 1);                  /* y-diaeresis -> U+0178 */
    check(strcmp(s.dst, "\xC5\xB8") == 0, "I  y-diaeresis -> capital Y-diaeresis");
    slot_reset(&s);
    text_truncate(s.dst, 32, "\xC4\xB1", 0, 1);                  /* dotless i -> I (shrinks) */
    check(strcmp(s.dst, "I") == 0, "I  dotless i -> I");
    slot_reset(&s);
    text_truncate(s.dst, 48,
        "\xD0\xBF\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82", 0, 1);     /* privet */
    check(strcmp(s.dst, "\xD0\x9F\xD0\xA0\xD0\x98\xD0\x92\xD0\x95\xD0\xA2") == 0,
          "I  Cyrillic privet -> PRIVET");
    slot_reset(&s);
    text_truncate(s.dst, 48, "\xCF\x89\xCE\xBC\xCE\xAD\xCE\xB3\xCE\xB1", 0, 1);  /* omega mu e-tonos gamma alpha */
    check(strcmp(s.dst, "\xCE\xA9\xCE\x9C\xCE\x88\xCE\x93\xCE\x91") == 0,
          "I  Greek omega-mu-e(tonos)-gamma-alpha upcased, tonos kept");
    slot_reset(&s);
    text_truncate(s.dst, 32, "\xCF\x82", 0, 1);                  /* final sigma */
    check(strcmp(s.dst, "\xCE\xA3") == 0, "I  Greek final sigma -> capital sigma");

    /* J -- the buffer cut never splits a sequence */
    slot_reset(&s);
    text_truncate(s.dst, 7, "\xC3\xA9\xC3\xA9\xC3\xA9", 0, 1);  /* 3 x e-acute = 6 bytes, dst exactly full */
    check(strcmp(s.dst, "\xC3\x89\xC3\x89\xC3\x89") == 0 && untouched_from(&s, 7),
          "J  3 x e-acute in dst[7]: whole, exactly full");
    slot_reset(&s);
    text_truncate(s.dst, 5, "\xC3\xA9\xC3\xA9\xC3\xA9", 0, 1);
    check(strcmp(s.dst, "...") == 0 && untouched_from(&s, 5),
          "J  3 x e-acute in dst[5]: no half sequence, \"...\" only");
    slot_reset(&s);
    text_truncate(s.dst, 7, "\xD0\xB6\xD0\xB6\xD0\xB6\xD0\xB6", 0, 1);  /* 4 x zhe = 8 bytes */
    check(strcmp(s.dst, "\xD0\x96...") == 0 && untouched_from(&s, 7),
          "J  4 x zhe in dst[7] -> one zhe + \"...\"");
    slot_reset(&s);
    text_truncate(s.dst, 1, "\xC3\xA9", 0, 1);
    check(s.dst[0] == '\0' && untouched_from(&s, 1), "J  dst[1], multibyte source -> empty");
    slot_reset(&s);
    text_truncate(s.dst, 2, "\xC3\xA9", 0, 1);
    check(strcmp(s.dst, ".") == 0 && untouched_from(&s, 2),
          "J  dst[2], e-acute: no half sequence, one dot");

    /* K -- the pixel-width cut counts code points, not bytes */
    slot_reset(&s);
    text_truncate(s.dst, 64, "\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9", 30, 1);
    check(strcmp(s.dst, "\xC3\x89\xC3\x89...") == 0,
          "K  8 x e-acute, 30 px -> 2 letters + \"...\"");
    check(text_measure_width(s.dst, 1) <= 30, "K  result fits 30 px");

    /* L -- every output is well-formed UTF-8 at every size and width */
    {
        const char *mix = "\xC5\x91\xC3\xA9\xD0\xB6\xCE\xA9\xC3\x9F" "abc\xE2\x82\xAC" "xyz";
        int bad = 0;
        for (size_t n = 1; n <= 24; n++) {
            for (int w = 0; w <= 120; w += 6) {
                slot_reset(&s);
                text_truncate(s.dst, n, mix, w, 1);
                const char *p = s.dst;
                while (*p) if (utf8_next(&p) == UTF8_REPLACEMENT) { bad++; break; }
                if (!terminated_within(s.dst, n) || !untouched_from(&s, n)) bad++;
            }
        }
        check(bad == 0, "L  no split sequence, always terminated, nothing past dest_size");
    }

    /* M -- text_to_uppercase directly: never grows, keeps bad bytes, whole code points only */
    {
        char u[8];
        memset(u, 'Q', sizeof u);
        text_to_uppercase(u, "a\xFF" "b", sizeof u);
        check(strcmp(u, "A\xFF" "B") == 0, "M  malformed byte kept verbatim");
        memset(u, 'Q', sizeof u);
        text_to_uppercase(u, "\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9", 6);   /* room for 5 bytes */
        check(strcmp(u, "\xC3\x89\xC3\x89") == 0 && u[5] == 'Q', "M  max_len 6 keeps 2 whole letters");
        memset(u, 'Q', sizeof u);
        text_to_uppercase(u, "\xC5\x89x", sizeof u);                    /* n-apostrophe has no single capital */
        check(strcmp(u, "\xC5\x89X") == 0, "M  letter without a single-code-point capital unchanged");
    }

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
