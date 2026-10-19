#ifndef KEYBOARD_H
#define KEYBOARD_H

#include "framebuffer.h"
#include "touch_input.h"
#include "gamepad.h"
#include <stdbool.h>

/* Predefined keyboard layouts */
typedef enum {
    KB_LAYOUT_ALPHA,        /* A-Z + space (high-score compatible) */
    KB_LAYOUT_ALPHANUM,     /* A-Z, 0-9, common symbols, space */
    KB_LAYOUT_FULL,         /* A-Z/a-z (shift toggle), 0-9, symbols */
    KB_LAYOUT_NUMERIC,      /* 0-9, dot, colon (for IP/port) */
} KeyboardLayout;

/* Result codes */
#define KB_RESULT_OK        0
#define KB_RESULT_CANCEL    1

/*
 * Show an on-screen keyboard and let the user type a string.
 * Blocks until the user presses OK or Cancel.
 *
 * @param fb       Framebuffer to draw on (32bpp)
 * @param touch    Touch input device
 * @param title    Title text shown above input field
 * @param buf      Buffer with initial value; result written on OK
 * @param max_len  Maximum string length (not counting NUL)
 * @param layout   Which keyboard layout to show
 * @param gm       Pad / keyboard input, or NULL for touch only.  With it:
 *                 UP/DOWN/LEFT/RIGHT move a focus frame over the keys (the
 *                 action row included), JUMP / ACTION press the focused key,
 *                 BACK deletes a character, PAUSE is OK.  The frame starts on
 *                 the first key; a touch hides it and the next key shows it
 *                 again.  Buttons already down on entry need a fresh press.
 * @return         KB_RESULT_OK or KB_RESULT_CANCEL
 */
int keyboard_enter(Framebuffer *fb, TouchInput *touch, GamepadManager *gm,
                   const char *title, char *buf, int max_len, KeyboardLayout layout);

/* Same as keyboard_enter(), but the field shows one '*' per character (a
 * password).  The returned text in buf is the real one. */
int keyboard_enter_masked(Framebuffer *fb, TouchInput *touch, GamepadManager *gm,
                          const char *title, char *buf, int max_len, KeyboardLayout layout);

#endif /* KEYBOARD_H */
