/* term_keymap.h -- evdev key code to terminal key, pure.
 *
 * The terminal app and the on-screen keyboard (common/keyboard.c, for a physical
 * keyboard) both read keyboards themselves (gamepad.c folds a keyboard into
 * abstract buttons and has no letters), and this is the one
 * place a key code becomes something to type.  No libvterm, no framebuffer, no
 * device: the app turns a TermKeyOut into vterm_keyboard_unichar() or
 * vterm_keyboard_key(), and tests/term_keymap_test.c drives every rule here.
 *
 * Layout: US.  Shift is resolved into the character here (so a caller never
 * guesses between 'a' and 'A'); it stays in `mods` only for the special keys,
 * where it changes the sequence (Shift-Tab), and for Space.  Ctrl on a letter
 * returns the lower-case letter with TMOD_CTRL set, which is the form libvterm
 * turns into a control code -- an upper-case letter with Ctrl would come out as
 * a CSI-u sequence the shell does not understand.
 */
#ifndef TERM_KEYMAP_H
#define TERM_KEYMAP_H

#include <stdbool.h>
#include <stdint.h>
#include <linux/input.h>

/* The same values as libvterm's VTERM_MOD_SHIFT / ALT / CTRL, so the app casts. */
#define TMOD_SHIFT 0x01u
#define TMOD_ALT   0x02u
#define TMOD_CTRL  0x04u

typedef enum {
    TK_NONE = 0,   /* modifier, lock, or a key the terminal has no use for */
    TK_CHAR,       /* `ch` is a Unicode character to type */
    TK_KEY         /* `key` is a special key */
} TermKeyKind;

/* The order matches libvterm's VTermKey up to PAGEDOWN; terminal.c maps by
 * name rather than by value, so this is a convenience and not a contract. */
typedef enum {
    TKEY_NONE = 0,
    TKEY_ENTER, TKEY_TAB, TKEY_BACKSPACE, TKEY_ESCAPE,
    TKEY_UP, TKEY_DOWN, TKEY_LEFT, TKEY_RIGHT,
    TKEY_INS, TKEY_DEL, TKEY_HOME, TKEY_END, TKEY_PAGEUP, TKEY_PAGEDOWN,
    TKEY_F1 = 16   /* F1..F12 are TKEY_F1 .. TKEY_F1 + 11 */
} TermKeyId;

typedef struct {
    TermKeyKind kind;
    uint32_t    ch;     /* TK_CHAR */
    int         key;    /* TK_KEY: a TermKeyId */
    unsigned    mods;   /* TMOD_* to hand to libvterm with it */
} TermKeyOut;

/* Header-only on purpose: gamepad-side text entry (common/keyboard.c, linked by
 * every app and a dozen host tests) and the terminal app both need the table,
 * and a header costs no extra object on any link line. */

typedef struct { int code; char base; char shifted; } CharKey;

/* US layout.  Letters are listed lower case with the upper-case letter as the
 * shifted glyph, so caps lock is one rule for the whole table. */
static const CharKey char_keys[] = {
    { KEY_1, '1', '!' }, { KEY_2, '2', '@' }, { KEY_3, '3', '#' }, { KEY_4, '4', '$' },
    { KEY_5, '5', '%' }, { KEY_6, '6', '^' }, { KEY_7, '7', '&' }, { KEY_8, '8', '*' },
    { KEY_9, '9', '(' }, { KEY_0, '0', ')' },
    { KEY_MINUS, '-', '_' }, { KEY_EQUAL, '=', '+' },
    { KEY_Q, 'q', 'Q' }, { KEY_W, 'w', 'W' }, { KEY_E, 'e', 'E' }, { KEY_R, 'r', 'R' },
    { KEY_T, 't', 'T' }, { KEY_Y, 'y', 'Y' }, { KEY_U, 'u', 'U' }, { KEY_I, 'i', 'I' },
    { KEY_O, 'o', 'O' }, { KEY_P, 'p', 'P' },
    { KEY_LEFTBRACE, '[', '{' }, { KEY_RIGHTBRACE, ']', '}' },
    { KEY_A, 'a', 'A' }, { KEY_S, 's', 'S' }, { KEY_D, 'd', 'D' }, { KEY_F, 'f', 'F' },
    { KEY_G, 'g', 'G' }, { KEY_H, 'h', 'H' }, { KEY_J, 'j', 'J' }, { KEY_K, 'k', 'K' },
    { KEY_L, 'l', 'L' },
    { KEY_SEMICOLON, ';', ':' }, { KEY_APOSTROPHE, '\'', '"' }, { KEY_GRAVE, '`', '~' },
    { KEY_BACKSLASH, '\\', '|' },
    { KEY_Z, 'z', 'Z' }, { KEY_X, 'x', 'X' }, { KEY_C, 'c', 'C' }, { KEY_V, 'v', 'V' },
    { KEY_B, 'b', 'B' }, { KEY_N, 'n', 'N' }, { KEY_M, 'm', 'M' },
    { KEY_COMMA, ',', '<' }, { KEY_DOT, '.', '>' }, { KEY_SLASH, '/', '?' },
    { KEY_102ND, '<', '>' },
    { KEY_SPACE, ' ', ' ' },
    /* keypad, num lock taken as on */
    { KEY_KP0, '0', '0' }, { KEY_KP1, '1', '1' }, { KEY_KP2, '2', '2' }, { KEY_KP3, '3', '3' },
    { KEY_KP4, '4', '4' }, { KEY_KP5, '5', '5' }, { KEY_KP6, '6', '6' }, { KEY_KP7, '7', '7' },
    { KEY_KP8, '8', '8' }, { KEY_KP9, '9', '9' }, { KEY_KPDOT, '.', '.' },
    { KEY_KPSLASH, '/', '/' }, { KEY_KPASTERISK, '*', '*' }, { KEY_KPMINUS, '-', '-' },
    { KEY_KPPLUS, '+', '+' },
};

typedef struct { int code; int key; } SpecialKey;

static const SpecialKey special_keys[] = {
    { KEY_ENTER, TKEY_ENTER }, { KEY_KPENTER, TKEY_ENTER },
    { KEY_TAB, TKEY_TAB }, { KEY_BACKSPACE, TKEY_BACKSPACE }, { KEY_ESC, TKEY_ESCAPE },
    { KEY_UP, TKEY_UP }, { KEY_DOWN, TKEY_DOWN }, { KEY_LEFT, TKEY_LEFT }, { KEY_RIGHT, TKEY_RIGHT },
    { KEY_INSERT, TKEY_INS }, { KEY_DELETE, TKEY_DEL },
    { KEY_HOME, TKEY_HOME }, { KEY_END, TKEY_END },
    { KEY_PAGEUP, TKEY_PAGEUP }, { KEY_PAGEDOWN, TKEY_PAGEDOWN },
    { KEY_F1, TKEY_F1 + 0 }, { KEY_F2, TKEY_F1 + 1 }, { KEY_F3, TKEY_F1 + 2 },
    { KEY_F4, TKEY_F1 + 3 }, { KEY_F5, TKEY_F1 + 4 }, { KEY_F6, TKEY_F1 + 5 },
    { KEY_F7, TKEY_F1 + 6 }, { KEY_F8, TKEY_F1 + 7 }, { KEY_F9, TKEY_F1 + 8 },
    { KEY_F10, TKEY_F1 + 9 }, { KEY_F11, TKEY_F1 + 10 }, { KEY_F12, TKEY_F1 + 11 },
};

static inline unsigned term_mod_bit(int code) {
    switch (code) {
    case KEY_LEFTSHIFT: case KEY_RIGHTSHIFT: return TMOD_SHIFT;
    case KEY_LEFTCTRL:  case KEY_RIGHTCTRL:  return TMOD_CTRL;
    case KEY_LEFTALT:   case KEY_RIGHTALT:   return TMOD_ALT;
    default: return 0;
    }
}

static inline TermKeyOut term_keymap(int code, unsigned mods, bool caps) {
    TermKeyOut out = { TK_NONE, 0, 0, 0 };

    for (unsigned i = 0; i < sizeof(special_keys) / sizeof(special_keys[0]); i++) {
        if (special_keys[i].code == code) {
            out.kind = TK_KEY;
            out.key  = special_keys[i].key;
            out.mods = mods;
            return out;
        }
    }

    for (unsigned i = 0; i < sizeof(char_keys) / sizeof(char_keys[0]); i++) {
        if (char_keys[i].code != code) continue;
        char base = char_keys[i].base;
        bool letter = (base >= 'a' && base <= 'z');
        bool shift = (mods & TMOD_SHIFT) != 0;
        /* Caps lock flips a letter's case and nothing else. */
        bool upper = (letter && caps) ? !shift : shift;

        out.kind = TK_CHAR;
        out.mods = mods & ~TMOD_SHIFT;
        if (mods & TMOD_CTRL) {
            /* libvterm wants the lower-case letter.  For a symbol Shift still
             * picks the glyph: Ctrl-Shift-6 is Ctrl-^, Ctrl-Shift-- is Ctrl-_. */
            out.ch = (uint32_t)(unsigned char)((shift && !letter) ? char_keys[i].shifted : base);
        } else {
            out.ch = (uint32_t)(unsigned char)(upper ? char_keys[i].shifted : base);
        }
        /* libvterm treats Shift-Space as a case of its own. */
        if (base == ' ' && shift) out.mods |= TMOD_SHIFT;
        return out;
    }
    return out;   /* modifiers, caps / num / scroll lock, media keys, ... */
}

/* The toolbar's sticky modifiers.  Each one is armed by a tap and applies to the
 * next key only; term_sticky_take() returns the TMOD_* bits and releases them,
 * setting *changed when something was released so the caller redraws the button. */
typedef struct { bool ctrl, alt; } TermSticky;

static inline unsigned term_sticky_take(TermSticky *s, bool *changed) {
    unsigned m = (s->ctrl ? TMOD_CTRL : 0) | (s->alt ? TMOD_ALT : 0);
    *changed = (m != 0);
    s->ctrl = false;
    s->alt = false;
    return m;
}

/* The bytes for one ASCII character with modifiers: Alt is an ESC prefix (what
 * nano reads as M-key), Ctrl clears bits 5-6 of '@'..'_' and lower case.  Returns
 * the byte count (0 for a non-ASCII character, which the caller hands to libvterm). */
static inline int term_char_bytes(uint32_t ch, unsigned mods, unsigned char out[3]) {
    int n = 0;
    if (ch >= 0x80) return 0;
    if (mods & TMOD_ALT) out[n++] = 0x1b;
    if ((mods & TMOD_CTRL) && ch >= 0x40 && ch <= 0x7f) ch &= 0x1f;
    else if ((mods & TMOD_CTRL) && ch == ' ') ch = 0;
    out[n++] = (unsigned char)ch;
    return n;
}

#endif /* TERM_KEYMAP_H */
