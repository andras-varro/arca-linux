/* Host-side regression for the symbols page of KB_LAYOUT_FULL in common/keyboard.c.
 *
 * keyboard.c is #included (not linked) so the static key tables are reachable;
 * run-all.sh therefore lists the object list of the usual keyboard-linking row
 * minus common/keyboard.c.
 *
 * Checks: (a) the FULL layout has a symbols toggle action (KA_SYM) and a page
 * holding every punctuation mark the letter pages lack; the page keeps the
 * 10 x 4 grid; (b) the ALPHA, ALPHANUM, NUMERIC and FULL letter tables equal
 * literal copies of their pre-change contents.
 *
 * Cannot see: the drawing, the toggle's label, touch or pad driving of the
 * screen (kb_run is a blocking loop). Those are the panel's to judge.
 */
#include "../common/keyboard.c"

static int fails, checks;
#define CHECK(cond) do { checks++; if (!(cond)) { fails++; \
    printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

static bool same(const char **a, const char **b) {
    int i = 0;
    for (; a[i] && b[i]; i++) if (strcmp(a[i], b[i]) != 0) return false;
    return !a[i] && !b[i];
}

static bool page_has(const char **keys, char ch) {
    for (int r = 0; keys[r]; r++) if (strchr(keys[r], ch)) return true;
    return false;
}

int main(void) {
    /* (b) the ABC-order tables (ALPHA now 10 wide, padded with KB_GAP) */
    static const char *alpha[]    = { "ABCDEFGHIJ", "KLMNOPQRST", "UVWXYZ_\001\001\001", NULL };
    static const char *alphanum[] = { "ABCDEFGHIJ", "KLMNOPQRST", "UVWXYZ.-_ ", "0123456789", NULL };
    static const char *upper[]    = { "ABCDEFGHIJ", "KLMNOPQRST", "UVWXYZ.-_!", "0123456789", NULL };
    static const char *lower[]    = { "abcdefghij", "klmnopqrst", "uvwxyz.-_!", "0123456789", NULL };
    static const char *numeric[]  = { "123.", "456:", "789 ", " 0  ", NULL };
    CHECK(same(alpha_abc_keys, alpha));
    CHECK(same(alphanum_abc_keys, alphanum));
    CHECK(same(full_upper_abc_keys, upper));
    CHECK(same(full_lower_abc_keys, lower));
    CHECK(same(numeric_keys, numeric));

    /* (a) symbols page */
    const char *want = ":;\"'<>/?=+()*&{[}]~`@#$%^,\\|";
    for (const char *p = want; *p; p++) {
        CHECK(page_has(full_sym_keys, *p));
        CHECK(!page_has(full_upper_abc_keys, *p) || strchr(".-_!", *p));
    }
    CHECK(kb_row_count(full_sym_keys) == 4);
    for (int r = 0; r < 4; r++) CHECK(strlen(full_sym_keys[r]) == 10);
    CHECK(KA_SYM != KA_SHIFT && KA_SYM != KA_SPACE && KA_SYM != KA_OK);
    /* no duplicate keys on the page */
    for (int r = 0; r < 3; r++)
        for (const char *p = full_sym_keys[r]; *p; p++) {
            int n = 0;
            for (int r2 = 0; r2 < 3; r2++) for (const char *q = full_sym_keys[r2]; *q; q++) n += (*q == *p);
            CHECK(n == 1);
        }

    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
