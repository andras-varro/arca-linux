/* Host regression for the terminal app's key map (terminal/term_keymap.c).
 *
 *   cd native_apps && gcc -Wall -Wextra -I. -o build/term_keymap_test \
 *       tests/term_keymap_test.c terminal/term_keymap.c && ./build/term_keymap_test
 *
 * What it asserts:
 *   A  letters and digits come out lower case, Shift gives the upper-case
 *      letter or the shifted symbol and is removed from `mods`, caps lock flips
 *      letters only, and Shift with caps lock gives lower case again;
 *   B  Ctrl on a letter gives the LOWER-case letter with TMOD_CTRL (libvterm's
 *      control-code form), whatever Shift and caps lock say; Ctrl-Shift on a
 *      symbol names the shifted glyph (Ctrl-^, Ctrl-_); Alt rides in `mods`;
 *   C  every special key maps to its TKEY_*, F1..F12 are consecutive, and
 *      Shift stays in `mods` for them (Shift-Tab);
 *   D  Shift-Space keeps Shift, the keypad types digits and operators;
 *   E  term_mod_bit names the six modifier codes and nothing else;
 *   F  modifiers, lock keys and unmapped codes give TK_NONE.
 * "Failing before" = compile against a stub whose term_keymap returns TK_NONE
 * for everything: every check in A-D fails, and E-F are the only ones left. */
#include "terminal/term_keymap.h"

#include <linux/input.h>
#include <stdio.h>

static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { fails++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void want_char(const char *what, int code, unsigned mods, bool caps,
                      uint32_t ch, unsigned out_mods) {
    TermKeyOut o = term_keymap(code, mods, caps);
    CHECK(o.kind == TK_CHAR, "%s: kind %d, want TK_CHAR", what, o.kind);
    CHECK(o.ch == ch, "%s: char 0x%x, want 0x%x", what, (unsigned)o.ch, (unsigned)ch);
    CHECK(o.mods == out_mods, "%s: mods 0x%x, want 0x%x", what, o.mods, out_mods);
}

static void want_key(const char *what, int code, unsigned mods, int key, unsigned out_mods) {
    TermKeyOut o = term_keymap(code, mods, false);
    CHECK(o.kind == TK_KEY, "%s: kind %d, want TK_KEY", what, o.kind);
    CHECK(o.key == key, "%s: key %d, want %d", what, o.key, key);
    CHECK(o.mods == out_mods, "%s: mods 0x%x, want 0x%x", what, o.mods, out_mods);
}

int main(void) {
    /* A */
    want_char("a", KEY_A, 0, false, 'a', 0);
    want_char("A (shift)", KEY_A, TMOD_SHIFT, false, 'A', 0);
    want_char("A (caps)", KEY_A, 0, true, 'A', 0);
    want_char("a (shift+caps)", KEY_A, TMOD_SHIFT, true, 'a', 0);
    want_char("1", KEY_1, 0, false, '1', 0);
    want_char("1 under caps stays a digit", KEY_1, 0, true, '1', 0);
    want_char("! (shift 1)", KEY_1, TMOD_SHIFT, false, '!', 0);
    want_char("; ", KEY_SEMICOLON, 0, false, ';', 0);
    want_char(": (shift ;)", KEY_SEMICOLON, TMOD_SHIFT, false, ':', 0);
    want_char("; under caps stays ;", KEY_SEMICOLON, 0, true, ';', 0);
    want_char("| (shift \\)", KEY_BACKSLASH, TMOD_SHIFT, false, '|', 0);
    want_char("~ (shift `)", KEY_GRAVE, TMOD_SHIFT, false, '~', 0);
    want_char("z", KEY_Z, 0, false, 'z', 0);
    want_char("/ ", KEY_SLASH, 0, false, '/', 0);
    want_char("? (shift /)", KEY_SLASH, TMOD_SHIFT, false, '?', 0);

    /* B */
    want_char("ctrl-c", KEY_C, TMOD_CTRL, false, 'c', TMOD_CTRL);
    want_char("ctrl-shift-c is still lower case", KEY_C, TMOD_CTRL | TMOD_SHIFT, false, 'c', TMOD_CTRL);
    want_char("ctrl-c under caps is still lower case", KEY_C, TMOD_CTRL, true, 'c', TMOD_CTRL);
    want_char("ctrl-^ (ctrl-shift-6)", KEY_6, TMOD_CTRL | TMOD_SHIFT, false, '^', TMOD_CTRL);
    want_char("ctrl-_ (ctrl-shift--)", KEY_MINUS, TMOD_CTRL | TMOD_SHIFT, false, '_', TMOD_CTRL);
    want_char("ctrl-\\ ", KEY_BACKSLASH, TMOD_CTRL, false, '\\', TMOD_CTRL);
    want_char("ctrl-] ", KEY_RIGHTBRACE, TMOD_CTRL, false, ']', TMOD_CTRL);
    want_char("alt-x", KEY_X, TMOD_ALT, false, 'x', TMOD_ALT);
    want_char("alt-X", KEY_X, TMOD_ALT | TMOD_SHIFT, false, 'X', TMOD_ALT);
    want_char("ctrl-alt-d", KEY_D, TMOD_CTRL | TMOD_ALT, false, 'd', TMOD_CTRL | TMOD_ALT);

    /* C */
    want_key("enter", KEY_ENTER, 0, TKEY_ENTER, 0);
    want_key("keypad enter", KEY_KPENTER, 0, TKEY_ENTER, 0);
    want_key("tab", KEY_TAB, 0, TKEY_TAB, 0);
    want_key("shift-tab", KEY_TAB, TMOD_SHIFT, TKEY_TAB, TMOD_SHIFT);
    want_key("backspace", KEY_BACKSPACE, 0, TKEY_BACKSPACE, 0);
    want_key("escape", KEY_ESC, 0, TKEY_ESCAPE, 0);
    want_key("up", KEY_UP, 0, TKEY_UP, 0);
    want_key("down", KEY_DOWN, 0, TKEY_DOWN, 0);
    want_key("left", KEY_LEFT, 0, TKEY_LEFT, 0);
    want_key("right", KEY_RIGHT, 0, TKEY_RIGHT, 0);
    want_key("ctrl-right", KEY_RIGHT, TMOD_CTRL, TKEY_RIGHT, TMOD_CTRL);
    want_key("insert", KEY_INSERT, 0, TKEY_INS, 0);
    want_key("delete", KEY_DELETE, 0, TKEY_DEL, 0);
    want_key("home", KEY_HOME, 0, TKEY_HOME, 0);
    want_key("end", KEY_END, 0, TKEY_END, 0);
    want_key("pgup", KEY_PAGEUP, 0, TKEY_PAGEUP, 0);
    want_key("pgdn", KEY_PAGEDOWN, 0, TKEY_PAGEDOWN, 0);
    {
        static const int fkeys[12] = { KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6,
                                       KEY_F7, KEY_F8, KEY_F9, KEY_F10, KEY_F11, KEY_F12 };
        for (int i = 0; i < 12; i++) {
            char name[8];
            snprintf(name, sizeof name, "F%d", i + 1);
            want_key(name, fkeys[i], 0, TKEY_F1 + i, 0);
        }
    }

    /* D */
    want_char("space", KEY_SPACE, 0, false, ' ', 0);
    want_char("shift-space keeps shift", KEY_SPACE, TMOD_SHIFT, false, ' ', TMOD_SHIFT);
    want_char("kp 7", KEY_KP7, 0, false, '7', 0);
    want_char("kp 0", KEY_KP0, 0, false, '0', 0);
    want_char("kp .", KEY_KPDOT, 0, false, '.', 0);
    want_char("kp +", KEY_KPPLUS, 0, false, '+', 0);
    want_char("kp /", KEY_KPSLASH, 0, false, '/', 0);

    /* E */
    CHECK(term_mod_bit(KEY_LEFTSHIFT) == TMOD_SHIFT, "lshift");
    CHECK(term_mod_bit(KEY_RIGHTSHIFT) == TMOD_SHIFT, "rshift");
    CHECK(term_mod_bit(KEY_LEFTCTRL) == TMOD_CTRL, "lctrl");
    CHECK(term_mod_bit(KEY_RIGHTCTRL) == TMOD_CTRL, "rctrl");
    CHECK(term_mod_bit(KEY_LEFTALT) == TMOD_ALT, "lalt");
    CHECK(term_mod_bit(KEY_RIGHTALT) == TMOD_ALT, "altgr");
    CHECK(term_mod_bit(KEY_A) == 0, "a is not a modifier");
    CHECK(term_mod_bit(KEY_CAPSLOCK) == 0, "caps lock is not a held modifier");
    CHECK(term_mod_bit(KEY_LEFTMETA) == 0, "meta is not mapped");

    /* F */
    CHECK(term_keymap(KEY_LEFTSHIFT, 0, false).kind == TK_NONE, "shift types nothing");
    CHECK(term_keymap(KEY_LEFTCTRL, 0, false).kind == TK_NONE, "ctrl types nothing");
    CHECK(term_keymap(KEY_CAPSLOCK, 0, false).kind == TK_NONE, "caps lock types nothing");
    CHECK(term_keymap(KEY_NUMLOCK, 0, false).kind == TK_NONE, "num lock types nothing");
    CHECK(term_keymap(KEY_VOLUMEUP, 0, false).kind == TK_NONE, "media key types nothing");
    CHECK(term_keymap(0, 0, false).kind == TK_NONE, "code 0 types nothing");
    CHECK(term_keymap(-1, 0, false).kind == TK_NONE, "negative code types nothing");

    if (fails) { printf("term_keymap_test: %d FAILED\n", fails); return 1; }
    printf("term_keymap_test: all passed\n");
    return 0;
}
