/* clock_core.c — see clock_core.h. */
#include "clock_core.h"
#include "../common/hour12.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const int clock_presets_min[CLOCK_PRESET_COUNT] = { 1, 5, 10, 25 };

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

/* ---- settings ---- */

void clock_settings_defaults(ClockSettings *s)
{
    s->alarm_on = false;
    s->alarm_min = CLOCK_DEF_ALARM_MIN;
    s->dim_mode = CLOCK_DIM_OFF;
    s->dim_start = CLOCK_DEF_DIM_START;
    s->dim_end = CLOCK_DEF_DIM_END;
    s->dim_level = CLOCK_DEF_DIM_LEVEL;
    s->snooze_min = CLOCK_DEF_SNOOZE_MIN;
    s->ring_s = CLOCK_DEF_RING_S;
    s->pom_work = CLOCK_DEF_POM_WORK;
    s->pom_break = CLOCK_DEF_POM_BREAK;
    s->pom_long = CLOCK_DEF_POM_LONG;
    s->pom_every = CLOCK_DEF_POM_EVERY;
}

/* "HH:MM" exactly -> minute of the day, or -1. */
static int parse_hhmm(const char *v)
{
    if (!isdigit((unsigned char)v[0]) || !isdigit((unsigned char)v[1]) || v[2] != ':' ||
        !isdigit((unsigned char)v[3]) || !isdigit((unsigned char)v[4]) || v[5] != '\0') return -1;
    int h = (v[0] - '0') * 10 + (v[1] - '0'), m = (v[3] - '0') * 10 + (v[4] - '0');
    return (h < 24 && m < 60) ? h * 60 + m : -1;
}

/* Plain decimal integer (optional '-'), whole string; false otherwise. */
static bool parse_int(const char *v, int *out)
{
    char *end;
    if (!*v) return false;
    long x = strtol(v, &end, 10);
    if (*end != '\0' || x < -100000L || x > 100000L) return false;
    *out = (int)x;
    return true;
}

static void apply(ClockSettings *s, const char *key, const char *val)
{
    int v;
    if (!strcmp(key, "alarm_on")) { if (parse_int(val, &v)) s->alarm_on = v != 0; }
    else if (!strcmp(key, "alarm")) { v = parse_hhmm(val); if (v >= 0) s->alarm_min = v; }
    else if (!strcmp(key, "dim_mode")) { if (parse_int(val, &v)) s->dim_mode = clampi(v, CLOCK_DIM_OFF, CLOCK_DIM_SCHEDULE); }
    else if (!strcmp(key, "dim_on")) { if (parse_int(val, &v)) s->dim_mode = v != 0 ? CLOCK_DIM_SCHEDULE : CLOCK_DIM_OFF; }   /* older files */
    else if (!strcmp(key, "dim_start")) { v = parse_hhmm(val); if (v >= 0) s->dim_start = v; }
    else if (!strcmp(key, "dim_end")) { v = parse_hhmm(val); if (v >= 0) s->dim_end = v; }
    else if (!strcmp(key, "dim_level")) { if (parse_int(val, &v)) s->dim_level = clampi(v, 0, 100); }
    else if (!strcmp(key, "snooze_min")) { if (parse_int(val, &v)) s->snooze_min = clampi(v, 1, 60); }
    else if (!strcmp(key, "ring_s")) { if (parse_int(val, &v)) s->ring_s = clampi(v, 10, 600); }
    else if (!strcmp(key, "pom_work")) { if (parse_int(val, &v)) s->pom_work = clampi(v, 1, 180); }
    else if (!strcmp(key, "pom_break")) { if (parse_int(val, &v)) s->pom_break = clampi(v, 1, 60); }
    else if (!strcmp(key, "pom_long")) { if (parse_int(val, &v)) s->pom_long = clampi(v, 1, 60); }
    else if (!strcmp(key, "pom_every")) { if (parse_int(val, &v)) s->pom_every = clampi(v, 2, 10); }
}

void clock_settings_parse(ClockSettings *s, const char *text)
{
    if (!text) return;
    while (*text) {
        char line[96];
        size_t l = 0;
        while (*text && *text != '\n') { if (l < sizeof line - 1) line[l++] = *text; text++; }
        if (*text == '\n') text++;
        line[l] = '\0';
        while (l && (line[l - 1] == '\r' || line[l - 1] == ' ' || line[l - 1] == '\t')) line[--l] = '\0';
        char *k = line;
        while (*k == ' ' || *k == '\t') k++;
        if (*k == '#' || *k == '\0') continue;
        char *eq = strchr(k, '=');
        if (!eq) continue;
        *eq = '\0';
        size_t kl = strlen(k);
        while (kl && (k[kl - 1] == ' ' || k[kl - 1] == '\t')) k[--kl] = '\0';
        const char *val = eq + 1;
        while (*val == ' ' || *val == '\t') val++;
        apply(s, k, val);
    }
}

int clock_settings_serialise(const ClockSettings *s, char *out, size_t n)
{
    int w = snprintf(out, n,
        "alarm_on=%d\nalarm=%02d:%02d\ndim_mode=%d\ndim_start=%02d:%02d\ndim_end=%02d:%02d\ndim_level=%d\n"
        "snooze_min=%d\nring_s=%d\npom_work=%d\npom_break=%d\npom_long=%d\npom_every=%d\n",
        s->alarm_on ? 1 : 0, s->alarm_min / 60, s->alarm_min % 60,
        s->dim_mode, s->dim_start / 60, s->dim_start % 60, s->dim_end / 60, s->dim_end % 60,
        s->dim_level, s->snooze_min, s->ring_s, s->pom_work, s->pom_break, s->pom_long, s->pom_every);
    return (w < 0 || (size_t)w >= n) ? -1 : w;
}

/* ---- text ---- */



int clock_fmt_minute_of_day(char *out, size_t n, int minute, bool h24)
{
    minute = clampi(minute, 0, 1439);
    int h = minute / 60, m = minute % 60;
    int w = h24 ? snprintf(out, n, "%02d:%02d", h, m)
                : snprintf(out, n, "%d:%02d %s", hour12_of(h), m, hour12_ampm(h));
    return (w < 0 || (size_t)w >= n) ? -1 : w;
}

int clock_fmt_time(char *out, size_t n, const ClockTime *t, bool h24)
{
    int h = clampi(t->hour, 0, 23), m = clampi(t->min, 0, 59);
    int w = h24 ? snprintf(out, n, "%02d:%02d", h, m) : snprintf(out, n, "%d:%02d", hour12_of(h), m);
    return (w < 0 || (size_t)w >= n) ? -1 : w;
}

const char *clock_fmt_ampm(const ClockTime *t, bool h24)
{
    if (h24) return "";
    return hour12_ampm(t->hour);
}

int clock_fmt_seconds(char *out, size_t n, const ClockTime *t)
{
    int w = snprintf(out, n, "%02d", clampi(t->sec, 0, 59));
    return (w < 0 || (size_t)w >= n) ? -1 : w;
}

int clock_fmt_date(char *out, size_t n, const ClockTime *t)
{
    static const char *const wd[7] = { "SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT" };
    static const char *const mo[12] = { "JAN", "FEB", "MAR", "APR", "MAY", "JUN",
                                        "JUL", "AUG", "SEP", "OCT", "NOV", "DEC" };
    int w = snprintf(out, n, "%s %d %s %d", wd[clampi(t->wday, 0, 6)], clampi(t->mday, 1, 31),
                     mo[clampi(t->mon, 1, 12) - 1], t->year);
    return (w < 0 || (size_t)w >= n) ? -1 : w;
}

/* ---- dim ---- */

int clock_backlight(const ClockSettings *s, const ClockTime *t, int max_level)
{
    max_level = clampi(max_level, 0, 100);
    if (s->dim_mode == CLOCK_DIM_ON) return s->dim_level < max_level ? s->dim_level : max_level;
    if (s->dim_mode != CLOCK_DIM_SCHEDULE || s->dim_start == s->dim_end) return max_level;
    int m = clampi(t->hour, 0, 23) * 60 + clampi(t->min, 0, 59);
    bool in = s->dim_start < s->dim_end ? (m >= s->dim_start && m < s->dim_end)
                                        : (m >= s->dim_start || m < s->dim_end);
    if (!in) return max_level;
    return s->dim_level < max_level ? s->dim_level : max_level;
}

/* ---- state ---- */

static int64_t minute_key(const ClockTime *t)
{
    return (((int64_t)t->year * 12 + t->mon) * 32 + t->mday) * 1440 + t->hour * 60 + t->min;
}

void clock_init(ClockState *c, const ClockTime *t, uint32_t ms)
{
    memset(c, 0, sizeof *c);
    c->alarm_last_key = minute_key(t);
    c->last_ms = ms;
}

static void ring_start(ClockState *c, ClockRingSrc src, const ClockSettings *s, uint32_t ms)
{
    c->ring = src;
    c->ring_start = ms;
    c->ring_limit_s = s->ring_s;
}

static int64_t phase_len_ms(const ClockState *c, ClockPhase p) { return (int64_t)c->len_min[p] * 60000; }

unsigned clock_tick(ClockState *c, const ClockSettings *s, const ClockTime *t, uint32_t ms)
{
    unsigned ev = 0;

    if (c->tstate == CLOCK_TIMER_RUNNING) {
        c->remaining_ms -= (int64_t)(uint32_t)(ms - c->last_ms);
        c->last_ms = ms;
        if (c->remaining_ms <= 0) {
            if (!c->pomodoro) {
                c->remaining_ms = 0;
                c->tstate = CLOCK_TIMER_DONE;
                ev |= CLOCK_EV_TIMER_DONE;
            } else {
                while (c->remaining_ms <= 0) {    /* a late tick carries its overshoot into the next phase */
                    if (c->phase == CLOCK_PHASE_WORK) {
                        c->breaks_started++;
                        c->phase = (c->breaks_started % c->every == 0) ? CLOCK_PHASE_LONG : CLOCK_PHASE_BREAK;
                    } else {
                        c->phase = CLOCK_PHASE_WORK;
                    }
                    c->remaining_ms += phase_len_ms(c, c->phase);
                }
                ev |= CLOCK_EV_PHASE;
            }
            ring_start(c, CLOCK_RING_TIMER, s, ms);
            ev |= CLOCK_EV_RING_START;
        }
    } else {
        c->last_ms = ms;
    }

    /* The alarm fires on entering its minute: a start inside or after it, or a second tick in it, does not. */
    int64_t key = minute_key(t);
    if (!s->alarm_on) {
        c->snoozing = false;
        if (c->ring == CLOCK_RING_ALARM) { c->ring = CLOCK_RING_NONE; ev |= CLOCK_EV_RING_STOP; }
    } else {
        if (key != c->alarm_last_key && t->hour * 60 + t->min == s->alarm_min) {
            c->snoozing = false;
            ring_start(c, CLOCK_RING_ALARM, s, ms);
            ev |= CLOCK_EV_RING_START;
        }
        if (c->snoozing && (int32_t)(ms - c->snooze_until) >= 0) {
            c->snoozing = false;
            ring_start(c, CLOCK_RING_ALARM, s, ms);
            ev |= CLOCK_EV_RING_START;
        }
    }
    c->alarm_last_key = key;

    if (c->ring != CLOCK_RING_NONE && !(ev & CLOCK_EV_RING_START) &&
        (uint32_t)(ms - c->ring_start) >= (uint32_t)c->ring_limit_s * 1000u) {
        c->ring = CLOCK_RING_NONE;
        ev |= CLOCK_EV_RING_STOP;
    }
    return ev;
}

bool clock_ringing(const ClockState *c) { return c->ring != CLOCK_RING_NONE; }
bool clock_snooze_pending(const ClockState *c) { return c->snoozing; }

void clock_dismiss(ClockState *c)
{
    c->ring = CLOCK_RING_NONE;
    c->snoozing = false;
}

void clock_snooze(ClockState *c, const ClockSettings *s, uint32_t ms)
{
    if (c->ring == CLOCK_RING_ALARM) {
        c->snoozing = true;
        c->snooze_until = ms + (uint32_t)s->snooze_min * 60000u;
    }
    c->ring = CLOCK_RING_NONE;
}

bool clock_ring_pulse(const ClockState *c, uint32_t ms)
{
    if (c->ring == CLOCK_RING_NONE) return false;
    return ((uint32_t)(ms - c->ring_start) % 1000u) < 500u;
}

/* ---- timer ---- */

void clock_timer_start_countdown(ClockState *c, int minutes, uint32_t ms)
{
    c->pomodoro = false;
    c->tstate = CLOCK_TIMER_RUNNING;
    c->remaining_ms = (int64_t)clampi(minutes, 1, 999) * 60000;
    c->last_ms = ms;
}

void clock_timer_start_pomodoro(ClockState *c, const ClockSettings *s, uint32_t ms)
{
    c->pomodoro = true;
    c->tstate = CLOCK_TIMER_RUNNING;
    c->phase = CLOCK_PHASE_WORK;
    c->breaks_started = 0;
    c->len_min[CLOCK_PHASE_WORK] = clampi(s->pom_work, 1, 180);
    c->len_min[CLOCK_PHASE_BREAK] = clampi(s->pom_break, 1, 60);
    c->len_min[CLOCK_PHASE_LONG] = clampi(s->pom_long, 1, 60);
    c->every = clampi(s->pom_every, 2, 10);
    c->remaining_ms = phase_len_ms(c, CLOCK_PHASE_WORK);
    c->last_ms = ms;
}

void clock_timer_pause(ClockState *c)
{
    if (c->tstate == CLOCK_TIMER_RUNNING) c->tstate = CLOCK_TIMER_PAUSED;
}

void clock_timer_resume(ClockState *c, uint32_t ms)
{
    if (c->tstate != CLOCK_TIMER_PAUSED) return;
    c->tstate = CLOCK_TIMER_RUNNING;
    c->last_ms = ms;
}

void clock_timer_cancel(ClockState *c)
{
    c->tstate = CLOCK_TIMER_OFF;
    c->remaining_ms = 0;
    c->pomodoro = false;
}

long clock_timer_remaining_ms(const ClockState *c)
{
    return c->tstate == CLOCK_TIMER_OFF ? 0 : (long)c->remaining_ms;
}

int clock_timer_fmt(const ClockState *c, char *out, size_t n)
{
    long sec = (clock_timer_remaining_ms(c) + 999) / 1000;
    int w = sec >= 3600 ? snprintf(out, n, "%ld:%02ld:%02ld", sec / 3600, (sec / 60) % 60, sec % 60)
                        : snprintf(out, n, "%02ld:%02ld", sec / 60, sec % 60);
    return (w < 0 || (size_t)w >= n) ? -1 : w;
}

const char *clock_timer_label(const ClockState *c)
{
    if (c->tstate == CLOCK_TIMER_OFF) return "";
    if (!c->pomodoro) return "TIMER";
    return c->phase == CLOCK_PHASE_WORK ? "WORK" : c->phase == CLOCK_PHASE_BREAK ? "BREAK" : "LONG BREAK";
}
