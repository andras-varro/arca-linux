/* system_page.c — control_panel's System page: system settings that otherwise
 * need SSH.  The clock and its RTC, a manual date/time set and the timezone.
 * (SSH access has its own page, ssh_page.c.)
 *
 * Exposed only as cp_system_page (cp_page.h).  The clock is read once a second.
 * The editor holds year, month, day, hour and minute, each stepped by its own
 * -/+ buttons (sys_dt_step() keeps the day valid); SET asks first, then runs
 * `date -s "YYYY-MM-DD HH:MM:00"` and `hwclock -w -u` by fork/exec, the argument
 * built from validated integers; `date` reads /etc/localtime, so the entered value
 * is LOCAL time.  The zone row cycles a short curated list (sys_tz_*) and APPLY
 * (confirmed) repoints /etc/localtime and rewrites /etc/timezone, each by a
 * temp file and rename(), then tzset().
 * The TIME FORMAT button persists "clock_24h" (12 h / 24 h) for every clock that
 * shows the current time.
 * The boot-time network sync (rdate) overrides a manual set.
 */
#include "cp_page.h"
#include "cp_ui.h"
#include "cp_exec.h"
#include "sys_settings.h"
#include "../common/common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define RTC_SINCE_EPOCH  "/sys/class/rtc/rtc0/since_epoch"
#define CLOCK_REFRESH_MS 1000
#define RTC_SYNC_SLACK_S 2          /* the two clocks are read a moment apart */
#define SYS_NOTE         "NETWORK TIME SYNC OVERRIDES THIS AT THE NEXT ONLINE BOOT"

/* ── State ──────────────────────────────────────────────────────────────── */

static char    now_str[48];         /* "YYYY-MM-DD HH:MM:SS", or "UNKNOWN" */
static char    rtc_str[80];
static uint32_t rtc_color = COLOR_WHITE;
static uint32_t sampled_ms;

#define LOCALTIME_PATH   "/etc/localtime"
#define TIMEZONE_PATH    "/etc/timezone"
#define ZONEINFO_DIR     "/usr/share/zoneinfo/"

static char    zone_abbr[12];       /* "EDT", from strftime %Z */
static int     tz_cur = -1;         /* list index of the zone in force, -1 = not listed */
static int     tz_sel = -1;         /* index shown on the zone row */
static int     tz_pending = -1;     /* what the open confirmation will apply */

static SysDateTime edit;
static SysDateTime pending;         /* what the open confirmation will apply */

static void read_zone(void) {
    char target[160];
    ssize_t n = readlink(LOCALTIME_PATH, target, sizeof(target) - 1);
    target[n > 0 ? n : 0] = '\0';
    tz_cur = n > 0 ? sys_tz_from_link(target) : -1;
    tz_sel = tz_cur;
}


static bool clock_24h = true;       /* the persisted TIME FORMAT, "clock_24h" */

static void fmt_tm(const struct tm *tm, char *out, size_t len) {
    if (clock_24h) {
        snprintf(out, len, "%04d-%02d-%02d %02d:%02d:%02d", tm->tm_year + 1900,
                 tm->tm_mon + 1, tm->tm_mday, tm->tm_hour, tm->tm_min, tm->tm_sec);
        return;
    }
    snprintf(out, len, "%04d-%02d-%02d %d:%02d:%02d %s", tm->tm_year + 1900,
             tm->tm_mon + 1, tm->tm_mday, tm->tm_hour % 12 ? tm->tm_hour % 12 : 12,
             tm->tm_min, tm->tm_sec, tm->tm_hour < 12 ? "AM" : "PM");
}

/* The clock and the RTC; true when what is shown changed. */
static bool read_clock(void) {
    char old_now[sizeof(now_str)], old_rtc[sizeof(rtc_str)];
    memcpy(old_now, now_str, sizeof(now_str));
    memcpy(old_rtc, rtc_str, sizeof(rtc_str));

    time_t t = time(NULL);
    struct tm tm;
    if (localtime_r(&t, &tm)) {
        fmt_tm(&tm, now_str, sizeof(now_str));
        if (!strftime(zone_abbr, sizeof(zone_abbr), "%Z", &tm)) zone_abbr[0] = '\0';
        size_t l = strlen(now_str);
        snprintf(now_str + l, sizeof(now_str) - l, " %s", zone_abbr);
    } else {
        snprintf(now_str, sizeof(now_str), "UNKNOWN");
        zone_abbr[0] = '\0';
    }

    char line[32];
    long rtc_s = 0;
    if (read_file_line(RTC_SINCE_EPOCH, line, sizeof(line)) == 0 &&
        sscanf(line, "%ld", &rtc_s) == 1 && rtc_s > 0) {
        time_t rt = (time_t)rtc_s;
        long diff = (long)(rt - t);
        if (diff < 0) diff = -diff;
        struct tm rtm;
        char hms[32] = "";
        if (localtime_r(&rt, &rtm))
            snprintf(hms, sizeof(hms), "%02d:%02d:%02d", rtm.tm_hour, rtm.tm_min, rtm.tm_sec);
        if (diff <= RTC_SYNC_SLACK_S) {
            snprintf(rtc_str, sizeof(rtc_str), "%s  SYNCED", hms);
            rtc_color = COLOR_GREEN;
        } else {
            snprintf(rtc_str, sizeof(rtc_str), "%s  OFF %ld S", hms, diff);
            rtc_color = COLOR_YELLOW;
        }
    } else {
        snprintf(rtc_str, sizeof(rtc_str), "NOT READABLE");
        rtc_color = COLOR_YELLOW;
    }
    return memcmp(old_now, now_str, sizeof(now_str)) != 0 ||
           memcmp(old_rtc, rtc_str, sizeof(rtc_str)) != 0;
}

static void edit_from_clock(void) {
    time_t t = time(NULL);
    struct tm tm;
    if (!localtime_r(&t, &tm)) return;
    edit = (SysDateTime){ tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                          tm.tm_hour, tm.tm_min };
    if (edit.year < SYS_YEAR_MIN) edit.year = SYS_YEAR_MIN;
    if (edit.year > SYS_YEAR_MAX) edit.year = SYS_YEAR_MAX;
    if (edit.day > sys_days_in_month(edit.year, edit.mon))
        edit.day = sys_days_in_month(edit.year, edit.mon);
}

static Button fmt_btn;
static int    fmt_y;

static void fmt_label(void) {
    snprintf(fmt_btn.text, sizeof(fmt_btn.text), "TIME FORMAT: %s", clock_24h ? "24 H" : "12 H");
}

/* Same shape as blank_persist() in display_page.c: the key goes into the file by
 * re-reading it, then saved.  Followers read the file themselves. */
static void fmt_persist(Config *mem) {
    config_set_bool(mem, "clock_24h", clock_24h);
    Config disk;
    config_init(&disk);
    config_load(&disk);
    config_set_bool(&disk, "clock_24h", clock_24h);
    if (config_save(&disk) != 0) {
        fprintf(stderr, "control_panel: clock_24h save failed\n");
        cp_status("TIME FORMAT SAVE FAILED", false);
    }
}

static void system_page_load(const Config *cfg) {
    clock_24h = config_clock_24h(cfg);
    read_zone();
    read_clock();
    edit_from_clock();
}

static void system_page_enter(void) {
    read_zone();
    read_clock();
    edit_from_clock();
    sampled_ms = get_time_ms();
}

/* ── Layout ─────────────────────────────────────────────────────────────── */

#define SYS_HEADER_H   26
#define SYS_ROW_H      28           /* draw_info_row()'s pitch */
#define SYS_GAP         6
#define SYS_FIELD_GAP   6
#define SYS_STEP_H     36
#define SYS_COL_MAX   110
#define SYS_SET_H      40
#define SYS_LABEL_H    18
#define SYS_VALUE_H    32

static const char *const field_names[SYS_DT_FIELDS] =
    { "YEAR", "MON", "DAY", "HOUR", "MIN" };

#define SYS_TZ_H        34
#define SYS_VALUE_GAP    8           /* a section's widest label to its value column */
#define SYS_TZ_ARROW_W  38
#define SYS_TZ_APPLY_W  76           /* "APPLY" at scale 2 is 60 px; wider and Los_Angeles hits the arrows in portrait */

static Button minus_btn[SYS_DT_FIELDS], plus_btn[SYS_DT_FIELDS], set_btn;
static Button tz_prev_btn, tz_next_btn, tz_apply_btn;
static int  sec_clock_y, sec_set_y;
static int  utc_y, rtc_y, tz_y, label_y, value_y, col_w, value_scale, note_y;
static int  clock_vx;   /* value columns: each section's widest label + SYS_VALUE_GAP */

static void system_page_layout(void) {
    int y = CONTENT_Y + CONTENT_TOP_GAP;
    /* The values start just past the labels, not at
     * draw_info_row()'s 150/270 column: in portrait that column put the LOCAL
     * time against the right edge. */
    clock_vx = CONTENT_LEFT + 10 + text_measure_width("LOCAL:", 2) + SYS_VALUE_GAP;
    sec_clock_y = y;  y += SYS_HEADER_H;
    utc_y = y;        y += SYS_ROW_H;
    rtc_y = y;        y += SYS_ROW_H;
    tz_y = y;         y += SYS_TZ_H;
    button_init_full(&tz_prev_btn, CONTENT_LEFT, tz_y, SYS_TZ_ARROW_W, SYS_TZ_H - 2, "<",
                     RGB(80, 80, 80), COLOR_WHITE, BTN_COLOR_HIGHLIGHT, 2);
    button_init_full(&tz_apply_btn, CONTENT_RIGHT - SYS_TZ_APPLY_W, tz_y, SYS_TZ_APPLY_W,
                     SYS_TZ_H - 2, "APPLY", RGB(0, 110, 60), COLOR_WHITE,
                     BTN_COLOR_HIGHLIGHT, 2);
    button_init_full(&tz_next_btn, CONTENT_RIGHT - SYS_TZ_APPLY_W - 6 - SYS_TZ_ARROW_W, tz_y,
                     SYS_TZ_ARROW_W, SYS_TZ_H - 2, ">", RGB(80, 80, 80), COLOR_WHITE,
                     BTN_COLOR_HIGHLIGHT, 2);
    fmt_y = y;        y += SYS_TZ_H;
    button_init_full(&fmt_btn, CONTENT_LEFT, fmt_y, CONTENT_WIDTH, SYS_TZ_H - 2, "",
                     RGB(80, 80, 80), COLOR_WHITE, BTN_COLOR_HIGHLIGHT, 2);
    y += SYS_GAP;
    sec_set_y = y;    y += SYS_HEADER_H;

    col_w = (CONTENT_WIDTH - (SYS_DT_FIELDS - 1) * SYS_FIELD_GAP) / SYS_DT_FIELDS;
    if (col_w > SYS_COL_MAX) col_w = SYS_COL_MAX;
    value_scale = col_w >= 80 ? 3 : 2;

    label_y = y;                      y += SYS_LABEL_H;
    int plus_y = y;                   y += SYS_STEP_H + 4;
    value_y = y;                      y += SYS_VALUE_H;
    int minus_y = y;                  y += SYS_STEP_H;
    int fields_right = CONTENT_LEFT + SYS_DT_FIELDS * col_w + (SYS_DT_FIELDS - 1) * SYS_FIELD_GAP;
    for (int i = 0; i < SYS_DT_FIELDS; i++) {
        int x = CONTENT_LEFT + i * (col_w + SYS_FIELD_GAP);
        button_init_full(&plus_btn[i], x, plus_y, col_w, SYS_STEP_H, "+",
                         RGB(80, 80, 80), COLOR_WHITE, BTN_COLOR_HIGHLIGHT, 2);
        button_init_full(&minus_btn[i], x, minus_y, col_w, SYS_STEP_H, "-",
                         RGB(80, 80, 80), COLOR_WHITE, BTN_COLOR_HIGHLIGHT, 2);
    }

    /* SET sits beside the fields where the width allows, else under them. */
    int beside_x = fields_right + 16;
    if (CONTENT_RIGHT - beside_x >= 100) {
        button_init_full(&set_btn, beside_x, value_y - 6, CONTENT_RIGHT - beside_x,
                         SYS_SET_H, "SET", RGB(0, 110, 60), COLOR_WHITE,
                         BTN_COLOR_HIGHLIGHT, 2);
    } else {
        y += 8;
        button_init_full(&set_btn, CONTENT_LEFT, y, CONTENT_WIDTH, SYS_SET_H, "SET",
                         RGB(0, 110, 60), COLOR_WHITE, BTN_COLOR_HIGHLIGHT, 2);
        y += SYS_SET_H;
    }
    y += 8;
    note_y = y;
    y += 8;                            /* scale-1 text */

    /* ⚠️ THE RECEIPT, in the settings stack's shape.  Everything hangs off
     * CONTENT_Y, which comes from a per-unit touch inset, so a button pushed
     * past the touchable rect looks fine in a screenshot and is dead to a
     * finger.  The bottom is the note line (or the lowest button, whichever is
     * lower); the right edge is the widest of the SET button, the info rows at
     * their widest strings and the note, each measured as drawn. */
    {
        int bottom = y - CONTENT_Y;
        int low = (minus_btn[0].y + minus_btn[0].height) - CONTENT_Y;
        if (set_btn.y + set_btn.height - CONTENT_Y > low) low = set_btn.y + set_btn.height - CONTENT_Y;
        if (low > bottom) bottom = low;
        int right = set_btn.x + set_btn.width;
        const char *clash = NULL;          /* a text that would run under a button */
        int w = clock_vx + text_measure_width(clock_24h ? "2026-10-06 14:00:00 AEDT" : "2026-10-06 12:00:00 PM AEDT", 2);
        if (w > right) right = w;
        /* The zone name sits between the arrows; the longest listed name must fit. */
        w = text_measure_width("America/Los_Angeles", 2);
        if (w > tz_next_btn.x - (tz_prev_btn.x + tz_prev_btn.width) - 8) clash = "ZONE NAME";
        w = clock_vx + text_measure_width("14:00:00  OFF 99999 S", 2);
        if (w > right) right = w;
        w = CONTENT_LEFT + text_measure_width(SYS_NOTE, 1);
        if (w > right) right = w;
        const char *verdict = bottom > CONTENT_H     ? "⚠ PAST CONTENT BOTTOM"
                            : right  > CONTENT_RIGHT ? "⚠ PAST CONTENT RIGHT"
                            : clash                  ? "⚠ OVERLAPS A BUTTON"
                            : "fits";
        printf("control_panel: system stack %s — bottom +%d of CONTENT_H %d, "
               "right %d of CONTENT_RIGHT %d, SET %s%s%s (safe %dx%d, %s)\n",
               verdict, bottom, CONTENT_H, right, CONTENT_RIGHT,
               set_btn.x > fields_right ? "beside" : "under",
               clash ? ", clash " : "", clash ? clash : "",
               SCREEN_SAFE_WIDTH, SCREEN_SAFE_HEIGHT,
               CONTENT_WIDTH < 600 ? "portrait" : "landscape");
    }
}

/* ── Draw ───────────────────────────────────────────────────────────────── */

static void system_page_draw(Framebuffer *fb) {
    draw_section_header(fb, sec_clock_y, "CLOCK AND TIMEZONE");
    draw_info_row_at(fb, utc_y, "LOCAL:", now_str, COLOR_WHITE, clock_vx);
    draw_info_row_at(fb, rtc_y, "RTC:", rtc_str, rtc_color, clock_vx);
    {
        const char *zn = sys_tz_name(tz_sel);
        if (!zn) zn = "OTHER";
        int ax = tz_prev_btn.x + tz_prev_btn.width;
        int cx = (ax + tz_next_btn.x) / 2;
        text_draw_centered(fb, cx, tz_y + 9, zn, tz_sel == tz_cur ? COLOR_WHITE : COLOR_YELLOW, 2);
        button_draw(fb, &tz_prev_btn);
        button_draw(fb, &tz_next_btn);
        button_draw(fb, &tz_apply_btn);
    }

    fmt_label();
    button_draw(fb, &fmt_btn);

    draw_section_header(fb, sec_set_y, "SET DATE AND TIME");
    char s[8];
    const int vals[SYS_DT_FIELDS] = { edit.year, edit.mon, edit.day, edit.hour, edit.min };
    for (int i = 0; i < SYS_DT_FIELDS; i++) {
        int cx = plus_btn[i].x + col_w / 2;
        text_draw_centered(fb, cx, label_y + 7, field_names[i], COLOR_LABEL, 2);
        snprintf(s, sizeof(s), i == SYS_DT_YEAR ? "%04d" : "%02d", vals[i]);
        text_draw_centered(fb, cx, value_y + 12, s, COLOR_WHITE, value_scale);
        button_draw(fb, &plus_btn[i]);
        button_draw(fb, &minus_btn[i]);
    }
    button_draw(fb, &set_btn);
    fb_draw_text(fb, CONTENT_LEFT, note_y, SYS_NOTE, COLOR_LABEL, 1);
}

/* ── Setting the clock ──────────────────────────────────────────────────── */

/* Runs argv (no shell) with output discarded; true if it exited 0. */
static bool run_cmd(char *const argv[]) {
    return cp_exec(argv, NULL, NULL, 0, NULL, 0) == 0;
}

/* Repoint /etc/localtime and rewrite /etc/timezone, each through a temp name
 * and rename() so a reader never sees a half-written file; then tzset(). */
static void apply_zone(Config *cfg) {
    (void)cfg;
    const char *zn = sys_tz_name(tz_pending);
    if (!zn) { cp_status("ZONE INVALID, NOT SET", false); return; }

    char zpath[96];
    snprintf(zpath, sizeof(zpath), ZONEINFO_DIR "%s", zn);
    if (access(zpath, R_OK) != 0) { cp_status("ZONE FILE MISSING", false); return; }

    const char *ltmp = LOCALTIME_PATH ".new";
    unlink(ltmp);
    if (symlink(zpath, ltmp) != 0 || rename(ltmp, LOCALTIME_PATH) != 0) {
        unlink(ltmp);
        cp_status("ZONE SET FAILED", false);
        return;
    }
    const char *ttmp = TIMEZONE_PATH ".new";
    FILE *f = fopen(ttmp, "w");
    bool ok = f != NULL;
    if (f) {
        ok = fprintf(f, "%s\n", zn) > 0;
        ok = (fclose(f) == 0) && ok;
    }
    if (!ok || rename(ttmp, TIMEZONE_PATH) != 0) {
        unlink(ttmp);
        cp_status("ZONE SET, TIMEZONE FILE FAILED", false);
    } else {
        cp_status("TIMEZONE SET", true);
    }
    unsetenv("TZ");
    tzset();
    read_zone();
    read_clock();
    edit_from_clock();
}

static void apply_clock(Config *cfg) {
    (void)cfg;
    char stamp[24];
    sys_dt_format(&pending, stamp, sizeof(stamp));
    if (!stamp[0]) { cp_status("DATE INVALID, NOT SET", false); return; }
    char *const date_argv[] = { "date", "-s", stamp, NULL };
    char *const hwc_argv[]  = { "hwclock", "-w", "-u", NULL };   /* RTC holds UTC (rcS UTC=yes) */
    if (!run_cmd(date_argv))      { cp_status("DATE SET FAILED", false); return; }
    if (!run_cmd(hwc_argv))       { cp_status("CLOCK SET, RTC WRITE FAILED", false); }
    else                          cp_status("DATE AND RTC SET", true);
    read_clock();
}

/* ── Input ──────────────────────────────────────────────────────────────── */

static CpPageResult system_page_input(Config *cfg, int tx, int ty,
                                      bool touching, uint32_t now) {
    CpPageResult act = CP_PAGE_IDLE;

    for (int i = 0; i < SYS_DT_FIELDS; i++) {
        if (button_update(&plus_btn[i], tx, ty, touching, now)) {
            sys_dt_step(&edit, i, +1);
            act = CP_PAGE_REDRAW;
        }
        if (button_update(&minus_btn[i], tx, ty, touching, now)) {
            sys_dt_step(&edit, i, -1);
            act = CP_PAGE_REDRAW;
        }
    }
    if (button_update(&tz_prev_btn, tx, ty, touching, now)) {
        tz_sel = sys_tz_step(tz_sel, -1);
        act = CP_PAGE_REDRAW;
    }
    if (button_update(&tz_next_btn, tx, ty, touching, now)) {
        tz_sel = sys_tz_step(tz_sel, +1);
        act = CP_PAGE_REDRAW;
    }
    if (button_update(&fmt_btn, tx, ty, touching, now)) {
        clock_24h = !clock_24h;
        read_clock();
        fmt_persist(cfg);
        act = CP_PAGE_REDRAW;
    }
    if (button_update(&tz_apply_btn, tx, ty, touching, now)) {
        const char *zn = sys_tz_name(tz_sel);
        if (!zn || tz_sel == tz_cur) {
            cp_status(zn ? "ZONE UNCHANGED" : "PICK A ZONE WITH < >", false);
            return CP_PAGE_REDRAW;
        }
        char msg[80];
        tz_pending = tz_sel;
        snprintf(msg, sizeof(msg), "%s\nLOCAL TIME CHANGES, THE CLOCK DOES NOT", zn);
        cp_confirm("SET TIMEZONE", msg, "APPLY", apply_zone);
        return CP_PAGE_IDLE;
    }
    if (button_update(&set_btn, tx, ty, touching, now)) {
        char stamp[24], msg[80];
        pending = edit;
        sys_dt_format(&pending, stamp, sizeof(stamp));
        snprintf(msg, sizeof(msg), "%.19s %s\nTHEN WRITES THE RTC", stamp, zone_abbr);
        cp_confirm("SET DATE AND TIME", msg, "SET", apply_clock);
        return CP_PAGE_IDLE;
    }

    if (now - sampled_ms >= CLOCK_REFRESH_MS) {
        sampled_ms = now;
        if (read_clock()) act = CP_PAGE_REDRAW;
    }
    return act;
}

static int system_page_focusables(UiRect *out, int max) {
    int n = 0;
    n = focus_add_button(out, n, max, &tz_prev_btn);
    n = focus_add_button(out, n, max, &tz_next_btn);
    n = focus_add_button(out, n, max, &tz_apply_btn);
    n = focus_add_button(out, n, max, &fmt_btn);
    for (int i = 0; i < SYS_DT_FIELDS; i++) {
        n = focus_add_button(out, n, max, &plus_btn[i]);
        n = focus_add_button(out, n, max, &minus_btn[i]);
    }
    return focus_add_button(out, n, max, &set_btn);
}

const CpPage cp_system_page = {
    .name       = "System",
    .icon       = "cp_system",
    .load       = system_page_load,
    .layout     = system_page_layout,
    .enter      = system_page_enter,
    .draw       = system_page_draw,
    .input      = system_page_input,
    .focusables = system_page_focusables,
};
