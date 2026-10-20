/* Host-side regression for the letter ORDER toggle (QWERTY default / ABC) of
 * common/keyboard.c.  keyboard.c is #included so the static tables are reachable;
 * run-all.sh lists the usual keyboard-linking object list minus keyboard.c.
 *
 * Checks: (a) every letter layout has a QWERTY and an ABC table, both with the
 * same grid size and the same key set (gaps aside); QWERTY is the default;
 * (b) QWERTY rows are the standard ones; (c) the order toggle has its own action
 * id and label, distinct from the symbols page's return label; (d) a gap slot is
 * never a key and focus moves skip it and stay on the grid.
 *
 * Cannot see: drawing, touch or pad driving of kb_run (a blocking loop). */
#include "../common/keyboard.c"

static int fails, checks;
#define CHECK(cond) do { checks++; if (!(cond)) { fails++; \
    printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

/* sorted multiset of the real keys on a page */
static void keyset(const char **k, char *out) {
    int n = 0;
    for (int r = 0; k[r]; r++)
        for (const char *p = k[r]; *p; p++) if (*p != KB_GAP) out[n++] = *p;
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && out[j-1] > out[j]; j--) { char t = out[j]; out[j] = out[j-1]; out[j-1] = t; }
    out[n] = 0;
}

static void same_keys_same_grid(KeyboardLayout l, bool shifted) {
    const char **q = kb_letter_keys(l, true, shifted), **a = kb_letter_keys(l, false, shifted);
    char sq[100], sa[100];
    CHECK(kb_row_count(q) == kb_row_count(a));
    for (int r = 0; q[r] && a[r]; r++) CHECK(strlen(q[r]) == strlen(a[r]));
    keyset(q, sq); keyset(a, sa);
    CHECK(strcmp(sq, sa) == 0);
    CHECK(q != a);
}

int main(void) {
    CHECK(kb_qwerty == true);                                  /* default */
    same_keys_same_grid(KB_LAYOUT_ALPHA, true);
    same_keys_same_grid(KB_LAYOUT_ALPHANUM, true);
    same_keys_same_grid(KB_LAYOUT_FULL, true);
    same_keys_same_grid(KB_LAYOUT_FULL, false);

    const char **f = kb_letter_keys(KB_LAYOUT_FULL, true, true);
    CHECK(!strcmp(f[0], "QWERTYUIOP") && !strcmp(f[1], "ASDFGHJKL!") &&
          !strcmp(f[2], "ZXCVBNM.-_") && !strcmp(f[3], "0123456789"));
    f = kb_letter_keys(KB_LAYOUT_FULL, true, false);
    CHECK(!strcmp(f[0], "qwertyuiop") && !strcmp(f[1], "asdfghjkl!"));
    f = kb_letter_keys(KB_LAYOUT_FULL, false, true);
    CHECK(!strcmp(f[0], "ABCDEFGHIJ") && !strcmp(f[3], "0123456789"));
    f = kb_letter_keys(KB_LAYOUT_ALPHA, true, true);
    CHECK(!strncmp(f[0], "QWERTYUIOP", 10) && !strncmp(f[1], "ASDFGHJKL", 9) && !strncmp(f[2], "ZXCVBNM", 7));
    CHECK(strchr(f[1], '_') || strchr(f[2], '_'));             /* the space key stays */
    f = kb_letter_keys(KB_LAYOUT_ALPHANUM, true, true);
    CHECK(!strcmp(f[0], "QWERTYUIOP") && strchr(f[2], ' ') && strchr(f[2], '-'));
    /* NUMERIC is order-free */
    CHECK(kb_letter_keys(KB_LAYOUT_NUMERIC, true, true) == kb_letter_keys(KB_LAYOUT_NUMERIC, false, true));

    /* (c) toggle labels */
    CHECK(KA_ORDER != KA_SYM && KA_ORDER != KA_SHIFT && KA_ORDER != KA_SPACE && KA_ORDER != KA_OK);
    CHECK(strcmp(kb_order_label(true), kb_order_label(false)) != 0);
    CHECK(strcmp(kb_order_label(true), "ABC") == 0);
    CHECK(strcmp(kb_order_label(false), "QWE") == 0);
    CHECK(strcmp(KB_SYM_BACK_LABEL, kb_order_label(true)) != 0);
    CHECK(strcmp(KB_SYM_BACK_LABEL, kb_order_label(false)) != 0);

    /* (d) gaps: ALPHA QWERTY row 2 has gaps; focus never lands on one */
    const char **al = kb_letter_keys(KB_LAYOUT_ALPHA, true, true);
    CHECK(!kb_cell_ok(al, KB_LAYOUT_ALPHA, 2, 9));
    CHECK(kb_cell_ok(al, KB_LAYOUT_ALPHA, 2, 6));
    int rows = kb_row_count(al), cols = kb_col_count(al), fr, fc;
    fr = 1; fc = 9;                                  /* down from col 9 lands on a real key */
    kb_focus_move(al, KB_LAYOUT_ALPHA, rows, cols, 4, 80, 100, &fr, &fc, BTN_ID_DOWN);
    CHECK(fr == 2 && kb_cell_ok(al, KB_LAYOUT_ALPHA, fr, fc) && fc == 7 - 0 - (al[2][7] == KB_GAP ? 1 : 0));
    fr = 2; fc = 6;                                  /* right from the last key wraps past the gaps */
    kb_focus_move(al, KB_LAYOUT_ALPHA, rows, cols, 4, 80, 100, &fr, &fc, BTN_ID_RIGHT);
    CHECK(fr == 2 && fc == 0);
    fr = 2; fc = 0;
    kb_focus_move(al, KB_LAYOUT_ALPHA, rows, cols, 4, 80, 100, &fr, &fc, BTN_ID_LEFT);
    CHECK(fr == 2 && fc == 6);

    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
