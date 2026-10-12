/* display_page.c — control_panel's Display page: backlight, orientation, what
 * is visible and what is touchable, SCREEN EDGES, and the display tests.
 *
 * Opened from the home grid's Display tile, and the one home for the panel's
 * own look and its two rectangles; touch calibration and whether it has been
 * done are the Input page's (input_page.c).  Exposed only as cp_display_page
 * (cp_page.h); its state lives in this file.
 *
 * ⚠️ There is no SAVE button, on purpose.  -/+ writes backlight_brightness to
 * the config file the moment it changes and reloads common/hardware.c's cache,
 * because the BACKLIGHT RAMP on the same screen drives the panel through
 * hw_set_backlight(), which is scaled by the SAVED value.  The portrait toggle
 * writes or removes the flag file at once for the same reason: what the page
 * shows is what the next launch gets.
 */
#include "cp_page.h"
#include "cp_ui.h"
#include "../common/ui_flow.h"
#include "../common/common.h"
#include "../common/hardware.h"

#include <stdio.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/fb.h>

/* Read at launch by fb_init(): present = portrait.  The file's presence is the
 * whole setting; its content is never read. */
#define PORTRAIT_FLAG_FILE  "/opt/games/portrait.mode"

#define DEFAULT_BACKLIGHT_BRIGHTNESS 100
#define BACKLIGHT_MIN                20   /* below this the panel reads as off */

/* ── The tests ──────────────────────────────────────────────────────────────
 * Blocking full-screen routines, run through run_fullscreen. */

static void test_backlight_run(Framebuffer *fb, TouchInput *touch) {
    int original = hw_get_backlight();
    int x, y;
    for (int i = 100; i >= 20; i -= 5) {
        char s[64]; snprintf(s, sizeof(s), "BRIGHTNESS: %d%%", i);
        draw_test_screen(fb, "BACKLIGHT TEST", s, 100 - i);
        hw_set_backlight(i); usleep(50000);
        if (check_touch(touch, &x, &y)) { hw_set_backlight(original); return; }
    }
    for (int i = 20; i <= 100; i += 5) {
        char s[64]; snprintf(s, sizeof(s), "BRIGHTNESS: %d%%", i);
        draw_test_screen(fb, "BACKLIGHT TEST", s, i);
        hw_set_backlight(i); usleep(50000);
        if (check_touch(touch, &x, &y)) { hw_set_backlight(original); return; }
    }
    hw_set_backlight(original);
    draw_test_screen(fb, "BACKLIGHT TEST", "COMPLETE!", 100);
    while (!check_touch(touch, &x, &y)) usleep(10000);
}

static void draw_pattern_frame(Framebuffer *fb, const char *title,
                               const char *footer) {
    fb_draw_text(fb, 4, 2, title, COLOR_WHITE, 2);
    fb_draw_text(fb, fb->width / 3, fb->height - 20, footer, RGB(140,140,140), 1);
}

static void test_display(Framebuffer *fb, TouchInput *touch) {
    int page = 0;
    const int pages = 6;
    bool disp_running = true;
    int x, y;
    struct fb_var_screeninfo vinfo;
    ioctl(fb->fd, FBIOGET_VSCREENINFO, &vinfo);

    while (disp_running) {
        fb_clear(fb, COLOR_BLACK);
        switch (page) {
        case 0: {
            draw_pattern_frame(fb, "DISPLAY INFO", "tap -> next | top-right -> exit");
            char buf[96]; int row = 60;
            #define INFO_LINE(fmt, ...) \
                snprintf(buf, sizeof(buf), fmt, __VA_ARGS__); \
                fb_draw_text(fb, 40, row, buf, COLOR_CYAN, 2); row += 30;
            INFO_LINE("Resolution:  %dx%d", fb->width, fb->height);
            INFO_LINE("BPP:         %d", vinfo.bits_per_pixel);
            INFO_LINE("Line length: %d bytes", fb->line_length);
            INFO_LINE("Screen size: %d bytes", (int)fb->screen_size);
            INFO_LINE("Bytes/pixel: %d", fb->bytes_per_pixel);
            INFO_LINE("Visible:     (%d,%d)-(%d,%d)",
                       SCREEN_VISIBLE_LEFT, SCREEN_VISIBLE_TOP,
                       SCREEN_VISIBLE_RIGHT, SCREEN_VISIBLE_BOTTOM);
            INFO_LINE("Touch-safe:  (%d,%d)-(%d,%d)",
                       SCREEN_SAFE_LEFT, SCREEN_SAFE_TOP,
                       SCREEN_SAFE_RIGHT, SCREEN_SAFE_BOTTOM);
            INFO_LINE("Double buf:  %s", fb->double_buffering ? "yes" : "no");
            #undef INFO_LINE
            break;
        }
        case 1: {
            draw_pattern_frame(fb, "COLOR BARS", "tap -> next");
            int bw = fb->width / 4;
            fb_fill_rect(fb, 0*bw, 40, bw, fb->height - 80, RGB(255,0,0));
            fb_fill_rect(fb, 1*bw, 40, bw, fb->height - 80, RGB(0,255,0));
            fb_fill_rect(fb, 2*bw, 40, bw, fb->height - 80, RGB(0,0,255));
            fb_fill_rect(fb, 3*bw, 40, bw, fb->height - 80, RGB(255,255,255));
            break;
        }
        case 2: {
            draw_pattern_frame(fb, "GRADIENT", "tap -> next");
            for (int col = 0; col < (int)fb->width; col++) {
                uint8_t v = (col * 255) / (fb->width - 1);
                fb_fill_rect(fb, col, 50, 1, fb->height - 100, RGB(v,v,v));
            }
            break;
        }
        case 3: {
            draw_pattern_frame(fb, "PIXEL GRID", "tap -> next");
            for (int gx = 0; gx < (int)fb->width; gx += 2)
                fb_fill_rect(fb, gx, 40, 1, fb->height - 80, RGB(200,200,200));
            for (int gy = 40; gy < (int)fb->height - 40; gy += 2)
                fb_fill_rect(fb, 0, gy, fb->width, 1, RGB(200,200,200));
            break;
        }
        case 4: {
            /* The two rectangles: red = SCREEN_VISIBLE_* (everything drawable),
             * green = SCREEN_SAFE_* (visible AND touchable). The gap between
             * them is the digitizer's dead band — good screen area, just not
             * somewhere to put a button. */
            draw_pattern_frame(fb, "SAFE AREA", "tap -> next");
            fb_draw_rect(fb, SCREEN_VISIBLE_LEFT, SCREEN_VISIBLE_TOP,
                         SCREEN_VISIBLE_WIDTH, SCREEN_VISIBLE_HEIGHT, COLOR_RED);
            fb_draw_rect(fb, SCREEN_VISIBLE_LEFT+1, SCREEN_VISIBLE_TOP+1,
                         SCREEN_VISIBLE_WIDTH-2, SCREEN_VISIBLE_HEIGHT-2, COLOR_RED);
            fb_draw_rect(fb, SCREEN_SAFE_LEFT, SCREEN_SAFE_TOP,
                         SCREEN_SAFE_WIDTH, SCREEN_SAFE_HEIGHT, COLOR_GREEN);
            fb_draw_rect(fb, SCREEN_SAFE_LEFT+1, SCREEN_SAFE_TOP+1,
                         SCREEN_SAFE_WIDTH-2, SCREEN_SAFE_HEIGHT-2, COLOR_GREEN);
            { char buf[64];
            snprintf(buf, sizeof(buf), "L=%d", SCREEN_SAFE_LEFT);
            fb_draw_text(fb, SCREEN_SAFE_LEFT+4, 240, buf, COLOR_GREEN, 1);
            snprintf(buf, sizeof(buf), "R=%d", SCREEN_SAFE_RIGHT);
            fb_draw_text(fb, SCREEN_SAFE_RIGHT-40, 240, buf, COLOR_GREEN, 1);
            snprintf(buf, sizeof(buf), "T=%d", SCREEN_SAFE_TOP);
            fb_draw_text(fb, 370, SCREEN_SAFE_TOP+4, buf, COLOR_GREEN, 1);
            snprintf(buf, sizeof(buf), "B=%d", SCREEN_SAFE_BOTTOM);
            fb_draw_text(fb, 370, SCREEN_SAFE_BOTTOM-16, buf, COLOR_GREEN, 1);
            snprintf(buf, sizeof(buf), "RED %dx%d VISIBLE  GREEN %dx%d TOUCHABLE",
                     SCREEN_VISIBLE_WIDTH, SCREEN_VISIBLE_HEIGHT,
                     SCREEN_SAFE_WIDTH, SCREEN_SAFE_HEIGHT);
            text_draw_centered(fb, fb->width/2, 200, buf, COLOR_WHITE, 1);
            text_draw_centered(fb, fb->width/2, 280,
                               "THE GAP IS DRAWABLE BUT NOT PRESSABLE",
                               COLOR_YELLOW, 1); }
            break;
        }
        case 5: {
            draw_pattern_frame(fb, "ALPHA BLEND", "tap -> exit");
            fb_fill_rect(fb, 100, 80, 300, 300, COLOR_RED);
            fb_fill_rect(fb, 400, 80, 300, 300, COLOR_BLUE);
            fb_fill_rect_alpha(fb, 200, 150, 400, 200, RGB(0,255,0), 128);
            fb_draw_text(fb, 300, 250, "alpha=128", COLOR_WHITE, 2);
            break;
        }
        }
        fb_swap(fb);
        while (1) {
            if (touch_wait_for_press(touch, &x, &y) == 0) {
                if (x > (int)fb->width - 100 && y < 40) { disp_running = false; break; }
                page++;
                if (page >= pages) disp_running = false;
                break;
            }
            if (cp_key_back()) { disp_running = false; break; }   /* Esc leaves */
            usleep(16000);
        }
    }
}

/* The calibration wizard, edges only: it measures what is drawable, so it sits
 * with the rows it changes.  cp_run_touch_tool() posts the outcome and lays
 * every page out again, this one included. */
static void run_screen_edges(Framebuffer *fb, TouchInput *touch) {
    cp_run_touch_tool(fb, touch, CP_TOUCH_EDGES);
}

/* Name and routine in one row, so the button a finger presses and the routine
 * that runs cannot drift apart the way two parallel lists can. */
static const struct {
    const char *name;
    void      (*run)(Framebuffer *, TouchInput *);
} disp_tests[] = {
    { "BACKLIGHT RAMP", test_backlight_run },
    { "TEST PATTERNS",  test_display       },
    { "SCREEN EDGES",   run_screen_edges   },
};
#define DISP_TEST_COUNT ((int)(sizeof(disp_tests) / sizeof(disp_tests[0])))

/* ── State and persistence ─────────────────────────────────────────────── */

static int  backlight;           /* config key backlight_brightness, 20..100 */
static bool portrait;            /* PORTRAIT_FLAG_FILE exists */
static int  queued_test = -1;    /* test to run full-screen, -1 none */
static int  blank_min;           /* config key blank_minutes: 0 = never */

/* What the SCREEN OFF button cycles through. */
static const int blank_choices[] = { 0, 1, 2, 5, 10, 30 };
#define BLANK_CHOICE_COUNT ((int)(sizeof(blank_choices) / sizeof(blank_choices[0])))

static int clamp_backlight(int v) {
    return v < BACKLIGHT_MIN ? BACKLIGHT_MIN : v > 100 ? 100 : v;
}

/* Live preview of the chosen value — unscaled on purpose: the value IS the
 * scale factor, so hw_set_backlight() would apply the outgoing one.  The raw
 * setter also owns the sysfs path; a private copy here named a node that does
 * not exist on this device and the preview silently did nothing. */
static void apply_backlight(int brightness_pct) {
    if (brightness_pct < 0)   brightness_pct = 0;
    if (brightness_pct > 100) brightness_pct = 100;
    if (hw_set_backlight_raw((uint8_t)brightness_pct) < 0)
        fprintf(stderr, "control_panel: backlight preview write failed\n");
}

static void display_page_load(const Config *cfg) {
    backlight = clamp_backlight(config_get_int(cfg, "backlight_brightness",
                                               DEFAULT_BACKLIGHT_BRIGHTNESS));
    blank_min = config_blank_minutes(cfg);
    portrait  = (access(PORTRAIT_FLAG_FILE, F_OK) == 0);
}

/* Writes the key into the FILE by re-reading it, not by saving the caller's
 * Config: every page shares that one, and a whole-file save of it from here
 * would persist whatever another page holds in it unsaved.  The in-memory copy
 * is updated too, so a later SAVE elsewhere writes this value, not the old. */
static void backlight_persist(Config *mem) {
    config_set_int(mem, "backlight_brightness", backlight);

    Config disk;
    config_init(&disk);
    config_load(&disk);                 /* a missing file just starts empty */
    config_set_int(&disk, "backlight_brightness", backlight);
    if (config_save(&disk) != 0) {
        fprintf(stderr, "control_panel: backlight save failed\n");
        cp_status("BACKLIGHT SAVE FAILED", false);
    }
    hw_reload_config();                 /* the ramp scales by the saved value */
}

/* Same shape as backlight_persist(): the key goes into the file by re-reading
 * it, then hardware.c's cache is reloaded so the new delay applies at once. */
static void blank_persist(Config *mem) {
    config_set_int(mem, "blank_minutes", blank_min);

    Config disk;
    config_init(&disk);
    config_load(&disk);
    config_set_int(&disk, "blank_minutes", blank_min);
    if (config_save(&disk) != 0) {
        fprintf(stderr, "control_panel: blank_minutes save failed\n");
        cp_status("SCREEN OFF SAVE FAILED", false);
    }
    hw_reload_config();
}

static void blank_label(char *out, size_t n, int minutes) {
    if (minutes <= 0) snprintf(out, n, "SCREEN OFF: NEVER");
    else              snprintf(out, n, "SCREEN OFF: %d MIN", minutes);
}

/* Present = portrait on the next launch; the file's content is never read. */
static void portrait_persist(void) {
    bool ok;
    if (portrait) {
        FILE *pf = fopen(PORTRAIT_FLAG_FILE, "w");
        ok = pf && fprintf(pf, "1\n") > 0;
        if (pf && fclose(pf) != 0) ok = false;
    } else {
        ok = (unlink(PORTRAIT_FLAG_FILE) == 0 || access(PORTRAIT_FLAG_FILE, F_OK) != 0);
    }
    if (!ok) {
        fprintf(stderr, "control_panel: portrait flag write failed\n");
        cp_status("PORTRAIT SAVE FAILED - RUN AS ROOT", false);
    }
}

/* For the control panel's RESET DEFAULTS, called AFTER config_clear(cfg):
 * removes the key from the file as well, reloads hardware.c's cache and
 * re-applies, so page, file and panel agree on the default — otherwise the
 * backlight keeps a value no longer in the file.  Orientation is not a config
 * key, but landscape is the factory state, so the flag file goes too; the
 * amber note then disappears with the toggle, and the next launch is
 * landscape. */
static void display_page_reset_defaults(Config *cfg) {
    Config disk;
    config_init(&disk);
    if (config_load(&disk) == 0) {
        config_remove(&disk, "backlight_brightness");
        config_remove(&disk, "blank_minutes");
        if (config_save(&disk) != 0)
            fprintf(stderr, "control_panel: backlight default save failed\n");
    }
    hw_reload_config();
    portrait = false;
    portrait_persist();
    display_page_load(cfg);
    apply_backlight(backlight);
}

static void display_page_run_fullscreen(Framebuffer *fb, TouchInput *touch) {
    int test = queued_test;
    queued_test = -1;
    if (test >= 0 && test < DISP_TEST_COUNT)
        disp_tests[test].run(fb, touch);
}

/* ── The two rectangles: EDGES and TOUCHABLE ───────────────────────────── */

/* The screen area the digitiser can actually reach, in LOGICAL pixels — what an
 * app author lays out in.
 *
 * This is NOT expected to be the whole logical screen. The digitiser saturates
 * before the physical panel edge, so a band at each end of Y is visible but not
 * pressable; on RW09 that is ~17 rows at the top and ~16 at the bottom. It is
 * measured per panel, never assumed.
 *
 * Read straight off the published inset rather than re-derived from the curve:
 * that is the same number every app's SCREEN_SAFE_* resolves to, so this row
 * cannot disagree with what layouts actually get. */
static void display_touchable_rect(int *lx0, int *lx1, int *ly0, int *ly1) {
    *lx0 = screen_touch_inset_left;
    *ly0 = screen_touch_inset_top;
    *lx1 = screen_base_width  - 1 - screen_touch_inset_right;
    *ly1 = screen_base_height - 1 - screen_touch_inset_bottom;
}

/* Both rows' values, and the TOUCHABLE colour: one formatter, read by the draw
 * and by the layout receipt, so the width the receipt checks is the text drawn.
 *
 * Visible is not the same as touchable, and the TOUCHABLE row is the only place
 * a reader finds that out without rediscovering it the hard way. A non-zero
 * inset is the CORRECT answer on this hardware — the digitiser saturates before
 * the panel edge — so it is only amber once it is large enough to be
 * suspicious. The band stays drawable either way. */
static uint32_t geom_rows_format(char *edges, size_t elen, char *reach, size_t rlen) {
    snprintf(edges, elen, "T:%d  B:%d  L:%d  R:%d",
             screen_bezel_top, screen_bezel_bottom,
             screen_bezel_left, screen_bezel_right);
    int tx0, tx1, ty0, ty1;
    display_touchable_rect(&tx0, &tx1, &ty0, &ty1);
    snprintf(reach, rlen, "X %d..%d  Y %d..%d", tx0, tx1, ty0, ty1);
    int worst_inset = screen_touch_inset_top;
    if (screen_touch_inset_bottom > worst_inset) worst_inset = screen_touch_inset_bottom;
    if (screen_touch_inset_left   > worst_inset) worst_inset = screen_touch_inset_left;
    if (screen_touch_inset_right  > worst_inset) worst_inset = screen_touch_inset_right;
    return worst_inset > DISP_INSET_SUSPECT ? COLOR_ORANGE : COLOR_GREEN;
}

/* ── Layout ─────────────────────────────────────────────────────────────── */

static ToggleSwitch portrait_toggle;
static Button       bl_minus_btn, bl_plus_btn;
static Button       blank_btn;
static Button       test_btns[DISP_TEST_COUNT];

static int  sec_disp_y, bl_label_y, bar_x, bar_y, bar_w;
static int  note_y, visible_y, edges_y, reach_y, sec_tests_y;
static bool stacked;      /* backlight controls on their own row under the label */

#define DISP_BL_LABEL        "BACKLIGHT"
#define DISP_PORTRAIT_LABEL  "PORTRAIT MODE"
#define DISP_PORTRAIT_NOTE   "ON NEXT LAUNCH - CALIBRATE IN LANDSCAPE"
#define DISP_STEP_BTN_W      45
#define DISP_STEP_BTN_H      30
#define DISP_TEST_BTN_H      50
#define DISP_TEST_GAP        10
#define DISP_INFO_ROW_H      28   /* draw_info_row()'s advance */

/* Re-run whenever the logical screen changes (rebuild_ui()). */
static void display_page_layout(void) {
    int pct_w   = text_measure_width("100%", 2);   /* draw_brightness_bar()'s widest */
    int label_w = text_measure_width(DISP_BL_LABEL, 2);

    sec_disp_y = CONTENT_Y + CONTENT_TOP_GAP;

    /* One row — label, [-], bar, percentage, [+] — when that fits the content
     * width, otherwise the controls drop under the label.  Decided from
     * measured widths rather than from "is this portrait", so a wide inset
     * cannot push [+] off the touchable rect. */
    bl_label_y    = sec_disp_y + 30;
    int row_bar_x = CONTENT_LEFT + 5 + label_w + 10 + DISP_STEP_BTN_W + 10;
    int row_right = row_bar_x + BAR_WIDTH + 10 + pct_w + 12 + DISP_STEP_BTN_W;
    stacked = (row_right > CONTENT_RIGHT);
    if (!stacked) {
        bar_x = row_bar_x;
        bar_y = bl_label_y;
        bar_w = BAR_WIDTH;
    } else {
        bar_x = CONTENT_LEFT + DISP_STEP_BTN_W + 10;
        bar_y = bl_label_y + 30;
        bar_w = CONTENT_RIGHT - DISP_STEP_BTN_W - 12 - pct_w - 10 - bar_x;
        if (bar_w < 80) bar_w = 80;
    }
    int step_y = bar_y + (BAR_HEIGHT - DISP_STEP_BTN_H) / 2;
    button_init_full(&bl_minus_btn, bar_x - 10 - DISP_STEP_BTN_W, step_y,
                     DISP_STEP_BTN_W, DISP_STEP_BTN_H, "-", RGB(80, 80, 80),
                     COLOR_WHITE, BTN_COLOR_HIGHLIGHT, 2);
    button_init_full(&bl_plus_btn, bar_x + bar_w + 10 + pct_w + 12, step_y,
                     DISP_STEP_BTN_W, DISP_STEP_BTN_H, "+", RGB(80, 80, 80),
                     COLOR_WHITE, BTN_COLOR_HIGHLIGHT, 2);

    /* The toggle, and under it the note it shows when on — on its own line,
     * because beside the toggle it runs past a portrait content width. */
    int blank_y = step_y + DISP_STEP_BTN_H + 12;
    int blank_w = text_measure_width("SCREEN OFF: NEVER", 2) + 24;
    button_init_full(&blank_btn, CONTENT_LEFT + 5, blank_y, blank_w, DISP_STEP_BTN_H,
                     "", RGB(80, 80, 80), COLOR_WHITE, BTN_COLOR_HIGHLIGHT, 2);
    toggle_init(&portrait_toggle, CONTENT_LEFT + 5, blank_y + DISP_STEP_BTN_H + 16,
                60, 28, DISP_PORTRAIT_LABEL, portrait);
    note_y    = portrait_toggle.y + 36;
    visible_y = note_y + 20;
    edges_y   = visible_y + DISP_INFO_ROW_H;
    reach_y   = edges_y + DISP_INFO_ROW_H;

    /* The tests: one row of three where every label fits a third at scale 2
     * (landscape), otherwise two columns, each as wide as half the content. */
    sec_tests_y = reach_y + DISP_INFO_ROW_H + 12;
    int cols    = 3;
    int btn_w   = (CONTENT_WIDTH - (cols - 1) * DISP_TEST_GAP) / cols;
    for (int i = 0; i < DISP_TEST_COUNT; i++)
        if (!ui_label_fits(disp_tests[i].name, 2, btn_w)) cols = 2;
    btn_w       = (CONTENT_WIDTH - (cols - 1) * DISP_TEST_GAP) / cols;
    int grid_y  = sec_tests_y + 26;
    UiRect test_r[DISP_TEST_COUNT];
    ui_flow_place(DISP_TEST_COUNT, btn_w, DISP_TEST_BTN_H, DISP_TEST_GAP,
                  DISP_TEST_GAP, CONTENT_LEFT, grid_y, CONTENT_WIDTH, cols, 0,
                  test_r);
    for (int i = 0; i < DISP_TEST_COUNT; i++)
        button_init_full(&test_btns[i], test_r[i].x, test_r[i].y,
                         test_r[i].w, test_r[i].h, disp_tests[i].name,
                         RGB(34, 34, 34), COLOR_WHITE, BTN_COLOR_HIGHLIGHT, 2);

    /* ⚠️ THE RECEIPT, in the settings stack's shape.  Everything here hangs off
     * CONTENT_Y, which comes from a per-unit touch inset, so a row pushed past
     * the touchable rect looks perfect in a screenshot and is dead to a finger.
     * The last test button is the lowest widget; the right edge is the widest of
     * the toggle's hit box, the note, [+] and the test grid, each read off the
     * widget as placed, not re-derived.  A test label wider than its button, or
     * an EDGES / TOUCHABLE value past the content edge, is cut, and counted. */
    {
        int clipped = 0;
        for (int i = 0; i < DISP_TEST_COUNT; i++)
            if (!ui_label_fits(disp_tests[i].name, 2, test_btns[i].width))
                clipped++;
        char edges[48], reach[48], cut[48];
        int value_x = CONTENT_LEFT + (CONTENT_WIDTH < 600 ? 150 : 270);   /* draw_info_row()'s */
        geom_rows_format(edges, sizeof(edges), reach, sizeof(reach));
        if (fit_value(edges, value_x, 2, cut, sizeof(cut))) clipped++;
        if (fit_value(reach, value_x, 2, cut, sizeof(cut))) clipped++;
        const Button *last = &test_btns[DISP_TEST_COUNT - 1];
        int bottom = (last->y + last->height) - CONTENT_Y;
        int right  = portrait_toggle.x - 5 + portrait_toggle.track_w
                   + text_measure_width(DISP_PORTRAIT_LABEL, 1) + 20;
        int note_r = CONTENT_LEFT + 5 + text_measure_width(DISP_PORTRAIT_NOTE, 1);
        if (note_r > right) right = note_r;
        if (blank_btn.x + blank_btn.width > right)
            right = blank_btn.x + blank_btn.width;
        if (bl_plus_btn.x + bl_plus_btn.width > right)
            right = bl_plus_btn.x + bl_plus_btn.width;
        for (int i = 0; i < DISP_TEST_COUNT; i++)
            if (test_btns[i].x + test_btns[i].width > right)
                right = test_btns[i].x + test_btns[i].width;
        const char *verdict = bottom > CONTENT_H     ? "⚠ PAST CONTENT BOTTOM"
                            : right  > CONTENT_RIGHT ? "⚠ PAST CONTENT RIGHT"
                            : "fits";
        printf("control_panel: display stack %s — bottom +%d of CONTENT_H %d, "
               "right %d of CONTENT_RIGHT %d, %d label(s) cut, tests in %d "
               "column(s) (safe %dx%d, %s)\n",
               verdict, bottom, CONTENT_H, right, CONTENT_RIGHT, clipped, cols,
               SCREEN_SAFE_WIDTH, SCREEN_SAFE_HEIGHT,
               CONTENT_WIDTH < 600 ? "portrait" : "landscape");
    }
}

/* ── Draw and input ─────────────────────────────────────────────────────── */

static void display_page_draw(Framebuffer *fb) {
    draw_section_header(fb, sec_disp_y, "DISPLAY");

    fb_draw_text(fb, CONTENT_LEFT + 5, bl_label_y + 2, DISP_BL_LABEL, COLOR_LABEL, 2);
    button_draw(fb, &bl_minus_btn);
    draw_brightness_bar(fb, bar_x, bar_y, backlight, BACKLIGHT_MIN, 100, bar_w, true);
    button_draw(fb, &bl_plus_btn);

    char bl[32];
    blank_label(bl, sizeof(bl), blank_min);
    button_set_text(&blank_btn, bl);
    button_draw(fb, &blank_btn);

    portrait_toggle.state = portrait;   /* derived every frame, never cached */
    toggle_draw(fb, &portrait_toggle);
    if (portrait)
        fb_draw_text(fb, CONTENT_LEFT + 5, note_y, DISP_PORTRAIT_NOTE,
                     RGB(255, 200, 80), 1);

    char buf[48];
    snprintf(buf, sizeof(buf), "%dx%d OF %dx%d",
             (int)fb->width, (int)fb->height,
             screen_panel_width, screen_panel_height);
    draw_info_row(fb, visible_y, "VISIBLE:", buf, COLOR_WHITE);

    char edges[48], reach[48];
    uint32_t reach_color = geom_rows_format(edges, sizeof(edges), reach, sizeof(reach));
    draw_info_row(fb, edges_y, "EDGES:", edges, COLOR_WHITE);
    draw_info_row(fb, reach_y, "TOUCHABLE:", reach, reach_color);

    draw_section_header(fb, sec_tests_y, "TESTS");
    for (int i = 0; i < DISP_TEST_COUNT; i++)
        button_draw(fb, &test_btns[i]);
}

static CpPageResult display_page_input(Config *cfg, int tx, int ty,
                                       bool touching, uint32_t now) {
    CpPageResult act = CP_PAGE_IDLE;

    portrait_toggle.state = portrait;   /* flip from the truth, not a stale widget */
    if (toggle_check_press(&portrait_toggle, tx, ty, touching, now)) {
        portrait = portrait_toggle.state;
        portrait_persist();
        act = CP_PAGE_REDRAW;
    }

    int step = 0;
    if (button_update(&bl_minus_btn, tx, ty, touching, now)) step = -10;
    if (button_update(&bl_plus_btn, tx, ty, touching, now))  step = +10;
    if (step) {
        int before = backlight;
        backlight = clamp_backlight(backlight + step);
        if (backlight != before) {
            backlight_persist(cfg);
            apply_backlight(backlight);
            act = CP_PAGE_REDRAW;
        }
    }

    if (button_update(&blank_btn, tx, ty, touching, now)) {
        int at = 0;
        for (int i = 0; i < BLANK_CHOICE_COUNT; i++)
            if (blank_choices[i] == blank_min) at = i;
        blank_min = blank_choices[(at + 1) % BLANK_CHOICE_COUNT];
        blank_persist(cfg);
        act = CP_PAGE_REDRAW;
    }

    for (int i = 0; i < DISP_TEST_COUNT; i++) {
        if (button_update(&test_btns[i], tx, ty, touching, now)) {
            queued_test = i;
            act = CP_PAGE_FULLSCREEN;
        }
    }
    return act;
}

/* Keyboard focus: what input() hit-tests. */
static int display_page_focusables(UiRect *out, int max) {
    int n = focus_add_toggle(out, 0, max, &portrait_toggle);
    n = focus_add_button(out, n, max, &bl_minus_btn);
    n = focus_add_button(out, n, max, &bl_plus_btn);
    n = focus_add_button(out, n, max, &blank_btn);
    for (int i = 0; i < DISP_TEST_COUNT; i++)
        n = focus_add_button(out, n, max, &test_btns[i]);
    return n;
}

const CpPage cp_display_page = {
    .name           = "Display",
    .icon           = "cp_display",
    .load           = display_page_load,
    .layout         = display_page_layout,
    .draw           = display_page_draw,
    .input          = display_page_input,
    .focusables     = display_page_focusables,
    .run_fullscreen = display_page_run_fullscreen,
    .reset_defaults = display_page_reset_defaults,
};
