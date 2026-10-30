/*
 * clock_core.h — the clock app's pure logic: time/date text, the daily alarm, the countdown/pomodoro
 * timer, the shared ringing state, the night-dim level, and the settings file (key=value text).
 * No I/O, no clock reads, no framebuffer: every call is handed the local time and a monotonic
 * millisecond counter, so tests/clock_core_test.c drives it on the host.  The UI owns the tone, the
 * LED and the backlight write, and calls clock_tick() every frame.
 *
 * `ms` is a uint32 monotonic counter (it wraps after 49 days); every comparison here is a wrap-safe
 * difference, so pass the same counter on every call.
 */
#ifndef CLOCK_CORE_H
#define CLOCK_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Defaults, in one place so they are easy to change. */
#define CLOCK_DEF_ALARM_MIN   (7 * 60)      /* minute of the day */
#define CLOCK_DEF_DIM_START   (22 * 60)
#define CLOCK_DEF_DIM_END     (7 * 60)
#define CLOCK_DEF_DIM_LEVEL   10
#define CLOCK_DEF_SNOOZE_MIN  9
#define CLOCK_DEF_RING_S      60
#define CLOCK_DEF_POM_WORK    25            /* minutes */
#define CLOCK_DEF_POM_BREAK   5
#define CLOCK_DEF_POM_LONG    15
#define CLOCK_DEF_POM_EVERY   4             /* every Nth break is the long one */
#define CLOCK_PRESET_COUNT    4
extern const int clock_presets_min[CLOCK_PRESET_COUNT];   /* 1, 5, 10, 25 */

/* Local time as the UI reads it from struct tm (mon is 1-12 here, not 0-11). */
typedef struct {
    int year, mon, mday;    /* 2026, 1-12, 1-31 */
    int wday;               /* 0 = Sunday */
    int hour, min, sec;
} ClockTime;

/* ---- settings ---- */
typedef struct {
    bool alarm_on;
    int  alarm_min;         /* minute of the day, 0-1439 */
    bool dim_on;
    int  dim_start, dim_end;/* minute of the day; start > end wraps midnight; start == end = no window */
    int  dim_level;         /* 0-100 backlight percent while dimmed */
    int  snooze_min;        /* 1-60 */
    int  ring_s;            /* 10-600: a ring stops by itself after this long */
    int  pom_work, pom_break, pom_long;   /* minutes, 1-180 / 1-60 / 1-60 */
    int  pom_every;         /* 2-10 */
} ClockSettings;

void clock_settings_defaults(ClockSettings *s);
/* Lenient: unknown keys, comments, lines without '=' and bad values are skipped (the field keeps its
 * current value, so call defaults first); numbers are clamped to their range. */
void clock_settings_parse(ClockSettings *s, const char *text);
/* Writes the file text; returns its length, or -1 if `n` is too small. */
int  clock_settings_serialise(const ClockSettings *s, char *out, size_t n);

/* ---- text ---- */
/* Time of day, no padding on the hour in 12 h mode: "07:05" (24 h) / "7:05" (12 h).  Returns length or -1. */
int  clock_fmt_time(char *out, size_t n, const ClockTime *t, bool h24);
const char *clock_fmt_ampm(const ClockTime *t, bool h24);   /* "AM"/"PM", or "" in 24 h mode */
int  clock_fmt_seconds(char *out, size_t n, const ClockTime *t);   /* "07" */
int  clock_fmt_date(char *out, size_t n, const ClockTime *t);      /* "FRI 30 OCT 2026" (uppercase ASCII) */
int  clock_fmt_minute_of_day(char *out, size_t n, int minute, bool h24);   /* for the alarm/dim rows: "22:00" / "10:00 PM" */

/* ---- night dim ---- */
/* The backlight percent to hold now: `max_level` outside the window or when dimming is off, otherwise
 * min(dim_level, max_level).  The window is [start, end), wrapping midnight when start > end. */
int  clock_backlight(const ClockSettings *s, const ClockTime *t, int max_level);

/* ---- one state for alarm + timer + the ring they share ---- */
typedef enum { CLOCK_TIMER_OFF, CLOCK_TIMER_RUNNING, CLOCK_TIMER_PAUSED, CLOCK_TIMER_DONE } ClockTimerState;
typedef enum { CLOCK_PHASE_WORK, CLOCK_PHASE_BREAK, CLOCK_PHASE_LONG } ClockPhase;   /* pomodoro only */
typedef enum { CLOCK_RING_NONE, CLOCK_RING_ALARM, CLOCK_RING_TIMER } ClockRingSrc;

typedef struct {
    /* alarm */
    int64_t  alarm_last_key;        /* minute last seen; the alarm fires on entering its minute */
    bool     snoozing;
    uint32_t snooze_until;
    /* ring */
    ClockRingSrc ring;
    uint32_t ring_start;
    int      ring_limit_s;
    /* timer */
    ClockTimerState tstate;
    bool     pomodoro;
    ClockPhase phase;
    int      breaks_started;        /* pomodoro breaks begun so far */
    int      len_min[3];            /* work/break/long minutes captured at start */
    int      every;
    int64_t  remaining_ms;
    uint32_t last_ms;
} ClockState;

/* Events returned by clock_tick (bit mask). */
#define CLOCK_EV_RING_START 1       /* start the tone and the LED */
#define CLOCK_EV_RING_STOP  2       /* ring ended by itself (60 s) or because its source was turned off */
#define CLOCK_EV_PHASE      4       /* pomodoro moved to another phase */
#define CLOCK_EV_TIMER_DONE 8       /* countdown reached zero */

/* Start-up: an alarm whose minute is already running (or passed) does not fire. */
void clock_init(ClockState *c, const ClockTime *t, uint32_t ms);
/* Call every frame (or at least a few times a second). */
unsigned clock_tick(ClockState *c, const ClockSettings *s, const ClockTime *t, uint32_t ms);

/* ringing: dismiss stops it; snooze stops an alarm ring and rings again after snooze_min (a timer ring is just dismissed). */
bool clock_ringing(const ClockState *c);
void clock_dismiss(ClockState *c);
void clock_snooze(ClockState *c, const ClockSettings *s, uint32_t ms);
/* Pulse for the LED/tone while ringing: true for the first half of each second since the ring began. */
bool clock_ring_pulse(const ClockState *c, uint32_t ms);
bool clock_snooze_pending(const ClockState *c);

/* timer */
void clock_timer_start_countdown(ClockState *c, int minutes, uint32_t ms);          /* 1-999 */
void clock_timer_start_pomodoro(ClockState *c, const ClockSettings *s, uint32_t ms);
void clock_timer_pause(ClockState *c);              /* time since the last tick is not counted */
void clock_timer_resume(ClockState *c, uint32_t ms);
void clock_timer_cancel(ClockState *c);
long clock_timer_remaining_ms(const ClockState *c);
int  clock_timer_fmt(const ClockState *c, char *out, size_t n);   /* "MM:SS", "H:MM:SS" from one hour (rounds up: 00:01 until zero) */
const char *clock_timer_label(const ClockState *c);   /* "TIMER", "WORK", "BREAK", "LONG BREAK", or "" when off */

#endif
