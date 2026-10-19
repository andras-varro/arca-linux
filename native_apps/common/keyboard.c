/*
 * Generic on-screen keyboard module for Arca native apps.
 * Uses the 32bpp native_apps drawing API (framebuffer.h / common.h).
 *
 * See keyboard.h for the public API.
 */

#include "keyboard.h"
#include "common.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* ── Layout definitions ──────────────────────────────────────────────────── */

/* KB_LAYOUT_ALPHA: 9 cols × 3 rows (matches original highscore keyboard) */
static const char *alpha_keys[] = {
    "ABCDEFGHI",
    "JKLMNOPQR",
    "STUVWXYZ_",   /* _ displayed as _ on key, stored as space */
    NULL
};

/* KB_LAYOUT_ALPHANUM: 10 cols × 4 rows */
static const char *alphanum_keys[] = {
    "ABCDEFGHIJ",
    "KLMNOPQRST",
    "UVWXYZ.-_ ",
    "0123456789",
    NULL
};

/* KB_LAYOUT_FULL: 10 cols × 4 rows, upper case */
static const char *full_upper_keys[] = {
    "ABCDEFGHIJ",
    "KLMNOPQRST",
    "UVWXYZ.-_!",
    "0123456789",
    NULL
};

/* KB_LAYOUT_FULL: 10 cols × 4 rows, lower case */
static const char *full_lower_keys[] = {
    "abcdefghij",
    "klmnopqrst",
    "uvwxyz.-_!",
    "0123456789",
    NULL
};

/* KB_LAYOUT_NUMERIC: 4 cols × 4 rows (space = empty slot, not rendered) */
static const char *numeric_keys[] = {
    "123.",
    "456:",
    "789 ",
    " 0  ",
    NULL
};

/* ── Helpers ─────────────────────────────────────────────────────────────── */

/* Count rows in a NULL-terminated key array */
static int kb_row_count(const char **keys) {
    int n = 0;
    while (keys[n]) n++;
    return n;
}

/* Column count = length of first row string */
static int kb_col_count(const char **keys) {
    return keys[0] ? (int)strlen(keys[0]) : 0;
}

/* Wait for finger lift + 300 ms settle (matches hs_drain_touches pattern) */
static void kb_drain_touches(TouchInput *touch) {
    uint32_t start = get_time_ms();
    while (get_time_ms() - start < 2000) {
        touch_poll(touch);
        TouchState s = touch_get_state(touch);
        if (!s.held) break;
        ui_frame_service();
        usleep(10000);
    }
    start = get_time_ms();
    while (get_time_ms() - start < 300) {
        touch_poll(touch);
        ui_frame_service();
        usleep(10000);
    }
}

/* ── Main entry point ────────────────────────────────────────────────────── */

/* Is (r, c) a real key?  NUMERIC leaves empty slots that are not rendered. */
static bool kb_cell_ok(const char **keys, KeyboardLayout layout, int r, int c) {
    return !(layout == KB_LAYOUT_NUMERIC && keys[r][c] == ' ');
}

static int kb_abs(int v) { return v < 0 ? -v : v; }

/* Move the focus one step.  Row `rows` is the action row.  Left/right wrap
 * within the row and skip empty slots; up/down stop at the top and bottom and
 * land on the key nearest in x, since the rows differ in length and pitch.
 * Shifts only, no divide: the cell centres are x + width/2 by a constant. */
static void kb_focus_move(const char **keys, KeyboardLayout layout, int rows, int cols,
                          int n_action, int btn_w, int aw, int *fr, int *fc, int dir) {
    if (dir == BTN_ID_LEFT || dir == BTN_ID_RIGHT) {
        int width = (*fr == rows) ? n_action : cols;
        int step  = (dir == BTN_ID_LEFT) ? -1 : 1;
        int c = *fc;
        for (int k = 0; k < width; k++) {
            c += step;
            if (c < 0) c = width - 1;
            if (c >= width) c = 0;
            if (*fr == rows || kb_cell_ok(keys, layout, *fr, c)) { *fc = c; return; }
        }
        return;
    }
    int nr = *fr + (dir == BTN_ID_UP ? -1 : 1);
    if (nr < 0 || nr > rows) return;
    int cx = (*fr == rows) ? *fc * aw + (aw >> 1) : *fc * btn_w + (btn_w >> 1);
    int width = (nr == rows) ? n_action : cols;
    int best = -1, best_d = 0;
    for (int c = 0; c < width; c++) {
        if (nr != rows && !kb_cell_ok(keys, layout, nr, c)) continue;
        int x = (nr == rows) ? c * aw + (aw >> 1) : c * btn_w + (btn_w >> 1);
        int d = kb_abs(x - cx);
        if (best < 0 || d < best_d) { best = c; best_d = d; }
    }
    if (best >= 0) { *fr = nr; *fc = best; }
}

enum { KA_DEL, KA_CLEAR, KA_SHIFT, KA_SPACE, KA_CANCEL, KA_OK };

static int kb_run(Framebuffer *fb, TouchInput *touch, GamepadManager *gm,
                  const char *title, char *buf, int max_len, KeyboardLayout layout,
                  bool mask) {

    /* ── Select key layout ───────────────────────────────────────────── */
    const char **keys      = alpha_keys;
    const char **alt_keys  = NULL;          /* for FULL shift toggle */
    bool         shifted   = true;          /* FULL starts upper */
    bool         is_alpha  = false;         /* ALPHA _ → space mapping */

    switch (layout) {
    case KB_LAYOUT_ALPHA:
        keys     = alpha_keys;
        is_alpha = true;
        break;
    case KB_LAYOUT_ALPHANUM:
        keys = alphanum_keys;
        break;
    case KB_LAYOUT_FULL:
        keys     = full_upper_keys;
        alt_keys = full_lower_keys;
        break;
    case KB_LAYOUT_NUMERIC:
        keys = numeric_keys;
        break;
    }

    int rows = kb_row_count(keys);
    int cols = kb_col_count(keys);

    /* ── Working buffer ──────────────────────────────────────────────── */
    char work[max_len + 1];
    strncpy(work, buf, max_len);
    work[max_len] = '\0';
    int cursor = (int)strlen(work);

    /* ── Geometry ────────────────────────────────────────────────────── */
    int safe_l = SCREEN_SAFE_LEFT;
    int safe_w = SCREEN_SAFE_WIDTH;

    int btn_w  = safe_w / cols;
    int btn_h  = 52;
    int kb_y   = 120;                          /* top of key grid */
    int action_y = kb_y + rows * btn_h + 10;   /* action button row */

    /* Clamp action_y so it doesn't run off-screen */
    if (action_y > 390) action_y = 390;

    /* ── Build letter buttons ────────────────────────────────────────── */
    Button letter_btns[rows][cols];
    for (int r = 0; r < rows; r++) {
        const char *row_str = keys[r];
        for (int c = 0; c < cols; c++) {
            char ch = row_str[c];
            char label[2] = { ch, '\0' };

            /* For NUMERIC layout, spaces are empty slots — skip init */
            if (layout == KB_LAYOUT_NUMERIC && ch == ' ') {
                memset(&letter_btns[r][c], 0, sizeof(Button));
                continue;
            }

            button_init_full(&letter_btns[r][c],
                             safe_l + c * btn_w, kb_y + r * btn_h,
                             btn_w - 3, btn_h - 3,
                             label,
                             RGB(30, 30, 70), COLOR_WHITE, RGB(80, 80, 200),
                             2);
        }
    }

    /* ── Action buttons ──────────────────────────────────────────────── */
    bool has_shift = (layout == KB_LAYOUT_FULL);
    bool has_space = (layout == KB_LAYOUT_FULL);   /* the others keep a space on a key */
    int n_action = 4 + (has_shift ? 1 : 0) + (has_space ? 1 : 0);   /* DEL CLEAR [SHIFT] [SPACE] CANCEL OK */
    int aw = safe_w / n_action;

    const char *del_label = (layout == KB_LAYOUT_NUMERIC) ? "<-" : "DEL";

    Button btn_del, btn_clear, btn_shift, btn_space, btn_cancel, btn_ok;
    int col_idx = 0;

    button_init_full(&btn_del, safe_l + col_idx * aw, action_y, aw - 4, btn_h,
                     del_label,
                     RGB(80, 40, 0), COLOR_WHITE, RGB(200, 100, 0), 2);
    col_idx++;

    button_init_full(&btn_clear, safe_l + col_idx * aw, action_y, aw - 4, btn_h,
                     "CLEAR",
                     RGB(60, 0, 0), COLOR_WHITE, RGB(200, 0, 0), 2);
    col_idx++;

    if (has_shift) {
        button_init_full(&btn_shift, safe_l + col_idx * aw, action_y, aw - 4, btn_h,
                         "SHIFT",
                         RGB(30, 30, 80), COLOR_WHITE, RGB(100, 100, 220), 2);
        col_idx++;
    }

    if (has_space) {
        button_init_full(&btn_space, safe_l + col_idx * aw, action_y, aw - 4, btn_h,
                         "SPACE",
                         RGB(30, 30, 80), COLOR_WHITE, RGB(100, 100, 220), 2);
        col_idx++;
    }

    button_init_full(&btn_cancel, safe_l + col_idx * aw, action_y, aw - 4, btn_h,
                     "CANCEL",
                     RGB(80, 0, 0), COLOR_WHITE, RGB(200, 0, 0), 2);
    col_idx++;

    button_init_full(&btn_ok, safe_l + col_idx * aw, action_y, aw - 4, btn_h,
                     "OK",
                     RGB(0, 70, 0), COLOR_WHITE, RGB(0, 180, 0), 2);

    /* The action row in order, so a focus column maps to one button and one id. */
    Button *act_btn[6];
    int     act_id[6];
    int     na = 0;
    act_btn[na] = &btn_del;    act_id[na++] = KA_DEL;
    act_btn[na] = &btn_clear;  act_id[na++] = KA_CLEAR;
    if (has_shift) { act_btn[na] = &btn_shift; act_id[na++] = KA_SHIFT; }
    if (has_space) { act_btn[na] = &btn_space; act_id[na++] = KA_SPACE; }
    act_btn[na] = &btn_cancel; act_id[na++] = KA_CANCEL;
    act_btn[na] = &btn_ok;     act_id[na++] = KA_OK;

    /* Pad / keyboard focus: (frow, fcol), frow == rows being the action row.
     * Edges are our own, from the level, seeded TRUE so a button still down
     * from the screen before must be released and pressed again. */
    int  frow = 0, fcol = 0;
    bool focus_shown = true;
    bool prev_held[BTN_ID_COUNT];
    for (int b = 0; b < BTN_ID_COUNT; b++) prev_held[b] = true;

    /* ── Drain stale touches ─────────────────────────────────────────── */
    kb_drain_touches(touch);

    /* ── Main loop ───────────────────────────────────────────────────── */
    uint32_t last_press = 0;

    /* Repaint only when the screen would change.  The whole frame is a
     * function of (work, cursor, shifted): no Button here is driven through
     * button_update()/button_check_press() — hits are tested with
     * button_is_touched(), which changes no visual_state — so there is no
     * pressed look, no timer and no animation to keep painting.  A full
     * clear + fb_swap every frame copies the 1.5 MB back buffer to the
     * uncached framebuffer and cost more CPU than a game in play. */
    bool dirty = true;                     /* the first frame always draws */

    for (;;) {
        /* Which glyph set the keys show — needed by input as well as drawing,
         * so it is computed whether or not this frame paints. */
        const char **cur_keys = keys;
        if (has_shift && !shifted)
            cur_keys = alt_keys;

        /* ── Draw ────────────────────────────────────────────────────── */
        if (dirty) {
            fb_clear(fb, COLOR_BLACK);

            /* Title */
            int tw = text_measure_width(title, 2);
            fb_draw_text(fb, fb->width / 2 - tw / 2, 10, title, COLOR_YELLOW, 2);

            /* Input field box */
            int box_x = safe_l + 20;
            int box_w = safe_w - 40;
            fb_fill_rect(fb, box_x, 50, box_w, 52, RGB(20, 20, 20));
            fb_draw_rect(fb, box_x, 50, box_w, 52, COLOR_CYAN);

            /* Current text with underscore cursor */
            char display[max_len + 2];
            strncpy(display, work, max_len);
            display[cursor] = '\0';           /* safety — cursor tracks length */
            int dlen = (int)strlen(display);
            if (mask) for (int mi = 0; mi < dlen; mi++) display[mi] = '*';
            if (dlen < max_len) {
                display[dlen]     = '_';
                display[dlen + 1] = '\0';
            }
            int nw = text_measure_width(display, 3);
            fb_draw_text(fb, box_x + (box_w - nw) / 2, 60, display, COLOR_CYAN, 3);

            /* Hint for ALPHA layout */
            if (is_alpha) {
                fb_draw_text(fb, safe_l, kb_y - 18, "TAP _ FOR SPACE",
                             RGB(80, 80, 80), 1);
            }

            /* Key grid */
            for (int r = 0; r < rows; r++) {
                for (int c = 0; c < cols; c++) {
                    char ch = cur_keys[r][c];
                    if (layout == KB_LAYOUT_NUMERIC && ch == ' ')
                        continue;   /* empty slot */

                    /* Update label to reflect current shift state */
                    char label[2] = { ch, '\0' };
                    strncpy(letter_btns[r][c].text, label, sizeof(letter_btns[r][c].text) - 1);

                    button_draw(fb, &letter_btns[r][c]);
                }
            }

            /* Action buttons */
            button_draw(fb, &btn_del);
            button_draw(fb, &btn_clear);
            if (has_shift)
                button_draw(fb, &btn_shift);
            if (has_space)
                button_draw(fb, &btn_space);
            button_draw(fb, &btn_cancel);
            button_draw(fb, &btn_ok);

            if (gm) {
                if (focus_shown) {
                    const Button *fbtn = (frow == rows) ? act_btn[fcol] : &letter_btns[frow][fcol];
                    for (int k = 0; k < 3; k++)
                        fb_draw_rect(fb, fbtn->x - k, fbtn->y - k,
                                     fbtn->width + 2 * k, fbtn->height + 2 * k, COLOR_YELLOW);
                }
                int hy = action_y + btn_h + 8;
                if (hy + 10 <= SCREEN_VISIBLE_BOTTOM)
                    fb_draw_text(fb, safe_l, hy,
                                 "ARROWS MOVE  A/SPACE/ENTER KEY  SELECT/BKSP DEL  START/ESC OK",
                                 RGB(120, 120, 120), 1);
            }

            fb_swap(fb);
            dirty = false;
        }

        /* ⚠️ A blocking sub-loop IS a render loop, and this one owns the screen
         * for as long as a player takes to type a name.  Without this the mix bus
         * stops advancing (it counts frames RENDERED), so the game-over fanfare
         * queued one statement before gameover_init() is DEFERRED until this
         * returns and a music bed's fade freezes mid-way.  Rationale and the
         * registration side: common.h → PER-FRAME SERVICE. */
        ui_frame_service();

        /* ── Input ───────────────────────────────────────────────────── */
        touch_poll(touch);
        TouchState state = touch_get_state(touch);
        uint32_t   now   = get_time_ms();

        /* What was pressed this frame, by finger or by key: a letter cell
         * (pick_r, pick_c) or an action id (pick_a). */
        int pick_r = -1, pick_c = -1, pick_a = -1;

        if (state.pressed && (now - last_press) > 180) {
            /* A real touch hides the focus frame; the next key shows it. */
            if (gm && focus_shown) { focus_shown = false; dirty = true; }

            for (int r = 0; r < rows && pick_r < 0; r++) {
                for (int c = 0; c < cols && pick_r < 0; c++) {
                    if (!kb_cell_ok(cur_keys, layout, r, c))
                        continue;
                    if (button_is_touched(&letter_btns[r][c], state.x, state.y)) {
                        pick_r = r;
                        pick_c = c;
                    }
                }
            }
            for (int a = 0; a < na && pick_r < 0; a++)
                if (button_is_touched(act_btn[a], state.x, state.y))
                    pick_a = act_id[a];
            if (pick_r >= 0 || pick_a >= 0) last_press = now;
        }

        if (gm && pick_r < 0 && pick_a < 0) {
            InputState in;
            memset(&in, 0, sizeof(in));
            gamepad_poll(gm, &in, 0, 0, false);
            bool edge[BTN_ID_COUNT];
            for (int b = 0; b < BTN_ID_COUNT; b++) {
                bool held = in.buttons[b].held;
                edge[b] = (held || in.buttons[b].pressed) && !prev_held[b];
                prev_held[b] = held;
            }
            int dir = edge[BTN_ID_UP] ? BTN_ID_UP : edge[BTN_ID_DOWN] ? BTN_ID_DOWN
                    : edge[BTN_ID_LEFT] ? BTN_ID_LEFT : edge[BTN_ID_RIGHT] ? BTN_ID_RIGHT : -1;
            bool act = edge[BTN_ID_JUMP] || edge[BTN_ID_ACTION];

            if (edge[BTN_ID_PAUSE]) {
                pick_a = KA_OK;
            } else if (edge[BTN_ID_BACK]) {
                pick_a = KA_DEL;
            } else if ((dir >= 0 || act) && !focus_shown) {
                focus_shown = true;           /* the first key only shows it */
                dirty = true;
            } else if (dir >= 0) {
                int fr = frow, fc = fcol;
                kb_focus_move(keys, layout, rows, cols, na, btn_w, aw, &fr, &fc, dir);
                if (fr != frow || fc != fcol) { frow = fr; fcol = fc; dirty = true; }
            } else if (act) {
                if (frow == rows) pick_a = act_id[fcol];
                else { pick_r = frow; pick_c = fcol; }
            }
        }

        if (pick_r >= 0) {
            char ch = cur_keys[pick_r][pick_c];
            if (cursor < max_len) {
                /* ALPHA layout: _ key stores space */
                char store = ch;
                if (is_alpha && ch == '_')
                    store = ' ';
                work[cursor++] = store;
                work[cursor]   = '\0';
                dirty = true;
            }
        } else if (pick_a == KA_DEL) {
            if (cursor > 0) {
                work[--cursor] = '\0';
                dirty = true;
            }
        } else if (pick_a == KA_CLEAR) {
            if (cursor > 0) dirty = true;
            cursor = 0;
            memset(work, 0, max_len + 1);
        } else if (pick_a == KA_SHIFT) {
            shifted = !shifted;
            dirty = true;
            /* Swap key pointer for next frame redraw */
            keys = shifted ? full_upper_keys : full_lower_keys;
        } else if (pick_a == KA_SPACE) {
            if (cursor < max_len) {
                work[cursor++] = ' ';
                work[cursor]   = '\0';
                dirty = true;
            }
        } else if (pick_a == KA_CANCEL) {
            return KB_RESULT_CANCEL;   /* buf unchanged */
        } else if (pick_a == KA_OK) {
            /* Trim trailing spaces -- but not from FULL, whose SPACE key is typed on
             * purpose (a password may end in one). */
            int l = (int)strlen(work);
            if (layout != KB_LAYOUT_FULL)
                while (l > 0 && work[l - 1] == ' ') work[--l] = '\0';

            strncpy(buf, work, max_len);
            buf[max_len] = '\0';
            return KB_RESULT_OK;
        }

        /* Still a full-rate tick when nothing paints: ui_frame_service() above
         * must keep the mix bus advancing, and a tap must be seen promptly. */
        usleep(FRAME_DELAY_ACTIVE_US);
    }
}

int keyboard_enter(Framebuffer *fb, TouchInput *touch, GamepadManager *gm,
                   const char *title, char *buf, int max_len, KeyboardLayout layout) {
    return kb_run(fb, touch, gm, title, buf, max_len, layout, false);
}

int keyboard_enter_masked(Framebuffer *fb, TouchInput *touch, GamepadManager *gm,
                          const char *title, char *buf, int max_len, KeyboardLayout layout) {
    return kb_run(fb, touch, gm, title, buf, max_len, layout, true);
}
