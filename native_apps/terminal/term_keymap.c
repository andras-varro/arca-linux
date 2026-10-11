/* term_keymap.c -- see term_keymap.h. */
#include "terminal/term_keymap.h"

#include <linux/input.h>

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

unsigned term_mod_bit(int code) {
    switch (code) {
    case KEY_LEFTSHIFT: case KEY_RIGHTSHIFT: return TMOD_SHIFT;
    case KEY_LEFTCTRL:  case KEY_RIGHTCTRL:  return TMOD_CTRL;
    case KEY_LEFTALT:   case KEY_RIGHTALT:   return TMOD_ALT;
    default: return 0;
    }
}

TermKeyOut term_keymap(int code, unsigned mods, bool caps) {
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
