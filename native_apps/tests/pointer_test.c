/* Host-side regression for the mouse pointer's pure model (pointer_model_*,
 * pointer_route_touch) — where the pointer is, whether it is shown, and what a
 * left click hands to the app's touch path.  Shared by app_launcher and the
 * Control Panel.
 *
 * Runs on the DEV MACHINE with native gcc.  Build and run:
 *
 *   cd native_apps && gcc -Wall -Wextra -Wno-unused-parameter -I common \
 *       -o build/pointer_test tests/pointer_test.c common/pointer.c \
 *       common/framebuffer.c common/font.c common/hardware.c common/config.c \
 *       && ./build/pointer_test
 *
 * Group 1 is the start state, group 2 motion and the clamp to the bounds
 * (right/bottom exclusive), group 3 the show/hide rule, group 4 a bounds change
 * with no motion, group 5 the click edges, group 6 the routing into a
 * TouchState, group 7 pointer_drain() after a full-screen run.
 *
 * ⚠️ What it cannot see: anything drawn (the save-under and the two
 * fb_swap_rect() calls), whether each app passes the REAL finger press and its
 * nav keys, and whether gamepad.c's mouse_dx/dy match the cursor it moves.
 */
#include "pointer.h"
#include <stdio.h>
#include <string.h>

static int fails;

static void expect(const char *what, int got, int want) {
    if (got != want) {
        printf("  FAIL %-56s got %d want %d\n", what, got, want);
        fails++;
    }
}

static PointerInput none(void) {
    PointerInput in = { 0 };
    return in;
}

static PointerInput move(int dx, int dy) {
    PointerInput in = { 0 };
    in.dx = dx; in.dy = dy;
    return in;
}

int main(void) {
    const PointerBounds b = { 10, 20, 790, 460 };   /* a swept panel's SAFE rect */
    PointerModel m;
    PointerInput in;
    PointerTouch t;

    printf("[1] start: hidden, at the centre of the bounds\n");
    pointer_model_init(&m, b);
    expect("x at centre", m.x, 400);
    expect("y at centre", m.y, 240);
    expect("hidden", m.shown, 0);
    in = none();
    t = pointer_model_step(&m, &in, b);
    expect("a quiet frame keeps it hidden", m.shown, 0);
    expect("a quiet frame emits no press", t.pressed, 0);

    printf("[2] motion moves and shows; the clamp holds all four edges\n");
    in = move(5, -3);
    pointer_model_step(&m, &in, b);
    expect("moved x", m.x, 405);
    expect("moved y", m.y, 237);
    expect("motion shows it", m.shown, 1);
    in = move(-5000, -5000);
    pointer_model_step(&m, &in, b);
    expect("clamped to left", m.x, 10);
    expect("clamped to top", m.y, 20);
    in = move(5000, 5000);
    pointer_model_step(&m, &in, b);
    expect("clamped to right - 1", m.x, 789);
    expect("clamped to bottom - 1", m.y, 459);
    in = none();
    pointer_model_step(&m, &in, b);
    expect("a quiet frame keeps it shown", m.shown, 1);

    printf("[3] a finger or a nav key hides it, keeping the position\n");
    in = none(); in.touch_press = true;
    pointer_model_step(&m, &in, b);
    expect("finger hides", m.shown, 0);
    expect("finger keeps x", m.x, 789);
    in = move(-1, 0);
    pointer_model_step(&m, &in, b);
    expect("motion shows it again", m.shown, 1);
    in = none(); in.nav_key = true;
    pointer_model_step(&m, &in, b);
    expect("nav key hides", m.shown, 0);
    in = move(1, 1); in.nav_key = true;
    pointer_model_step(&m, &in, b);
    expect("motion and a key in one frame: hidden", m.shown, 0);

    printf("[4] the bounds shrinking re-clamps with no motion\n");
    {
        const PointerBounds small = { 50, 60, 300, 200 };
        in = none();
        pointer_model_step(&m, &in, small);
        expect("re-clamped x", m.x, 299);
        expect("re-clamped y", m.y, 199);
        const PointerBounds empty = { 100, 100, 100, 100 };
        pointer_model_step(&m, &in, empty);
        expect("empty bounds pin x to left", m.x, 100);
        expect("empty bounds pin y to top", m.y, 100);
    }

    printf("[5] left click: press, hold, release; quick click; strays\n");
    pointer_model_init(&m, b);
    in = none(); in.left_pressed = true; in.left_held = true;
    t = pointer_model_step(&m, &in, b);
    expect("press emits pressed", t.pressed, 1);
    expect("press emits held", t.held, 1);
    expect("press emits no release", t.released, 0);
    expect("a click shows the pointer", m.shown, 1);
    in = move(3, 0); in.left_held = true;
    t = pointer_model_step(&m, &in, b);
    expect("drag: held", t.held, 1);
    expect("drag: no second press", t.pressed, 0);
    in = none(); in.left_released = true;
    t = pointer_model_step(&m, &in, b);
    expect("release emits released", t.released, 1);
    expect("release: not held", t.held, 0);
    in = none(); in.left_released = true;
    t = pointer_model_step(&m, &in, b);
    expect("a second release is a stray", t.released, 0);
    in = none(); in.left_pressed = true; in.left_released = true;
    t = pointer_model_step(&m, &in, b);
    expect("quick click: pressed", t.pressed, 1);
    expect("quick click: released", t.released, 1);
    expect("quick click: not held", t.held, 0);
    in = none(); in.left_pressed = true; in.left_held = true; in.touch_press = true;
    t = pointer_model_step(&m, &in, b);
    expect("finger and click together: the finger wins", t.pressed, 0);
    in = none(); in.left_released = true;
    t = pointer_model_step(&m, &in, b);
    expect("... and that click's release is a stray", t.released, 0);
    in = none(); in.left_pressed = true; in.left_held = true;
    pointer_model_step(&m, &in, b);
    in = none(); in.left_held = true; in.touch_press = true;
    t = pointer_model_step(&m, &in, b);
    expect("a finger mid-click cancels the hold", t.held, 0);
    in = none(); in.left_released = true;
    t = pointer_model_step(&m, &in, b);
    expect("... and the release after it", t.released, 0);

    printf("[6] routing into the app's TouchState\n");
    {
        pointer_model_init(&m, b);
        m.x = 123; m.y = 45;
        TouchState ts = { 7, 8, false, false, false };
        PointerTouch click = { true, false, true };
        expect("a click is routed", pointer_route_touch(&m, click, &ts), 1);
        expect("routed x", ts.x, 123);
        expect("routed y", ts.y, 45);
        expect("routed pressed", ts.pressed, 1);
        expect("routed held", ts.held, 1);
        expect("routed released", ts.released, 0);

        TouchState finger = { 7, 8, true, false, true };
        expect("a real finger is not overridden",
               pointer_route_touch(&m, click, &finger), 0);
        expect("finger x kept", finger.x, 7);

        TouchState lift = { 7, 8, false, true, false };
        expect("a finger's release is not overridden",
               pointer_route_touch(&m, click, &lift), 0);

        TouchState quiet = { 7, 8, false, false, false };
        PointerTouch nothing = { false, false, false };
        expect("no mouse edge: nothing routed",
               pointer_route_touch(&m, nothing, &quiet), 0);
        expect("quiet x kept", quiet.x, 7);
    }

    printf("[7] a full-screen run in between: the press is not replayed\n");
    {
        /* The press starts the run; its release goes to the caller's drain,
         * never to the model.  Back from the run, the button still reads
         * down (held, or its release lost) with no edge — that must not
         * become a held touch over the button that started the run. */
        Pointer p;
        memset(&p, 0, sizeof(p));
        pointer_model_init(&p.m, b);
        in = none();
        in.left_pressed = in.left_held = true;
        t = pointer_model_step(&p.m, &in, b);
        expect("the press that starts the run is routed", t.pressed, 1);
        const int px = p.m.x, py = p.m.y;

        pointer_drain(&p);
        expect("drain keeps x", p.m.x, px);
        expect("drain keeps y", p.m.y, py);

        in = none();
        in.left_held = true;
        t = pointer_model_step(&p.m, &in, b);
        expect("a level still down after the run is no held touch", t.held, 0);
        expect("... and no press", t.pressed, 0);
        TouchState ts = { 0, 0, false, false, false };
        expect("... so nothing is routed",
               pointer_route_touch(&p.m, t, &ts), 0);

        in = none();
        in.left_released = true;
        t = pointer_model_step(&p.m, &in, b);
        expect("its late release is no release", t.released, 0);

        in = none();
        in.left_pressed = in.left_released = true;   /* the next whole click */
        t = pointer_model_step(&p.m, &in, b);
        expect("the next click still presses", t.pressed, 1);
        expect("the next click still releases", t.released, 1);
    }

    printf("\n%s (%d failure%s)\n", fails ? "REGRESSION" : "ALL PASS",
           fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
