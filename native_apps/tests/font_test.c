/* Host regression for common/font.c (the glyph lookup).
 *
 *   cd native_apps && gcc -Wall -Wextra -Wno-unused-parameter -I common \
 *       -o build/font_test tests/font_test.c common/font.c && ./build/font_test
 *
 * Every required code point maps to a real glyph (not the fallback pointer);
 * every unmapped one returns the fallback; ASCII keeps the old shapes of a few
 * spot glyphs; g j p q y reach the descender rows 7-8 and nothing else in ASCII
 * does; accented capitals use row 0 for the accent; no glyph has a bit above
 * row 8.  Negative controls: the same check functions run against a lookup
 * that is off by one and one that always answers "fallback", and must FAIL.
 */
#include <stdio.h>
#include <string.h>
#include "font.h"
#include "utf8.h"

typedef const uint16_t *(*lookup_fn)(uint32_t);

static const uint16_t *real_lookup(uint32_t cp) { return font_glyph(cp); }
static const uint16_t *shifted_lookup(uint32_t cp) { return font_glyph(cp + 1); }
static const uint16_t *all_fallback(uint32_t cp) { (void)cp; return font_fallback_glyph(); }

static int has_ink_in_rows(const uint16_t *g, unsigned mask) {
    for (int c = 0; c < FONT_COLS; c++) if (g[c] & mask) return 1;
    return 0;
}

static int run_checks(lookup_fn lk, int verbose) {
    int fails = 0;
#define EXPECT(cond, ...) do { if (!(cond)) { fails++; if (verbose) { printf("  FAIL " __VA_ARGS__); printf("\n"); } } } while (0)
    const uint16_t *fb = font_fallback_glyph();

    for (uint32_t cp = 0x20; cp <= 0x7E; cp++) EXPECT(lk(cp) != fb, "U+%04X unmapped", cp);
    for (uint32_t cp = 0xA0; cp <= 0xFF; cp++) EXPECT(lk(cp) != fb, "U+%04X unmapped", cp);
    for (uint32_t cp = 0x100; cp <= 0x17F; cp++) EXPECT(lk(cp) != fb, "U+%04X unmapped", cp);
    static const uint32_t punct[] = { 0x2018, 0x2019, 0x201A, 0x201C, 0x201D, 0x201E, 0x2013, 0x2014, 0x2022, 0x2026, 0x20AC };
    for (unsigned i = 0; i < sizeof punct / sizeof punct[0]; i++) EXPECT(lk(punct[i]) != fb, "U+%04X unmapped", punct[i]);

    static const uint32_t ext[] = { 0x0416, 0x0410, 0x044F, 0x0401, 0x0451, 0x03A9, 0x03B1, 0x03C9, 0x03C2, 0x0386, 0x03CE, 0x0490, 0x04E9 };
    for (unsigned i = 0; i < sizeof ext / sizeof ext[0]; i++) EXPECT(lk(ext[i]) != fb, "U+%04X unmapped", ext[i]);
    EXPECT(memcmp(lk(0x0416), lk(0x0436), sizeof(uint16_t) * FONT_COLS) != 0, "Zhe upper and lower identical");
    static const uint32_t unmapped[] = { 0x00, 0x09, 0x0A, 0x1F, 0x7F, 0x80, 0x9F, 0x180, 0x2012, 0x2015, 0x20AD,
                                         0x4E00, 0x4E2D, 0x0378, 0x0380, 0x0460, 0x04FF, 0x0500, 0x1F600, 0xFFFD, 0x10FFFF };
    for (unsigned i = 0; i < sizeof unmapped / sizeof unmapped[0]; i++) EXPECT(lk(unmapped[i]) == fb, "U+%04X should be the fallback", unmapped[i]);

    /* descenders: rows 7-8 are bits 7-8 */
    for (const char *p = "gjpqy"; *p; p++) EXPECT(has_ink_in_rows(lk((unsigned char)*p), 0x180), "'%c' has no descender", *p);
    for (uint32_t cp = 0x20; cp <= 0x7E; cp++) {
        if (strchr("gjpqy", (int)cp)) continue;
        EXPECT(!has_ink_in_rows(lk(cp), 0x180), "'%c' has ink in rows 7-8", (int)cp);
    }
    /* accents: capital with an accent reaches row 0, plain capital does not touch it for A */
    static const uint32_t acc[] = { 0xC1, 0xC4, 0xD6, 0xDC, 0x150, 0x170 };   /* A-acute A-uml O-uml U-uml O-dacute U-dacute */
    for (unsigned i = 0; i < sizeof acc / sizeof acc[0]; i++) EXPECT(has_ink_in_rows(lk(acc[i]), 0x001), "U+%04X has no accent row", acc[i]);
    EXPECT(lk('A') != lk(0xC1) && memcmp(lk('A'), lk(0xC1), sizeof(uint16_t) * FONT_COLS) != 0, "A and A-acute identical");
    EXPECT(memcmp(lk('e'), lk(0xE9), sizeof(uint16_t) * FONT_COLS) != 0, "e and e-acute identical");

    /* every glyph within 9 rows; fallback is a visible box */
    for (uint32_t cp = 0x20; cp <= 0x4FF; cp++)
        for (int c = 0; c < FONT_COLS; c++) EXPECT((lk(cp)[c] & ~0x1FFu) == 0, "U+%04X has a bit above row 8", cp);
    EXPECT(has_ink_in_rows(fb, 0x7F), "fallback box is blank");

    /* case map and encoder (utf8.h): an upper-cased code point must have a glyph whenever
     * its source has one, utf8_put must round-trip through utf8_next, and spot values. */
    for (uint32_t cp = 0x20; cp <= 0x4FF; cp++) {
        uint32_t up = utf8_upper_cp(cp);
        if (lk(cp) != fb) EXPECT(lk(up) != fb, "U+%04X upper U+%04X has no glyph", cp, up);
        char enc[5] = {0};
        size_t n = utf8_put(enc, up);
        const char *p = enc;
        EXPECT(n >= 1 && n <= 4 && utf8_next(&p) == up && p == enc + n, "U+%04X does not round-trip", up);
        EXPECT(utf8_upper_cp(up) == up, "U+%04X: upper is not idempotent", cp);
    }
    EXPECT(utf8_upper_cp(0x151) == 0x150 && utf8_upper_cp(0xDF) == 0xDF && utf8_upper_cp(0xFF) == 0x178 &&
           utf8_upper_cp(0x131) == 'I' && utf8_upper_cp(0x3C2) == 0x3A3 && utf8_upper_cp(0x3AD) == 0x388 &&
           utf8_upper_cp(0x436) == 0x416 && utf8_upper_cp(0x451) == 0x401 && utf8_upper_cp(0x3C9) == 0x3A9 &&
           utf8_upper_cp('q') == 'Q' && utf8_upper_cp(0x149) == 0x149 && utf8_upper_cp(0x4E2D) == 0x4E2D,
           "case map spot values");

    /* spot: the ASCII shapes that must not change */
    EXPECT(lk('!')[2] == 0x5F && lk('H')[0] == 0x7F && lk('0')[0] == 0x3E, "ASCII spot glyphs changed");
    EXPECT(lk(' ')[0] == 0 && lk(' ')[5] == 0 && lk(0xA0)[2] == 0, "space / NBSP not blank");
#undef EXPECT
    return fails;
}

int main(void) {
    int real = run_checks(real_lookup, 1);
    int c1 = run_checks(shifted_lookup, 0);
    int c2 = run_checks(all_fallback, 0);
    printf("real lookup failed %d; control off-by-one failed %d; control all-fallback failed %d\n", real, c1, c2);
    int bad = real;
    if (c1 == 0) { printf("  FAIL control: the off-by-one lookup passed every check\n"); bad++; }
    if (c2 == 0) { printf("  FAIL control: the all-fallback lookup passed every check\n"); bad++; }
    printf("%s\n", bad ? "FAIL" : "PASS");
    return bad ? 1 : 0;
}
