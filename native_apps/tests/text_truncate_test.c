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

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
