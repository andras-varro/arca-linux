/* font.c -- glyph lookup over the generated tables (font_table.inc).
 * Direct arrays for the common ranges, a binary search for the rare
 * punctuation, a fallback box for everything else.  No division or modulo
 * (Cortex-A8 has no hardware divide). */
#include "font.h"

#include "font_table.inc"

const uint16_t *font_fallback_glyph(void)
{
    return font_fallback;
}

const uint16_t *font_glyph(uint32_t cp)
{
    if (cp >= 0x20 && cp <= 0x7E)   return font_ascii[cp - 0x20];
    if (cp >= 0xA0 && cp <= 0xFF)   return font_l1[cp - 0xA0];
    if (cp >= 0x100 && cp <= 0x17F) return font_ea[cp - 0x100];
    if (cp >= 0x2013 && cp <= 0x20AC) {
        unsigned lo = 0, hi = (unsigned)(sizeof font_punct / sizeof font_punct[0]);
        while (lo < hi) {
            unsigned mid = (lo + hi) >> 1;
            if (font_punct[mid].cp == cp) return font_punct[mid].g;
            if (font_punct[mid].cp < cp) lo = mid + 1; else hi = mid;
        }
    }
    return font_fallback;
}
