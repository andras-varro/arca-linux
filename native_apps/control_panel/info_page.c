/* info_page.c — control_panel's Information page: what this unit is.
 *
 * Opened from the home grid's Information tile, and the one home for the
 * static identity figures: kernel release and build, hostname, boot target,
 * CPU, framebuffer format, and the config-file keys no settings page shows.
 * Exposed only as cp_info_page (cp_page.h); its state lives in this file.
 *
 * Nothing here is live or duplicated.  Uptime, load, memory and storage are
 * the Monitor page's; LED state is the LED page's; backlight and resolution
 * are the Display page's, whether touch is calibrated the Input page's; the
 * audio keys are the Audio page's.  So a key those pages own is left out of the
 * config list.
 *
 * Read on load and on every enter(), never per second.  The one widget is the
 * global RESET DEFAULTS, beside the config file's row because the file is what
 * it backs up before resetting; the reset itself is control_panel.c's
 * (cp_reset_all_defaults()), and where the backup went is posted with
 * cp_status().  It asks first, through the panel's one confirmation dialog
 * (cp_confirm()); nothing is backed up or reset until OK.
 */
#include "cp_page.h"
#include "cp_ui.h"
#include "../common/common.h"

#include <stdio.h>
#include <string.h>

/* ── Reading ───────────────────────────────────────────────────────────── */

/* Config keys a settings page already shows, and so this page does not. */
static const char *const shown_elsewhere[] = {
    "audio_enabled", "music_enabled", "effects_enabled", "audio_device",
    "led_enabled", "led_brightness", "backlight_brightness",
};
#define NUM_SHOWN_ELSEWHERE ((int)(sizeof(shown_elsewhere) / sizeof(shown_elsewhere[0])))

static struct {
    char kernel[64];     /* the release, third field of /proc/version */
    char built[128];     /* everything after its last ") ": "#1 SMP <date>" */
    char hostname[64];
    char default_app[128];
    char cpu[128];
    char bogomips[64];
    char keys[CONFIG_MAX_KEYS][CONFIG_KEY_LEN + CONFIG_VAL_LEN + 4];
    int  key_count;
    bool config_found;
} info;

/* The value after "Name<ws>: " on the first /proc/cpuinfo line naming it. */
static void cpuinfo_field(const char *name, char *buf, size_t len) {
    snprintf(buf, len, "N/A");
    FILE *fp = fopen("/proc/cpuinfo", "r");
    if (!fp) return;
    char line[256];
    size_t n = strlen(name);
    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, name, n) != 0) continue;
        char *c = strchr(line, ':');
        if (!c) continue;
        c++;
        while (*c == ' ' || *c == '\t') c++;
        c[strcspn(c, "\r\n")] = '\0';
        snprintf(buf, len, "%s", c);
        break;
    }
    fclose(fp);
}

static void read_kernel(void) {
    char v[256], a[32], b[32];
    snprintf(info.kernel, sizeof(info.kernel), "N/A");
    snprintf(info.built, sizeof(info.built), "N/A");
    if (read_file_line("/proc/version", v, sizeof(v)) < 0) return;
    if (sscanf(v, "%31s %31s %63s", a, b, info.kernel) != 3)
        snprintf(info.kernel, sizeof(info.kernel), "N/A");
    const char *p = strrchr(v, ')');
    if (p && p[1] == ' ') snprintf(info.built, sizeof(info.built), "%s", p + 2);
}

static void read_config_keys(void) {
    static Config cfg;   /* 4 KB: off the stack */
    config_init(&cfg);
    info.key_count = 0;
    info.config_found = (config_load(&cfg) == 0);
    if (!info.config_found) return;
    for (int i = 0; i < cfg.count; i++) {
        bool shown = false;
        for (int k = 0; k < NUM_SHOWN_ELSEWHERE; k++)
            if (strcmp(cfg.entries[i].key, shown_elsewhere[k]) == 0) { shown = true; break; }
        if (shown) continue;
        snprintf(info.keys[info.key_count], sizeof(info.keys[0]), "%s = %s",
                 cfg.entries[i].key, cfg.entries[i].value);
        info.key_count++;
    }
}

static void info_read(void) {
    read_kernel();
    if (read_file_line("/etc/hostname", info.hostname, sizeof(info.hostname)) < 0)
        snprintf(info.hostname, sizeof(info.hostname), "N/A");
    if (read_file_line("/opt/roomwizard/default-app", info.default_app,
                       sizeof(info.default_app)) < 0)
        snprintf(info.default_app, sizeof(info.default_app), "(NOT SET)");
    cpuinfo_field("model name", info.cpu, sizeof(info.cpu));
    cpuinfo_field("BogoMIPS", info.bogomips, sizeof(info.bogomips));
    read_config_keys();
}

/* Read here as well as in enter() so the receipt measures this unit's strings. */
static void info_page_load(const Config *cfg) {
    (void)cfg;
    info_read();
}

static void info_page_enter(void) {
    info_read();
}

/* The two framebuffer rows, formatted from the Framebuffer the page is drawn
 * on; draw() and the receipt share them so the width measured is the width
 * drawn.  The receipt passes worst-case numbers. */
static void fmt_fb_format(unsigned bpp, bool dbl, char *buf, size_t len) {
    snprintf(buf, len, "%u BPP, %s BUFFERED", bpp, dbl ? "DOUBLE" : "SINGLE");
}

static void fmt_fb_memory(unsigned stride, unsigned long size, char *buf, size_t len) {
    snprintf(buf, len, "%u B/LINE, %lu B", stride, size);
}

/* ── Layout ─────────────────────────────────────────────────────────────── */

#define INFO_ROW_H     24   /* a scale-2 label/value row */
#define INFO_HEADER_H  26   /* draw_section_header() and the space under it */
#define INFO_LINE_H    12   /* a scale-1 text line and its gap to the next */
#define INFO_GAP        6   /* between sections */
#define INFO_WIDE_LABEL "DEFAULT APP:"   /* the widest label: values align after it */
#define RESET_W       180   /* RESET DEFAULTS: 168 px of scale-2 label + padding */
#define RESET_H        40
#define RESET_GAP       4   /* under the button, before the key list */
#define RESET_TEXT_DY  13   /* scale-2 text (14 px) centred on the button's 40 */

static Button reset_btn;

enum {
    ROW_KERNEL, ROW_BUILT, ROW_HOSTNAME, ROW_DEFAULT_APP,
    ROW_CPU, ROW_BOGOMIPS, ROW_FB_FORMAT, ROW_FB_MEMORY,
    ROW_CONFIG_FILE, ROW_COUNT
};
static const char *const row_labels[ROW_COUNT] = {
    [ROW_KERNEL]      = "KERNEL:",
    [ROW_BUILT]       = "BUILT:",
    [ROW_HOSTNAME]    = "HOSTNAME:",
    [ROW_DEFAULT_APP] = "DEFAULT APP:",
    [ROW_CPU]         = "CPU:",
    [ROW_BOGOMIPS]    = "BOGOMIPS:",
    [ROW_FB_FORMAT]   = "FRAMEBUFFER:",
    [ROW_FB_MEMORY]   = "FB MEMORY:",
    [ROW_CONFIG_FILE] = "FILE:",
};

static bool stacked;              /* portrait: value on the line under its label */
static int  label_y[ROW_COUNT], value_y[ROW_COUNT], value_x;
static int  sec_sys_y, sec_hw_y, sec_cfg_y;
static int  keys_y, key_lines;    /* scale-1 config lines: first y, how many fit */

static const char *row_value(int row, const Framebuffer *fb, char *buf, size_t len) {
    switch (row) {
    case ROW_KERNEL:      return info.kernel;
    case ROW_BUILT:       return info.built;
    case ROW_HOSTNAME:    return info.hostname;
    case ROW_DEFAULT_APP: return info.default_app;
    case ROW_CPU:         return info.cpu;
    case ROW_BOGOMIPS:    return info.bogomips;
    case ROW_FB_FORMAT:
        if (!fb) return "";
        fmt_fb_format(fb->bytes_per_pixel * 8, fb->double_buffering, buf, len);
        return buf;
    case ROW_FB_MEMORY:
        if (!fb) return "";
        fmt_fb_memory(fb->line_length, (unsigned long)fb->screen_size, buf, len);
        return buf;
    case ROW_CONFIG_FILE: return CONFIG_FILE_PATH;
    default:              return "";
    }
}

/* The x a row's value is cut from.  fit_value() cuts at CONTENT_RIGHT, so the
 * FILE value beside the button in landscape is passed an x shifted right by
 * the button's width plus a 10 px gap: the room it gets then ends at the
 * button.  draw() and the receipt both use this, so what is measured is what
 * is drawn and the value cannot run under the button. */
static int fit_x(int row) {
    if (row == ROW_CONFIG_FILE && !stacked)
        return value_x + (CONTENT_RIGHT - (reset_btn.x - 10));
    return value_x;
}

static int place_row(int row, int y) {
    label_y[row] = y;
    if (stacked) { value_y[row] = y + INFO_ROW_H; return y + 2 * INFO_ROW_H; }
    value_y[row] = y;
    return y + INFO_ROW_H;
}

/* Re-run whenever the logical screen changes (rebuild_ui()).  Landscape puts
 * each value beside its label; portrait's narrower column puts it on the line
 * below, which its taller CONTENT_H pays for.  Whatever height is left under
 * the fixed rows goes to the config-key list. */
static void info_page_layout(void) {
    stacked = CONTENT_WIDTH < 600;
    value_x = stacked ? CONTENT_LEFT + 30
                      : CONTENT_LEFT + 10 + text_measure_width(INFO_WIDE_LABEL, 2) + 12;
    int y = CONTENT_Y + CONTENT_TOP_GAP;

    sec_sys_y = y;  y += INFO_HEADER_H;
    for (int r = ROW_KERNEL; r <= ROW_DEFAULT_APP; r++) y = place_row(r, y);
    y += INFO_GAP;

    sec_hw_y = y;   y += INFO_HEADER_H;
    for (int r = ROW_CPU; r <= ROW_FB_MEMORY; r++) y = place_row(r, y);
    y += INFO_GAP;

    /* RESET DEFAULTS goes beside the FILE row in landscape, which grows to the
     * button's height with its text centred on it.  Portrait's stacked value
     * reaches under where the button would be, so there it takes its own line
     * under the value, right-aligned the same. */
    sec_cfg_y = y;  y += INFO_HEADER_H;
    if (stacked)
        y = place_row(ROW_CONFIG_FILE, y);
    else
        label_y[ROW_CONFIG_FILE] = value_y[ROW_CONFIG_FILE] = y + RESET_TEXT_DY;
    button_init_full(&reset_btn, CONTENT_RIGHT - RESET_W, y, RESET_W, RESET_H,
                     "RESET DEFAULTS", BTN_COLOR_DANGER, COLOR_WHITE,
                     BTN_COLOR_HIGHLIGHT, 2);
    y += RESET_H + RESET_GAP;

    keys_y = y;
    key_lines = (CONTENT_Y + CONTENT_H - y) / INFO_LINE_H;
    if (key_lines > CONFIG_MAX_KEYS) key_lines = CONFIG_MAX_KEYS;
    if (key_lines < 1) key_lines = 1;
    y += key_lines * INFO_LINE_H;

    /* ⚠️ THE RECEIPT, in the settings stack's shape.  Everything here hangs off
     * CONTENT_Y, which comes from a per-unit touch inset, so a row pushed past
     * the content rect looks fine on one unit and clips on another.  The bottom
     * is the last config-key line the space allows.  Values are cut at
     * CONTENT_RIGHT by fit_value(), so the right edge cannot pass it; what the
     * receipt reports instead is how many of THIS unit's values were cut, the
     * framebuffer rows measured at their widest (32 BPP, 99999 B/LINE, 10 MB).
     * The FILE value is cut at the button instead (fit_x()), and the button's
     * own top and bottom are printed, since it is the one thing here a finger
     * has to reach. */
    {
        char s[160], cut[160];
        int bottom = y - CONTENT_Y;
        int right  = CONTENT_LEFT + 10 + text_measure_width(INFO_WIDE_LABEL, 2);
        int clipped = 0;
        for (int r = 0; r < ROW_COUNT; r++) {
            const char *v;
            if (r == ROW_FB_FORMAT)      { fmt_fb_format(32, false, s, sizeof(s)); v = s; }
            else if (r == ROW_FB_MEMORY) { fmt_fb_memory(99999, 9999999, s, sizeof(s)); v = s; }
            else                         v = row_value(r, NULL, s, sizeof(s));
            if (fit_value(v, fit_x(r), 2, cut, sizeof(cut))) clipped++;
            int w = value_x + text_measure_width(cut, 2);
            if (w > right) right = w;
        }
        for (int i = 0; i < info.key_count; i++) {
            if (fit_value(info.keys[i], CONTENT_LEFT + 10, 1, cut, sizeof(cut))) clipped++;
            int w = CONTENT_LEFT + 10 + text_measure_width(cut, 1);
            if (w > right) right = w;
        }
        const char *verdict = bottom > CONTENT_H     ? "⚠ PAST CONTENT BOTTOM"
                            : right  > CONTENT_RIGHT ? "⚠ PAST CONTENT RIGHT"
                            : "fits";
        printf("control_panel: information stack %s — bottom +%d of CONTENT_H %d, "
               "right %d of CONTENT_RIGHT %d, %d value(s) cut, %d key line(s) "
               "for %d key(s), reset button +%d..+%d (safe %dx%d, %s)\n",
               verdict, bottom, CONTENT_H, right, CONTENT_RIGHT, clipped,
               key_lines, info.key_count, reset_btn.y - CONTENT_Y,
               reset_btn.y + reset_btn.height - CONTENT_Y,
               SCREEN_SAFE_WIDTH, SCREEN_SAFE_HEIGHT,
               stacked ? "portrait" : "landscape");
    }
}

/* ── Draw and input ─────────────────────────────────────────────────────── */

static void info_page_draw(Framebuffer *fb) {
    char s[160], cut[160];

    draw_section_header(fb, sec_sys_y, "SYSTEM");
    draw_section_header(fb, sec_hw_y, "HARDWARE");
    draw_section_header(fb, sec_cfg_y, "CONFIG");
    for (int r = 0; r < ROW_COUNT; r++) {
        fb_draw_text(fb, CONTENT_LEFT + 10, label_y[r], row_labels[r], COLOR_LABEL, 2);
        fit_value(row_value(r, fb, s, sizeof(s)), fit_x(r), 2, cut, sizeof(cut));
        fb_draw_text(fb, value_x, value_y[r], cut, COLOR_WHITE, 2);
    }
    button_draw(fb, &reset_btn);

    int x = CONTENT_LEFT + 10;
    if (!info.config_found) {
        fb_draw_text(fb, x, keys_y, "NOT FOUND - USING DEFAULTS", RGB(200, 80, 80), 1);
        return;
    }
    if (info.key_count == 0) {
        fb_draw_text(fb, x, keys_y, "NO OTHER KEYS", COLOR_LABEL, 1);
        return;
    }
    /* All that fit; if some do not, the last line says how many are missing. */
    int shown = info.key_count <= key_lines ? info.key_count : key_lines - 1;
    for (int i = 0; i < shown; i++) {
        fit_value(info.keys[i], x, 1, cut, sizeof(cut));
        fb_draw_text(fb, x, keys_y + i * INFO_LINE_H, cut, COLOR_WHITE, 1);
    }
    if (shown < info.key_count) {
        snprintf(s, sizeof(s), "+%d MORE", info.key_count - shown);
        fb_draw_text(fb, x, keys_y + shown * INFO_LINE_H, s, COLOR_LABEL, 1);
    }
}

/* OK on the confirmation.  The config list is re-read after the reset, because
 * a page's reset_defaults() rewrites the file this page lists; the panel
 * repaints the page after an OK. */
static void info_reset_confirmed(Config *cfg) {
    char msg[160];
    int rc = cp_reset_all_defaults(cfg, msg, sizeof(msg));
    cp_status(msg, rc == 0);
    info_read();
}

/* RESET DEFAULTS is the only thing here that changes anything, and it asks
 * first.  The dialog appearing is the repaint; the button's own look is
 * button_take_dirty()'s. */
static CpPageResult info_page_input(Config *cfg, int tx, int ty,
                                    bool touching, uint32_t now) {
    (void)cfg;
    if (button_update(&reset_btn, tx, ty, touching, now))
        cp_confirm("RESET DEFAULTS?",
                   "BACKLIGHT, LED, AUDIO, ORIENTATION\n(TOUCH CALIBRATION IS KEPT)",
                   "OK", info_reset_confirmed);
    return CP_PAGE_IDLE;
}

/* Keyboard focus: the one button. */
static int info_page_focusables(UiRect *out, int max) {
    return focus_add_button(out, 0, max, &reset_btn);
}

const CpPage cp_info_page = {
    .name   = "Information",
    .icon   = "cp_info",
    .load   = info_page_load,
    .layout = info_page_layout,
    .enter  = info_page_enter,
    .draw   = info_page_draw,
    .input  = info_page_input,
    .focusables     = info_page_focusables,
};
