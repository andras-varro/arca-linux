/**
 * Common UI Library Implementation for Arca
 */

#include "common.h"
/* For the high-score chime in gameover_update().  ⚠️ common.h itself must stay
 * free of this include — see its PER-FRAME SERVICE block. */
#include "audio.h"
#include "start_menu.h"
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/file.h>

// ============================================================================
// TIME UTILITIES
// ============================================================================

uint32_t get_time_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (tv.tv_sec * 1000) + (tv.tv_usec / 1000);
}

// ============================================================================
// SINGLETON INSTANCE LOCK
// ============================================================================

int acquire_instance_lock(const char *app_name) {
    char lock_path[256];
    snprintf(lock_path, sizeof(lock_path), "/tmp/.%s.lock", app_name);

    int fd = open(lock_path, O_CREAT | O_RDWR, 0644);
    if (fd < 0) {
        perror("acquire_instance_lock: open");
        return -1;
    }

    if (flock(fd, LOCK_EX | LOCK_NB) < 0) {
        /* Another instance holds the lock */
        close(fd);
        return -1;
    }

    /* Write our PID for diagnostics */
    char pid_str[16];
    int len = snprintf(pid_str, sizeof(pid_str), "%d\n", getpid());
    if (ftruncate(fd, 0) == 0) {
        ssize_t w = write(fd, pid_str, len);
        (void)w;  /* Best-effort; failure is non-fatal */
    }

    return fd;  /* Caller must keep fd open — lock held until close/exit */
}

// ============================================================================
// PER-FRAME SERVICE FOR BLOCKING SUB-LOOPS   (rationale: common.h)
// ============================================================================

/* One slot, not a list: audio is the only registrant and a list would invite a
 * game to add its own, which is the signature-threading this avoided. */
static void (*ui_service_fn)(void *ctx) = NULL;
static void  *ui_service_ctx            = NULL;

void ui_frame_service_set(void (*fn)(void *ctx), void *ctx) {
    ui_service_fn  = fn;
    ui_service_ctx = ctx;
}

void ui_frame_service(void) {
    if (ui_service_fn) ui_service_fn(ui_service_ctx);
}

void *ui_frame_service_ctx(void) {
    return ui_service_ctx;
}

// ============================================================================
// TEXT UTILITIES
// ============================================================================

void text_to_uppercase(char *dest, const char *src, size_t max_len) {
    size_t i;
    for (i = 0; i < max_len - 1 && src[i] != '\0'; i++) {
        dest[i] = toupper((unsigned char)src[i]);
    }
    dest[i] = '\0';
}

int text_measure_width(const char *text, int scale) {
    return strlen(text) * 6 * scale;  // 5x7 font + 1px spacing = 6px per char
}

int text_measure_height(int scale) {
    return 7 * scale;  // 5x7 font height
}

void text_draw_centered(Framebuffer *fb, int center_x, int center_y,
                       const char *text, uint32_t color, int scale) {
    int text_width = text_measure_width(text, scale);
    int text_height = text_measure_height(scale);
    int x = center_x - (text_width / 2);
    int y = center_y - (text_height / 2);
    fb_draw_text(fb, x, y, text, color, scale);
}

void text_truncate(char *dest, size_t dest_size, const char *src,
                   int max_width, int scale) {
    if (dest == NULL || dest_size == 0) return;

    // Convert to uppercase first
    char upper[256];
    text_to_uppercase(upper, src, sizeof(upper));
    size_t len = strlen(upper);

    /* Decide what the pixel width allows: `keep` characters of `upper`,
     * followed by "..." when `ellipsis` is set. */
    size_t keep = len;
    bool ellipsis = false;
    if (max_width > 0 && text_measure_width(upper, scale) > max_width) {
        ellipsis = true;
        int available_width = max_width - text_measure_width("...", scale);
        int char_width = text_measure_width("M", scale);
        keep = (available_width > 0) ? (size_t)(available_width / char_width) : 0;
        if (keep > len) keep = len;
    }

    /* Then what the buffer allows.  A result that does not fit is cut the
     * same way a too-wide one is, so the reader still sees "..." whenever
     * there is room for it. */
    size_t room = dest_size - 1;
    if (keep + (ellipsis ? 3 : 0) > room) {
        ellipsis = true;
        keep = (room > 3) ? room - 3 : 0;
    }

    memcpy(dest, upper, keep);
    size_t n = keep;
    if (ellipsis) {
        size_t dots = (room - keep < 3) ? room - keep : 3;
        memcpy(dest + n, "...", dots);
        n += dots;
    }
    dest[n] = '\0';
}

// ============================================================================
// BUTTON AUTO-SIZING
// ============================================================================

int button_calc_min_width(const char *text, int scale, int padding) {
    int text_width = text_measure_width(text, scale);
    return text_width + (padding * 2);
}

void button_auto_size(Button *btn, int padding) {
    btn->width = button_calc_min_width(btn->text, btn->text_scale, padding);
}

// ============================================================================
// ICON DRAWING HELPERS
// ============================================================================

void icon_draw_hamburger(Framebuffer *fb, int x, int y, int size, uint32_t color) {
    int icon_width = size;
    int icon_height = size / 10;
    if (icon_height < 3) icon_height = 3;
    int icon_spacing = size / 5;
    if (icon_spacing < 6) icon_spacing = 6;
    
    int icon_x = x - icon_width / 2;
    int icon_y = y - (3 * icon_height + 2 * icon_spacing) / 2;
    
    fb_fill_rect(fb, icon_x, icon_y, icon_width, icon_height, color);
    fb_fill_rect(fb, icon_x, icon_y + icon_height + icon_spacing, icon_width, icon_height, color);
    fb_fill_rect(fb, icon_x, icon_y + 2 * (icon_height + icon_spacing), icon_width, icon_height, color);
}

void icon_draw_x(Framebuffer *fb, int x, int y, int size, uint32_t color) {
    int icon_size = size;
    int icon_x = x - icon_size / 2;
    int icon_y = y - icon_size / 2;
    int thickness = size / 8;
    if (thickness < 3) thickness = 3;
    
    // Draw X using thick lines
    for (int i = 0; i < icon_size; i++) {
        for (int t = 0; t < thickness; t++) {
            // Top-left to bottom-right
            fb_draw_pixel(fb, icon_x + i, icon_y + i + t, color);
            // Top-right to bottom-left
            fb_draw_pixel(fb, icon_x + icon_size - i, icon_y + i + t, color);
        }
    }
}

// ============================================================================
// BUTTON MANAGEMENT
// ============================================================================

void button_init_full(Button *btn, int x, int y, int width, int height,
                      const char *text, uint32_t bg_color, uint32_t text_color,
                      uint32_t highlight_color, int text_scale) {
    btn->x = x;
    btn->y = y;
    btn->width = width;
    btn->height = height;
    
    // Convert text to uppercase and store
    text_to_uppercase(btn->text, text, sizeof(btn->text));
    btn->max_text_width = 0;  // No limit by default
    
    btn->bg_color = bg_color;
    btn->text_color = text_color;
    btn->highlight_color = highlight_color;
    btn->border_color = text_color;
    btn->text_scale = text_scale;
    btn->border_width = 2;
    
    btn->visual_state = BTN_STATE_NORMAL;
    btn->disabled = false;
    btn->was_pressed = false;
    btn->last_press_time_ms = 0;
    btn->debounce_ms = BTN_DEBOUNCE_MS;
    
    btn->draw_icon = NULL;
}

void button_init_simple(Button *btn, int x, int y, int width, int height,
                        const char *text) {
    button_init_full(btn, x, y, width, height, text,
                     BTN_COLOR_PRIMARY, COLOR_WHITE, BTN_COLOR_HIGHLIGHT, 2);
}

void button_set_text(Button *btn, const char *text) {
    if (btn->max_text_width > 0) {
        text_truncate(btn->text, sizeof(btn->text), text,
                      btn->max_text_width, btn->text_scale);
    } else {
        text_to_uppercase(btn->text, text, sizeof(btn->text));
    }
}

void button_set_max_text_width(Button *btn, int max_width) {
    btn->max_text_width = max_width;
    // Re-apply text with new constraint
    if (btn->text[0] != '\0') {
        char temp[128];
        strcpy(temp, btn->text);
        button_set_text(btn, temp);
    }
}

void button_set_colors(Button *btn, uint32_t bg, uint32_t text, uint32_t highlight) {
    btn->bg_color = bg;
    btn->text_color = text;
    btn->highlight_color = highlight;
}

void button_set_border(Button *btn, uint32_t color, int width) {
    btn->border_color = color;
    btn->border_width = width;
}

void button_set_debounce(Button *btn, uint32_t ms) {
    btn->debounce_ms = ms;
}

void button_set_icon(Button *btn, void (*draw_icon)(Framebuffer*, int, int, int, uint32_t)) {
    btn->draw_icon = draw_icon;
}

// ============================================================================
// TOUCH HANDLING
// ============================================================================

UiRect button_rect(const Button *btn) {
    UiRect r = { btn->x, btn->y, btn->width, btn->height };
    return r;
}

int focus_add_button(UiRect *out, int n, int max, const Button *btn) {
    if (btn->disabled || n >= max) return n;
    out[n] = button_rect(btn);
    return n + 1;
}

bool button_is_touched(Button *btn, int touch_x, int touch_y) {
    if (btn->disabled) return false;    // a disabled button is never hit
    UiRect r = button_rect(btn);
    return (touch_x >= r.x && touch_x < r.x + r.w &&
            touch_y >= r.y && touch_y < r.y + r.h);
}

/* Set when button_update()/button_check_press() move any button's visual_state;
 * read-and-cleared by button_take_dirty().  A caller that repaints only on
 * change cannot otherwise see these moves: a release frame the caller did not
 * receive (a full-screen run consumed it), or a slide-off while held, changes
 * the look with no touch edge to trigger a repaint. */
static bool button_look_changed = false;

static void button_set_look(Button *btn, ButtonVisualState s) {
    if (btn->visual_state != s) {
        btn->visual_state = s;
        button_look_changed = true;
    }
}

bool button_take_dirty(void) {
    bool changed = button_look_changed;
    button_look_changed = false;
    return changed;
}

bool button_update(Button *btn, int touch_x, int touch_y, bool is_touching, uint32_t current_time_ms) {
    if (btn->disabled) {                // no press, no highlight, latch cleared
        btn->was_pressed = false;
        button_set_look(btn, BTN_STATE_NORMAL);
        return false;
    }
    bool is_touched = button_is_touched(btn, touch_x, touch_y);
    bool pressed = false;
    
    if (is_touching && is_touched) {
        // Touch is on button
        if (!btn->was_pressed) {
            // Check debounce
            if (current_time_ms - btn->last_press_time_ms > btn->debounce_ms) {
                btn->was_pressed = true;
                btn->last_press_time_ms = current_time_ms;
                button_set_look(btn, BTN_STATE_PRESSED);
                pressed = true;
            }
        } else {
            button_set_look(btn, BTN_STATE_PRESSED);
        }
    } else if (is_touching && !is_touched) {
        // Touch is elsewhere
        button_set_look(btn, BTN_STATE_NORMAL);
    } else {
        // No touch.  NORMAL even when the retained x/y are on the button: a
        // touchscreen has no hover, and touch_input leaves x/y at the last
        // point after release, so a "hover" look here only ever meant "tapped
        // last" and stuck yellow until a tap elsewhere.  Keyboard focus rings
        // write visual_state themselves at draw time, never through here.
        btn->was_pressed = false;
        button_set_look(btn, BTN_STATE_NORMAL);
    }
    
    return pressed;
}

bool button_check_press(Button *btn, bool currently_pressed, uint32_t current_time_ms) {
    // Legacy API for compatibility with existing games
    if (btn->disabled) {
        btn->was_pressed = false;
        button_set_look(btn, BTN_STATE_NORMAL);
        return false;
    }
    if (currently_pressed) {
        if (!btn->was_pressed) {
            uint32_t time_since_last = current_time_ms - btn->last_press_time_ms;
            if (time_since_last > btn->debounce_ms) {
                // Leading edge — fire and highlight briefly this frame
                btn->was_pressed = true;
                btn->last_press_time_ms = current_time_ms;
                button_set_look(btn, BTN_STATE_HIGHLIGHTED);
                return true;
            }
            // Still within debounce window from a previous bounce — look normal
            button_set_look(btn, BTN_STATE_NORMAL);
        } else {
            // Already fired; finger still down — show normal so button never
            // appears stuck/yellow. Resets to was_pressed=false on release.
            button_set_look(btn, BTN_STATE_NORMAL);
        }
    } else {
        // Finger lifted — ready for next press
        btn->was_pressed = false;
        button_set_look(btn, BTN_STATE_NORMAL);
    }

    return false;
}

/* One safe way to ask "was this button just tapped?".
 *
 * button_check_press() derives the press edge from btn->was_pressed and clears
 * that latch on exactly one kind of call: one where currently_pressed is false.
 * A caller that only ever calls it with true — the
 *   if (button_is_touched(...) && button_check_press(..., true, now))
 * short-circuit — therefore fires ONCE per process and is dead afterwards.
 * That shipped in platformer.c and frogger.c; Office Runner's hamburger menu
 * died after its first use and the report came off the panel 2026-08-10.
 *
 * Two things this gets right that a call site keeps getting wrong: it is called
 * on every frame, including the quiet ones, and it derives the level signal from
 * pressed||held rather than from the coordinates.  touch_input.c leaves
 * state.x/y at the last touched point after release, so button_is_touched()
 * alone stays true with no finger on the panel — and the latch never clears.
 *
 * Covered by tests/button_latch_test.c (group A reproduces the old idiom's
 * symptom, so a green run cannot mean "the harness cannot see it").
 */
bool button_check_tap(Button *btn, const TouchState *ts, uint32_t current_time_ms) {
    bool down = ts && (ts->pressed || ts->held) &&
                button_is_touched(btn, ts->x, ts->y);
    return button_check_press(btn, down, current_time_ms);
}

// ============================================================================
// RENDERING
// ============================================================================

void button_draw(Framebuffer *fb, Button *btn) {
    // Determine colors based on state
    uint32_t bg = btn->bg_color;
    uint32_t text = btn->text_color;
    uint32_t border = btn->border_color;
    const uint32_t disabled_grey = RGB(120, 120, 120);
    
    if (btn->disabled) {
        text = disabled_grey;           // regardless of text_color; never a pressed look
        border = disabled_grey;
    } else if (btn->visual_state == BTN_STATE_HIGHLIGHTED) {
        border = btn->highlight_color;
        text = btn->highlight_color;
    } else if (btn->visual_state == BTN_STATE_PRESSED) {
        bg = btn->highlight_color;
        text = btn->bg_color;
        border = btn->highlight_color;
    }
    
    // Draw background
    fb_fill_rect(fb, btn->x, btn->y, btn->width, btn->height, bg);
    
    // Draw border
    for (int i = 0; i < btn->border_width; i++) {
        // Top
        fb_draw_rect(fb, btn->x + i, btn->y + i, btn->width - 2*i, 1, border);
        // Bottom
        fb_draw_rect(fb, btn->x + i, btn->y + btn->height - 1 - i, btn->width - 2*i, 1, border);
        // Left
        fb_draw_rect(fb, btn->x + i, btn->y + i, 1, btn->height - 2*i, border);
        // Right
        fb_draw_rect(fb, btn->x + btn->width - 1 - i, btn->y + i, 1, btn->height - 2*i, border);
    }
    
    // Draw icon or text
    int center_x = btn->x + btn->width / 2;
    int center_y = btn->y + btn->height / 2;
    
    if (btn->draw_icon) {
        // Draw custom icon
        int icon_size = (btn->height < btn->width ? btn->height : btn->width) - 20;
        btn->draw_icon(fb, center_x, center_y, icon_size, text);
    } else if (btn->text[0] != '\0') {
        // Draw centered text
        text_draw_centered(fb, center_x, center_y, btn->text, text, btn->text_scale);
    }
}

void button_draw_menu(Framebuffer *fb, Button *btn) {
    // Temporarily set icon and draw
    void (*old_icon)(Framebuffer*, int, int, int, uint32_t) = btn->draw_icon;
    btn->draw_icon = icon_draw_hamburger;
    button_draw(fb, btn);
    btn->draw_icon = old_icon;
}

void button_draw_exit(Framebuffer *fb, Button *btn) {
    // Temporarily set icon and draw
    void (*old_icon)(Framebuffer*, int, int, int, uint32_t) = btn->draw_icon;
    btn->draw_icon = icon_draw_x;
    button_draw(fb, btn);
    btn->draw_icon = old_icon;
}

// ============================================================================
// MODAL DIALOG
// ============================================================================

void modal_dialog_init(ModalDialog *dlg, const char *title, const char *message,
                       int button_count) {
    memset(dlg, 0, sizeof(ModalDialog));
    dlg->active = false;

    if (title) snprintf(dlg->title, sizeof(dlg->title), "%s", title);
    if (message) snprintf(dlg->message, sizeof(dlg->message), "%s", message);

    /* Clamp button count */
    if (button_count < 1) button_count = 1;
    if (button_count > MODAL_MAX_BUTTONS) button_count = MODAL_MAX_BUTTONS;
    dlg->button_count = button_count;
    dlg->focus = -1;               /* no focus: opt-in, modal_dialog_set_focus() */

    /* Default visual settings */
    dlg->overlay_alpha = 180;
    dlg->dialog_width = 420;
    dlg->bg_color = RGB(30, 30, 45);
    dlg->border_color = COLOR_WHITE;
    dlg->title_color = COLOR_YELLOW;
    dlg->message_color = RGB(180, 180, 180);

    /* Auto-calculate dialog height based on button count */
    if (button_count <= 2)
        dlg->dialog_height = 200;
    else if (button_count == 3)
        dlg->dialog_height = 260;
    else
        dlg->dialog_height = 310;
}

void modal_dialog_set_button(ModalDialog *dlg, int index, const char *text,
                             uint32_t bg_color, uint32_t text_color) {
    if (index < 0 || index >= dlg->button_count) return;
    button_init_full(&dlg->buttons[index], 0, 0, 150, 50,
                     text ? text : "", bg_color, text_color,
                     BTN_COLOR_HIGHLIGHT, 2);
}

void modal_dialog_init_confirm(ModalDialog *dlg, const char *title,
                               const char *message,
                               const char *confirm_text, uint32_t confirm_color,
                               const char *cancel_text, uint32_t cancel_color) {
    modal_dialog_init(dlg, title, message, 2);
    modal_dialog_set_button(dlg, 0, confirm_text ? confirm_text : "CONFIRM",
                            confirm_color, COLOR_WHITE);
    modal_dialog_set_button(dlg, 1, cancel_text ? cancel_text : "CANCEL",
                            cancel_color, COLOR_WHITE);
}

void modal_dialog_show(ModalDialog *dlg) {
    dlg->active = true;
}

void modal_dialog_hide(ModalDialog *dlg) {
    dlg->active = false;
}

bool modal_dialog_is_active(ModalDialog *dlg) {
    return dlg->active;
}

void modal_dialog_set_focus(ModalDialog *dlg, int index) {
    if (index >= -1 && index < dlg->button_count) dlg->focus = index;
}

/* Buttons are in reading order whichever layout draw picks (side by side for
 * two, stacked otherwise), so previous/next is right for both. */
void modal_dialog_focus_step(ModalDialog *dlg, UiDir d) {
    int n = dlg->button_count;
    if (dlg->focus < 0 || n <= 0) return;
    if (d == UI_DIR_UP || d == UI_DIR_LEFT)
        dlg->focus = dlg->focus > 0 ? dlg->focus - 1 : n - 1;
    else
        dlg->focus = dlg->focus + 1 < n ? dlg->focus + 1 : 0;
}

void modal_dialog_draw(ModalDialog *dlg, Framebuffer *fb) {
    if (!dlg->active) return;

    int dw = dlg->dialog_width;
    int dh = dlg->dialog_height;
    int n  = dlg->button_count;

    /* Semi-transparent overlay */
    if (dlg->overlay_alpha > 0) {
        fb_fill_rect_alpha(fb, 0, 0, fb->width, fb->height,
                           COLOR_BLACK, dlg->overlay_alpha);
    }

    /* Centered dialog box */
    int dx = (fb->width - dw) / 2;
    int dy = (fb->height - dh) / 2;

    fb_fill_rect(fb, dx, dy, dw, dh, dlg->bg_color);
    fb_draw_rect(fb, dx, dy, dw, dh, dlg->border_color);

    /* Title text — centered, scale 3 */
    if (dlg->title[0]) {
        text_draw_centered(fb, fb->width / 2, dy + 45, dlg->title,
                           dlg->title_color, 3);
    }

    /* Message text — centered, scale 2.  fb_draw_text() does not break on
     * '\n', so each line is drawn here, the block centred on dy + 90 (a
     * one-line message lands exactly where it always did), 22 px apart. */
    if (dlg->message[0]) {
        int lines = 1;
        for (const char *p = dlg->message; *p; p++) if (*p == '\n') lines++;
        int y = dy + 90 - (lines - 1) * 11;
        const char *line = dlg->message;
        while (line) {
            const char *nl = strchr(line, '\n');
            char buf[sizeof(dlg->message)];
            size_t n = nl ? (size_t)(nl - line) : strlen(line);
            memcpy(buf, line, n);
            buf[n] = '\0';
            /* A line too wide for the box drops to scale 1 rather than
             * spilling over its edges; the 10 px each side is the frame. */
            int ms = (text_measure_width(buf, 2) <= dw - 20) ? 2 : 1;
            text_draw_centered(fb, fb->width / 2, y, buf, dlg->message_color, ms);
            y += 22;
            line = nl ? nl + 1 : NULL;
        }
    }

    /* Position and draw buttons */
    if (n == 2) {
        /* Side-by-side layout (preserves original 2-button look) */
        int btn_w = 150, btn_h = 50;
        int btn_gap = 30;
        int total_btn_w = btn_w * 2 + btn_gap;
        int btn_x = (fb->width - total_btn_w) / 2;
        int btn_y = dy + dh - btn_h - 25;

        dlg->buttons[0].x = btn_x;
        dlg->buttons[0].y = btn_y;
        dlg->buttons[0].width = btn_w;
        dlg->buttons[0].height = btn_h;

        dlg->buttons[1].x = btn_x + btn_w + btn_gap;
        dlg->buttons[1].y = btn_y;
        dlg->buttons[1].width = btn_w;
        dlg->buttons[1].height = btn_h;
    } else {
        /* Vertically stacked layout (1, 3, or 4 buttons) */
        int btn_w = 200, btn_h = 44;
        int btn_gap = 8;
        int total_h = n * btn_h + (n - 1) * btn_gap;
        int btn_x = (fb->width - btn_w) / 2;
        int btn_y_start = dy + dh - total_h - 20;

        for (int i = 0; i < n; i++) {
            dlg->buttons[i].x = btn_x;
            dlg->buttons[i].y = btn_y_start + i * (btn_h + btn_gap);
            dlg->buttons[i].width = btn_w;
            dlg->buttons[i].height = btn_h;
        }
    }

    /* The focused button — the one Enter presses — is drawn highlighted on a
     * copy: writing visual_state here would fight button_update(), which sets
     * it back to NORMAL each quiet frame and reports that as a change, and the
     * dialog would repaint forever.  A pressed look wins over the focus look. */
    for (int i = 0; i < n; i++) {
        Button b = dlg->buttons[i];
        if (i == dlg->focus && b.visual_state == BTN_STATE_NORMAL)
            b.visual_state = BTN_STATE_HIGHLIGHTED;
        button_draw(fb, &b);
    }
}

ModalDialogAction modal_dialog_update(ModalDialog *dlg, int touch_x, int touch_y,
                                       bool touch_active, uint32_t now_ms) {
    if (!dlg->active) return MODAL_ACTION_NONE;

    for (int i = 0; i < dlg->button_count; i++) {
        if (button_update(&dlg->buttons[i], touch_x, touch_y, touch_active, now_ms)) {
            dlg->active = false;
            return (ModalDialogAction)i;
        }
    }

    return MODAL_ACTION_NONE;
}

ModalDialogAction modal_dialog_input(ModalDialog *dlg, const InputState *in,
                                     int cancel) {
    if (!dlg->active || in == NULL || dlg->button_count <= 0)
        return MODAL_ACTION_NONE;
    if (dlg->focus < 0) dlg->focus = 0;

    const ButtonState *b = in->buttons;
    int pick = -1;
    if (b[BTN_ID_PAUSE].pressed || b[BTN_ID_BACK].pressed)
        pick = cancel;
    else if (b[BTN_ID_JUMP].pressed || b[BTN_ID_ACTION].pressed)
        pick = dlg->focus;
    else if (b[BTN_ID_UP].pressed)    modal_dialog_focus_step(dlg, UI_DIR_UP);
    else if (b[BTN_ID_DOWN].pressed)  modal_dialog_focus_step(dlg, UI_DIR_DOWN);
    else if (b[BTN_ID_LEFT].pressed)  modal_dialog_focus_step(dlg, UI_DIR_LEFT);
    else if (b[BTN_ID_RIGHT].pressed) modal_dialog_focus_step(dlg, UI_DIR_RIGHT);

    if (pick < 0 || pick >= dlg->button_count) return MODAL_ACTION_NONE;
    dlg->active = false;
    return (ModalDialogAction)pick;
}

// ============================================================================
// TOGGLE SWITCH CONTROL
// ============================================================================

void toggle_init(ToggleSwitch *sw, int x, int y, int track_w, int track_h,
                 const char *label, bool initial_state) {
    sw->x = x;
    sw->y = y;
    sw->track_w = track_w;
    sw->track_h = track_h;
    sw->state = initial_state;
    text_to_uppercase(sw->label, label, sizeof(sw->label));

    // Default colors
    sw->on_color = RGB(0, 180, 60);
    sw->off_color = RGB(100, 100, 100);
    sw->knob_color = COLOR_WHITE;
    sw->label_color = RGB(200, 200, 200);

    sw->disabled = false;
    sw->was_pressed = false;
    sw->last_press_time_ms = 0;
    sw->debounce_ms = 300;
}

void toggle_set_colors(ToggleSwitch *sw, uint32_t on_color, uint32_t off_color,
                       uint32_t knob_color, uint32_t label_color) {
    sw->on_color = on_color;
    sw->off_color = off_color;
    sw->knob_color = knob_color;
    sw->label_color = label_color;
}

// Generous hit area: track + label area + some padding
UiRect toggle_hit_rect(const ToggleSwitch *sw) {
    UiRect r = { sw->x - 5, sw->y - 5,
                 sw->track_w + text_measure_width(sw->label, 1) + 20,
                 sw->track_h + 10 };
    return r;
}

int focus_add_toggle(UiRect *out, int n, int max, const ToggleSwitch *sw) {
    if (sw->disabled || n >= max) return n;
    out[n] = toggle_hit_rect(sw);
    return n + 1;
}

bool toggle_check_press(ToggleSwitch *sw, int touch_x, int touch_y,
                        bool is_pressed, uint32_t current_time_ms) {
    if (sw->disabled) { sw->was_pressed = false; return false; }
    UiRect hit = toggle_hit_rect(sw);
    bool in_bounds = (touch_x >= hit.x && touch_x < hit.x + hit.w &&
                      touch_y >= hit.y && touch_y < hit.y + hit.h);

    if (is_pressed && in_bounds) {
        if (!sw->was_pressed &&
            (current_time_ms - sw->last_press_time_ms > sw->debounce_ms)) {
            sw->was_pressed = true;
            sw->last_press_time_ms = current_time_ms;
            sw->state = !sw->state;
            return true;  // State changed
        }
    } else if (!is_pressed) {
        sw->was_pressed = false;
    }

    return false;
}

void toggle_draw(Framebuffer *fb, ToggleSwitch *sw) {
    const uint32_t disabled_grey = RGB(120, 120, 120);
    uint32_t track_color = sw->disabled ? RGB(60, 60, 60)
                         : sw->state    ? sw->on_color : sw->off_color;
    int r = sw->track_h / 2;  // Corner radius = half height for pill shape

    // Draw track (pill shape)
    fb_fill_rounded_rect(fb, sw->x, sw->y, sw->track_w, sw->track_h, r, track_color);

    // Draw knob
    int knob_r = r - 2;
    int knob_cx, knob_cy;
    knob_cy = sw->y + sw->track_h / 2;
    if (sw->state) {
        knob_cx = sw->x + sw->track_w - r;  // Right side
    } else {
        knob_cx = sw->x + r;                 // Left side
    }
    fb_fill_circle(fb, knob_cx, knob_cy, knob_r, sw->knob_color);

    // Draw label to the right of the track
    int label_x = sw->x + sw->track_w + 8;
    int label_y = sw->y + (sw->track_h - 7) / 2;  // Vertically center (7px font height)
    fb_draw_text(fb, label_x, label_y, sw->label,
                 sw->disabled ? disabled_grey : sw->label_color, 1);
}

// ============================================================================
// CYCLER CONTROL
// ============================================================================

void cycler_init(Cycler *c, int x, int y, int width, int height) {
    c->x = x;
    c->y = y;
    c->width = width;
    c->height = height;
    c->text_scale = 2;
    c->bg_color = RGB(40, 40, 48);
    c->text_color = COLOR_WHITE;
    c->arrow_color = BTN_COLOR_INFO;
    c->border_color = RGB(100, 100, 100);
    c->border_width = 2;
    c->was_pressed = false;
    c->disabled = false;
}

void cycler_set_colors(Cycler *c, uint32_t bg, uint32_t text, uint32_t arrow,
                       uint32_t border) {
    c->bg_color = bg;
    c->text_color = text;
    c->arrow_color = arrow;
    c->border_color = border;
}

UiRect cycler_rect(const Cycler *c) {
    UiRect r = { c->x, c->y, c->width, c->height };
    return r;
}

int focus_add_cycler(UiRect *out, int n, int max, const Cycler *c) {
    if (c->disabled || n >= max) return n;
    out[n] = cycler_rect(c);
    return n + 1;
}

int cycler_hit(const Cycler *c, int x, int y) {
    if (c->disabled) return 0;
    UiRect r = cycler_rect(c);
    if (x < r.x || x >= r.x + r.w || y < r.y || y >= r.y + r.h) return 0;
    /* The same split ui_tap_begin() centres on, so a keyboard activation
     * lands on the right half: +1. */
    return (x - r.x) < (r.w >> 1) ? -1 : +1;
}

int cycler_check_tap(Cycler *c, int touch_x, int touch_y, bool touching) {
    if (c->disabled || !touching) { c->was_pressed = false; return 0; }
    if (c->was_pressed) return 0;           /* held: one step per press */
    c->was_pressed = true;
    /* A press that lands outside still latches, so a finger sliding on
     * from elsewhere does not step it. */
    return cycler_hit(c, touch_x, touch_y);
}

int cycler_step(int index, int count, int dir) {
    if (count <= 0) return 0;
    if (index < 0 || index >= count) index = 0;
    index += dir < 0 ? -1 : dir > 0 ? 1 : 0;
    if (index < 0) index = count - 1;
    if (index >= count) index = 0;
    return index;
}

/* A solid triangle pointing left (dir < 0) or right, tip at (tip_x, cy),
 * `half` px tall above and below cy and `half` px deep. */
static void cycler_draw_arrow(Framebuffer *fb, int tip_x, int cy, int half,
                              int dir, uint32_t color) {
    for (int i = 0; i <= half; i++) {
        int col = dir < 0 ? tip_x + i : tip_x - i;
        fb_fill_rect(fb, col, cy - i, 1, 2 * i + 1, color);
    }
}

void cycler_draw(Framebuffer *fb, const Cycler *c, const char *text) {
    const uint32_t disabled_grey = RGB(120, 120, 120);
    uint32_t txt    = c->disabled ? disabled_grey : c->text_color;
    uint32_t arrow  = c->disabled ? disabled_grey : c->arrow_color;
    uint32_t border = c->disabled ? disabled_grey : c->border_color;

    fb_fill_rect(fb, c->x, c->y, c->width, c->height, c->bg_color);
    for (int i = 0; i < c->border_width; i++) {
        fb_draw_rect(fb, c->x + i, c->y + i, c->width - 2*i, 1, border);
        fb_draw_rect(fb, c->x + i, c->y + c->height - 1 - i, c->width - 2*i, 1, border);
        fb_draw_rect(fb, c->x + i, c->y + i, 1, c->height - 2*i, border);
        fb_draw_rect(fb, c->x + c->width - 1 - i, c->y + i, 1, c->height - 2*i, border);
    }

    /* Each arrow sits in a square zone at its end; the text gets what is
     * between the zones, less a gap, and is cut to fit it. */
    int cy     = c->y + (c->height >> 1);
    int zone   = c->height < (c->width >> 2) ? c->height : (c->width >> 2);
    int half   = (zone >> 2) > 2 ? (zone >> 2) : 2;
    int pad    = (zone - half) >> 1;
    cycler_draw_arrow(fb, c->x + pad, cy, half, -1, arrow);
    cycler_draw_arrow(fb, c->x + c->width - 1 - pad, cy, half, +1, arrow);

    int text_w = c->width - 2 * zone - 8;
    if (text && text[0] && text_w > 0) {
        char shown[128];
        text_truncate(shown, sizeof(shown), text, text_w, c->text_scale);
        text_draw_centered(fb, c->x + (c->width >> 1), cy, shown, txt,
                           c->text_scale);
    }
}

// ============================================================================
// UNIFIED GAME OVER SCREEN
// ============================================================================

_Static_assert(GAMEOVER_SCORES_MS == SM_ATTRACT_MS,
               "the scores page holds as long as the start menu's SCORES page");

GameOverPhase gameover_phase_next(GameOverPhase cur, uint32_t phase_ms, bool input,
                                  bool qualifies, bool has_table, bool name_done) {
    switch (cur) {
    case GAMEOVER_PHASE_SHOW:
        // The press that ended the game must not skip: input counts only once
        // the page has been up for GAMEOVER_ARM_MS.
        if (phase_ms >= GAMEOVER_SHOW_MS ||
            (input && phase_ms >= GAMEOVER_ARM_MS)) {
            if (!has_table) return GAMEOVER_PHASE_DONE;
            return (qualifies && !name_done) ? GAMEOVER_PHASE_NAME
                                             : GAMEOVER_PHASE_SCORES;
        }
        return GAMEOVER_PHASE_SHOW;
    case GAMEOVER_PHASE_NAME:
        return name_done ? GAMEOVER_PHASE_SCORES : GAMEOVER_PHASE_NAME;
    case GAMEOVER_PHASE_SCORES:
        return (input || phase_ms >= GAMEOVER_SCORES_MS) ? GAMEOVER_PHASE_DONE
                                                         : GAMEOVER_PHASE_SCORES;
    default:
        return GAMEOVER_PHASE_DONE;
    }
}

// Own edges from the level, so the prev_held[] seeded TRUE can refuse a button
// that was already down.  `pressed` counts as held for this frame: a press and
// release inside one poll still makes an edge.
static bool gameover_input_edge(GameOverScreen *gos, const InputState *in, bool touching) {
    bool edge = touching && !gos->was_touching;
    gos->was_touching = touching;
    for (int b = 0; b < BTN_ID_COUNT; b++) {
        bool held = in && in->buttons[b].held;
        bool down = held || (in && in->buttons[b].pressed);
        if (down && !gos->prev_held[b]) edge = true;
        gos->prev_held[b] = held;
    }
    return edge;
}

static void gameover_reseed(GameOverScreen *gos) {
    for (int b = 0; b < BTN_ID_COUNT; b++) gos->prev_held[b] = true;
    gos->was_touching = true;
}

void gameover_init(GameOverScreen *gos, Framebuffer *fb,
                   int score, const char *title, const char *info_line,
                   const char *game_title, HighScoreTable *hs_table,
                   TouchInput *touch, GamepadManager *gm) {
    (void)fb;  // Reserved for future use (e.g., pre-render calculations)
    memset(gos, 0, sizeof(*gos));
    gos->phase = GAMEOVER_PHASE_SHOW;
    gos->score = score;
    gos->hs_table = hs_table;
    gos->touch = touch;
    gos->gm = gm;
    gos->pending_draw = true;   /* nothing of ours is on screen yet */
    gameover_reseed(gos);       /* ignore the press that got us here */

    // Store title (default to "GAME OVER" if NULL/empty)
    if (title && title[0]) {
        text_to_uppercase(gos->title, title, sizeof(gos->title));
    } else {
        snprintf(gos->title, sizeof(gos->title), "GAME OVER");
    }

    // Store optional info line
    if (info_line && info_line[0]) {
        text_to_uppercase(gos->info_line, info_line, sizeof(gos->info_line));
    }

    if (game_title && game_title[0]) {
        text_to_uppercase(gos->game_title, game_title, sizeof(gos->game_title));
    } else {
        snprintf(gos->game_title, sizeof(gos->game_title), "HIGH SCORES");
    }
}

GameOverAction gameover_update(GameOverScreen *gos, Framebuffer *fb,
                               int touch_x, int touch_y, bool touching,
                               const InputState *in) {
    (void)touch_x;
    (void)touch_y;
    uint32_t now = get_time_ms();

    // First call: the clock starts here, and the table is asked once.  It must
    // draw in this SAME call: the caller only runs its draw path when its dirty
    // flag is set, so a call that returns without drawing costs a frame the loop
    // will not grant until the player taps.
    if (!gos->started) {
        gos->started = true;
        gos->phase_t0 = now;
        if (gos->hs_table != NULL) {
            gos->hs_qualifies = (hs_qualifies(gos->hs_table, gos->score) >= 0);
            if (gos->hs_qualifies) {
                /* Sound the chime.  Until 2026-08-31 nothing here played
                 * anything: name entry opened, and a player who had just beaten
                 * the table heard only the game-over descent the game had fired
                 * a moment earlier — reported by ear on .188, and invisible to
                 * every host test because no gate can hear a call that was never
                 * written.
                 *
                 * ⚠️ **`audio_sparkle()`, not `audio_success()`.** Success is the
                 * level-up fanfare and is heard DURING play; reusing it here
                 * would repeat the mistake `audio_gameover()` exists to undo —
                 * two different events on one sound, which the operator could
                 * not tell apart (audio.c's audio_gameover comment).  Sparkle's
                 * documented event is "a bonus / rare reward", which is what
                 * this is, and its clip is already sourced and deployed.
                 *
                 * It overlaps the game-over sound rather than cutting it, and
                 * that needs no arbitration: the canned sounds are RAM clips and
                 * AUDIO_CLIP_VOICES is 4, so both simply sound.  ⚠️ No
                 * audio_interrupt() — on the mix bus that means "drop what is
                 * rendered", not "make room".
                 *
                 * ⚠️ Nothing drains it here, deliberately.  keyboard_enter()
                 * calls ui_frame_service() beside its fb_swap(), so the chime is
                 * rendered WHILE the player types, and the SHOW page's frames
                 * render it before that. */
                Audio *audio = (Audio *)ui_frame_service_ctx();
                if (audio != NULL) audio_sparkle(audio);
            }
        }
    }

    bool input = gameover_input_edge(gos, in, touching);
    bool has_table = (gos->hs_table != NULL);

    GameOverPhase next = gameover_phase_next(gos->phase, now - gos->phase_t0, input,
                                             gos->hs_qualifies, has_table, gos->name_done);
    if (next != gos->phase) {
        gos->phase = next;
        gos->phase_t0 = now;
        gos->pending_draw = true;
    }

    // NAME: blocking keyboard.  It repaints and swaps the framebuffer itself,
    // so it runs before we draw: the SCORES page below is a full clear and
    // composites over nothing of the keyboard.
    if (gos->phase == GAMEOVER_PHASE_NAME) {
        char hs_name[HS_NAME_LEN];
        hs_enter_name(fb, gos->touch, gos->gm, hs_name, gos->score);
        hs_insert(gos->hs_table, hs_name, gos->score);
        hs_save(gos->hs_table);
        hs_drain_touches(gos->touch);
        gos->name_done = true;
        // The button that closed the keyboard may still be down: it must be
        // released and pressed again before it skips the scores page.
        gameover_reseed(gos);
        gos->phase = gameover_phase_next(gos->phase, 0, false, gos->hs_qualifies,
                                         has_table, gos->name_done);
        gos->phase_t0 = get_time_ms();
        gos->pending_draw = true;
    }

    if (gos->phase == GAMEOVER_PHASE_DONE)
        return GAMEOVER_ACTION_MENU;

    gameover_draw(gos, fb);
    gos->pending_draw = false;
    return GAMEOVER_ACTION_NONE;
}

bool gameover_needs_redraw(const GameOverScreen *gos, const InputState *in) {
    if (gos->pending_draw || gos->phase == GAMEOVER_PHASE_DONE)
        return true;

    // A phase whose time is up must be given a frame to end on.
    uint32_t elapsed = get_time_ms() - gos->phase_t0;
    if (gos->phase == GAMEOVER_PHASE_SHOW && elapsed >= GAMEOVER_SHOW_MS)
        return true;
    if (gos->phase == GAMEOVER_PHASE_SCORES && elapsed >= GAMEOVER_SCORES_MS)
        return true;

    // Input is read inside gameover_update(), which the caller only runs from
    // its draw path — so a frame we do not ask for is also an input event we
    // never see.  And a release must be SEEN too: prev_held[] / was_touching
    // stay TRUE until an update runs with the button up, and a press that
    // follows would then be no edge.  So any level we have not yet seen drop
    // asks for a frame.
    for (int b = 0; b < BTN_ID_COUNT; b++) {
        if (gos->prev_held[b]) return true;
        if (in && (in->buttons[b].held || in->buttons[b].pressed)) return true;
    }
    if (gos->was_touching) return true;
    if (gos->touch != NULL) {
        TouchState ts = touch_get_state(gos->touch);
        if (ts.pressed || ts.held)
            return true;
    }

    return false;
}

void gameover_draw(GameOverScreen *gos, Framebuffer *fb) {
    if (gos->phase == GAMEOVER_PHASE_SCORES) {
        start_menu_draw_scores_page(fb, gos->game_title, gos->hs_table);
        return;
    }

    // Semi-transparent black overlay
    fb_fill_rect_alpha(fb, 0, 0, fb->width, fb->height, COLOR_BLACK, 160);

    int center_x = fb->width / 2;

    // Title — upper portion of screen.  All of this is read, not pressed, so
    // it uses the full visible screen.
    int title_y = SCREEN_VISIBLE_TOP + SCREEN_VISIBLE_HEIGHT / 5;
    text_draw_centered(fb, center_x, title_y, gos->title, COLOR_YELLOW, 4);

    // Score — below title
    char score_buf[64];
    snprintf(score_buf, sizeof(score_buf), "SCORE: %d", gos->score);
    int score_y = title_y + text_measure_height(4) + 20;
    text_draw_centered(fb, center_x, score_y, score_buf, COLOR_WHITE, 3);

    // Optional info line — below score
    int info_y = score_y + text_measure_height(3) + 12;
    if (gos->info_line[0]) {
        text_draw_centered(fb, center_x, info_y, gos->info_line, COLOR_CYAN, 2);
        info_y += text_measure_height(2) + 12;
    }

    if (gos->hs_qualifies)
        text_draw_centered(fb, center_x, info_y + 20, "NEW HIGH SCORE!", COLOR_GREEN, 3);
}
