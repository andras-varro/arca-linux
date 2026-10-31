/*
 * start_menu_test.c — the arcade start menu's decisions, on the host.
 *
 * Drives start_menu_step(), which is start_menu_update() without its two
 * device calls: the ping comes back as a flag and the player mask goes in as
 * an int, so blink timing, pings, navigation, the players choice and touch
 * are all checked here with no framebuffer, no audio device and no pad.
 *
 * Every group asserts a CHANGE (a ping, a moved selection, a cycled value, an
 * activation), so a widget that never pings or never moves fails it; the
 * stubbed copy that proves this is measured by hand, not kept in the tree.
 *
 * Build (host gcc, from native_apps/):
 *   gcc -Wall -Wextra -Wno-unused-parameter -I common -o build/start_menu_test \
 *       tests/start_menu_test.c common/start_menu.c common/common.c \
 *       common/framebuffer.c common/font.c common/touch_input.c common/hardware.c \
 *       common/config.c common/highscore.c common/keyboard.c common/audio.c \
 *       common/audio_gen.c common/audio_out.c common/audio_wav.c \
 *       common/gamepad.c common/input_slots.c common/input_scan.c -lm && \
 *   ./build/start_menu_test
 */

#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include "../common/start_menu.h"

static int failures = 0;
static int checks   = 0;

static void check(bool cond, const char *what)
{
    checks++;
    if (!cond) { failures++; printf("  FAIL: %s\n", what); }
    else       { printf("  ok:   %s\n", what); }
}

/* ── Driving helpers ───────────────────────────────────────────────────── */

static uint32_t T0 = 100000;    /* not 0: the clock is the caller's, any origin */

/* One quiet frame: nothing held, no finger. */
static int quiet(StartMenu *m, int mask, uint32_t now, bool *ping)
{
    return start_menu_step(m, NULL, 0, 0, false, mask, now, ping);
}

/* A key press then its release, two frames at `now`.  Returns the press
 * frame's result. */
static int press(StartMenu *m, ButtonId b, int mask, uint32_t now)
{
    InputState in;
    bool ping;
    memset(&in, 0, sizeof(in));
    in.buttons[b].held = true;
    in.buttons[b].pressed = true;
    int r = start_menu_step(m, &in, 0, 0, false, mask, now, &ping);
    quiet(m, mask, now, &ping);
    return r;
}

/* A finger down at (x0,y0), up after last being seen at (x1,y1).  Returns the
 * release frame's result. */
static int touch(StartMenu *m, int x0, int y0, int x1, int y1, uint32_t now)
{
    bool ping;
    start_menu_step(m, NULL, x0, y0, true, 1, now, &ping);
    start_menu_step(m, NULL, x1, y1, true, 1, now, &ping);
    return start_menu_step(m, NULL, x1, y1, false, 1, now, &ping);
}

static int cx(const StartMenu *m, int i) { return m->rect[i].x + m->rect[i].w / 2; }
static int cy(const StartMenu *m, int i) { return m->rect[i].y + m->rect[i].h / 2; }

/* A fresh three-action menu, armed (one quiet frame clears the seeded edges). */
static void menu3(StartMenu *m)
{
    start_menu_init(m, "Test", "Line one\nLine two", T0);
    start_menu_add_action(m, "Start");
    start_menu_add_action(m, "Options");
    start_menu_add_action(m, "Exit");
    bool ping;
    quiet(m, 1, T0, &ping);
}

/* ── Groups ────────────────────────────────────────────────────────────── */

static void group_phase(void)
{
    printf("1. blink phase\n");
    static const struct { uint32_t ms; bool on; uint32_t idx; } t[] = {
        { 0, true, 0 }, { 499, true, 0 }, { 500, false, 0 }, { 999, false, 0 },
        { 1000, true, 1 }, { 1499, true, 1 }, { 1500, false, 1 }, { 6000, true, 6 },
    };
    for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
        bool on; uint32_t idx; char what[96];
        start_menu_phase(t[i].ms, &on, &idx);
        snprintf(what, sizeof(what), "%u ms -> %s, blink %u", t[i].ms,
                 t[i].on ? "on" : "off", t[i].idx);
        check(on == t[i].on && idx == t[i].idx, what);
    }
    /* The reciprocal quotient against real division, across the whole range. */
    int bad = 0;
    for (uint64_t e = 0; e <= 0xFFFFFFFFull; e += 9973) {
        bool on; uint32_t idx;
        start_menu_phase((uint32_t)e, &on, &idx);
        if (idx != (uint32_t)e / 1000u || on != ((uint32_t)e % 1000u < 500u)) bad++;
    }
    { bool on; uint32_t idx; start_menu_phase(0xFFFFFFFFu, &on, &idx);
      if (idx != 0xFFFFFFFFu / 1000u) bad++; }
    check(bad == 0, "phase quotient equals e/1000 over a full-range sweep");

    int badmod = 0;
    for (uint64_t i = 0; i <= 0xFFFFFFFFull; i += 7919) {
        uint32_t seen = SM_NO_BLINK;
        bool due = start_menu_ping_due(true, (uint32_t)i, 0, &seen);
        if (due != ((uint32_t)i % 6u < 3u)) badmod++;
    }
    check(badmod == 0, "ping cycle position equals idx%6 over a full-range sweep");
}

static void group_ping_cycle(void)
{
    printf("2. pings on blinks 0,1,2, silent 3,4,5, again from 6\n");
    StartMenu m;
    menu3(&m);
    int per_blink[9] = { 0 };
    /* 100 ms steps: the idle loop's rate.  The arming frame at T0 already
     * pinged blink 0, so start the count from scratch. */
    start_menu_select(&m, 0, T0);
    for (uint32_t t = 0; t < 9000; t += 100) {
        bool ping;
        quiet(&m, 1, T0 + t, &ping);
        if (ping) per_blink[t / 1000]++;
    }
    static const int want[9] = { 1, 1, 1, 0, 0, 0, 1, 1, 1 };
    for (int b = 0; b < 9; b++) {
        char what[64];
        snprintf(what, sizeof(what), "blink %d pings %d time(s)", b, want[b]);
        check(per_blink[b] == want[b], what);
    }
}

static void group_no_double(void)
{
    printf("3. one ping per on-phase, at most one per update, no catch-up\n");
    StartMenu m;
    bool p0, p1, p2;
    menu3(&m);
    start_menu_select(&m, 0, T0);
    quiet(&m, 1, T0, &p0);
    quiet(&m, 1, T0 + 100, &p1);
    quiet(&m, 1, T0 + 100, &p2);
    check(p0, "first update of blink 0 pings");
    check(!p1 && !p2, "later updates in the same on-phase do not");

    start_menu_select(&m, 0, T0);
    quiet(&m, 1, T0, &p0);
    quiet(&m, 1, T0 + 2400, &p1);      /* blink 1 skipped entirely */
    quiet(&m, 1, T0 + 2450, &p2);
    check(p0 && p1 && !p2, "a 2.4 s stall into blink 2 on-phase pings once, no catch-up for blink 1");

    start_menu_select(&m, 0, T0);
    quiet(&m, 1, T0, &p0);
    quiet(&m, 1, T0 + 1600, &p1);   /* blink 1 off-phase: a pinging blink, unseen lit */
    check(!p1, "a stall landing in an off-phase does not ping");

    m.dirty = false;
    start_menu_select(&m, 0, T0);
    m.dirty = false;
    quiet(&m, 1, T0 + 200, &p0);
    check(!start_menu_needs_redraw(&m), "no redraw inside one phase");
    quiet(&m, 1, T0 + 500, &p0);
    check(start_menu_needs_redraw(&m), "redraw on the bracket off-edge");
}

static void group_wrap(void)
{
    printf("4. selection moves and wraps both ways, skipping disabled entries\n");
    StartMenu m;
    menu3(&m);
    press(&m, BTN_ID_UP, 1, T0 + 10);
    check(m.sel == 2, "UP from the first entry wraps to the last");
    press(&m, BTN_ID_DOWN, 1, T0 + 20);
    check(m.sel == 0, "DOWN from the last wraps to the first");
    press(&m, BTN_ID_DOWN, 1, T0 + 30);
    check(m.sel == 1, "DOWN moves one");
    start_menu_set_disabled(&m, 2, true);
    press(&m, BTN_ID_DOWN, 1, T0 + 40);
    check(m.sel == 0, "DOWN skips a disabled entry");

    bool ping;
    StartMenu n;
    menu3(&n);
    start_menu_select(&n, 0, T0);
    quiet(&n, 1, T0 + 700, &ping);       /* off-phase, blink 0 seen */
    press(&n, BTN_ID_DOWN, 1, T0 + 800);
    check(n.marker_on && n.t0 == T0 + 800, "a move restarts the blink lit");
    check(n.seen_on_idx == 0, "... and its blink 0 was seen (pinged) at once");
}

static void group_choice(void)
{
    printf("5. CHOICE cycles with LEFT/RIGHT and taps, wrapping\n");
    static const char *const v[] = { "Easy", "Normal", "Hard" };
    StartMenu m;
    bool ping;
    start_menu_init(&m, "Test", NULL, T0);
    int c = start_menu_add_choice(&m, "Difficulty", v, 3, 1);
    int s = start_menu_add_action(&m, "Start");
    quiet(&m, 1, T0, &ping);
    check(start_menu_value(&m, c) == 1, "initial value kept");
    press(&m, BTN_ID_RIGHT, 1, T0 + 10);
    check(start_menu_value(&m, c) == 2, "RIGHT steps forward");
    press(&m, BTN_ID_RIGHT, 1, T0 + 20);
    check(start_menu_value(&m, c) == 0, "RIGHT wraps last -> first");
    press(&m, BTN_ID_LEFT, 1, T0 + 30);
    check(start_menu_value(&m, c) == 2, "LEFT wraps first -> last");
    check(press(&m, BTN_ID_JUMP, 1, T0 + 40) == SM_NONE && start_menu_value(&m, c) == 0,
          "JUMP on a CHOICE cycles it and activates nothing");
    press(&m, BTN_ID_DOWN, 1, T0 + 50);
    check(press(&m, BTN_ID_JUMP, 1, T0 + 60) == s, "JUMP on START returns its index");
    check(press(&m, BTN_ID_BACK, 1, T0 + 70) == SM_EXIT, "BACK returns SM_EXIT");
    /* Escape is PAUSE: the launcher's rule, back — never "activate START" */
    check(press(&m, BTN_ID_PAUSE, 1, T0 + 75) == SM_EXIT, "PAUSE (Escape / Start) returns SM_EXIT");
    check(touch(&m, cx(&m, c), cy(&m, c), cx(&m, c), cy(&m, c), T0 + 80) == SM_NONE
          && start_menu_value(&m, c) == 1, "a tap on a CHOICE cycles it");
}

static void group_players(void)
{
    printf("6. 2 PLAYERS needs P1 and P2\n");
    StartMenu m;
    bool ping;
    start_menu_init(&m, "Test", NULL, T0);
    int p = start_menu_players(&m);
    start_menu_add_action(&m, "Start");
    quiet(&m, 1, T0, &ping);
    press(&m, BTN_ID_RIGHT, 1, T0 + 10);
    check(start_menu_value(&m, p) == 0, "mask P1 only: RIGHT cannot reach 2 PLAYERS");
    check(strcmp(m.warning, SM_WARN_NEED_P2) == 0, "... and says why");
    press(&m, BTN_ID_RIGHT, 2, T0 + 20);
    check(start_menu_value(&m, p) == 0, "mask P2 only: still not");
    press(&m, BTN_ID_RIGHT, 3, T0 + 30);
    check(start_menu_value(&m, p) == 1 && start_menu_player_count(&m) == 2,
          "mask P1|P2: RIGHT selects 2 PLAYERS");
    check(m.warning[0] == '\0', "the players warning clears once P2 is there");
    quiet(&m, 1, T0 + 40, &ping);
    check(start_menu_value(&m, p) == 0 && start_menu_player_count(&m) == 1,
          "P2 leaves: drops back to 1 PLAYER");
    check(strcmp(m.warning, SM_WARN_NEED_P2) == 0, "... with the warning");
    quiet(&m, 7, T0 + 50, &ping);
    check(m.warning[0] == '\0' && start_menu_value(&m, p) == 0,
          "P2 back: warning clears, choice stays 1 until chosen");
}

static void group_touch(void)
{
    printf("7. touch: press and release on the SAME entry\n");
    StartMenu m;
    menu3(&m);
    check(touch(&m, cx(&m, 0), cy(&m, 0), cx(&m, 1), cy(&m, 1), T0 + 10) == SM_NONE,
          "press on START, release over OPTIONS: nothing activates");
    check(m.sel == 0, "... and the selection did not move");
    check(touch(&m, cx(&m, 1), cy(&m, 1), cx(&m, 1), cy(&m, 1), T0 + 20) == 1,
          "tap on OPTIONS activates OPTIONS");
    check(m.sel == 1 && !m.shown, "... selects it and hides the brackets");

    bool ping;
    quiet(&m, 1, T0 + 5000, &ping);
    check(!ping, "hidden brackets do not ping");
    press(&m, BTN_ID_DOWN, 1, T0 + 5100);
    check(m.shown && m.sel == 1, "first key after a touch reveals without moving");
    start_menu_set_disabled(&m, 2, true);
    check(touch(&m, cx(&m, 2), cy(&m, 2), cx(&m, 2), cy(&m, 2), T0 + 5200) == SM_NONE,
          "a disabled entry refuses a tap");
}

static void group_held(void)
{
    printf("8. a button or finger already down at open never activates\n");
    StartMenu m;
    InputState in;
    bool ping;
    start_menu_init(&m, "Test", NULL, T0);
    int s = start_menu_add_action(&m, "Start");
    memset(&in, 0, sizeof(in));
    in.buttons[BTN_ID_JUMP].held = true;
    in.buttons[BTN_ID_JUMP].pressed = true;   /* the very press that opened it */
    check(start_menu_step(&m, &in, 0, 0, false, 1, T0, &ping) == SM_NONE,
          "JUMP edge on the opening frame: nothing");
    in.buttons[BTN_ID_JUMP].pressed = false;
    check(start_menu_step(&m, &in, 0, 0, false, 1, T0 + 33, &ping) == SM_NONE,
          "JUMP still held: nothing");
    quiet(&m, 1, T0 + 66, &ping);
    check(press(&m, BTN_ID_JUMP, 1, T0 + 99) == s, "released and pressed again: START");

    StartMenu n;
    start_menu_init(&n, "Test", NULL, T0);
    start_menu_add_action(&n, "Start");
    check(start_menu_step(&n, NULL, 400, 240, true, 1, T0, &ping) == SM_NONE &&
          start_menu_step(&n, NULL, cx(&n, 0), cy(&n, 0), false, 1, T0 + 33, &ping) == SM_NONE,
          "a finger down at open, lifted over START: nothing");
}

/* Quiet frames every 100 ms over [from, to) after T0; returns the pings. */
static int idle_run(StartMenu *m, uint32_t from, uint32_t to)
{
    int pings = 0;
    for (uint32_t t = from; t < to; t += 100) {
        bool ping;
        quiet(m, 1, T0 + t, &ping);
        if (ping) pings++;
    }
    return pings;
}

static void group_idle_cutoff(void)
{
    printf("9. pings stop after 60 s without input, and come back with input\n");
    uint32_t seen = SM_NO_BLINK;
    check(start_menu_ping_due(true, 0, 59000, &seen), "59 s idle: a lit blink 0 pings");
    seen = SM_NO_BLINK;
    check(!start_menu_ping_due(true, 1, 61000, &seen) && seen == SM_NO_BLINK,
          "61 s idle: no ping, latch untouched");
    check(start_menu_ping_due(true, 1, 0, &seen), "input again: the same blink pings");

    StartMenu m;
    menu3(&m);                           /* no demo: MENU again at 30-45 s, 60-75 s */
    int early = idle_run(&m, 0, 30000);
    int mid   = idle_run(&m, 30000, 45000);
    idle_run(&m, 45000, 60000);
    int late  = idle_run(&m, 60000, 65000);
    check(early > 0 && mid > 0, "the menu pings while idle under 60 s");
    check(start_menu_attract(&m) == SM_ATTRACT_MENU && late == 0,
          "menu on screen at 60-65 s idle: silent");
    InputState in;
    bool ping;
    memset(&in, 0, sizeof(in));
    in.buttons[BTN_ID_DOWN].held = in.buttons[BTN_ID_DOWN].pressed = true;
    start_menu_step(&m, &in, 0, 0, false, 1, T0 + 65000, &ping);
    check(ping && m.sel == 1, "a nav input at 65 s moves and pings again");
}

static void group_attract(void)
{
    printf("10. attract cycle MENU -> DEMO -> SCORES -> MENU, input swallowed\n");
    static const struct { uint32_t ms; bool demo; SmAttract want; } t[] = {
        { 0, true, SM_ATTRACT_MENU },      { 14999, true, SM_ATTRACT_MENU },
        { 15000, true, SM_ATTRACT_DEMO },  { 29999, true, SM_ATTRACT_DEMO },
        { 30000, true, SM_ATTRACT_SCORES },{ 45000, true, SM_ATTRACT_MENU },
        { 60000, true, SM_ATTRACT_DEMO },  { 105000, true, SM_ATTRACT_DEMO },
        { 14999, false, SM_ATTRACT_MENU }, { 15000, false, SM_ATTRACT_SCORES },
        { 30000, false, SM_ATTRACT_MENU }, { 45000, false, SM_ATTRACT_SCORES },
        { 60000, false, SM_ATTRACT_MENU },
    };
    for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
        char what[80];
        snprintf(what, sizeof(what), "%u ms idle, %s: phase %d", t[i].ms,
                 t[i].demo ? "demo" : "no demo", (int)t[i].want);
        check(start_menu_attract_at(t[i].ms, t[i].demo) == t[i].want, what);
    }
    int bad = 0;
    for (uint64_t e = 0; e <= 0xFFFFFFFFull; e += 4999) {
        uint32_t n = (uint32_t)e / 15000u;
        SmAttract wd = n == 0 ? SM_ATTRACT_MENU
                     : (n - 1) % 3 == 0 ? SM_ATTRACT_DEMO
                     : (n - 1) % 3 == 1 ? SM_ATTRACT_SCORES : SM_ATTRACT_MENU;
        SmAttract wn = n == 0 ? SM_ATTRACT_MENU
                     : (n - 1) % 2 == 0 ? SM_ATTRACT_SCORES : SM_ATTRACT_MENU;
        if (start_menu_attract_at((uint32_t)e, true) != wd ||
            start_menu_attract_at((uint32_t)e, false) != wn) bad++;
    }
    check(bad == 0, "phase equals the divide-based reference over a full-range sweep");

    StartMenu m;
    InputState in;
    bool ping;
    start_menu_init(&m, "Test", NULL, T0);
    int s = start_menu_add_action(&m, "Start");
    start_menu_set_attract(&m, true);
    quiet(&m, 1, T0, &ping);
    idle_run(&m, 0, 15000);
    check(start_menu_attract(&m) == SM_ATTRACT_MENU, "menu until 15 s idle");
    int demo_pings = idle_run(&m, 15000, 16000);
    check(start_menu_attract(&m) == SM_ATTRACT_DEMO && demo_pings == 0, "then DEMO, silent");
    memset(&in, 0, sizeof(in));
    in.buttons[BTN_ID_JUMP].held = in.buttons[BTN_ID_JUMP].pressed = true;
    check(start_menu_step(&m, &in, 0, 0, false, 1, T0 + 16000, &ping) == SM_NONE &&
          start_menu_attract(&m) == SM_ATTRACT_MENU, "JUMP in DEMO: back to MENU, nothing started");
    in.buttons[BTN_ID_JUMP].pressed = false;
    check(start_menu_step(&m, &in, 0, 0, false, 1, T0 + 16033, &ping) == SM_NONE,
          "... the same JUMP still held does not start either");
    quiet(&m, 1, T0 + 16066, &ping);
    check(press(&m, BTN_ID_JUMP, 1, T0 + 16100) == s, "a fresh press starts");

    StartMenu n;
    start_menu_init(&n, "Test", NULL, T0);
    start_menu_add_action(&n, "Start");    /* no demo */
    quiet(&n, 1, T0, &ping);
    idle_run(&n, 0, 15100);
    check(start_menu_attract(&n) == SM_ATTRACT_SCORES, "no demo: MENU goes straight to SCORES");
    /* The game draws SCORES, so start_menu_draw() never clears the flag. */
    check(start_menu_needs_redraw(&n), "SCORES: one redraw on the page edge");
    check(!start_menu_needs_redraw(&n), "... and none after it, with no draw between");
    idle_run(&n, 15100, 15200);
    check(!start_menu_needs_redraw(&n), "... nor while SCORES idles");
    check(touch(&n, cx(&n, 0), cy(&n, 0), cx(&n, 0), cy(&n, 0), T0 + 15200) == SM_NONE &&
          start_menu_attract(&n) == SM_ATTRACT_MENU, "a tap on START in SCORES only returns to MENU");
    idle_run(&n, 15300, 15200 + 14900);
    check(start_menu_attract(&n) == SM_ATTRACT_MENU, "the idle clock restarted at that tap");
    idle_run(&n, 15200 + 14900, 15200 + 15100);
    check(start_menu_attract(&n) == SM_ATTRACT_SCORES, "... and runs out 15 s after it");
}

static void group_demo_over_and_sound(void)
{
    printf("11. a demo can end early; sound is allowed under 60 s idle\n");
    StartMenu m;
    bool ping;
    start_menu_init(&m, "Test", NULL, T0);
    start_menu_add_action(&m, "Start");
    start_menu_set_attract(&m, true);
    quiet(&m, 1, T0, &ping);
    idle_run(&m, 0, 18000);
    check(start_menu_attract(&m) == SM_ATTRACT_DEMO, "DEMO at 18 s");
    start_menu_demo_over(&m);
    quiet(&m, 1, T0 + 18000, &ping);
    check(start_menu_attract(&m) == SM_ATTRACT_SCORES, "demo over: SCORES at once");
    idle_run(&m, 18100, 18000 + 14900);
    check(start_menu_attract(&m) == SM_ATTRACT_SCORES, "... for its full 15 s");
    idle_run(&m, 18000 + 14900, 18000 + 15100);
    check(start_menu_attract(&m) == SM_ATTRACT_MENU, "... then MENU");
    start_menu_demo_over(&m);
    quiet(&m, 1, T0 + 33200, &ping);
    check(start_menu_attract(&m) == SM_ATTRACT_MENU, "demo_over outside DEMO changes nothing");

    check(start_menu_sound_allowed(59000), "sound allowed at 59 s idle");
    check(!start_menu_sound_allowed(61000), "silent at 61 s idle");
    idle_run(&m, 33300, 61000);
    check(!start_menu_sound_allowed(start_menu_idle_ms(&m, T0 + 61000)),
          "the menu's own idle clock is past the cutoff at 61 s");
    InputState in;
    memset(&in, 0, sizeof(in));
    in.buttons[BTN_ID_UP].held = in.buttons[BTN_ID_UP].pressed = true;
    start_menu_step(&m, &in, 0, 0, false, 1, T0 + 61000, &ping);
    check(start_menu_sound_allowed(start_menu_idle_ms(&m, T0 + 61100)),
          "after an input, sound is allowed again");
}

/* An add past SM_MAX_ENTRIES is refused with -1 and leaves the menu as it was (a caller that adds more than fits loses the tail). */
static void group_overflow(void)
{
    printf("overflow\n");
    StartMenu m;
    start_menu_init(&m, "T", NULL, T0);
    int last = -2;
    for (int i = 0; i < SM_MAX_ENTRIES; i++) last = start_menu_add_action(&m, "X");
    check(last == SM_MAX_ENTRIES - 1 && m.count == SM_MAX_ENTRIES, "the last entry that fits is added");
    check(start_menu_add_action(&m, "ONE TOO MANY") == -1, "an action past the cap returns -1");
    const char *v[2] = { "A", "B" };
    check(start_menu_add_choice(&m, "CH", v, 2, 0) == -1, "a choice past the cap returns -1");
    check(m.count == SM_MAX_ENTRIES, "the count is unchanged");
}

static void group_reopen(void)
{
    printf("12. reopen (a game returning to its menu)\n");
    StartMenu m;
    bool ping;
    menu3(&m);
    start_menu_select(&m, 1, T0);
    quiet(&m, 1, T0 + 16000, &ping);
    check(start_menu_attract(&m) == SM_ATTRACT_SCORES, "setup: idle 16 s shows SCORES");

    uint32_t t1 = T0 + 16000;
    start_menu_reopen(&m, t1);
    check(start_menu_attract(&m) == SM_ATTRACT_MENU, "reopen: attract is MENU");
    check(m.sel == 1, "reopen keeps the selection");
    check(m.shown && m.dirty, "reopen shows the brackets and asks for a frame");

    quiet(&m, 1, t1 + 14999, &ping);
    check(start_menu_attract(&m) == SM_ATTRACT_MENU, "14999 ms later still MENU (clock restarted)");
    quiet(&m, 1, t1 + 15000, &ping);
    check(start_menu_attract(&m) == SM_ATTRACT_SCORES, "15000 ms later SCORES (a full slot away)");

    /* The button that left the scores page is still down at the reopen. */
    menu3(&m);
    start_menu_select(&m, 2, T0);
    quiet(&m, 1, T0 + 16000, &ping);
    start_menu_reopen(&m, t1);
    InputState in;
    memset(&in, 0, sizeof(in));
    in.buttons[BTN_ID_JUMP].held = true;
    in.buttons[BTN_ID_JUMP].pressed = true;
    int r = start_menu_step(&m, &in, 0, 0, false, 1, t1, &ping);
    check(r == SM_NONE, "held JUMP at reopen activates nothing");
    in.buttons[BTN_ID_JUMP].pressed = false;
    r = start_menu_step(&m, &in, 0, 0, false, 1, t1 + 50, &ping);
    check(r == SM_NONE, "still held next frame: nothing");
    quiet(&m, 1, t1 + 80, &ping);
    r = press(&m, BTN_ID_JUMP, 1, t1 + 100);
    check(r == 2, "released and pressed again activates the kept selection");

    /* A finger down across the reopen must lift before it counts. */
    menu3(&m);
    quiet(&m, 1, T0 + 16000, &ping);
    start_menu_reopen(&m, t1);
    int x = cx(&m, 0), y = cy(&m, 0);
    start_menu_step(&m, NULL, x, y, true, 1, t1, &ping);
    r = start_menu_step(&m, NULL, x, y, false, 1, t1 + 50, &ping);
    check(r == SM_NONE && m.pressed == -1, "finger already down at reopen does not press START");
}

int main(void)
{
    group_phase();
    group_ping_cycle();
    group_no_double();
    group_wrap();
    group_choice();
    group_players();
    group_touch();
    group_held();
    group_idle_cutoff();
    group_attract();
    group_demo_over_and_sound();
    group_reopen();
    group_overflow();
    printf("\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
