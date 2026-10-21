/* Host-side regression: an evdev buffer overflow (SYN_DROPPED) must not leave a
 * button, key, axis or touch reading as held after it was let go.
 *
 * MECHANISM, read from the vanilla 4.14.52 drivers/input/evdev.c.  Each client
 * has a ring of max(hint_events_per_packet * 8, 64) events.  When the reader
 * falls behind and the ring fills, __pass_event() throws away EVERY unread
 * event and leaves EV_SYN/SYN_DROPPED plus the newest event; nothing is
 * replayed.  A release that was among the discarded events is gone, and the
 * only record of the true level is the kernel's own key/abs state, which
 * EVIOCGKEY / EVIOCGABS return (evdev_handle_get_val() also flushes the queued
 * EV_KEY events, so the snapshot and the remaining queue agree).  A reader that
 * ignores SYN_DROPPED keeps the last level it saw: a mouse button, a key, a pad
 * button, a hat, a stick or a finger reads as held until the next press and
 * release.  An app that stops reading for a while (a blocking wizard, a load)
 * while the device keeps reporting is all it takes.
 *
 * Build (this line is the CTEST_ROWS row in tests/run-all.sh):
 *   cd native_apps && gcc -Wall -Wextra -Wno-unused-parameter -I common \
 *       -o build/evdev_resync_test tests/evdev_resync_test.c \
 *       common/gamepad.c common/input_slots.c common/framebuffer.c common/font.c common/hardware.c \
 *       common/config.c common/touch_input.c -lm
 *
 * SEAMS — the two gamepad_rescan_hold_test.c uses, and no change to shipped
 * code: input_scan.c compiled into this file with input_scan() renamed so the
 * fake devices are what gets "plugged in", and ioctl() defined here so a fake
 * fd answers EVIOCGKEY from its "kernel" key bitmap and EVIOCGABS from its
 * "kernel" axis values.  An overflow is staged by writing exactly what the
 * kernel leaves in the ring — SYN_DROPPED, the newest event, SYN_REPORT — into
 * the fake fd and changing the bitmap/axis behind the reader's back, which is
 * what the discarded events would have reported.
 *
 * CONTROLS.  Every "before overflow" row is the negative control for its
 * "after" row (a level never set cannot be lost).  The "still held across an
 * overflow" rows pass before the fix too and guard against a fix that simply
 * clears everything on SYN_DROPPED.  "packet after the drop" proves the reader
 * still takes complete packets that follow the dropped span.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <linux/input.h>
#include "gamepad.h"
#include "input_scan.h"
#include "touch_input.h"

#define input_scan input_scan_real_unused
#include "input_scan.c"
#undef input_scan

static int fails = 0;

static void expect_bool(const char *what, bool got, bool want) {
    if (got != want) {
        printf("  FAIL %-58s got %s want %s\n", what,
               got ? "true" : "false", want ? "true" : "false");
        fails++;
    } else {
        printf("  ok   %-58s %s\n", what, got ? "true" : "false");
    }
}

/* ── The fake devices ──────────────────────────────────────────────────── */
typedef struct {
    InputKind     kind;
    const char   *name;
    unsigned long keys[INPUT_SCAN_NLONGS(KEY_MAX + 1)];  /* what EVIOCGKEY reports */
    int           abs[ABS_CNT];                           /* what EVIOCGABS reports */
    int           abs_min, abs_max;                       /* non-hat axis range */
    int           fd;
} FakeDev;

enum { DEV_PAD, DEV_KBD, DEV_MOUSE, DEV_TOUCH, DEV_COUNT };
static FakeDev g_dev[DEV_COUNT];

static void devs_reset(void) {
    memset(g_dev, 0, sizeof(g_dev));
    g_dev[DEV_PAD]   = (FakeDev){ .kind = INPUT_KIND_PAD,      .name = "fake pad",
                                  .abs_min = -32768, .abs_max = 32767 };
    g_dev[DEV_KBD]   = (FakeDev){ .kind = INPUT_KIND_KEYBOARD, .name = "fake keyboard" };
    g_dev[DEV_MOUSE] = (FakeDev){ .kind = INPUT_KIND_MOUSE,    .name = "fake mouse" };
    g_dev[DEV_TOUCH] = (FakeDev){ .kind = INPUT_KIND_COUNT,    .name = "fake touch",
                                  .abs_min = 0, .abs_max = 4095 };
    for (int i = 0; i < DEV_COUNT; i++) g_dev[i].fd = -1;
}

static FakeDev *dev_for_fd(int fd) {
    for (int i = 0; i < DEV_COUNT; i++)
        if (fd >= 0 && g_dev[i].fd == fd) return &g_dev[i];
    return NULL;
}

static int fake_fd(void) {
    char path[] = "/tmp/arca_evdev_resync_XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) { perror("mkstemp"); exit(2); }
    unlink(path);
    return fd;
}

static void caps_clear(unsigned long *bits, int bit) {
    bits[bit / INPUT_SCAN_LONG_BITS] &= ~(1UL << (bit % INPUT_SCAN_LONG_BITS));
}

/* ── Seam 1: input_scan() — pad, keyboard, mouse; the touch is opened by
 * touch_init() on its own path. ──────────────────────────────────────────── */
int input_scan(InputNode *nodes, int n, int max, const int cap[INPUT_KIND_COUNT]) {
    for (int i = 0; i < DEV_TOUCH && n < max; i++) {
        FakeDev *d = &g_dev[i];
        d->fd = fake_fd();
        memset(&nodes[n], 0, sizeof(nodes[n]));
        snprintf(nodes[n].path, sizeof(nodes[n].path), "/dev/input/event%d", i);
        snprintf(nodes[n].name, sizeof(nodes[n].name), "%s", d->name);
        nodes[n].fd = d->fd;
        nodes[n].kind = d->kind;
        n++;
    }
    return n;
}

/* ── Seam 2: ioctl() ───────────────────────────────────────────────────── */
static int g_gkey_calls;

int ioctl(int fd, unsigned long req, ...) {
    va_list ap;
    va_start(ap, req);
    void *arg = va_arg(ap, void *);
    va_end(ap);

    FakeDev *d = dev_for_fd(fd);
    if (!d) return (int)syscall(SYS_ioctl, fd, req, arg);

    if (_IOC_TYPE(req) == 'E' && _IOC_NR(req) == _IOC_NR(EVIOCGKEY(0))) {
        g_gkey_calls++;
        size_t len = _IOC_SIZE(req);
        if (len > sizeof(d->keys)) len = sizeof(d->keys);
        memset(arg, 0, _IOC_SIZE(req));
        memcpy(arg, d->keys, len);
        return (int)len;
    }
    if (_IOC_TYPE(req) == 'E' && _IOC_NR(req) >= 0x40 && _IOC_NR(req) < 0x40 + ABS_CNT) {
        int axis = (int)_IOC_NR(req) - 0x40;
        struct input_absinfo *ai = arg;
        memset(ai, 0, sizeof(*ai));
        if (axis == ABS_HAT0X || axis == ABS_HAT0Y) {
            ai->minimum = -1; ai->maximum = 1;
        } else {
            ai->minimum = d->abs_min; ai->maximum = d->abs_max;
        }
        ai->value = d->abs[axis];
        return 0;
    }
    errno = ENOTTY;
    return -1;
}

/* ── Driving it ────────────────────────────────────────────────────────── */
typedef struct { uint16_t type, code; int32_t value; } Ev;

/* Replace what the next read sees on a device's fd with these events. */
static void deliver(FakeDev *d, const Ev *evs, int n) {
    if (ftruncate(d->fd, 0) < 0 || lseek(d->fd, 0, SEEK_SET) < 0) { perror("deliver"); exit(2); }
    for (int i = 0; i < n; i++) {
        struct input_event ev;
        memset(&ev, 0, sizeof(ev));
        ev.type = evs[i].type; ev.code = evs[i].code; ev.value = evs[i].value;
        if (write(d->fd, &ev, sizeof(ev)) != (ssize_t)sizeof(ev)) { perror("deliver"); exit(2); }
    }
    if (lseek(d->fd, 0, SEEK_SET) < 0) { perror("deliver"); exit(2); }
}
#define DELIVER(d, ...) do { const Ev e_[] = { __VA_ARGS__ }; \
                             deliver((d), e_, (int)(sizeof(e_) / sizeof(e_[0]))); } while (0)
#define SYN     { EV_SYN, SYN_REPORT, 0 }
#define DROPPED { EV_SYN, SYN_DROPPED, 0 }

static void start(GamepadManager *gm, InputState *st) {
    devs_reset();
    gamepad_init(gm);
    memset(st, 0, sizeof(*st));
    gamepad_poll(gm, st, 0, 0, false);       /* settle: nothing held */
}

/* ── 1. mouse ──────────────────────────────────────────────────────────── */
static void test_mouse_release_lost(void) {
    printf("\n1. mouse: left release lost to an overflow\n");
    GamepadManager gm; InputState st;
    start(&gm, &st);
    FakeDev *m = &g_dev[DEV_MOUSE];

    input_caps_set(m->keys, BTN_LEFT);
    DELIVER(m, { EV_KEY, BTN_LEFT, 1 }, SYN);
    gamepad_poll(&gm, &st, 0, 0, false);
    expect_bool("before overflow: left held", st.mouse_left_held != 0, true);

    /* The release and a run of motion overflowed the ring: the kernel kept
     * SYN_DROPPED and the newest event, and its key state says up. */
    caps_clear(m->keys, BTN_LEFT);
    DELIVER(m, DROPPED, { EV_REL, REL_X, 3 }, SYN);
    gamepad_poll(&gm, &st, 0, 0, false);
    expect_bool("after overflow: left released", st.mouse_left_held != 0, false);
    expect_bool("after overflow: release edge reported", st.mouse_left_released != 0, true);
    gamepad_close(&gm);
}

static void test_mouse_still_held(void) {
    printf("\n2. control: mouse left still held across an overflow\n");
    GamepadManager gm; InputState st;
    start(&gm, &st);
    FakeDev *m = &g_dev[DEV_MOUSE];

    input_caps_set(m->keys, BTN_LEFT);
    DELIVER(m, { EV_KEY, BTN_LEFT, 1 }, SYN);
    gamepad_poll(&gm, &st, 0, 0, false);
    expect_bool("before overflow: left held", st.mouse_left_held != 0, true);

    DELIVER(m, DROPPED, { EV_REL, REL_X, 3 }, SYN);
    gamepad_poll(&gm, &st, 0, 0, false);
    expect_bool("after overflow: left still held", st.mouse_left_held != 0, true);
    expect_bool("after overflow: no spurious release edge", st.mouse_left_released != 0, false);
    expect_bool("after overflow: no spurious press edge", st.mouse_left_pressed != 0, false);
    gamepad_close(&gm);
}

static void test_mouse_press_lost(void) {
    printf("\n3. mouse: right press lost to an overflow, then a packet after the drop\n");
    GamepadManager gm; InputState st;
    start(&gm, &st);
    FakeDev *m = &g_dev[DEV_MOUSE];

    input_caps_set(m->keys, BTN_RIGHT);
    DELIVER(m, DROPPED, { EV_REL, REL_Y, 1 }, SYN);
    gamepad_poll(&gm, &st, 0, 0, false);
    expect_bool("after overflow: right held", st.mouse_right_held != 0, true);

    /* A complete packet following a drop span is real and must be taken. */
    caps_clear(m->keys, BTN_RIGHT);
    input_caps_set(m->keys, BTN_LEFT);
    DELIVER(m, DROPPED, { EV_REL, REL_X, 1 }, SYN,
               { EV_KEY, BTN_RIGHT, 0 }, { EV_KEY, BTN_LEFT, 1 }, SYN);
    gamepad_poll(&gm, &st, 0, 0, false);
    expect_bool("packet after the drop: right released", st.mouse_right_held != 0, false);
    expect_bool("packet after the drop: left held", st.mouse_left_held != 0, true);
    gamepad_close(&gm);
}

/* ── 2. keyboard ───────────────────────────────────────────────────────── */
static void test_keyboard_release_lost(void) {
    printf("\n4. keyboard: key release lost to an overflow\n");
    GamepadManager gm; InputState st;
    start(&gm, &st);
    FakeDev *k = &g_dev[DEV_KBD];

    input_caps_set(k->keys, KEY_RIGHT);
    input_caps_set(k->keys, KEY_SPACE);
    DELIVER(k, { EV_KEY, KEY_RIGHT, 1 }, SYN, { EV_KEY, KEY_SPACE, 1 }, SYN);
    gamepad_poll(&gm, &st, 0, 0, false);
    expect_bool("before overflow: RIGHT held", st.buttons[BTN_ID_RIGHT].held, true);
    expect_bool("before overflow: JUMP held", st.buttons[BTN_ID_JUMP].held, true);

    caps_clear(k->keys, KEY_RIGHT);           /* SPACE stays down */
    DELIVER(k, DROPPED, { EV_KEY, KEY_SPACE, 2 }, SYN);
    gamepad_poll(&gm, &st, 0, 0, false);
    expect_bool("after overflow: RIGHT released", st.buttons[BTN_ID_RIGHT].held, false);
    expect_bool("control: JUMP still held", st.buttons[BTN_ID_JUMP].held, true);
    gamepad_close(&gm);
}

/* ── 3. pad ────────────────────────────────────────────────────────────── */
static void test_pad_release_lost(void) {
    printf("\n5. pad: button, hat and stick returns lost to an overflow\n");
    GamepadManager gm; InputState st;
    start(&gm, &st);
    FakeDev *p = &g_dev[DEV_PAD];

    input_caps_set(p->keys, BTN_SOUTH);
    p->abs[ABS_HAT0Y] = -1;
    p->abs[ABS_X] = -32768;
    DELIVER(p, { EV_KEY, BTN_SOUTH, 1 }, SYN, { EV_ABS, ABS_HAT0Y, -1 }, SYN,
               { EV_ABS, ABS_X, -32768 }, SYN);
    gamepad_poll(&gm, &st, 0, 0, false);
    expect_bool("before overflow: JUMP held", st.buttons[BTN_ID_JUMP].held, true);
    expect_bool("before overflow: UP held (hat)", st.buttons[BTN_ID_UP].held, true);
    expect_bool("before overflow: LEFT held (stick)", st.buttons[BTN_ID_LEFT].held, true);

    caps_clear(p->keys, BTN_SOUTH);
    p->abs[ABS_HAT0Y] = 0;
    p->abs[ABS_X] = 0;
    DELIVER(p, DROPPED, { EV_ABS, ABS_RY, 5 }, SYN);
    gamepad_poll(&gm, &st, 0, 0, false);
    expect_bool("after overflow: JUMP released", st.buttons[BTN_ID_JUMP].held, false);
    expect_bool("after overflow: UP released (hat)", st.buttons[BTN_ID_UP].held, false);
    expect_bool("after overflow: LEFT released (stick)", st.buttons[BTN_ID_LEFT].held, false);
    gamepad_close(&gm);
}

/* ── 4. touch ──────────────────────────────────────────────────────────── */
static void test_touch_release_lost(void) {
    printf("\n6. touch: finger lift lost to an overflow\n");
    devs_reset();
    char path[] = "/tmp/arca_evdev_resync_touch_XXXXXX";
    int tmp = mkstemp(path);
    if (tmp < 0) { perror("mkstemp"); exit(2); }
    close(tmp);
    TouchInput t;
    memset(&t, 0, sizeof(t));
    if (touch_init(&t, path) != 0) { printf("  FAIL touch_init\n"); fails++; unlink(path); return; }
    unlink(path);
    /* touch_init() opens read-only; swap in a writable fake so events can be
     * staged.  Its init-time EVIOCGABS fell back to 0..4095, the same range. */
    close(t.fd);
    t.fd = fake_fd();
    FakeDev *d = &g_dev[DEV_TOUCH];
    d->fd = t.fd;

    input_caps_set(d->keys, BTN_TOUCH);
    d->abs[ABS_X] = 2000; d->abs[ABS_Y] = 2000;
    DELIVER(d, { EV_ABS, ABS_X, 2000 }, { EV_ABS, ABS_Y, 2000 }, { EV_KEY, BTN_TOUCH, 1 }, SYN);
    touch_poll(&t);
    expect_bool("before overflow: touch held", t.state.held, true);

    caps_clear(d->keys, BTN_TOUCH);
    DELIVER(d, DROPPED, { EV_ABS, ABS_X, 2100 }, SYN);
    touch_poll(&t);
    expect_bool("after overflow: touch released", t.state.held, false);
    expect_bool("after overflow: release edge reported", t.state.released, true);

    /* Control: a finger still down across an overflow stays down, no edge. */
    input_caps_set(d->keys, BTN_TOUCH);
    DELIVER(d, { EV_ABS, ABS_X, 2000 }, { EV_KEY, BTN_TOUCH, 1 }, SYN);
    touch_poll(&t);
    expect_bool("control, before overflow: touch held", t.state.held, true);
    DELIVER(d, DROPPED, { EV_ABS, ABS_X, 2100 }, SYN);
    touch_poll(&t);
    expect_bool("control, after overflow: touch still held", t.state.held, true);
    expect_bool("control, after overflow: no spurious press edge", t.state.pressed, false);
    expect_bool("control, after overflow: no spurious release edge", t.state.released, false);
    touch_close(&t);
}

int main(void) {
    printf("evdev resync: SYN_DROPPED re-reads levels instead of keeping stale ones\n");
    test_mouse_release_lost();
    test_mouse_still_held();
    test_mouse_press_lost();
    test_keyboard_release_lost();
    test_pad_release_lost();
    test_touch_release_lost();
    printf("\n%s (%d failure%s)\n", fails ? "FAILED" : "PASSED",
           fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
