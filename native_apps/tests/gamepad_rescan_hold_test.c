/* Host-side regression: a key, pad button or D-pad direction that is physically
 * held must stay held across gamepad_rescan().
 *
 * gamepad_tick() calls gamepad_rescan() whenever /dev/input changes.  It closes
 * every evdev node and reopens it, and it used to zero held_latched[] and
 * re-seed only the mouse buttons from EVIOCGKEY.  A reopened node delivers no
 * press event for a key that is already down (the kernel sends events on
 * change only, and an EV_ABS hat value is filtered when it repeats), so a held
 * pad button or hat direction read as released from the first poll after the
 * rescan until it was let go and pressed again.  A keyboard is rescued by its
 * autorepeat a few tens of ms later, at the price of a spurious press edge.
 *
 * Build (this line is the CTEST_ROWS row in tests/run-all.sh):
 *   cd native_apps && gcc -Wall -Wextra -Wno-unused-parameter -I common \
 *       -o build/gamepad_rescan_hold_test tests/gamepad_rescan_hold_test.c \
 *       common/gamepad.c common/input_slots.c common/framebuffer.c common/hardware.c \
 *       common/config.c common/touch_input.c -lm
 *
 * HOW A RESCAN IS REACHED ON A HOST WITH NO SUCH DEVICES.  Two link-time seams,
 * and no change to shipped code:
 *   - input_scan.c is deliberately NOT linked.  It is compiled INTO this file
 *     with its input_scan() renamed out of the way, so the pure helpers
 *     gamepad.c takes from there (input_caps_test, input_pad_key) are the real
 *     ones, and this file's input_scan() returns whatever fake devices the test
 *     says are plugged in, each on a fresh fd, exactly as a real reopen hands
 *     back a new fd.
 *   - ioctl() is defined here too, which pre-empts glibc's for every call in
 *     this executable.  On a fake fd it answers EVIOCGKEY from the device's
 *     "kernel" key bitmap and EVIOCGABS from its hat position; any other fd is
 *     forwarded to the real syscall.
 * Events reach the poll the way gamepad_latch_test.c does it: written into the
 * fake fd (a temp file) and read back by the real poll code.
 *
 * CONTROLS.  "mouse left held across a rescan" already passed before the fix:
 * the mouse path was the one that re-seeded.  It proves the ioctl seam reaches
 * EVIOCGKEY, so a failure on the pad/keyboard rows is the defect and not the
 * harness.  "unplugged while held" also passes on both sides; it guards against
 * a fix that keeps the latch instead of re-reading it.  Every "before rescan"
 * row is the negative control for its "after" row: a level never set cannot
 * be lost.
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

#define input_scan input_scan_real_unused
#include "input_scan.c"
#undef input_scan

static int fails = 0;

static void expect_bool(const char *what, bool got, bool want) {
    if (got != want) {
        printf("  FAIL %-56s got %s want %s\n", what,
               got ? "true" : "false", want ? "true" : "false");
        fails++;
    } else {
        printf("  ok   %-56s %s\n", what, got ? "true" : "false");
    }
}

/* ── The fake devices ──────────────────────────────────────────────────── */
typedef struct {
    InputKind     kind;
    const char   *name;
    bool          present;
    unsigned long keys[INPUT_SCAN_NLONGS(KEY_MAX + 1)];  /* what EVIOCGKEY reports */
    int           hat_x, hat_y;                           /* what EVIOCGABS reports */
    int           fd;                                      /* current open fd, or -1 */
    InputPadLayout layout;                                 /* what the scan reports */
} FakeDev;

enum { DEV_PAD, DEV_KBD, DEV_MOUSE, DEV_COUNT };
static FakeDev g_dev[DEV_COUNT];

static void devs_reset(void) {
    memset(g_dev, 0, sizeof(g_dev));
    g_dev[DEV_PAD]   = (FakeDev){ .kind = INPUT_KIND_PAD,      .name = "fake pad" };
    g_dev[DEV_KBD]   = (FakeDev){ .kind = INPUT_KIND_KEYBOARD, .name = "fake keyboard" };
    g_dev[DEV_MOUSE] = (FakeDev){ .kind = INPUT_KIND_MOUSE,    .name = "fake mouse" };
    for (int i = 0; i < DEV_COUNT; i++) { g_dev[i].present = true; g_dev[i].fd = -1; }
}

static FakeDev *dev_for_fd(int fd) {
    for (int i = 0; i < DEV_COUNT; i++)
        if (fd >= 0 && g_dev[i].fd == fd) return &g_dev[i];
    return NULL;
}

static int fake_fd(void) {
    char path[] = "/tmp/arca_rescan_hold_XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) { perror("mkstemp"); exit(2); }
    unlink(path);
    return fd;
}

/* ── Seam 1: input_scan() ─────────────────────────────────────────────── */
static void caps_set(unsigned long *bits, int bit) {
    input_caps_set(bits, bit);
}
static void caps_clear(unsigned long *bits, int bit) {
    bits[bit / INPUT_SCAN_LONG_BITS] &= ~(1UL << (bit % INPUT_SCAN_LONG_BITS));
}

static int g_scan_calls;   /* every full scan, for the gamepad_tick() case */

int input_scan(InputNode *nodes, int n, int max, const int cap[INPUT_KIND_COUNT]) {
    g_scan_calls++;
    for (int i = 0; i < DEV_COUNT && n < max; i++) {
        FakeDev *d = &g_dev[i];
        d->fd = -1;          /* the old fd was closed by gamepad_close() */
        if (!d->present) continue;
        d->fd = fake_fd();
        memset(&nodes[n], 0, sizeof(nodes[n]));
        snprintf(nodes[n].path, sizeof(nodes[n].path), "/dev/input/event%d", i);
        snprintf(nodes[n].name, sizeof(nodes[n].name), "%s", d->name);
        nodes[n].fd = d->fd;
        nodes[n].kind = d->kind;
        nodes[n].pad_layout = d->layout;
        n++;
    }
    return n;
}

/* ── Seam 2: ioctl() ───────────────────────────────────────────────────── */
int ioctl(int fd, unsigned long req, ...) {
    va_list ap;
    va_start(ap, req);
    void *arg = va_arg(ap, void *);
    va_end(ap);

    FakeDev *d = dev_for_fd(fd);
    if (!d) return (int)syscall(SYS_ioctl, fd, req, arg);

    if (_IOC_TYPE(req) == 'E' && _IOC_NR(req) == _IOC_NR(EVIOCGKEY(0))) {
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
            ai->value = (axis == ABS_HAT0X) ? d->hat_x : d->hat_y;
        } else {
            ai->minimum = -32768; ai->maximum = 32767;
        }
        return 0;
    }
    errno = ENOTTY;
    return -1;
}

/* ── Driving it ────────────────────────────────────────────────────────── */
/* Deliver one event on a device's current fd for the next poll to read. */
static void deliver(FakeDev *d, uint16_t type, uint16_t code, int32_t value) {
    struct input_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = type; ev.code = code; ev.value = value;
    if (ftruncate(d->fd, 0) < 0 || lseek(d->fd, 0, SEEK_SET) < 0 ||
        write(d->fd, &ev, sizeof(ev)) != (ssize_t)sizeof(ev) ||
        lseek(d->fd, 0, SEEK_SET) < 0) { perror("deliver"); exit(2); }
}

static void start(GamepadManager *gm, InputState *st) {
    devs_reset();
    gamepad_init(gm);
    memset(st, 0, sizeof(*st));
    gamepad_poll(gm, st, 0, 0, false);       /* settle: nothing held */
}

static void test_pad_button(void) {
    printf("\n1. pad button held across a rescan\n");
    GamepadManager gm; InputState st;
    start(&gm, &st);
    FakeDev *pad = &g_dev[DEV_PAD];

    caps_set(pad->keys, BTN_SOUTH);
    deliver(pad, EV_KEY, BTN_SOUTH, 1);
    gamepad_poll(&gm, &st, 0, 0, false);
    expect_bool("before rescan: JUMP held", st.buttons[BTN_ID_JUMP].held, true);

    gamepad_rescan(&gm);
    gamepad_poll(&gm, &st, 0, 0, false);
    expect_bool("after rescan: JUMP still held", st.buttons[BTN_ID_JUMP].held, true);
    expect_bool("after rescan: no spurious release edge", st.buttons[BTN_ID_JUMP].released, false);
    expect_bool("after rescan: no spurious press edge", st.buttons[BTN_ID_JUMP].pressed, false);

    /* And the release after the rescan still arrives on the new fd. */
    caps_clear(pad->keys, BTN_SOUTH);
    deliver(pad, EV_KEY, BTN_SOUTH, 0);
    gamepad_poll(&gm, &st, 0, 0, false);
    expect_bool("released after rescan: JUMP cleared", st.buttons[BTN_ID_JUMP].held, false);
    expect_bool("released after rescan: release edge", st.buttons[BTN_ID_JUMP].released, true);
    gamepad_close(&gm);
}

static void test_pad_hat(void) {
    printf("\n2. D-pad hat held across a rescan\n");
    GamepadManager gm; InputState st;
    start(&gm, &st);
    FakeDev *pad = &g_dev[DEV_PAD];

    pad->hat_x = -1;
    deliver(pad, EV_ABS, ABS_HAT0X, -1);
    gamepad_poll(&gm, &st, 0, 0, false);
    expect_bool("before rescan: LEFT held", st.buttons[BTN_ID_LEFT].held, true);

    gamepad_rescan(&gm);
    gamepad_poll(&gm, &st, 0, 0, false);
    expect_bool("after rescan: LEFT still held", st.buttons[BTN_ID_LEFT].held, true);
    expect_bool("after rescan: RIGHT not invented", st.buttons[BTN_ID_RIGHT].held, false);
    gamepad_close(&gm);
}

static void test_keyboard(void) {
    printf("\n3. keyboard key held across a rescan (the frame before autorepeat)\n");
    GamepadManager gm; InputState st;
    start(&gm, &st);
    FakeDev *kbd = &g_dev[DEV_KBD];

    caps_set(kbd->keys, KEY_RIGHT);
    deliver(kbd, EV_KEY, KEY_RIGHT, 1);
    gamepad_poll(&gm, &st, 0, 0, false);
    expect_bool("before rescan: RIGHT held", st.buttons[BTN_ID_RIGHT].held, true);

    gamepad_rescan(&gm);
    gamepad_poll(&gm, &st, 0, 0, false);
    expect_bool("after rescan: RIGHT still held", st.buttons[BTN_ID_RIGHT].held, true);
    expect_bool("after rescan: no spurious release edge", st.buttons[BTN_ID_RIGHT].released, false);

    /* The autorepeat that used to rescue it must not then fire a press edge. */
    deliver(kbd, EV_KEY, KEY_RIGHT, 2);
    gamepad_poll(&gm, &st, 0, 0, false);
    expect_bool("autorepeat after rescan: no spurious press edge", st.buttons[BTN_ID_RIGHT].pressed, false);
    gamepad_close(&gm);
}

static void test_released_during_rescan(void) {
    printf("\n4. released while the rescan had the node closed\n");
    GamepadManager gm; InputState st;
    start(&gm, &st);
    FakeDev *pad = &g_dev[DEV_PAD];

    caps_set(pad->keys, BTN_SOUTH);
    deliver(pad, EV_KEY, BTN_SOUTH, 1);
    gamepad_poll(&gm, &st, 0, 0, false);
    expect_bool("before rescan: JUMP held", st.buttons[BTN_ID_JUMP].held, true);

    caps_clear(pad->keys, BTN_SOUTH);         /* key-up lands on the closed fd */
    gamepad_rescan(&gm);
    gamepad_poll(&gm, &st, 0, 0, false);
    expect_bool("after rescan: JUMP cleared", st.buttons[BTN_ID_JUMP].held, false);
    expect_bool("after rescan: release edge reported", st.buttons[BTN_ID_JUMP].released, true);
    gamepad_close(&gm);
}

static void test_unplugged_while_held(void) {
    printf("\n5. control: unplugged while held must not stay latched\n");
    GamepadManager gm; InputState st;
    start(&gm, &st);
    FakeDev *pad = &g_dev[DEV_PAD];

    caps_set(pad->keys, BTN_SOUTH);
    deliver(pad, EV_KEY, BTN_SOUTH, 1);
    gamepad_poll(&gm, &st, 0, 0, false);
    expect_bool("before rescan: JUMP held", st.buttons[BTN_ID_JUMP].held, true);

    pad->present = false;
    gamepad_rescan(&gm);
    gamepad_poll(&gm, &st, 0, 0, false);
    expect_bool("pad gone: JUMP cleared", st.buttons[BTN_ID_JUMP].held, false);
    gamepad_close(&gm);
}

static void test_mouse_control(void) {
    printf("\n6. control: mouse button held across a rescan (seam reaches EVIOCGKEY)\n");
    GamepadManager gm; InputState st;
    start(&gm, &st);
    FakeDev *mouse = &g_dev[DEV_MOUSE];

    caps_set(mouse->keys, BTN_LEFT);
    deliver(mouse, EV_KEY, BTN_LEFT, 1);
    gamepad_poll(&gm, &st, 0, 0, false);
    expect_bool("before rescan: mouse left held", st.mouse_left_held != 0, true);

    gamepad_rescan(&gm);
    gamepad_poll(&gm, &st, 0, 0, false);
    expect_bool("after rescan: mouse left still held", st.mouse_left_held != 0, true);
    expect_bool("after rescan: no mouse release edge", st.mouse_left_released != 0, false);
    gamepad_close(&gm);
}

/* A hid-generic pad (the 8BitDo Pro 2 in Bluetooth X mode) reports Start as
 * raw 0x137 and LB as raw 0x134 — the codes the native map reads as RB and as
 * btn_action.  Both the event path and the EVIOCGKEY re-seed must translate
 * before mapping: Start must latch PAUSE, and LB must latch nothing.  Each row
 * fails in its own direction if either path compares raw codes. */
static void test_sequential_layout(void) {
    printf("\n7. hid-generic SEQUENTIAL pad: Start and LB, by event and across a rescan\n");
    GamepadManager gm; InputState st;
    devs_reset();
    g_dev[DEV_PAD].layout = INPUT_PAD_SEQUENTIAL;
    gamepad_init(&gm);
    memset(&st, 0, sizeof(st));
    gamepad_poll(&gm, &st, 0, 0, false);
    FakeDev *pad = &g_dev[DEV_PAD];

    caps_set(pad->keys, BTN_GAMEPAD + 7);          /* Menu/Start */
    deliver(pad, EV_KEY, BTN_GAMEPAD + 7, 1);
    gamepad_poll(&gm, &st, 0, 0, false);
    caps_set(pad->keys, BTN_GAMEPAD + 4);          /* LB */
    deliver(pad, EV_KEY, BTN_GAMEPAD + 4, 1);
    gamepad_poll(&gm, &st, 0, 0, false);
    expect_bool("event: raw 0x137 latches PAUSE", st.buttons[BTN_ID_PAUSE].held, true);
    expect_bool("event: raw 0x134 does not latch ACTION", st.buttons[BTN_ID_ACTION].held, false);

    gamepad_rescan(&gm);
    gamepad_poll(&gm, &st, 0, 0, false);
    expect_bool("after rescan: PAUSE still held", st.buttons[BTN_ID_PAUSE].held, true);
    expect_bool("after rescan: ACTION not invented from raw 0x134",
                st.buttons[BTN_ID_ACTION].held, false);
    gamepad_close(&gm);
}

/* gamepad_tick(): the rescan is gated, not periodic.  An unchanged /dev/input
 * fingerprint must never close and reopen anything; a changed one, or a read
 * that finds its fd gone, must.  The host's /dev/input does not change during
 * the run, so "changed" is staged by making the baseline stale, and "gone" by
 * closing the pad's fd behind the manager's back (read() then fails EBADF,
 * the same branch an unplug's ENODEV takes). */
static void test_tick_gate(void) {
    printf("\n8. gamepad_tick rescans only on a change or a gone fd\n");
    GamepadManager gm; InputState st;
    start(&gm, &st);
    int base = g_scan_calls;

    gamepad_tick(&gm, 10000);
    gamepad_tick(&gm, 11000);
    gamepad_tick(&gm, 15000);
    expect_bool("unchanged fingerprint: no rescan in 3 checks", g_scan_calls == base, true);

    input_sig_gate_baseline(&gm.node_gate, gm.node_gate.sig + 1);
    gamepad_tick(&gm, 15500);
    expect_bool("changed, check not yet due: no rescan", g_scan_calls == base, true);
    gamepad_tick(&gm, 16000);
    expect_bool("changed fingerprint: one rescan", g_scan_calls == base + 1, true);
    gamepad_tick(&gm, 17000);
    expect_bool("after the rescan: baseline current, no rescan", g_scan_calls == base + 1, true);

    close(gm.gamepad_fd);                     /* the fd goes stale */
    gamepad_poll(&gm, &st, 0, 0, false);
    expect_bool("gone fd: dropped by the poll", gm.gamepad_fd < 0, true);
    gamepad_tick(&gm, 17001);                 /* not due: forced anyway */
    expect_bool("gone fd: next tick rescans", g_scan_calls == base + 2, true);
    expect_bool("gone fd: pad reopened", gm.gamepad_fd >= 0, true);
    gamepad_close(&gm);
}

int main(void) {
    printf("gamepad rescan: held input survives the hot-plug rescan\n");
    test_pad_button();
    test_pad_hat();
    test_keyboard();
    test_released_during_rescan();
    test_unplugged_while_held();
    test_mouse_control();
    test_sequential_layout();
    test_tick_gate();
    printf("\n%s (%d failure%s)\n", fails ? "FAILED" : "PASSED",
           fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
