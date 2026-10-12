/* led_page.c — control_panel's LED page: enable, brightness, and the LED tests.
 *
 * Opened from the home grid's LED tile, and the one home for everything about
 * the two indicator LEDs.  Exposed only as cp_led_page (cp_page.h); its state lives in this file.
 *
 * ⚠️ There is no SAVE button, on purpose.  The toggle and -/+ write the config
 * file the moment they change and reload common/hardware.c's cache, because the
 * tests on the same screen drive the LEDs through hw_set_led(), which is gated
 * and scaled by the SAVED values — a page whose tests ignored its own switches
 * until a SAVE would contradict itself on screen.
 */
#include "cp_page.h"
#include "cp_ui.h"
#include "../common/common.h"
#include "../common/hardware.h"

#include <stdio.h>
#include <unistd.h>

/* ── The tests ──────────────────────────────────────────────────────────────
 * Blocking full-screen routines, run by control_panel's full-screen path, the
 * one every page's run_fullscreen() goes through.  They drive the LEDs through the gated,
 * scaled setters, so they show what a game would show with the values on this
 * page — which is why the page saves as it goes. */

static void test_red_led(Framebuffer *fb, TouchInput *touch) {
    int x, y;
    for (int i = 0; i <= 100; i += 5) {
        char s[64]; snprintf(s, sizeof(s), "BRIGHTNESS: %d%%", i);
        draw_test_screen(fb, "RED LED TEST", s, i);
        hw_set_red_led(i); usleep(50000);
        if (check_touch(touch, &x, &y)) { hw_set_red_led(0); return; }
    }
    for (int i = 0; i < 20; i++) { usleep(50000); if (check_touch(touch, &x, &y)) { hw_set_red_led(0); return; } }
    for (int i = 100; i >= 0; i -= 5) {
        char s[64]; snprintf(s, sizeof(s), "BRIGHTNESS: %d%%", i);
        draw_test_screen(fb, "RED LED TEST", s, 100 - i);
        hw_set_red_led(i); usleep(50000);
        if (check_touch(touch, &x, &y)) { hw_set_red_led(0); return; }
    }
    hw_set_red_led(0);
    draw_test_screen(fb, "RED LED TEST", "COMPLETE!", 100);
    while (!check_touch(touch, &x, &y)) usleep(10000);
}

static void test_green_led(Framebuffer *fb, TouchInput *touch) {
    int x, y;
    for (int i = 0; i <= 100; i += 5) {
        char s[64]; snprintf(s, sizeof(s), "BRIGHTNESS: %d%%", i);
        draw_test_screen(fb, "GREEN LED TEST", s, i);
        hw_set_green_led(i); usleep(50000);
        if (check_touch(touch, &x, &y)) { hw_set_green_led(0); return; }
    }
    for (int i = 0; i < 20; i++) { usleep(50000); if (check_touch(touch, &x, &y)) { hw_set_green_led(0); return; } }
    for (int i = 100; i >= 0; i -= 5) {
        char s[64]; snprintf(s, sizeof(s), "BRIGHTNESS: %d%%", i);
        draw_test_screen(fb, "GREEN LED TEST", s, 100 - i);
        hw_set_green_led(i); usleep(50000);
        if (check_touch(touch, &x, &y)) { hw_set_green_led(0); return; }
    }
    hw_set_green_led(0);
    draw_test_screen(fb, "GREEN LED TEST", "COMPLETE!", 100);
    while (!check_touch(touch, &x, &y)) usleep(10000);
}

static void test_both_leds(Framebuffer *fb, TouchInput *touch) {
    int x, y;
    for (int i = 0; i <= 100; i += 5) {
        char s[64]; snprintf(s, sizeof(s), "BOTH LEDS: %d%%", i);
        draw_test_screen(fb, "BOTH LEDS TEST", s, i);
        hw_set_leds(i, i); usleep(50000);
        if (check_touch(touch, &x, &y)) { hw_leds_off(); return; }
    }
    for (int i = 0; i < 20; i++) { usleep(50000); if (check_touch(touch, &x, &y)) { hw_leds_off(); return; } }
    for (int c = 0; c < 5; c++) {
        draw_test_screen(fb, "BOTH LEDS TEST", "RED ONLY", 50);
        hw_set_leds(100, 0);
        for (int i = 0; i < 10; i++) { usleep(50000); if (check_touch(touch, &x, &y)) { hw_leds_off(); return; } }
        draw_test_screen(fb, "BOTH LEDS TEST", "GREEN ONLY", 50);
        hw_set_leds(0, 100);
        for (int i = 0; i < 10; i++) { usleep(50000); if (check_touch(touch, &x, &y)) { hw_leds_off(); return; } }
    }
    hw_leds_off();
    draw_test_screen(fb, "BOTH LEDS TEST", "COMPLETE!", 100);
    while (!check_touch(touch, &x, &y)) usleep(10000);
}

static void test_pulse(Framebuffer *fb, TouchInput *touch) {
    draw_test_screen(fb, "PULSE EFFECT", "PULSING GREEN LED...", 50);
    hw_pulse_led(LED_GREEN, 3000, 100);
    draw_test_screen(fb, "PULSE EFFECT", "COMPLETE!", 100);
    int x, y; while (!check_touch(touch, &x, &y)) usleep(10000);
}

static void test_blink(Framebuffer *fb, TouchInput *touch) {
    draw_test_screen(fb, "BLINK EFFECT", "BLINKING RED LED...", 50);
    hw_blink_led(LED_RED, 10, 200, 200, 100);
    draw_test_screen(fb, "BLINK EFFECT", "COMPLETE!", 100);
    int x, y; while (!check_touch(touch, &x, &y)) usleep(10000);
}

static void test_colors(Framebuffer *fb, TouchInput *touch) {
    const char *cnames[] = {"RED", "ORANGE", "YELLOW", "GREEN", "OFF"};
    const struct { uint8_t r; uint8_t g; } cols[] = {
        {100,0},{100,50},{100,100},{0,100},{0,0}
    };
    int x, y;
    for (int i = 0; i < 5; i++) {
        draw_test_screen(fb, "COLOR CYCLE", cnames[i], (i * 100) / 4);
        hw_set_leds(cols[i].r, cols[i].g);
        for (int j = 0; j < 20; j++) {
            usleep(50000);
            if (check_touch(touch, &x, &y)) { hw_leds_off(); return; }
        }
    }
    draw_test_screen(fb, "COLOR CYCLE", "COMPLETE!", 100);
    while (!check_touch(touch, &x, &y)) usleep(10000);
}

/* Name and routine in one row, so the button a finger presses and the routine
 * that runs cannot drift apart the way two parallel lists can. */
static const struct {
    const char *name;
    void      (*run)(Framebuffer *, TouchInput *);
} led_tests[] = {
    { "RED LED",   test_red_led   },
    { "GREEN LED", test_green_led },
    { "BOTH LEDS", test_both_leds },
    { "PULSE",     test_pulse     },
    { "BLINK",     test_blink     },
    { "COLORS",    test_colors    },
};
#define LED_TEST_COUNT ((int)(sizeof(led_tests) / sizeof(led_tests[0])))

/* ── State and persistence ─────────────────────────────────────────────── */

typedef struct {
    bool enabled;       /* config key led_enabled */
    int  brightness;    /* config key led_brightness, 0..100 */
} LedPageState;

static LedPageState led_state;          /* the values, as saved */
static int          queued_test = -1;   /* test to run full-screen, -1 none */

/* After input() returned CP_PAGE_FULLSCREEN.  Whatever a test left lit, the
 * page turns the LEDs off itself: its hardware is its own to clean up. */
static void led_page_run_fullscreen(Framebuffer *fb, TouchInput *touch) {
    int test = queued_test;
    queued_test = -1;
    if (test >= 0 && test < LED_TEST_COUNT)
        led_tests[test].run(fb, touch);
    hw_leds_off();
}

static int clamp_pct(int v) { return v < 0 ? 0 : v > 100 ? 100 : v; }

/* Both values read through config.c's helpers, so the default shown here is
 * the one hardware.c resolves on a file without the keys. */
static void led_page_load(const Config *cfg) {
    led_state.enabled    = config_led_enabled(cfg);
    led_state.brightness = clamp_pct(config_led_brightness(cfg));
}

/* Writes the two keys into the FILE by re-reading it, not by saving the
 * caller's Config: every page shares that one, and a whole-file save of it from
 * here would silently persist whatever another page holds in it unsaved, as a
 * side effect of touching an LED switch.  (RESET DEFAULTS, on the Information
 * page, saves the cleared file itself, so a reset is not such a case.)  The
 * in-memory copy is updated too, so
 * a later SAVE elsewhere writes these values rather than the old ones. */
static void led_persist(const LedPageState *s, Config *mem) {
    config_set_bool(mem, "led_enabled", s->enabled);
    config_set_int(mem, "led_brightness", s->brightness);

    Config disk;
    config_init(&disk);
    config_load(&disk);                 /* a missing file just starts empty */
    config_set_bool(&disk, "led_enabled", s->enabled);
    config_set_int(&disk, "led_brightness", s->brightness);
    if (config_save(&disk) != 0)
        fprintf(stderr, "control_panel: LED settings save failed\n");
    hw_reload_config();                 /* the tests read the saved values */
}

/* For the control panel's RESET DEFAULTS, called AFTER config_clear(cfg):
 * removes the two keys from the file as well, reloads hardware.c's cache, and
 * reads the state back from the cleared cfg — so this page, the file and the
 * LEDs agree on the default. */
static void led_page_reset_defaults(Config *cfg) {
    Config disk;
    config_init(&disk);
    if (config_load(&disk) == 0) {
        config_remove(&disk, "led_enabled");
        config_remove(&disk, "led_brightness");
        if (config_save(&disk) != 0)
            fprintf(stderr, "control_panel: LED defaults save failed\n");
    }
    /* With the keys gone, hardware.c resolves config.c's defaults — the same
     * ones led_page_load() reads off the cleared cfg below. */
    hw_reload_config();
    led_page_load(cfg);
}

/* The -/+ preview: a ~500 ms flash of both LEDs at the chosen value.  Raw
 * sysfs on purpose, like the Display page's backlight preview: it shows the
 * brightness being CHOSEN, whether or not the LEDs are enabled, and the scaled
 * setters would gate it on the very switch next to it.  hw_leds_off() is
 * config-independent, so it is the right way back to dark.
 * Known trait, accepted by the operator: the usleep() blocks the UI for ~500 ms
 * on every -/+ press. */
static void led_preview(int brightness_pct) {
    char buf[8];
    snprintf(buf, sizeof(buf), "%d", brightness_pct);
    FILE *f;
    f = fopen("/sys/class/leds/red_led/brightness", "w");
    if (f) { fputs(buf, f); fclose(f); }
    f = fopen("/sys/class/leds/green_led/brightness", "w");
    if (f) { fputs(buf, f); fclose(f); }
    usleep(500000);
    hw_leds_off();
}

/* ── Layout ─────────────────────────────────────────────────────────────── */

static ToggleSwitch led_toggle;
static Button       led_minus_btn, led_plus_btn;
static Button       test_btns[LED_TEST_COUNT];

static int  sec_led_y, bright_label_y, bar_x, bar_y, bar_w;
static int  sec_tests_y;
static bool stacked;      /* brightness controls on their own row under the label */

#define LED_TOGGLE_LABEL     "LEDS ENABLED"
#define LED_TOGGLE_LABEL_OFF "LEDS DISABLED"   /* the wider one: the receipt measures it */
#define LED_BRIGHT_LABEL "BRIGHTNESS"
#define LED_STEP_BTN_W   45
#define LED_STEP_BTN_H   30
#define LED_TEST_BTN_H   50
#define LED_TEST_GAP     10

/* Re-run whenever the logical screen changes (rebuild_ui()). */
static void led_page_layout(void) {
    int pct_w   = text_measure_width("100%", 2);   /* draw_brightness_bar()'s widest */
    int label_w = text_measure_width(LED_BRIGHT_LABEL, 2);

    sec_led_y = CONTENT_Y + CONTENT_TOP_GAP;
    toggle_init(&led_toggle, CONTENT_LEFT + 5, sec_led_y + 20, 60, 28,
                LED_TOGGLE_LABEL, true);

    /* One row — label, [-], bar, percentage, [+] — when that fits the content
     * width, otherwise the controls drop under the label and the bar takes what
     * is left.  Decided from measured widths rather than from "is this
     * portrait", so a wide inset cannot push [+] off the touchable rect. */
    bright_label_y = sec_led_y + 62;
    int row_bar_x  = CONTENT_LEFT + 5 + label_w + 10 + LED_STEP_BTN_W + 10;
    int row_right  = row_bar_x + BAR_WIDTH + 10 + pct_w + 12 + LED_STEP_BTN_W;
    stacked = (row_right > CONTENT_RIGHT);
    if (!stacked) {
        bar_x = row_bar_x;
        bar_y = bright_label_y;
        bar_w = BAR_WIDTH;
    } else {
        bar_x = CONTENT_LEFT + LED_STEP_BTN_W + 10;
        bar_y = bright_label_y + 30;
        bar_w = CONTENT_RIGHT - LED_STEP_BTN_W - 12 - pct_w - 10 - bar_x;
        if (bar_w < 80) bar_w = 80;
    }
    int step_y = bar_y + (BAR_HEIGHT - LED_STEP_BTN_H) / 2;
    button_init_full(&led_minus_btn, bar_x - 10 - LED_STEP_BTN_W, step_y,
                     LED_STEP_BTN_W, LED_STEP_BTN_H, "-", RGB(80, 80, 80),
                     COLOR_WHITE, BTN_COLOR_HIGHLIGHT, 2);
    button_init_full(&led_plus_btn, bar_x + bar_w + 10 + pct_w + 12, step_y,
                     LED_STEP_BTN_W, LED_STEP_BTN_H, "+", RGB(80, 80, 80),
                     COLOR_WHITE, BTN_COLOR_HIGHLIGHT, 2);

    /* The tests: three columns where the content is wide, two where it is not. */
    sec_tests_y = step_y + LED_STEP_BTN_H + 20;
    int cols    = (CONTENT_WIDTH >= 600) ? 3 : 2;
    int btn_w   = (CONTENT_WIDTH - (cols - 1) * LED_TEST_GAP) / cols;
    int grid_y  = sec_tests_y + 26;
    for (int i = 0; i < LED_TEST_COUNT; i++) {
        int c = i % cols, r = i / cols;
        button_init_full(&test_btns[i],
                         CONTENT_LEFT + c * (btn_w + LED_TEST_GAP),
                         grid_y + r * (LED_TEST_BTN_H + LED_TEST_GAP),
                         btn_w, LED_TEST_BTN_H, led_tests[i].name,
                         RGB(34, 34, 34), COLOR_WHITE, BTN_COLOR_HIGHLIGHT, 2);
    }

    /* ⚠️ THE RECEIPT, in the settings stack's shape.  Everything here hangs off
     * CONTENT_Y, which comes from a per-unit touch inset, so a row pushed past
     * the touchable rect looks perfect in a screenshot and is dead to a finger.
     * The last test button is the lowest widget; the right edge is the widest of
     * the toggle's hit box, [+] and the test grid, each read off the widget as
     * placed, not re-derived. */
    {
        const Button *last = &test_btns[LED_TEST_COUNT - 1];
        int bottom = (last->y + last->height) - CONTENT_Y;
        int right  = led_toggle.x - 5 + led_toggle.track_w
                   + text_measure_width(LED_TOGGLE_LABEL_OFF, 1) + 20;
        if (led_plus_btn.x + led_plus_btn.width > right)
            right = led_plus_btn.x + led_plus_btn.width;
        for (int i = 0; i < LED_TEST_COUNT; i++)
            if (test_btns[i].x + test_btns[i].width > right)
                right = test_btns[i].x + test_btns[i].width;
        const char *verdict = bottom > CONTENT_H     ? "⚠ PAST CONTENT BOTTOM"
                            : right  > CONTENT_RIGHT ? "⚠ PAST CONTENT RIGHT"
                            : "fits";
        printf("control_panel: led stack %s — bottom +%d of CONTENT_H %d, "
               "right %d of CONTENT_RIGHT %d (safe %dx%d, %s, %s)\n",
               verdict, bottom, CONTENT_H, right, CONTENT_RIGHT,
               SCREEN_SAFE_WIDTH, SCREEN_SAFE_HEIGHT,
               CONTENT_WIDTH < 600 ? "portrait" : "landscape",
               stacked ? "stacked" : "one row");
    }
}

/* ── Draw and input ─────────────────────────────────────────────────────── */

/* Derived from the state every time, never cached: the -/+ and test buttons are
 * disabled exactly when the LEDs are.  Button.disabled makes them grey and
 * deaf to every input. */
static void led_sync_disabled(void) {
    bool off = !led_state.enabled;
    led_minus_btn.disabled = off;
    led_plus_btn.disabled  = off;
    for (int i = 0; i < LED_TEST_COUNT; i++)
        test_btns[i].disabled = off;
}

static void led_page_draw(Framebuffer *fb) {
    const LedPageState *s = &led_state;
    draw_section_header(fb, sec_led_y, "LEDS");
    led_toggle.state = s->enabled;      /* derived every frame, never cached */
    snprintf(led_toggle.label, sizeof(led_toggle.label), "%s",
             s->enabled ? LED_TOGGLE_LABEL : LED_TOGGLE_LABEL_OFF);
    toggle_draw(fb, &led_toggle);

    led_sync_disabled();

    /* Disabled, brightness is kept but not adjustable: -/+ would flash the
     * LEDs through led_preview(), which bypasses the enable setting.  The
     * buttons grey themselves (Button.disabled); only the label is ours. */
    fb_draw_text(fb, CONTENT_LEFT + 5, bright_label_y + 2, LED_BRIGHT_LABEL,
                 s->enabled ? COLOR_LABEL : COLOR_DISABLED, 2);
    button_draw(fb, &led_minus_btn);
    draw_brightness_bar(fb, bar_x, bar_y, s->brightness, 0, 100, bar_w,
                        s->enabled);
    button_draw(fb, &led_plus_btn);

    /* With the LEDs disabled a test would light nothing, so the buttons are
     * disabled (grey, no press — button_update ignores them) and the header
     * says why. */
    draw_section_header(fb, sec_tests_y,
                        s->enabled ? "TESTS" : "TESTS (LEDS DISABLED)");
    for (int i = 0; i < LED_TEST_COUNT; i++) {
        button_draw(fb, &test_btns[i]);
    }
}

static CpPageResult led_page_input(Config *cfg, int tx, int ty,
                                   bool touching, uint32_t now) {
    LedPageState *s = &led_state;
    CpPageResult act = CP_PAGE_IDLE;

    led_toggle.state = s->enabled;      /* flip from the truth, not a stale widget */
    if (toggle_check_press(&led_toggle, tx, ty, touching, now)) {
        s->enabled = led_toggle.state;
        led_persist(s, cfg);
        act = CP_PAGE_REDRAW;
    }
    /* Disabled, the -/+ and test buttons take no press at all — not even the
     * pressed highlight: a test would run and light nothing, and -/+ would
     * flash the LEDs through led_preview(), which bypasses the enable setting.
     * That is Button.disabled's doing, set from the state just above (after the
     * toggle may have flipped it), so no guard is needed around the calls. */
    led_sync_disabled();
    int step = 0;
    if (button_update(&led_minus_btn, tx, ty, touching, now)) step = -10;
    if (button_update(&led_plus_btn, tx, ty, touching, now))  step = +10;
    if (step) {
        int before = s->brightness;
        s->brightness = clamp_pct(s->brightness + step);
        led_persist(s, cfg);
        led_preview(s->brightness);
        if (s->brightness != before) act = CP_PAGE_REDRAW;
    }
    for (int i = 0; i < LED_TEST_COUNT; i++) {
        if (button_update(&test_btns[i], tx, ty, touching, now)) {
            queued_test = i;
            act = CP_PAGE_FULLSCREEN;
        }
    }
    return act;
}

/* Keyboard focus: what input() hit-tests, minus whatever is disabled. */
static int led_page_focusables(UiRect *out, int max) {
    int n = focus_add_toggle(out, 0, max, &led_toggle);
    n = focus_add_button(out, n, max, &led_minus_btn);
    n = focus_add_button(out, n, max, &led_plus_btn);
    for (int i = 0; i < LED_TEST_COUNT; i++)
        n = focus_add_button(out, n, max, &test_btns[i]);
    return n;
}

const CpPage cp_led_page = {
    .name           = "LED",
    .icon           = "cp_led",
    .load           = led_page_load,
    .layout         = led_page_layout,
    .draw           = led_page_draw,
    .input          = led_page_input,
    .focusables     = led_page_focusables,
    .run_fullscreen = led_page_run_fullscreen,
    .reset_defaults = led_page_reset_defaults,
};
