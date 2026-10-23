/* Host regression for gamepad.c's typed-key capture (gamepad_text_entry /
 * gamepad_take_keys), the physical-keyboard half of the on-screen keyboard.
 *
 *   cd native_apps && gcc -Wall -Wextra -Wno-unused-parameter -I common \
 *       -o build/gamepad_text_test tests/gamepad_text_test.c \
 *       common/gamepad.c common/input_slots.c common/input_scan.c common/framebuffer.c \
 *       common/font.c common/hardware.c common/config.c common/touch_input.c -lm \
 *       && ./build/gamepad_text_test
 *
 * Asserts: with text entry off nothing is queued and W/A/S/D/Space latch their
 * buttons exactly as before (games keep WASD); with it on, those five keys are
 * queued and latch nothing while arrows and Enter still latch; Shift and Caps
 * state ride along on the queued key; autorepeat is flagged; the queue is
 * bounded and drains oldest first; turning it off clears it.
 * Fails to compile against the pre-change source (no GamepadKeyEv, no
 * gamepad_text_entry, no gamepad_take_keys).
 * Cannot see: a real evdev node, or the on-screen keyboard's drawing. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <linux/input.h>
#include "gamepad.h"
#include "framebuffer.h"
#include "input_scan.h"

static int fails, checks;
#define CHECK(cond) do { checks++; if (!(cond)) { fails++; \
    printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

static int fake_fd(void) {
    char path[] = "/tmp/arca_gamepad_text_XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) { perror("mkstemp"); exit(2); }
    unlink(path);
    return fd;
}
static void key(int fd, int code, int value) {
    struct input_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = EV_KEY; ev.code = code; ev.value = value;
    if (write(fd, &ev, sizeof(ev)) != (ssize_t)sizeof(ev)) { perror("write"); exit(2); }
}
/* Read back from where the previous poll left off: rewind to `from`. */
static off_t poll_with(GamepadManager *gm, InputState *st, int fd, off_t from) {
    off_t end = lseek(fd, 0, SEEK_END);
    if (lseek(fd, from, SEEK_SET) < 0) { perror("lseek"); exit(2); }
    gamepad_poll(gm, st, 0, 0, false);
    return end;
}
static void fresh(GamepadManager *gm, int *fd) {
    gamepad_init(gm);
    gamepad_close(gm);
    memset(gm->held_latched, 0, sizeof(gm->held_latched));
    memset(gm->prev_held, 0, sizeof(gm->prev_held));
    *fd = fake_fd();
    gm->keyboard_fds[0] = *fd;
    gm->keyboard_count = 1;
}

int main(void) {
    GamepadManager gm; InputState st; int fd; GamepadKeyEv k[GAMEPAD_KEYQ_MAX];
    off_t pos;

    /* 1. off: games unchanged */
    memset(&st, 0, sizeof(st)); fresh(&gm, &fd);
    key(fd, KEY_W, 1); key(fd, KEY_SPACE, 1); key(fd, KEY_Q, 1);
    poll_with(&gm, &st, fd, 0);
    CHECK(st.buttons[BTN_ID_UP].held);
    CHECK(st.buttons[BTN_ID_JUMP].held);
    CHECK(gamepad_take_keys(&gm, k, GAMEPAD_KEYQ_MAX) == 0);
    gamepad_close(&gm);

    /* 2. on: W/A/S/D/Space are text, arrows/Enter stay buttons */
    memset(&st, 0, sizeof(st)); fresh(&gm, &fd);
    gamepad_text_entry(&gm, true);
    key(fd, KEY_W, 1); key(fd, KEY_A, 1); key(fd, KEY_S, 1); key(fd, KEY_D, 1);
    key(fd, KEY_SPACE, 1); key(fd, KEY_UP, 1); key(fd, KEY_ENTER, 1);
    pos = poll_with(&gm, &st, fd, 0);
    CHECK(st.buttons[BTN_ID_UP].held);              /* from the arrow only */
    CHECK(!st.buttons[BTN_ID_DOWN].held && !st.buttons[BTN_ID_LEFT].held);
    CHECK(!st.buttons[BTN_ID_RIGHT].held && !st.buttons[BTN_ID_JUMP].held);
    CHECK(st.buttons[BTN_ID_ACTION].held);
    int n = gamepad_take_keys(&gm, k, GAMEPAD_KEYQ_MAX);
    CHECK(n == 6);                                  /* WASD, Space, Enter; not Up */
    CHECK(k[0].code == KEY_W && k[4].code == KEY_SPACE && k[5].code == KEY_ENTER);
    CHECK(gamepad_take_keys(&gm, k, GAMEPAD_KEYQ_MAX) == 0);   /* drained */

    /* 3. releases still reach the latch: no stuck direction */
    key(fd, KEY_W, 0); key(fd, KEY_UP, 0);
    pos = poll_with(&gm, &st, fd, pos);
    CHECK(!st.buttons[BTN_ID_UP].held);

    /* 4. Shift, Caps, repeat */
    key(fd, KEY_LEFTSHIFT, 1); key(fd, KEY_A, 1); key(fd, KEY_A, 2);
    key(fd, KEY_LEFTSHIFT, 0); key(fd, KEY_B, 1);
    key(fd, KEY_CAPSLOCK, 1); key(fd, KEY_C, 1);
    pos = poll_with(&gm, &st, fd, pos);
    n = gamepad_take_keys(&gm, k, GAMEPAD_KEYQ_MAX);
    CHECK(n == 4);
    CHECK(k[0].code == KEY_A && k[0].shift && !k[0].repeat);
    CHECK(k[1].code == KEY_A && k[1].repeat);
    CHECK(k[2].code == KEY_B && !k[2].shift && !k[2].caps);
    CHECK(k[3].code == KEY_C && k[3].caps);

    /* 5. bounded queue, partial take keeps order, off clears */
    for (int i = 0; i < GAMEPAD_KEYQ_MAX + 5; i++) key(fd, KEY_X, 1);
    key(fd, KEY_Q, 1);
    pos = poll_with(&gm, &st, fd, pos);
    n = gamepad_take_keys(&gm, k, 3);
    CHECK(n == 3);
    n = gamepad_take_keys(&gm, k, GAMEPAD_KEYQ_MAX);
    CHECK(n == GAMEPAD_KEYQ_MAX - 3);
    key(fd, KEY_Q, 1);
    pos = poll_with(&gm, &st, fd, pos);
    gamepad_text_entry(&gm, false);
    CHECK(gamepad_take_keys(&gm, k, GAMEPAD_KEYQ_MAX) == 0);
    (void)pos;
    gamepad_close(&gm);

    printf("gamepad_text_test: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
