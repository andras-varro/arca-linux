/*
 * button_latch_test.c — the pause-menu-dies-after-one-use bug, on the host.
 *
 * Reported from the panel 2026-08-10: Office Runner stopped responding to the
 * hamburger menu mid-session, while the red EXIT button in the same HUD row
 * still worked.  Cause is not in either button — it is in how the caller asks.
 *
 *   button_check_press(btn, currently_pressed, now)  derives the press EDGE
 *   itself, from btn->was_pressed.  It clears that latch on exactly one kind of
 *   call: one where currently_pressed is FALSE.  A caller that only ever calls
 *   it with true therefore fires once per process and is dead thereafter.
 *
 * platformer.c and frogger.c both had:
 *     if (button_is_touched(&menu_button, ts.x, ts.y) &&
 *         button_check_press(&menu_button, true, now))        <-- short-circuit
 * so the false call never happens.  brick_breaker.c and tetris.c pass the value
 * instead, which is why their pause menus keep working — but only because their
 * players tap elsewhere constantly, which is what produces the false call.
 *
 * EXIT hid the whole thing: it only ever needs to fire once, because it ends the
 * process.  Same defect, no symptom.
 *
 * Group A is the negative control on this harness: it drives the OLD idiom and
 * asserts the reported symptom reproduces.  Without it, a green suite would not
 * distinguish "the fix works" from "the harness cannot see the bug".
 *
 * Build (host gcc, from native_apps/):
 *   gcc -Wall -Wextra -Wno-unused-parameter -I common -o build/button_latch_test \
 *       tests/button_latch_test.c common/common.c common/framebuffer.c common/font.c \
 *       common/touch_input.c common/hardware.c common/config.c \
 *       common/highscore.c common/keyboard.c common/audio.c common/audio_gen.c \
 *       common/audio_out.c common/audio_wav.c -lm && \
 *   ./build/button_latch_test
 */

#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include "../common/common.h"

static int failures = 0;
static int checks   = 0;

static void check(bool cond, const char *what) {
    checks++;
    if (!cond) {
        failures++;
        printf("  FAIL: %s\n", what);
    } else {
        printf("  ok:   %s\n", what);
    }
}

/* A button at a known rect, debounce left at whatever button_init sets. */
static void make_button(Button *b) {
    memset(b, 0, sizeof(*b));
    button_init(b, 10, 10, 70, 40, "", 0x808080, 0xFFFFFF, 0xFFFF00);
}

/* ── The two call idioms, as the games write them ──────────────────────── */

/* Pre-fix: only ever called with true, and only when the finger is inside.
 * Returns whether the tap fired. */
static bool tap_old_idiom(Button *b, const TouchState *ts, uint32_t now) {
    if (ts->pressed && button_is_touched(b, ts->x, ts->y))
        return button_check_press(b, true, now);
    return false;   /* no call at all — the latch is never cleared */
}

/* Post-fix: called every frame with a LEVEL signal.  Gating on pressed||held
 * matters: touch_input.c leaves state.x/y at the last coordinate after release,
 * so button_is_touched() alone stays true with no finger on the panel and the
 * latch would never clear either. */
static bool tap_helper(Button *b, const TouchState *ts, uint32_t now) {
    return button_check_tap(b, ts, now);
}

/* Frame helpers.  A real tap is one `pressed` frame then quiet frames; the
 * quiet frames keep the last coordinate, as the driver does. */
static TouchState frame_down(int x, int y) {
    TouchState ts; memset(&ts, 0, sizeof(ts));
    ts.x = x; ts.y = y; ts.pressed = true; ts.held = true;
    return ts;
}
static TouchState frame_quiet(int x, int y) {
    TouchState ts; memset(&ts, 0, sizeof(ts));
    ts.x = x; ts.y = y;             /* coords retained, nothing pressed */
    return ts;
}

#define IN_X  40
#define IN_Y  25
#define OUT_X 400
#define OUT_Y 300

int main(void) {
    uint32_t now = 100000;          /* well past any debounce baseline */
    const uint32_t QUIET = 400;     /* > BTN_DEBOUNCE_MS between taps */

    printf("\n=== A. the reported symptom, old idiom (must reproduce) ===\n");
    {
        Button b; make_button(&b);
        TouchState d = frame_down(IN_X, IN_Y);
        check(tap_old_idiom(&b, &d, now), "1st tap on the button fires");

        /* Finger lifts: the caller makes no call, because it short-circuits. */
        TouchState q = frame_quiet(IN_X, IN_Y);
        (void)tap_old_idiom(&b, &q, now + 10);

        now += QUIET;
        TouchState d2 = frame_down(IN_X, IN_Y);
        check(tap_old_idiom(&b, &d2, now) == false,
              "2nd tap is SWALLOWED — this is the panel report, reproduced");
        check(b.was_pressed == true,
              "the latch is still set, with no finger on the panel");
    }

    printf("\n=== B. same sequence through button_check_tap (must work) ===\n");
    {
        Button b; make_button(&b);
        now = 200000;
        int fired = 0;
        for (int tap = 0; tap < 4; tap++) {
            TouchState d = frame_down(IN_X, IN_Y);
            if (tap_helper(&b, &d, now)) fired++;
            /* two quiet frames, as the main loop would deliver */
            TouchState q = frame_quiet(IN_X, IN_Y);
            tap_helper(&b, &q, now + 10);
            tap_helper(&b, &q, now + 20);
            now += QUIET;
        }
        check(fired == 4, "4 separate taps fire 4 times");
    }

    printf("\n=== C. negative control: debounce must still work ===\n");
    {
        Button b; make_button(&b);
        now = 300000;
        int fired = 0;
        TouchState d = frame_down(IN_X, IN_Y);
        if (tap_helper(&b, &d, now)) fired++;
        TouchState q = frame_quiet(IN_X, IN_Y);
        tap_helper(&b, &q, now + 5);            /* release */
        TouchState d2 = frame_down(IN_X, IN_Y); /* bounce, 20 ms later */
        if (tap_helper(&b, &d2, now + 20)) fired++;
        check(fired == 1, "a 20 ms bounce fires once, not twice");
    }

    printf("\n=== D. retained coordinates must not hold the latch ===\n");
    {
        Button b; make_button(&b);
        now = 400000;
        TouchState d = frame_down(IN_X, IN_Y);
        check(tap_helper(&b, &d, now), "tap fires");
        /* Quiet frames with x/y STILL inside the rect — the real driver state.
         * A fix that tested button_is_touched() alone would stay latched here. */
        TouchState q = frame_quiet(IN_X, IN_Y);
        for (int i = 0; i < 5; i++) tap_helper(&b, &q, now + 10 + i);
        check(b.was_pressed == false,
              "latch cleared despite x/y still inside the button");
        now += QUIET;
        TouchState d2 = frame_down(IN_X, IN_Y);
        check(tap_helper(&b, &d2, now), "next tap fires");
    }

    printf("\n=== E. a tap elsewhere must not fire this button ===\n");
    {
        Button b; make_button(&b);
        now = 500000;
        TouchState d = frame_down(OUT_X, OUT_Y);
        check(tap_helper(&b, &d, now) == false, "tap outside does not fire");
        check(b.was_pressed == false, "and does not set the latch");
    }

    /* F: a caller that repaints only on change must learn when a button's
     * look changed.  Reported from the panel 2026-09-30 once control_panel
     * stopped repainting every iteration: a button tapped last stayed yellow
     * (the no-finger branch kept it HIGHLIGHTED from the retained x/y), and a
     * button whose press launched a full-screen run came back PRESSED.  Both
     * are button_update() moving visual_state with nobody told. */
    printf("\n=== F. button_update: stale look after release (must be NORMAL + reported) ===\n");
    {
        Button b; make_button(&b);
        now = 600000;
        (void)button_take_dirty();                  /* start from a clean flag */

        check(button_update(&b, IN_X, IN_Y, true, now), "press fires");
        check(b.visual_state == BTN_STATE_PRESSED, "held: PRESSED");
        check(button_take_dirty(), "press is reported as a look change");

        /* Finger lifts; x/y stay on the button, as touch_input leaves them. */
        (void)button_update(&b, IN_X, IN_Y, false, now + 10);
        check(b.visual_state == BTN_STATE_NORMAL,
              "released with retained x/y on the button: NORMAL, not hover-yellow");
        check(button_take_dirty(), "release is reported as a look change");

        /* The negative control on the flag: quiet frames change nothing, so a
         * repaint test built on it must not become always-true. */
        for (int i = 0; i < 5; i++)
            (void)button_update(&b, IN_X, IN_Y, false, now + 20 + i);
        check(!button_take_dirty(), "quiet frames report nothing (not always-true)");
    }
    {
        /* The LED-test shape: press, the frame is consumed by a full-screen
         * run (no release frame reaches this button), then quiet frames. */
        Button b; make_button(&b);
        now = 700000;
        (void)button_update(&b, IN_X, IN_Y, true, now);
        (void)button_take_dirty();                  /* the press frame was painted */
        (void)button_update(&b, IN_X, IN_Y, false, now + 3000);
        check(b.visual_state == BTN_STATE_NORMAL, "after a consumed release: NORMAL");
        check(button_take_dirty(), "and reported, so the page repaints it hollow");
    }
    {
        /* Slide off while held: PRESSED -> NORMAL with no press/release edge. */
        Button b; make_button(&b);
        now = 800000;
        (void)button_update(&b, IN_X, IN_Y, true, now);
        (void)button_take_dirty();
        (void)button_update(&b, OUT_X, OUT_Y, true, now + 10);
        check(b.visual_state == BTN_STATE_NORMAL, "slid off while held: NORMAL");
        check(button_take_dirty(), "slide-off is reported");
    }
    {
        /* button_check_press's one-frame HIGHLIGHTED flash is a change too. */
        Button b; make_button(&b);
        now = 900000;
        (void)button_take_dirty();
        check(button_check_press(&b, true, now), "check_press fires");
        check(button_take_dirty(), "check_press leading-edge flash is reported");
        (void)button_check_press(&b, true, now + 10);
        check(b.visual_state == BTN_STATE_NORMAL && button_take_dirty(),
              "and its return to NORMAL is reported");
    }

    printf("\n=== F. touch guard: a touch from before a screen change never reaches the new screen ===\n");
    {
        /* Control: with no guard, the bounce re-press 60 ms after the change fires the new screen's button. */
        Button b; make_button(&b);
        uint32_t t = 500000;
        TouchState bounce = frame_down(IN_X, IN_Y);
        check(button_check_tap(&b, &bounce, t + 60), "control: unguarded bounce re-press fires the button");

        /* Guarded: the tap that changed the screen released at t; bounce at +60, held, lifted, bounce again. */
        TouchGuard g; touch_guard_init(&g);
        make_button(&b);
        check(touch_guard_pass(&g, false, t - 20), "armed guard passes a quiet frame");
        touch_guard_screen_changed(&g, t);
        bool fired = false;
        TouchState q = frame_quiet(IN_X, IN_Y);
        fired |= touch_guard_pass(&g, false, t + 16) && button_check_tap(&b, &q, t + 16);
        TouchState d = frame_down(IN_X, IN_Y);
        for (uint32_t k = 60; k < 400; k += 16) {              /* bounce, then a long contact: all shut */
            bool pass = touch_guard_pass(&g, true, t + k);
            fired |= pass && button_check_tap(&b, &d, t + k);
        }
        check(!fired, "bounce and the contact that follows never fire the new screen's button");
        for (uint32_t k = 400; k < 640; k += 16) (void)touch_guard_pass(&g, false, t + k);
        check(g.armed, "quiet period re-arms");
        check(touch_guard_pass(&g, true, t + 700) && button_check_tap(&b, &d, t + 700),
              "a fresh press after the finger was up for the quiet period counts");

        /* A lift-and-bounce inside the quiet period keeps it shut; the quiet restarts from the last contact. */
        TouchGuard h; touch_guard_init(&h);
        touch_guard_screen_changed(&h, t);
        (void)touch_guard_pass(&h, true, t + 100);
        check(!touch_guard_pass(&h, false, t + 250), "150 ms after the last contact is still shut");
        check(touch_guard_pass(&h, false, t + 300), "200 ms after the last contact re-arms");
        /* No quiet frame needed: a press that starts long after the change (an idle loop sends none) counts at once. */
        TouchGuard s; touch_guard_init(&s);
        touch_guard_screen_changed(&s, t);
        check(touch_guard_pass(&s, true, t + 1000), "a first press a second after the change passes");
        /* ...but the same contact started inside the quiet period never earns the pass by lasting. */
        TouchGuard c; touch_guard_init(&c);
        touch_guard_screen_changed(&c, t);
        bool any = false;
        for (uint32_t k = 50; k < 1000; k += 16) any |= touch_guard_pass(&c, true, t + k);
        check(!any, "a contact begun inside the quiet period stays shut however long it is held");
        /* Clock wrap does not strand the guard shut. */
        TouchGuard w; touch_guard_init(&w);
        touch_guard_screen_changed(&w, 0xFFFFFFF0u);
        check(touch_guard_pass(&w, false, 0xFFFFFFF0u + 300u), "re-arms across the 32-bit clock wrap");
    }

    printf("\n%s  %d checks, %d failure(s)\n",
           failures ? "FAILED" : "PASSED", checks, failures);
    return failures ? 1 : 0;
}
