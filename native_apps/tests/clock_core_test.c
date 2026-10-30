/*
 * clock_core_test.c — host regression for clock/clock_core.c: time/date text, the daily alarm (fires
 * once on entering its minute, snooze, 60 s stop, no fire when started inside or after it), the
 * countdown and pomodoro timer, the night-dim window and the settings file.
 *
 * Build: gcc -I clock tests/clock_core_test.c clock/clock_core.c -o clock_core_test
 */
#include <stdio.h>
#include <string.h>
#include "clock_core.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static ClockTime T(int day, int h, int m, int s)
{
    ClockTime t = { 2026, 10, day, (day + 3) % 7, h, m, s };   /* 30 Oct 2026 is a Friday */
    return t;
}

int main(void)
{
    char b[64];
    ClockSettings s;
    clock_settings_defaults(&s);

    /* ---- text ---- */
    ClockTime t = T(30, 7, 5, 9);
    CHECK(clock_fmt_time(b, sizeof b, &t, true) == 5 && !strcmp(b, "07:05"));
    CHECK(clock_fmt_time(b, sizeof b, &t, false) == 4 && !strcmp(b, "7:05"));
    CHECK(!strcmp(clock_fmt_ampm(&t, false), "AM") && !strcmp(clock_fmt_ampm(&t, true), ""));
    t = T(30, 0, 0, 0);
    clock_fmt_time(b, sizeof b, &t, false); CHECK(!strcmp(b, "12:00"));
    CHECK(!strcmp(clock_fmt_ampm(&t, false), "AM"));
    t = T(30, 12, 30, 0);
    clock_fmt_time(b, sizeof b, &t, false); CHECK(!strcmp(b, "12:30"));
    CHECK(!strcmp(clock_fmt_ampm(&t, false), "PM"));
    t = T(30, 23, 59, 7);
    clock_fmt_time(b, sizeof b, &t, false); CHECK(!strcmp(b, "11:59"));
    clock_fmt_seconds(b, sizeof b, &t); CHECK(!strcmp(b, "07"));
    clock_fmt_date(b, sizeof b, &t); CHECK(!strcmp(b, "FRI 30 OCT 2026"));
    t = T(30, 1, 1, 1); t.mday = 1; t.mon = 1; t.wday = 0;
    clock_fmt_date(b, sizeof b, &t); CHECK(!strcmp(b, "SUN 1 JAN 2026"));
    CHECK(clock_fmt_time(b, 4, &t, true) == -1);
    clock_fmt_minute_of_day(b, sizeof b, 22 * 60, true); CHECK(!strcmp(b, "22:00"));
    clock_fmt_minute_of_day(b, sizeof b, 22 * 60, false); CHECK(!strcmp(b, "10:00 PM"));

    /* ---- alarm ---- */
    ClockState c;
    s.alarm_on = true; s.alarm_min = 23 * 60 + 59;
    ClockTime t0 = T(30, 23, 58, 59);
    clock_init(&c, &t0, 1000);
    CHECK(clock_tick(&c, &s, &t0, 1000) == 0);
    ClockTime t1 = T(30, 23, 59, 0);
    CHECK(clock_tick(&c, &s, &t1, 2000) == CLOCK_EV_RING_START);
    CHECK(clock_ringing(&c));
    ClockTime t1b = T(30, 23, 59, 30);
    CHECK(clock_tick(&c, &s, &t1b, 32000) == 0 && clock_ringing(&c));
    clock_dismiss(&c);
    CHECK(!clock_ringing(&c));
    CHECK(clock_tick(&c, &s, &t1b, 33000) == 0 && !clock_ringing(&c));   /* same minute: no re-fire */
    ClockTime t1c = T(30, 23, 59, 59);
    CHECK(clock_tick(&c, &s, &t1c, 34000) == 0);

    /* 00:00 across midnight, and the next day's 23:59 fires again */
    s.alarm_min = 0;
    ClockTime m0 = T(30, 23, 59, 59), m1 = T(31, 0, 0, 0);
    clock_init(&c, &m0, 0);
    CHECK(clock_tick(&c, &s, &m0, 0) == 0);
    CHECK(clock_tick(&c, &s, &m1, 1000) == CLOCK_EV_RING_START);
    clock_dismiss(&c);
    ClockTime m2 = T(31, 0, 0, 40);
    CHECK(clock_tick(&c, &s, &m2, 41000) == 0);
    ClockTime d2a = T(31, 23, 59, 0), d2b = T(32 - 31 + 0, 0, 0, 0);
    d2b.mon = 11; d2b.mday = 1;
    clock_tick(&c, &s, &d2a, 80000000u);
    CHECK(clock_tick(&c, &s, &d2b, 80060000u) == CLOCK_EV_RING_START);   /* next midnight fires again */
    clock_dismiss(&c);

    /* the same minute on the next day fires again (day is part of the key) */
    s.alarm_min = 7 * 60;
    ClockTime a0 = T(30, 6, 59, 59), a1 = T(30, 7, 0, 0), a2 = T(31, 6, 59, 59), a3 = T(31, 7, 0, 0);
    clock_init(&c, &a0, 0);
    CHECK(clock_tick(&c, &s, &a1, 1000) == CLOCK_EV_RING_START);
    clock_dismiss(&c);
    clock_tick(&c, &s, &a2, 2000);
    CHECK(clock_tick(&c, &s, &a3, 3000) == CLOCK_EV_RING_START);

    /* started inside or after the alarm minute: nothing */
    clock_dismiss(&c);
    clock_init(&c, &a1, 0);                                  /* started at 07:00:00 */
    CHECK(clock_tick(&c, &s, &a1, 0) == 0 && !clock_ringing(&c));
    ClockTime a1b = T(30, 7, 0, 45);
    CHECK(clock_tick(&c, &s, &a1b, 45000) == 0);
    ClockTime late = T(30, 7, 30, 0);
    clock_init(&c, &late, 0);
    CHECK(clock_tick(&c, &s, &late, 0) == 0);
    ClockTime late2 = T(30, 8, 0, 0);
    CHECK(clock_tick(&c, &s, &late2, 1000) == 0 && !clock_ringing(&c));

    /* alarm off: never rings */
    s.alarm_on = false;
    clock_init(&c, &a0, 0);
    CHECK(clock_tick(&c, &s, &a1, 1000) == 0 && !clock_ringing(&c));
    s.alarm_on = true;

    /* 60 s auto-stop */
    clock_init(&c, &a0, 0);
    CHECK(clock_tick(&c, &s, &a1, 10000) == CLOCK_EV_RING_START);
    CHECK(clock_tick(&c, &s, &a1b, 69999) == 0 && clock_ringing(&c));
    CHECK(clock_tick(&c, &s, &a1b, 70000) == CLOCK_EV_RING_STOP);
    CHECK(!clock_ringing(&c) && !clock_snooze_pending(&c));
    CHECK(clock_tick(&c, &s, &a1b, 71000) == 0);

    /* snooze: rings again after 9 min, once, and stops by itself again */
    clock_init(&c, &a0, 0);
    clock_tick(&c, &s, &a1, 10000);
    clock_snooze(&c, &s, 20000);
    CHECK(!clock_ringing(&c) && clock_snooze_pending(&c));
    ClockTime sn = T(30, 7, 8, 0);
    CHECK(clock_tick(&c, &s, &sn, 20000 + 9 * 60000 - 1) == 0);
    ClockTime sn2 = T(30, 7, 9, 20);
    CHECK(clock_tick(&c, &s, &sn2, 20000 + 9 * 60000) == CLOCK_EV_RING_START);
    CHECK(clock_ringing(&c) && !clock_snooze_pending(&c));
    CHECK(clock_tick(&c, &s, &sn2, 20000 + 9 * 60000 + 60000) == CLOCK_EV_RING_STOP);
    CHECK(!clock_snooze_pending(&c));
    /* snooze twice, then dismiss during a snooze cancels it */
    clock_init(&c, &a0, 0);
    clock_tick(&c, &s, &a1, 1000);
    clock_snooze(&c, &s, 2000);
    clock_tick(&c, &s, &sn2, 2000 + 540000);
    clock_snooze(&c, &s, 2000 + 540000);
    CHECK(clock_snooze_pending(&c));
    clock_dismiss(&c);
    CHECK(!clock_snooze_pending(&c));
    CHECK(clock_tick(&c, &s, &sn2, 2000 + 1100000) == 0);
    /* turning the alarm off silences a ring and a snooze */
    clock_init(&c, &a0, 0);
    clock_tick(&c, &s, &a1, 1000);
    s.alarm_on = false;
    CHECK(clock_tick(&c, &s, &a1b, 2000) == CLOCK_EV_RING_STOP);
    s.alarm_on = true;
    /* the ms counter wrapping inside a snooze */
    clock_init(&c, &a0, 0xFFFFF000u);
    clock_tick(&c, &s, &a1, 0xFFFFF000u);
    clock_snooze(&c, &s, 0xFFFFF000u);
    CHECK(clock_tick(&c, &s, &sn, 0xFFFFF000u + 100000u) == 0 && clock_snooze_pending(&c));
    CHECK(clock_tick(&c, &s, &sn2, 0xFFFFF000u + 540000u) == CLOCK_EV_RING_START);
    /* the pulse */
    CHECK(clock_ring_pulse(&c, 0xFFFFF000u + 540000u + 100) && !clock_ring_pulse(&c, 0xFFFFF000u + 540000u + 600));
    clock_dismiss(&c);
    CHECK(!clock_ring_pulse(&c, 5));

    /* ---- countdown ---- */
    ClockTime now = T(30, 12, 0, 0);
    s.alarm_on = false;
    clock_init(&c, &now, 0);
    CHECK(clock_timer_remaining_ms(&c) == 0 && !strcmp(clock_timer_label(&c), ""));
    clock_timer_start_countdown(&c, 1, 1000);
    CHECK(clock_timer_remaining_ms(&c) == 60000 && !strcmp(clock_timer_label(&c), "TIMER"));
    clock_tick(&c, &s, &now, 11000);
    CHECK(clock_timer_remaining_ms(&c) == 50000);
    clock_timer_fmt(&c, b, sizeof b); CHECK(!strcmp(b, "00:50"));
    clock_timer_pause(&c);
    clock_tick(&c, &s, &now, 41000);
    CHECK(clock_timer_remaining_ms(&c) == 50000);           /* paused: nothing counted */
    clock_timer_resume(&c, 100000);
    clock_tick(&c, &s, &now, 110000);
    CHECK(clock_timer_remaining_ms(&c) == 40000);
    clock_timer_fmt(&c, b, sizeof b); CHECK(!strcmp(b, "00:40"));
    CHECK(clock_tick(&c, &s, &now, 149000) == 0);
    clock_timer_fmt(&c, b, sizeof b); CHECK(!strcmp(b, "00:01"));   /* rounds up, never 00:00 while running */
    CHECK(clock_tick(&c, &s, &now, 150000) == (CLOCK_EV_TIMER_DONE | CLOCK_EV_RING_START));
    CHECK(clock_ringing(&c) && clock_timer_remaining_ms(&c) == 0);
    clock_snooze(&c, &s, 150500);                             /* a timer ring cannot snooze */
    CHECK(!clock_ringing(&c) && !clock_snooze_pending(&c));
    CHECK(clock_tick(&c, &s, &now, 151000) == 0);             /* done: no second ring */
    clock_timer_cancel(&c);
    CHECK(clock_timer_remaining_ms(&c) == 0 && !strcmp(clock_timer_label(&c), ""));
    clock_timer_start_countdown(&c, 90, 0);
    clock_timer_fmt(&c, b, sizeof b); CHECK(!strcmp(b, "1:30:00"));
    clock_timer_cancel(&c);
    CHECK(clock_presets_min[0] == 1 && clock_presets_min[1] == 5 && clock_presets_min[2] == 10 &&
          clock_presets_min[3] == 25);
    /* ring from a timer stops by itself after the limit */
    clock_timer_start_countdown(&c, 1, 0);
    clock_tick(&c, &s, &now, 60000);
    CHECK(clock_ringing(&c));
    CHECK(clock_tick(&c, &s, &now, 120000) == CLOCK_EV_RING_STOP);
    /* pausing a stopped timer, resuming a non-paused one: no effect */
    clock_timer_cancel(&c);
    clock_timer_pause(&c); clock_timer_resume(&c, 5);
    CHECK(clock_timer_remaining_ms(&c) == 0);

    /* ---- pomodoro: W B W B W B W L W B ... ---- */
    clock_init(&c, &now, 0);
    clock_timer_start_pomodoro(&c, &s, 0);
    CHECK(!strcmp(clock_timer_label(&c), "WORK") && clock_timer_remaining_ms(&c) == 25 * 60000L);
    static const char *const want[9] = { "BREAK", "WORK", "BREAK", "WORK", "BREAK", "WORK", "LONG BREAK", "WORK", "BREAK" };
    static const long lens[9] = { 5, 25, 5, 25, 5, 25, 15, 25, 5 };
    uint32_t ms = 0;
    long cur = 25;
    for (int i = 0; i < 9; i++) {
        ms += (uint32_t)(cur * 60000);
        clock_dismiss(&c);
        unsigned ev = clock_tick(&c, &s, &now, ms);
        CHECK(ev == (CLOCK_EV_PHASE | CLOCK_EV_RING_START));
        CHECK(!strcmp(clock_timer_label(&c), want[i]));
        CHECK(clock_timer_remaining_ms(&c) == lens[i] * 60000L);
        cur = lens[i];
    }
    /* a late tick carries its overshoot; a pause inside a phase holds the phase */
    clock_timer_start_pomodoro(&c, &s, 0);
    clock_tick(&c, &s, &now, 25 * 60000 + 7000);
    CHECK(!strcmp(clock_timer_label(&c), "BREAK") && clock_timer_remaining_ms(&c) == 5 * 60000L - 7000);
    clock_timer_pause(&c);
    clock_dismiss(&c);
    CHECK(clock_tick(&c, &s, &now, 99 * 60000u) == 0 && !strcmp(clock_timer_label(&c), "BREAK"));
    /* pomodoro uses the settings captured at its start */
    s.pom_work = 50; s.pom_break = 10; s.pom_long = 30; s.pom_every = 2;
    clock_timer_start_pomodoro(&c, &s, 0);
    CHECK(clock_timer_remaining_ms(&c) == 50 * 60000L);
    clock_tick(&c, &s, &now, 50 * 60000u);
    clock_tick(&c, &s, &now, 60 * 60000u);
    clock_tick(&c, &s, &now, 110 * 60000u);
    CHECK(!strcmp(clock_timer_label(&c), "LONG BREAK"));     /* every 2nd break */
    clock_settings_defaults(&s);

    /* ---- dim ---- */
    s.dim_on = true;                                          /* 22:00-07:00, level 10 */
    ClockTime d;
    d = T(30, 21, 59, 0); CHECK(clock_backlight(&s, &d, 80) == 80);
    d = T(30, 22, 0, 0);  CHECK(clock_backlight(&s, &d, 80) == 10);
    d = T(30, 23, 59, 0); CHECK(clock_backlight(&s, &d, 80) == 10);
    d = T(31, 0, 0, 0);   CHECK(clock_backlight(&s, &d, 80) == 10);
    d = T(31, 6, 59, 59); CHECK(clock_backlight(&s, &d, 80) == 10);
    d = T(31, 7, 0, 0);   CHECK(clock_backlight(&s, &d, 80) == 80);
    d = T(31, 12, 0, 0);  CHECK(clock_backlight(&s, &d, 80) == 80);
    d = T(30, 23, 0, 0);  CHECK(clock_backlight(&s, &d, 5) == 5);     /* never brighter than the maximum */
    s.dim_on = false;
    d = T(30, 23, 0, 0);  CHECK(clock_backlight(&s, &d, 80) == 80);
    s.dim_on = true; s.dim_start = 13 * 60; s.dim_end = 15 * 60;       /* non-wrapping */
    d = T(30, 12, 59, 0); CHECK(clock_backlight(&s, &d, 70) == 70);
    d = T(30, 13, 0, 0);  CHECK(clock_backlight(&s, &d, 70) == 10);
    d = T(30, 14, 59, 0); CHECK(clock_backlight(&s, &d, 70) == 10);
    d = T(30, 15, 0, 0);  CHECK(clock_backlight(&s, &d, 70) == 70);
    d = T(30, 23, 0, 0);  CHECK(clock_backlight(&s, &d, 70) == 70);
    s.dim_start = s.dim_end = 6 * 60;                                  /* start == end: no window */
    d = T(30, 6, 0, 0);   CHECK(clock_backlight(&s, &d, 70) == 70);
    d = T(30, 18, 0, 0);  CHECK(clock_backlight(&s, &d, 70) == 70);
    s.dim_start = 22 * 60; s.dim_end = 7 * 60; s.dim_level = 0;
    d = T(30, 23, 0, 0);  CHECK(clock_backlight(&s, &d, 70) == 0);

    /* ---- settings ---- */
    clock_settings_defaults(&s);
    CHECK(!s.alarm_on && s.alarm_min == 420 && !s.dim_on && s.dim_start == 1320 && s.dim_end == 420 &&
          s.dim_level == 10 && s.snooze_min == 9 && s.ring_s == 60 && s.pom_work == 25 &&
          s.pom_break == 5 && s.pom_long == 15 && s.pom_every == 4);
    ClockSettings r;
    clock_settings_defaults(&r);
    clock_settings_parse(&r,
        "# comment\n"
        "alarm_on=1\r\n"
        "  alarm = 06:45 \n"
        "dim_on=1\n"
        "dim_start=23:30\n"
        "dim_end=05:15\n"
        "dim_level=250\n"                  /* clamped to 100 */
        "snooze_min=0\n"                   /* clamped to 1 */
        "ring_s=5000\n"                    /* clamped to 600 */
        "pom_work=abc\n"                   /* bad value: keeps default */
        "pom_break=7x\n"
        "pom_long=\n"
        "pom_every=3\n"
        "unknown=5\n"
        "no equals here\n"
        "=novalue\n"
        "alarm=25:00\n"                    /* out of range: keeps 06:45 */
        "dim_end=7:00\n"                   /* wrong shape: keeps 05:15 */
        "\n");
    CHECK(r.alarm_on && r.alarm_min == 6 * 60 + 45 && r.dim_on && r.dim_start == 23 * 60 + 30 &&
          r.dim_end == 5 * 60 + 15 && r.dim_level == 100 && r.snooze_min == 1 && r.ring_s == 600 &&
          r.pom_work == 25 && r.pom_break == 5 && r.pom_long == 15 && r.pom_every == 3);
    clock_settings_defaults(&r);
    clock_settings_parse(&r, NULL);
    clock_settings_parse(&r, "");
    clock_settings_parse(&r, "\x01\xff garbage\n=\n==\nalarm_on\n");
    CHECK(r.alarm_min == 420 && !r.alarm_on);
    /* an overlong line is cut, not overrun, and the line after it still parses */
    char longl[400];
    memset(longl, 'x', sizeof longl); longl[sizeof longl - 1] = '\0';
    char txt[600];
    snprintf(txt, sizeof txt, "%s\ndim_level=33\n", longl);
    clock_settings_parse(&r, txt);
    CHECK(r.dim_level == 33);

    /* round trip, including a non-default set */
    ClockSettings w = { true, 23 * 60 + 59, true, 0, 12 * 60 + 5, 77, 15, 120, 45, 2, 12, 5 };
    char file[512];
    int n = clock_settings_serialise(&w, file, sizeof file);
    CHECK(n > 0 && (size_t)n == strlen(file));
    clock_settings_defaults(&r);
    clock_settings_parse(&r, file);
    CHECK(r.alarm_on && r.alarm_min == 1439 && r.dim_on && r.dim_start == 0 && r.dim_end == 725 &&
          r.dim_level == 77 && r.snooze_min == 15 && r.ring_s == 120 && r.pom_work == 45 &&
          r.pom_break == 2 && r.pom_long == 12 && r.pom_every == 5);
    char again[512];
    clock_settings_serialise(&r, again, sizeof again);
    CHECK(!strcmp(file, again));
    clock_settings_defaults(&r);
    clock_settings_serialise(&r, file, sizeof file);
    ClockSettings r2;
    memset(&r2, 0xAA, sizeof r2);
    clock_settings_defaults(&r2);
    clock_settings_parse(&r2, file);
    clock_settings_serialise(&r2, again, sizeof again);
    CHECK(!strcmp(file, again));                            /* defaults survive a round trip too */
    CHECK(clock_settings_serialise(&w, file, 20) == -1);

    printf(fails ? "clock_core_test: %d FAILED\n" : "clock_core_test: all passed\n", fails);
    return fails ? 1 : 0;
}
