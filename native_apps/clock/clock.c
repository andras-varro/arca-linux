/*
 * clock — the full-screen clock.  Time and date, a daily alarm, a countdown / pomodoro timer and
 * night dimming.  All decisions (alarm minute, timer, ring, dim window, text) are clock_core.c; this
 * file owns the screens, the tone, the red LED and the backlight write.
 *
 *   clock                  main screen; a tap, Enter, Space or Esc opens the menu
 *   clock --fb <dev> --touch <dev>   other devices (tests)
 *
 * The alarm and the timer run only while this app is in the foreground: the platform runs one app at
 * a time, so leaving the clock stops them.  The panel is kept awake the whole time and the backlight
 * follows clock_backlight(): the level the unit had at start is the maximum, and it is put back on
 * every way out of main().
 *
 * Settings: /home/root/data/clock.conf (clock_core's key=value text); the 12/24 h choice is the
 * app's own file, /home/root/data/clock_fmt.conf, because clock_core has no field for it.
 */

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "../common/framebuffer.h"
#include "../common/touch_input.h"
#include "../common/hardware.h"
#include "../common/common.h"
#include "../common/gamepad.h"
#include "../common/pointer.h"
#include "../common/start_menu.h"
#include "../common/audio.h"
#include "../common/config.h"
#include "clock_core.h"
#include "seg7.h"

#define SETTINGS_PATH "/home/root/data/clock.conf"
#define FMT_PATH      "/home/root/data/clock_fmt.conf"
#define TONE_HZ       880
#define TONE_MS       350

static volatile sig_atomic_t g_running = 1;
static void on_signal(int sig) { (void)sig; g_running = 0; }

typedef struct { Framebuffer fb; TouchInput touch; GamepadManager gp; Pointer ptr; } Dev;

static Dev    g_dev;
static Config g_fmt_cfg;
static const char *g_fb_dev = "/dev/fb0", *g_touch_dev = "/dev/input/touchscreen0";

static ClockSettings g_set;
static ClockState    g_state;
static bool          g_h24 = true;
static int           g_max_bl = 100;      /* the backlight the unit had at start: the "bright" level */
static int           g_last_bl = -1;
static int           g_led_saved = 0;

static Audio g_audio;
static bool  g_audio_open;
static bool  g_pulse_prev;

static InputState g_in;
static TouchState g_ts;

/* ── time, settings ─────────────────────────────────────────────────────── */

static void now_time(ClockTime *t)
{
    time_t tt = time(NULL);
    struct tm tm;
    memset(t, 0, sizeof *t);
    if (!localtime_r(&tt, &tm)) return;
    t->year = tm.tm_year + 1900;
    t->mon = tm.tm_mon + 1;
    t->mday = tm.tm_mday;
    t->wday = tm.tm_wday;
    t->hour = tm.tm_hour;
    t->min = tm.tm_min;
    t->sec = tm.tm_sec;
}

static void settings_load(void)
{
    char buf[1024];
    clock_settings_defaults(&g_set);
    FILE *f = fopen(SETTINGS_PATH, "r");
    if (!f) return;
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    clock_settings_parse(&g_set, buf);
}

static void settings_save(void)
{
    char buf[1024], tmp[] = SETTINGS_PATH ".tmp";
    int n = clock_settings_serialise(&g_set, buf, sizeof buf);
    if (n < 0) return;
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    bool ok = fwrite(buf, 1, (size_t)n, f) == (size_t)n;
    ok = (fclose(f) == 0) && ok;
    if (ok) rename(tmp, SETTINGS_PATH); else remove(tmp);
}

static void fmt_save(void)
{
    config_set_int(&g_fmt_cfg, "h24", g_h24 ? 1 : 0);
    config_save(&g_fmt_cfg);
}

/* ── ring and backlight service, called from every loop ─────────────────── */

static void ring_silence(void)
{
    hw_set_led(LED_RED, (uint8_t)g_led_saved);
    if (g_audio_open) { audio_close(&g_audio); g_audio_open = false; }
    g_pulse_prev = false;
}

static void service(uint32_t now)
{
    ClockTime t;
    now_time(&t);
    unsigned ev = clock_tick(&g_state, &g_set, &t, now);
    if (ev & CLOCK_EV_RING_START) {
        g_led_saved = hw_get_led(LED_RED);
        if (g_led_saved < 0) g_led_saved = 0;
        if (!g_audio_open && audio_init(&g_audio) == 0) g_audio_open = true;
        g_pulse_prev = false;
    }
    if (clock_ringing(&g_state)) {
        bool p = clock_ring_pulse(&g_state, now);
        hw_set_led(LED_RED, p ? 100 : 0);
        if (p && !g_pulse_prev && g_audio_open) audio_tone(&g_audio, TONE_HZ, TONE_MS);
        g_pulse_prev = p;
    } else if (ev & CLOCK_EV_RING_STOP) {
        ring_silence();
    }
    if (g_audio_open) audio_pump(&g_audio);

    int bl = clock_backlight(&g_set, &t, g_max_bl);
    if (bl != g_last_bl) {
        hw_set_backlight((uint8_t)bl);
        g_last_bl = bl;
    }
}

static void ring_end(void)
{
    ring_silence();
}

/* ── input ──────────────────────────────────────────────────────────────── */

static void poll_input(uint32_t now)
{
    touch_poll(&g_dev.touch);
    g_ts = touch_get_state(&g_dev.touch);
    gamepad_poll(&g_dev.gp, &g_in, 0, 0, false);
    gamepad_tick(&g_dev.gp, now);
    const ButtonState *bt = g_in.buttons;
    bool nav = bt[BTN_ID_UP].pressed   || bt[BTN_ID_DOWN].pressed  ||
               bt[BTN_ID_LEFT].pressed || bt[BTN_ID_RIGHT].pressed ||
               bt[BTN_ID_JUMP].pressed || bt[BTN_ID_ACTION].pressed ||
               bt[BTN_ID_BACK].pressed || bt[BTN_ID_PAUSE].pressed;
    pointer_update(&g_dev.ptr, &g_in, g_ts.pressed, nav, &g_ts);
}

static bool back_pressed(void)
{
    const ButtonState *bt = g_in.buttons;
    return bt[BTN_ID_BACK].pressed || bt[BTN_ID_PAUSE].pressed || bt[BTN_ID_CANCEL].pressed;
}

static bool any_pressed(void)
{
    for (int i = 0; i < BTN_ID_COUNT; i++) if (g_in.buttons[i].pressed) return true;
    return false;
}

static void present(void)
{
    fb_swap(&g_dev.fb);
    pointer_invalidate(&g_dev.ptr);
}

static bool in_rect(const UiRect *r, int x, int y)
{
    return x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h;
}

static void centered(const char *text, int y, int scale, uint32_t color)
{
    int w = text_measure_width(text, scale);
    fb_draw_text(&g_dev.fb, SCREEN_VISIBLE_LEFT + (SCREEN_VISIBLE_WIDTH - w) / 2, y, text, color, scale);
}

/* ── start-menu runner: leaves at once when a ring begins ───────────────── */

typedef void (*MenuHook)(StartMenu *m);

static int menu_run(StartMenu *m, MenuHook each)
{
    bool redraw = true;
    while (g_running) {
        uint32_t now = get_time_ms();
        poll_input(now);
        service(now);
        if (clock_ringing(&g_state)) return SM_EXIT;
        bool ping = false;
        int r = start_menu_step(m, &g_in, g_ts.x, g_ts.y, g_ts.held || g_ts.pressed,
                                gamepad_player_mask(&g_dev.gp), now, &ping);
        if (each) each(m);
        if (r != SM_NONE) return r;
        if (start_menu_needs_redraw(m) || g_ts.pressed || g_ts.released) redraw = true;
        if (redraw) {
            start_menu_draw(m, &g_dev.fb);
            present();
            redraw = false;
        }
        bool moved = pointer_paint(&g_dev.ptr, &g_dev.fb);
        usleep(moved ? 8000 : 16000);
    }
    return SM_EXIT;
}

/* ── alarm screen: on/off, hour, minute ─────────────────────────────────── */

enum { A_TOGGLE, A_HOUR, A_MIN, A_DONE, A_ROWS };

typedef struct { UiRect row[A_ROWS], minus[A_ROWS], plus[A_ROWS]; } AlarmLayout;

static void alarm_layout(AlarmLayout *L)
{
    int cx = SCREEN_VISIBLE_LEFT + SCREEN_VISIBLE_WIDTH / 2;
    int top = SCREEN_SAFE_TOP + 62, h = 56, gap = 14;
    memset(L, 0, sizeof *L);
    for (int i = 0; i < A_ROWS; i++) {
        int y = top + i * (h + gap);
        L->row[i] = (UiRect){ cx - 200, y, 400, h };
        L->minus[i] = (UiRect){ cx - 200, y, 96, h };
        L->plus[i]  = (UiRect){ cx + 104, y, 96, h };
    }
}

static void alarm_adjust(int row, int d)
{
    if (row == A_TOGGLE) g_set.alarm_on = !g_set.alarm_on;
    else if (row == A_HOUR) g_set.alarm_min = (g_set.alarm_min + d * 60 + 1440) % 1440;
    else if (row == A_MIN) {
        int h = g_set.alarm_min / 60, m = (g_set.alarm_min % 60 + d + 60) % 60;
        g_set.alarm_min = h * 60 + m;
    }
    settings_save();
}

static void alarm_draw(const AlarmLayout *L, int sel)
{
    Framebuffer *fb = &g_dev.fb;
    fb_clear(fb, COLOR_BLACK);
    centered("ALARM", SCREEN_VISIBLE_TOP + 10, 5, COLOR_WHITE);
    for (int i = 0; i < A_ROWS; i++) {
        const UiRect *r = &L->row[i];
        uint32_t edge = (i == sel) ? RGB(255, 200, 60) : RGB(110, 110, 110);
        fb_draw_rect(fb, r->x, r->y, r->w, r->h, edge);
        if (i == sel) fb_draw_rect(fb, r->x + 1, r->y + 1, r->w - 2, r->h - 2, edge);
        char t[40];
        if (i == A_TOGGLE) {
            snprintf(t, sizeof t, g_set.alarm_on ? "ALARM ON" : "ALARM OFF");
            uint32_t c = g_set.alarm_on ? RGB(110, 230, 120) : COLOR_GRAY;
            fb_draw_text(fb, r->x + (r->w - text_measure_width(t, 3)) / 2, r->y + 16, t, c, 3);
        } else if (i == A_DONE) {
            fb_draw_text(fb, r->x + (r->w - text_measure_width("DONE", 3)) / 2, r->y + 16, "DONE", COLOR_WHITE, 3);
        } else {
            int v = (i == A_HOUR) ? g_set.alarm_min / 60 : g_set.alarm_min % 60;
            snprintf(t, sizeof t, "%s %02d", i == A_HOUR ? "HOUR" : "MIN", v);
            fb_draw_text(fb, r->x + (r->w - text_measure_width(t, 3)) / 2, r->y + 16, t, COLOR_WHITE, 3);
            fb_draw_rect(fb, L->minus[i].x, L->minus[i].y, L->minus[i].w, L->minus[i].h, edge);
            fb_draw_rect(fb, L->plus[i].x, L->plus[i].y, L->plus[i].w, L->plus[i].h, edge);
            fb_draw_text(fb, L->minus[i].x + 36, r->y + 16, "-", COLOR_WHITE, 3);
            fb_draw_text(fb, L->plus[i].x + 36, r->y + 16, "+", COLOR_WHITE, 3);
        }
    }
    centered("ALARM ONLY WHILE CLOCK IS OPEN", SCREEN_SAFE_BOTTOM - 26, 2, COLOR_GRAY);
}

static void alarm_screen(void)
{
    AlarmLayout L;
    alarm_layout(&L);
    int sel = A_TOGGLE;
    int rep_row = -1, rep_dir = 0;
    uint32_t rep_next = 0;
    bool redraw = true;
    while (g_running) {
        uint32_t now = get_time_ms();
        poll_input(now);
        service(now);
        if (clock_ringing(&g_state)) return;
        const ButtonState *bt = g_in.buttons;
        if (back_pressed()) return;
        if (bt[BTN_ID_UP].pressed && sel > 0) { sel--; redraw = true; }
        if (bt[BTN_ID_DOWN].pressed && sel < A_ROWS - 1) { sel++; redraw = true; }
        if (bt[BTN_ID_LEFT].pressed) {
            if (sel == A_HOUR || sel == A_MIN || sel == A_TOGGLE) { alarm_adjust(sel, -1); redraw = true; }
        }
        if (bt[BTN_ID_RIGHT].pressed) {
            if (sel == A_HOUR || sel == A_MIN || sel == A_TOGGLE) { alarm_adjust(sel, 1); redraw = true; }
        }
        if (bt[BTN_ID_JUMP].pressed || bt[BTN_ID_ACTION].pressed) {
            if (sel == A_DONE) return;
            if (sel == A_TOGGLE) { alarm_adjust(A_TOGGLE, 0); redraw = true; }
        }
        if (g_ts.pressed) {
            for (int i = 0; i < A_ROWS; i++) {
                if (i == A_TOGGLE || i == A_DONE) {
                    if (in_rect(&L.row[i], g_ts.x, g_ts.y)) {
                        sel = i;
                        if (i == A_DONE) return;
                        alarm_adjust(A_TOGGLE, 0);
                        redraw = true;
                    }
                } else if (in_rect(&L.minus[i], g_ts.x, g_ts.y) || in_rect(&L.plus[i], g_ts.x, g_ts.y)) {
                    sel = i;
                    rep_row = i;
                    rep_dir = in_rect(&L.minus[i], g_ts.x, g_ts.y) ? -1 : 1;
                    rep_next = now + 450;
                    alarm_adjust(i, rep_dir);
                    redraw = true;
                }
            }
        }
        if (g_ts.released) rep_row = -1;
        if (rep_row >= 0 && g_ts.held && (int32_t)(now - rep_next) >= 0) {
            alarm_adjust(rep_row, rep_dir);
            rep_next = now + 90;
            redraw = true;
        }
        if (redraw) {
            alarm_draw(&L, sel);
            present();
            redraw = false;
        }
        bool moved = pointer_paint(&g_dev.ptr, &g_dev.fb);
        usleep(moved ? 8000 : 16000);
    }
}

/* ── timer menu ─────────────────────────────────────────────────────────── */

/* Returns true when a timer was started (so the caller goes back to the clock). */
static bool timer_menu(void)
{
    StartMenu m;
    bool idle = g_state.tstate == CLOCK_TIMER_OFF || g_state.tstate == CLOCK_TIMER_DONE;
    start_menu_init(&m, "TIMER", NULL, get_time_ms());
    int e[CLOCK_PRESET_COUNT], e_pom = -1, e_pause = -1, e_cancel = -1, e_back;
    if (idle) {
        for (int i = 0; i < CLOCK_PRESET_COUNT; i++) {
            char l[32];
            snprintf(l, sizeof l, "%d MIN", clock_presets_min[i]);
            e[i] = start_menu_add_action(&m, l);
        }
        e_pom = start_menu_add_action(&m, "POMODORO");
    } else {
        e_pause = start_menu_add_action(&m, g_state.tstate == CLOCK_TIMER_PAUSED ? "RESUME" : "PAUSE");
        e_cancel = start_menu_add_action(&m, "CANCEL TIMER");
    }
    e_back = start_menu_add_action(&m, "BACK");
    int r = menu_run(&m, NULL);
    uint32_t now = get_time_ms();
    if (r == SM_EXIT || r == e_back || r == SM_NONE) return false;
    if (idle) {
        if (r == e_pom) { clock_timer_start_pomodoro(&g_state, &g_set, now); return true; }
        for (int i = 0; i < CLOCK_PRESET_COUNT; i++)
            if (r == e[i]) { clock_timer_start_countdown(&g_state, clock_presets_min[i], now); return true; }
    } else if (r == e_pause) {
        if (g_state.tstate == CLOCK_TIMER_PAUSED) clock_timer_resume(&g_state, now); else clock_timer_pause(&g_state);
        return true;
    } else if (r == e_cancel) {
        clock_timer_cancel(&g_state);
        return true;
    }
    return false;
}

/* ── main menu ──────────────────────────────────────────────────────────── */

static const char *const DIM_NAMES[] = { "OFF", "ON" };
static const char *const FMT_NAMES[] = { "24 H", "12 H" };
static int g_dim_idx, g_fmt_idx;

static void menu_hook(StartMenu *m)
{
    bool dim = start_menu_value(m, g_dim_idx) == 1;
    bool h24 = start_menu_value(m, g_fmt_idx) == 0;
    if (dim != g_set.dim_on) { g_set.dim_on = dim; settings_save(); }
    if (h24 != g_h24) { g_h24 = h24; fmt_save(); }
}

/* Returns false when the app should exit. */
static bool main_menu(void)
{
    StartMenu m;
    char l[32];
    ClockTime t;
    now_time(&t);
    start_menu_init(&m, "CLOCK", NULL, get_time_ms());
    char am[16];
    clock_fmt_minute_of_day(am, sizeof am, g_set.alarm_min, g_h24);
    snprintf(l, sizeof l, "ALARM %s", g_set.alarm_on ? am : "OFF");
    int e_alarm = start_menu_add_action(&m, l);
    int e_timer = start_menu_add_action(&m, "TIMER");
    g_dim_idx = start_menu_add_choice(&m, "NIGHT DIM", DIM_NAMES, 2, g_set.dim_on ? 1 : 0);
    g_fmt_idx = start_menu_add_choice(&m, "TIME FORMAT", FMT_NAMES, 2, g_h24 ? 0 : 1);
    int e_back = start_menu_add_action(&m, "BACK TO CLOCK");
    int e_exit = start_menu_add_action(&m, "EXIT");
    start_menu_select(&m, e_back, get_time_ms());
    bool stay = true;
    while (g_running && stay) {
        int r = menu_run(&m, menu_hook);
        if (clock_ringing(&g_state)) return true;
        if (r == SM_EXIT || r == e_back) break;
        if (r == e_exit) return false;
        if (r == e_alarm) {
            alarm_screen();
            if (clock_ringing(&g_state)) return true;
            clock_fmt_minute_of_day(am, sizeof am, g_set.alarm_min, g_h24);
            snprintf(m.entries[e_alarm].label, sizeof m.entries[e_alarm].label, "ALARM %s", g_set.alarm_on ? am : "OFF");
        } else if (r == e_timer) {
            if (timer_menu()) break;
        }
        memset(&g_in, 0, sizeof g_in);
        pointer_drain(&g_dev.ptr);
        start_menu_reopen(&m, get_time_ms());
    }
    return true;
}

/* ── ringing screen ─────────────────────────────────────────────────────── */

static void ring_screen(void)
{
    Framebuffer *fb = &g_dev.fb;
    bool alarm = g_state.ring == CLOCK_RING_ALARM;
    int bw = 300, bh = 80;
    int by = SCREEN_SAFE_BOTTOM - bh - 20;
    UiRect snooze = { SCREEN_VISIBLE_LEFT + SCREEN_VISIBLE_WIDTH / 2 + 20, by, bw, bh };
    UiRect dismiss = alarm ? (UiRect){ SCREEN_VISIBLE_LEFT + SCREEN_VISIBLE_WIDTH / 2 - 20 - bw, by, bw, bh }
                           : (UiRect){ SCREEN_VISIBLE_LEFT + (SCREEN_VISIBLE_WIDTH - bw) / 2, by, bw, bh };
    int last_phase = -2;
    char last_clock[16] = "";
    while (g_running && clock_ringing(&g_state)) {
        uint32_t now = get_time_ms();
        poll_input(now);
        service(now);
        if (!clock_ringing(&g_state)) break;
        bool snz = alarm && (g_in.buttons[BTN_ID_LEFT].pressed ||
                             (g_ts.pressed && in_rect(&snooze, g_ts.x, g_ts.y)));
        if (snz) { clock_snooze(&g_state, &g_set, now); break; }
        if (any_pressed() || g_ts.pressed) { clock_dismiss(&g_state); break; }

        bool p = clock_ring_pulse(&g_state, now);
        ClockTime t;
        now_time(&t);
        char tm[16];
        clock_fmt_time(tm, sizeof tm, &t, g_h24);
        if ((int)p != last_phase || strcmp(tm, last_clock) != 0) {
            last_phase = p;
            snprintf(last_clock, sizeof last_clock, "%s", tm);
            fb_clear(fb, p ? RGB(150, 0, 0) : RGB(40, 0, 0));
            centered(alarm ? "ALARM" : (g_state.pomodoro ? clock_timer_label(&g_state) : "TIME IS UP"),
                     SCREEN_VISIBLE_TOP + 40, 9, COLOR_WHITE);
            centered(tm, SCREEN_VISIBLE_TOP + 170, 10, COLOR_WHITE);
            fb_draw_rect(fb, dismiss.x, dismiss.y, dismiss.w, dismiss.h, COLOR_WHITE);
            fb_draw_text(fb, dismiss.x + (dismiss.w - text_measure_width("DISMISS", 3)) / 2, dismiss.y + 28, "DISMISS", COLOR_WHITE, 3);
            if (alarm) {
                fb_draw_rect(fb, snooze.x, snooze.y, snooze.w, snooze.h, COLOR_WHITE);
                fb_draw_text(fb, snooze.x + (snooze.w - text_measure_width("SNOOZE", 3)) / 2, snooze.y + 28, "SNOOZE", COLOR_WHITE, 3);
                centered("ANY KEY: DISMISS   LEFT: SNOOZE", by - 30, 2, COLOR_WHITE);
            } else {
                centered("ANY KEY OR TAP: DISMISS", by - 30, 2, COLOR_WHITE);
            }
            present();
        }
        bool moved = pointer_paint(&g_dev.ptr, fb);
        usleep(moved ? 8000 : 16000);
    }
    if (!clock_ringing(&g_state)) ring_end();
    else ring_end();
    memset(&g_in, 0, sizeof g_in);
    pointer_drain(&g_dev.ptr);
}

/* ── main screen ────────────────────────────────────────────────────────── */

static Seg7Face g_big, g_small;   /* lit digits 0-9 and colon, rendered once at start-up */

static struct { int x, y, w, h; } g_sec;   /* where draw_main put the seconds; w == 0 until it has */
static int g_sec_space = 6;

/* A tick of the seconds alone: the back buffer still holds the rest of the screen, so only the
 * seconds box is cleared, redrawn and presented (a full redraw costs about 6 ms of CPU a second). */
static void draw_seconds(const char *ss)
{
    Framebuffer *fb = &g_dev.fb;
    fb_fill_rect(fb, g_sec.x, g_sec.y, g_sec.w, g_sec.h, COLOR_BLACK);
    seg7_draw_text(fb->back_buffer, (int)fb->width, (int)fb->height, &g_small, ss, g_sec.x, g_sec.y, g_sec_space);
}

static void draw_main(const ClockTime *t, const char *status)
{
    Framebuffer *fb = &g_dev.fb;
    char hm[16], ss[8], dt[32];
    clock_fmt_time(hm, sizeof hm, t, g_h24);
    clock_fmt_seconds(ss, sizeof ss, t);
    clock_fmt_date(dt, sizeof dt, t);
    const char *ap = clock_fmt_ampm(t, g_h24);
    int bsp = 12, ssp = g_sec_space;
    int wb = seg7_text_width(&g_big, hm, bsp), ws = seg7_text_width(&g_small, ss, ssp);
    int gap = 22;
    int total = wb + gap + ws;
    int x = SCREEN_VISIBLE_LEFT + (SCREEN_VISIBLE_WIDTH - total) / 2;
    int y = SCREEN_VISIBLE_TOP + 46;
    int bigh = g_big.h;
    fb_clear(fb, COLOR_BLACK);
    seg7_draw_text(fb->back_buffer, (int)fb->width, (int)fb->height, &g_big, hm, x, y, bsp);
    int sx = x + wb + gap, sy = y + bigh - g_small.h;
    seg7_draw_text(fb->back_buffer, (int)fb->width, (int)fb->height, &g_small, ss, sx, sy, ssp);
    g_sec.x = sx; g_sec.y = sy; g_sec.w = ws; g_sec.h = g_small.h;
    if (ap[0]) fb_draw_text(fb, sx, sy - text_measure_height(5) - 12, ap, RGB(255, 200, 60), 5);
    centered(dt, y + bigh + 30, 5, RGB(120, 200, 255));
    if (status[0]) centered(status, y + bigh + 30 + text_measure_height(5) + 24, 3, RGB(110, 230, 120));
    centered("TAP FOR MENU", SCREEN_VISIBLE_BOTTOM - 30, 2, RGB(90, 90, 90));
}

static void build_status(char *out, size_t n)
{
    char a[40] = "", tm[24] = "", tmr[48] = "";
    if (g_set.alarm_on) {
        char am[16];
        clock_fmt_minute_of_day(am, sizeof am, g_set.alarm_min, g_h24);
        snprintf(a, sizeof a, "ALARM %s%s", am, clock_snooze_pending(&g_state) ? " SNOOZED" : "");
    }
    if (g_state.tstate == CLOCK_TIMER_RUNNING || g_state.tstate == CLOCK_TIMER_PAUSED) {
        clock_timer_fmt(&g_state, tm, sizeof tm);
        snprintf(tmr, sizeof tmr, "%s %s%s", clock_timer_label(&g_state), tm,
                 g_state.tstate == CLOCK_TIMER_PAUSED ? " PAUSED" : "");
    }
    snprintf(out, n, "%s%s%s", a, (a[0] && tmr[0]) ? "   " : "", tmr);
}

int main(int argc, char *argv[])
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--fb") == 0 && i + 1 < argc) g_fb_dev = argv[++i];
        else if (strcmp(argv[i], "--touch") == 0 && i + 1 < argc) g_touch_dev = argv[++i];
    }
    int lock_fd = acquire_instance_lock("clock");
    if (lock_fd < 0) return 1;
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    hw_init();
    g_max_bl = hw_get_backlight();
    if (g_max_bl <= 0) g_max_bl = 100;
    memset(&g_dev, 0, sizeof g_dev);
    fb_set_bpp(g_fb_dev, 32);
    if (fb_init(&g_dev.fb, g_fb_dev) < 0) { fprintf(stderr, "clock: cannot open framebuffer %s\n", g_fb_dev); return 1; }
    if (g_dev.fb.bytes_per_pixel != 4 ||
        seg7_face_build(&g_big, 112, 216, 26, 3, COLOR_WHITE, COLOR_BLACK, 9) < 0 ||
        seg7_face_build(&g_small, 46, 90, 11, 2, RGB(170, 170, 170), COLOR_BLACK, 9) < 0) {
        fprintf(stderr, "clock: needs a 32bpp framebuffer and memory for the digit cache\n");
        return 1;
    }
    if (touch_init(&g_dev.touch, g_touch_dev) == 0) {
        touch_set_screen_size(&g_dev.touch, g_dev.fb.width, g_dev.fb.height);
        touch_drain_events(&g_dev.touch);
    }
    gamepad_init(&g_dev.gp);
    pointer_init(&g_dev.ptr);
    memset(&g_in, 0, sizeof g_in);

    config_init_path(&g_fmt_cfg, FMT_PATH);
    config_load(&g_fmt_cfg);
    g_h24 = config_get_int(&g_fmt_cfg, "h24", 1) != 0;
    settings_load();

    hw_blank_set_enabled(false);
    ClockTime t;
    now_time(&t);
    clock_init(&g_state, &t, get_time_ms());

    char drawn_hm[16] = "", drawn_ss[8] = "", drawn_status[96] = "";
    bool touch_down = false, force = true;
    while (g_running) {
        uint32_t now = get_time_ms();
        poll_input(now);
        service(now);
        if (clock_ringing(&g_state)) { ring_screen(); force = true; touch_down = false; continue; }

        bool open = false;
        if (g_ts.pressed) touch_down = true;
        if (g_ts.released && touch_down) { touch_down = false; open = true; }
        if (any_pressed()) open = true;
        if (open) {
            bool keep = main_menu();
            memset(&g_in, 0, sizeof g_in);
            pointer_drain(&g_dev.ptr);
            touch_down = false;
            force = true;
            if (!keep) break;
            continue;
        }

        now_time(&t);
        char hm[16], ss[8], status[96];
        clock_fmt_time(hm, sizeof hm, &t, g_h24);
        clock_fmt_seconds(ss, sizeof ss, &t);
        build_status(status, sizeof status);
        bool changed = force || strcmp(hm, drawn_hm) || strcmp(ss, drawn_ss) || strcmp(status, drawn_status);
        bool seconds_only = !force && g_sec.w > 0 && !strcmp(hm, drawn_hm) && !strcmp(status, drawn_status);
        if (changed && seconds_only) {
            draw_seconds(ss);
            fb_swap_rect(&g_dev.fb, g_sec.x, g_sec.y, g_sec.w, g_sec.h);
            pointer_invalidate(&g_dev.ptr);
            snprintf(drawn_ss, sizeof drawn_ss, "%s", ss);
        } else if (changed) {
            draw_main(&t, status);
            present();
            snprintf(drawn_hm, sizeof drawn_hm, "%s", hm);
            snprintf(drawn_ss, sizeof drawn_ss, "%s", ss);
            snprintf(drawn_status, sizeof drawn_status, "%s", status);
            force = false;
        }
        bool moved = pointer_paint(&g_dev.ptr, &g_dev.fb);
        usleep((changed || moved || (g_audio_open && audio_pump_active(&g_audio))) ? FRAME_DELAY_ACTIVE_US : FRAME_DELAY_IDLE_US);
    }

    ring_silence();
    hw_blank_set_enabled(true);
    hw_set_backlight((uint8_t)g_max_bl);
    fb_clear(&g_dev.fb, COLOR_BLACK);
    fb_swap(&g_dev.fb);
    gamepad_close(&g_dev.gp);
    touch_close(&g_dev.touch);
    fb_close(&g_dev.fb);
    return 0;
}
