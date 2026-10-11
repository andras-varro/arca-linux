/* term_keymap.h -- evdev key code to terminal key, pure.
 *
 * The terminal app reads USB / Bluetooth keyboards itself (gamepad.c folds a
 * keyboard into nine abstract buttons and has no letters), and this is the one
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

/* The TMOD_* bit a key code holds down (Shift, Ctrl, Alt, AltGr), else 0. */
unsigned term_mod_bit(int code);

/* The terminal key for a press of `code` while `mods` are held and caps lock
 * is `caps`.  TK_NONE for modifiers, lock keys and anything unmapped. */
TermKeyOut term_keymap(int code, unsigned mods, bool caps);

#endif /* TERM_KEYMAP_H */
