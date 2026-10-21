/* utf8.h -- header-only UTF-8 decoding for the display font.
 *
 * Pure, no object file, no link edit: include it where it is needed.
 *
 * utf8_next(&p)  returns the next code point and advances p.
 *   - at the terminating NUL it returns 0 and does NOT advance, so a loop
 *     `while (*p) utf8_next(&p)` can never run past the string;
 *   - malformed input returns U+FFFD and consumes exactly ONE byte (the next
 *     call re-synchronises at the following lead byte), so a loop always makes
 *     progress.  Malformed means: a stray continuation byte (0x80-0xBF), the
 *     never-valid leads 0xC0 0xC1 0xF5-0xFF, a truncated sequence (including
 *     one cut short by the NUL), an overlong form (0xE0 0x80.., 0xF0 0x80..), a UTF-16
 *     surrogate (0xED 0xA0..) and anything above U+10FFFF (0xF4 0x90..).
 *   - it reads at most one byte past the last valid byte of a sequence, and
 *     stops at the first byte that is not a valid continuation, so a NUL ends
 *     every sequence it appears in.
 *
 * The other helpers are defined in terms of utf8_next, so they all agree on
 * where a sequence ends.  Policy for invalid bytes: a visible fallback glyph
 * per bad byte, never a Latin-1 reinterpretation.
 */
#ifndef UTF8_H
#define UTF8_H

#include <stddef.h>
#include <stdint.h>

#define UTF8_REPLACEMENT 0xFFFDu

static inline uint32_t utf8_next(const char **pp)
{
    const unsigned char *s = (const unsigned char *)*pp;
    uint32_t c = s[0];
    if (c == 0) return 0;
    if (c < 0x80) { *pp += 1; return c; }

    int n;                                   /* continuation bytes expected */
    uint32_t cp;
    unsigned lo = 0x80, hi = 0xBF;           /* range for the FIRST continuation */
    if (c >= 0xC2 && c <= 0xDF)      { n = 1; cp = c & 0x1Fu; }
    else if (c >= 0xE0 && c <= 0xEF) {
        n = 2; cp = c & 0x0Fu;
        if (c == 0xE0) lo = 0xA0;            /* no overlongs   */
        else if (c == 0xED) hi = 0x9F;       /* no surrogates  */
    }
    else if (c >= 0xF0 && c <= 0xF4) {
        n = 3; cp = c & 0x07u;
        if (c == 0xF0) lo = 0x90;            /* no overlongs   */
        else if (c == 0xF4) hi = 0x8F;       /* <= U+10FFFF    */
    }
    else { *pp += 1; return UTF8_REPLACEMENT; }

    for (int i = 1; i <= n; i++) {
        unsigned b = s[i];                   /* NUL fails the range test */
        if (b < lo || b > hi) { *pp += 1; return UTF8_REPLACEMENT; }
        cp = (cp << 6) | (b & 0x3Fu);
        lo = 0x80; hi = 0xBF;
    }
    *pp += n + 1;
    return cp;
}

/* Number of code points in s (an invalid byte counts as one). */
static inline size_t utf8_len_cp(const char *s)
{
    size_t n = 0;
    while (*s) { utf8_next(&s); n++; }
    return n;
}

/* Bytes occupied by the first max_cp code points of s (or all of s). */
static inline size_t utf8_prefix_bytes(const char *s, size_t max_cp)
{
    const char *p = s;
    while (max_cp > 0 && *p) { utf8_next(&p); max_cp--; }
    return (size_t)(p - s);
}

/* Largest cut position <= nbytes that does not fall inside a sequence. */
static inline size_t utf8_cut_boundary(const char *s, size_t nbytes)
{
    const char *p = s;
    while (*p) {
        const char *q = p;
        utf8_next(&q);
        if ((size_t)(q - s) > nbytes) break;
        p = q;
    }
    return (size_t)(p - s);
}

/* Offset of the code point that ends at `end` (end > 0): what Backspace deletes. */
static inline size_t utf8_prev(const char *s, size_t end)
{
    size_t pos = 0, last = 0;
    const char *p = s;
    while (pos < end && *p) {
        const char *q = p;
        last = pos;
        utf8_next(&q);
        pos += (size_t)(q - p);
        p = q;
    }
    return last;
}

#endif /* UTF8_H */
