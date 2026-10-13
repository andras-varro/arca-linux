/* Host-side regression: up to four pads plus keyboards, each in its own player
 * slot (P1..P4), and gamepad_poll() still the OR of all of them.
 *
 * gamepad.c used to open ONE pad and latch every pad, keyboard and hat event
 * into one held_latched[], so a second player could not exist.  It now asks
 * input_slots.c (the pure P1..P4 table) for a slot per device, keyed by the
 * device's EVIOCGID / EVIOCGUNIQ / EVIOCGPHYS identity, and keeps the latched
 * levels, axes and edges per slot.  This proves:
 *   - two pads' presses land in their own gamepad_player() slots;
 *   - gamepad_poll()'s merged state sees both (any-device);
 *   - a keyboard's two event nodes are one player;
 *   - a pad removed and re-added via rescan keeps its slot, even when it comes
 *     back later in the scan order, while another pad stays in its own and a
 *     new pad takes a slot nobody reserved;
 *   - a keyboard+touchpad on ONE node (classified MOUSE) is a keyboard player:
 *     its own slot, listed, pinnable; a plain mouse is none of those.
 *
 * Build (this line is the CTEST_ROWS row in tests/run-all.sh):
 *   cd native_apps && gcc -Wall -Wextra -Wno-unused-parameter -I common \
 *       -o build/gamepad_slots_test tests/gamepad_slots_test.c \
 *       common/gamepad.c common/input_slots.c common/framebuffer.c \
 *       common/hardware.c common/config.c common/touch_input.c -lm
 *
 * The seams are gamepad_rescan_hold_test.c's: input_scan.c is compiled INTO
 * this file with its input_scan() renamed away, so this file's input_scan()
 * hands back the fake devices on fresh temp-file fds; and ioctl() is defined
 * here, so the identity ioctls a regular file cannot answer (EVIOCGID,
 * EVIOCGUNIQ, EVIOCGPHYS), plus EVIOCGKEY and EVIOCGABS, come from the fake
 * device.  No test-only code in gamepad.c.
 *
 * CONTROLS.  The merged rows ("any-device sees ...") pass against the one-pad
 * library for the first pad; every per-player "not held" row is the negative
 * control for the "held" row beside it.
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
        printf("  FAIL %-58s got %s want %s\n", what,
               got ? "true" : "false", want ? "true" : "false");
        fails++;
    } else {
        printf("  ok   %-58s %s\n", what, got ? "true" : "false");
    }
}

static void expect_int(const char *what, int got, int want) {
    if (got != want) {
        printf("  FAIL %-58s got %d want %d\n", what, got, want);
        fails++;
    } else {
        printf("  ok   %-58s %d\n", what, got);
    }
}

/* ── The fake devices ──────────────────────────────────────────────────── */
typedef struct {
    InputKind     kind;
    const char   *name;
    uint16_t      bus, vid, pid;
    const char   *uniq, *phys;
    bool          present;
    unsigned long keys[INPUT_SCAN_NLONGS(KEY_MAX + 1)];  /* what EVIOCGKEY reports */
    int           fd;
    bool          has_kbd;   /* InputNode.keys: a MOUSE node that also carries a keyboard */
} FakeDev;

/* Two identical wired pads on different ports, one Bluetooth pad (its phys is
 * the adapter's), a keyboard with two event nodes, a Bluetooth keyboard whose
 * keys and touchpad are ONE node (input_scan classifies it MOUSE, keys set),
 * and a plain mouse.  The last two start unplugged. */
enum { PAD_A, PAD_B, PAD_C, KBD0, KBD1, COMBO, MOUSE0, DEV_COUNT };
static FakeDev g_dev[DEV_COUNT];
static int g_order[DEV_COUNT];   /* scan order: event-node order on a real box */

static void devs_reset(void) {
    memset(g_dev, 0, sizeof(g_dev));
    g_dev[PAD_A] = (FakeDev){ INPUT_KIND_PAD, "pad A", 3, 0x045e, 0x028e, "",
                              "usb-musb-hdrc.1.auto-1.1/input0", true, {0}, -1, false };
    g_dev[PAD_B] = (FakeDev){ INPUT_KIND_PAD, "pad B", 3, 0x045e, 0x028e, "",
                              "usb-musb-hdrc.1.auto-1.2/input0", true, {0}, -1, false };
    g_dev[PAD_C] = (FakeDev){ INPUT_KIND_PAD, "pad C", 5, 0x045e, 0x02e0,
                              "e4:17:d8:00:00:01", "00:1a:7d:da:71:13", false, {0}, -1, false };
    g_dev[KBD0]  = (FakeDev){ INPUT_KIND_KEYBOARD, "kbd", 3, 0x046d, 0xc31c, "",
                              "usb-musb-hdrc.1.auto-1.3/input0", true, {0}, -1, false };
    g_dev[KBD1]  = (FakeDev){ INPUT_KIND_KEYBOARD, "kbd consumer", 3, 0x046d, 0xc31c, "",
                              "usb-musb-hdrc.1.auto-1.3/input1", true, {0}, -1, false };
    g_dev[COMBO] = (FakeDev){ INPUT_KIND_MOUSE, "BT Keyboard 5.1", 5, 0x04e8, 0x7021,
                              "e4:17:d8:00:00:02", "00:1a:7d:da:71:13", false, {0}, -1, true };
    g_dev[MOUSE0] = (FakeDev){ INPUT_KIND_MOUSE, "mouse", 3, 0x093a, 0x2510, "",
                               "usb-musb-hdrc.1.auto-1.4/input0", false, {0}, -1, false };
    for (int i = 0; i < DEV_COUNT; i++) g_order[i] = i;
}

static FakeDev *dev_for_fd(int fd) {
    for (int i = 0; i < DEV_COUNT; i++)
        if (fd >= 0 && g_dev[i].fd == fd) return &g_dev[i];
    return NULL;
}

static int fake_fd(void) {
    char path[] = "/tmp/arca_slots_XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) { perror("mkstemp"); exit(2); }
    unlink(path);
    return fd;
}

/* ── Seam 1: input_scan() ─────────────────────────────────────────────── */
int input_scan(InputNode *nodes, int n, int max, const int cap[INPUT_KIND_COUNT]) {
    int held[INPUT_KIND_COUNT] = {0};
    for (int i = 0; i < DEV_COUNT; i++) g_dev[i].fd = -1;   /* closed by gamepad_close() */
    for (int o = 0; o < DEV_COUNT && n < max; o++) {
        int i = g_order[o];
        FakeDev *d = &g_dev[i];
        if (!d->present || held[d->kind] >= cap[d->kind]) continue;
        held[d->kind]++;
        d->fd = fake_fd();
        memset(&nodes[n], 0, sizeof(nodes[n]));
        snprintf(nodes[n].path, sizeof(nodes[n].path), "/dev/input/event%d", i);
        snprintf(nodes[n].name, sizeof(nodes[n].name), "%s", d->name);
        nodes[n].fd = d->fd;
        nodes[n].kind = d->kind;
        nodes[n].keys = d->has_kbd;
        n++;
    }
    return n;
}

/* ── Seam 2: ioctl() ───────────────────────────────────────────────────── */
static int put_str(void *arg, size_t len, const char *s) {
    if (!s || !*s) { errno = ENOENT; return -1; }
    size_t n = strlen(s) + 1;
    if (n > len) n = len;
    memcpy(arg, s, n);
    return (int)n;
}

int ioctl(int fd, unsigned long req, ...) {
    va_list ap;
    va_start(ap, req);
    void *arg = va_arg(ap, void *);
    va_end(ap);

    FakeDev *d = dev_for_fd(fd);
    if (!d) return (int)syscall(SYS_ioctl, fd, req, arg);

    if (req == EVIOCGID) {
        struct input_id *id = arg;
        memset(id, 0, sizeof(*id));
        id->bustype = d->bus; id->vendor = d->vid; id->product = d->pid;
        return 0;
    }
    if (_IOC_TYPE(req) == 'E' && _IOC_NR(req) == _IOC_NR(EVIOCGUNIQ(0)))
        return put_str(arg, _IOC_SIZE(req), d->uniq);
    if (_IOC_TYPE(req) == 'E' && _IOC_NR(req) == _IOC_NR(EVIOCGPHYS(0)))
        return put_str(arg, _IOC_SIZE(req), d->phys);
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
        if (axis == ABS_HAT0X || axis == ABS_HAT0Y) { ai->minimum = -1; ai->maximum = 1; }
        else { ai->minimum = -32768; ai->maximum = 32767; }
        return 0;
    }
    errno = ENOTTY;
    return -1;
}

/* ── Driving it ────────────────────────────────────────────────────────── */
static void deliver(FakeDev *d, uint16_t type, uint16_t code, int32_t value) {
    struct input_event ev;
    if (d->fd < 0) return;   /* the library did not open it: a FAIL row, not an abort */
    memset(&ev, 0, sizeof(ev));
    ev.type = type; ev.code = code; ev.value = value;
    if (ftruncate(d->fd, 0) < 0 || lseek(d->fd, 0, SEEK_SET) < 0 ||
        write(d->fd, &ev, sizeof(ev)) != (ssize_t)sizeof(ev) ||
        lseek(d->fd, 0, SEEK_SET) < 0) { perror("deliver"); exit(2); }
}

static void press(FakeDev *d, int code, bool down) {
    if (down) input_caps_set(d->keys, code);
    else d->keys[code / INPUT_SCAN_LONG_BITS] &= ~(1UL << (code % INPUT_SCAN_LONG_BITS));
    deliver(d, EV_KEY, (uint16_t)code, down ? 1 : 0);
}

static bool p_held(const GamepadManager *gm, int slot, ButtonId b) {
    return gamepad_player(gm, slot)->buttons[b].held;
}

/* ── Pins: loaded, applied without a rescan, persisted ────────────────────
 * Every manager here is pointed at a temp file by gamepad_load_slot_pins()
 * BEFORE any pin or unpin, so nothing is ever written to CONFIG_FILE_PATH. */
static InputIdent ident_of(const FakeDev *d) {
    InputIdent id;
    memset(&id, 0, sizeof(id));
    id.bus = d->bus; id.vid = d->vid; id.pid = d->pid;
    snprintf(id.uniq, sizeof(id.uniq), "%s", d->uniq);
    snprintf(id.phys, sizeof(id.phys), "%s", d->phys);
    return id;
}

/* The value of `key` in the file at `path`, or "(absent)". */
static const char *file_key(const char *path, const char *key) {
    static Config c;
    config_init_path(&c, path);
    config_load(&c);
    return config_get(&c, key, "(absent)");
}

static void expect_str(const char *what, const char *got, const char *want) {
    bool ok = strcmp(got, want) == 0;
    printf("  %s %-58s %s%s%s\n", ok ? "ok  " : "FAIL", what, got,
           ok ? "" : " want ", ok ? "" : want);
    if (!ok) fails++;
}

static void pin_tests(void) {
    GamepadManager gm; InputState st; Config mem;
    char path[] = "/tmp/arca_slots_cfg_XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) { perror("mkstemp"); exit(2); }
    close(fd);

    devs_reset();
    InputIdent id_a = ident_of(&g_dev[PAD_A]), id_c = ident_of(&g_dev[PAD_C]);
    InputIdent id_k = ident_of(&g_dev[KBD1]);
    char txt_a[CONFIG_VAL_LEN], txt_c[CONFIG_VAL_LEN];
    if (!input_ident_format(&id_a, txt_a, sizeof(txt_a)) ||
        !input_ident_format(&id_c, txt_c, sizeof(txt_c))) { puts("format"); exit(2); }
    FILE *f = fopen(path, "w");
    if (!f) { perror(path); exit(2); }
    fprintf(f, "led_brightness=40\nslot_p2=not an identity\nslot_p3=%s\n", txt_c);
    fclose(f);

    printf("\n6. pins loaded from the config land a device in its slot when it appears\n");
    for (int i = 0; i < DEV_COUNT; i++) g_dev[i].present = false;
    gamepad_init(&gm);
    expect_int("load: one parseable pin", gamepad_load_slot_pins(&gm, path), 1);
    bool pinned = false, present = true;
    InputIdent got;
    expect_bool("P3 holds an identity", gamepad_slot_info(&gm, 2, &got, &pinned, &present), true);
    expect_bool("P3 is pinned", pinned, true);
    expect_bool("P3's pad is not plugged in", present, false);
    expect_bool("P3 holds pad C", input_ident_equal(&got, &id_c), true);
    expect_bool("P2 (bad value) holds nothing", gamepad_slot_info(&gm, 1, NULL, NULL, NULL), false);
    g_dev[PAD_A].present = g_dev[PAD_B].present = true;
    g_dev[KBD0].present = g_dev[KBD1].present = true;
    gamepad_rescan(&gm);
    gamepad_poll(&gm, &st, 0, 0, false);
    expect_int("present mask: P1 P2 P4 (P3 kept for pad C)", gamepad_player_mask(&gm), 0xb);
    expect_bool("P4 has the keyboard", gamepad_player(&gm, 3)->keyboard_connected, true);
    g_dev[PAD_C].present = true;
    gamepad_rescan(&gm);
    press(&g_dev[PAD_C], BTN_START, true);
    gamepad_poll(&gm, &st, 0, 0, false);
    expect_bool("P3: pad C's PAUSE", p_held(&gm, 2, BTN_ID_PAUSE), true);
    expect_bool("P1: pad C's PAUSE not held", p_held(&gm, 0, BTN_ID_PAUSE), false);

    printf("\n7. a pin re-buckets the open nodes with no rescan, and persists\n");
    config_init_path(&mem, "/nonexistent/never-saved");
    config_set(&mem, "slot_p4", "stale");
    config_set(&mem, "slot_p3", "stale");
    expect_int("pin pad A to P4 returns 3", gamepad_slot_pin(&gm, &id_a, 3, &mem), 3);
    press(&g_dev[PAD_A], BTN_SOUTH, true);
    deliver(&g_dev[KBD0], EV_KEY, KEY_ENTER, 1);
    gamepad_poll(&gm, &st, 0, 0, false);
    expect_bool("P4: pad A's JUMP", p_held(&gm, 3, BTN_ID_JUMP), true);
    expect_bool("P1: pad A's JUMP not held", p_held(&gm, 0, BTN_ID_JUMP), false);
    expect_bool("P1: the evicted keyboard's ENTER", p_held(&gm, 0, BTN_ID_ACTION), true);
    expect_bool("P4 has a pad now", gamepad_player(&gm, 3)->gamepad_connected, true);
    GamepadDevice dv[8];
    int n = gamepad_devices(&gm, dv, 8);
    expect_int("devices: three pads + one keyboard (two nodes)", n, 4);
    if (n == 4) {
        expect_str("devices[0] is pad A (event order)", dv[0].name, "pad A");
        expect_int("pad A's slot", dv[0].slot, 3);
        expect_bool("pad A pinned", dv[0].pinned, true);
        expect_bool("pad A is no keyboard", dv[0].keyboard, false);
        expect_int("pad B's slot", dv[1].slot, 1);
        expect_bool("pad B not pinned", dv[1].pinned, false);
        expect_bool("pad C pinned", dv[2].pinned, true);
        expect_bool("devices[3] is the keyboard", dv[3].keyboard, true);
        expect_int("keyboard's slot", dv[3].slot, 0);
        expect_bool("keyboard's ident", input_ident_equal(&dv[3].ident, &id_k), true);
    }
    expect_str("file: slot_p4 = pad A", file_key(path, "slot_p4"), txt_a);
    expect_str("file: slot_p3 = pad C kept", file_key(path, "slot_p3"), txt_c);
    expect_str("file: bad slot_p2 removed", file_key(path, "slot_p2"), "(absent)");
    expect_str("file: another app's key kept", file_key(path, "led_brightness"), "40");
    expect_str("mem: slot_p4 = pad A", config_get(&mem, "slot_p4", "(absent)"), txt_a);
    expect_str("mem: slot_p3 = pad C", config_get(&mem, "slot_p3", "(absent)"), txt_c);
    expect_int("pin to slot 4 refused", gamepad_slot_pin(&gm, &id_a, 4, NULL), -1);

    printf("\n8. unpin: back to auto, the holder stays, the key goes\n");
    expect_int("unpin P4", gamepad_slot_unpin(&gm, 3, &mem), 0);
    pinned = true; present = false;
    expect_bool("P4 still holds pad A", gamepad_slot_info(&gm, 3, &got, &pinned, &present), true);
    expect_bool("P4 not pinned", pinned, false);
    expect_bool("P4's pad present", present, true);
    expect_str("file: slot_p4 removed", file_key(path, "slot_p4"), "(absent)");
    expect_str("mem: slot_p4 removed", config_get(&mem, "slot_p4", "(absent)"), "(absent)");
    expect_str("file: slot_p3 untouched", file_key(path, "slot_p3"), txt_c);

    printf("\n9. round trip: a pin moved to P1 survives a re-init\n");
    expect_int("pin pad C to P1", gamepad_slot_pin(&gm, &id_c, 0, &mem), 0);
    expect_str("file: slot_p1 = pad C", file_key(path, "slot_p1"), txt_c);
    expect_str("file: slot_p3 (left) removed", file_key(path, "slot_p3"), "(absent)");
    gamepad_close(&gm);
    devs_reset();
    gamepad_init(&gm);
    expect_int("reload: one pin", gamepad_load_slot_pins(&gm, path), 1);
    pinned = present = false;
    expect_bool("P1 holds an identity", gamepad_slot_info(&gm, 0, &got, &pinned, &present), true);
    expect_bool("P1 holds pad C", input_ident_equal(&got, &id_c), true);
    expect_bool("P1 pinned", pinned, true);
    expect_bool("P1 absent (pad C unplugged)", present, false);
    expect_bool("P1 has no open device", gamepad_player_mask(&gm) & 1, false);
    gamepad_close(&gm);
    unlink(path);
}

/* ── A keyboard+touchpad on one node is a keyboard player ─────────────────
 * Its node is MOUSE with keys set.  It must take its own slot, be listed as a
 * keyboard, follow a pin, and latch its keys into that slot — once.  The plain
 * mouse beside it stays unlisted and takes no slot (the negative control). */
static void combo_tests(void) {
    GamepadManager gm; InputState st;
    char path[] = "/tmp/arca_slots_combo_XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) { perror("mkstemp"); exit(2); }
    close(fd);

    printf("\n10. a keyboard+touchpad combo node owns a slot, as a keyboard\n");
    devs_reset();
    for (int i = 0; i < DEV_COUNT; i++) g_dev[i].present = false;
    g_dev[PAD_A].present = g_dev[COMBO].present = g_dev[MOUSE0].present = true;
    gamepad_init(&gm);
    gamepad_load_slot_pins(&gm, path);   /* pins below write here, nowhere else */
    gamepad_poll(&gm, &st, 0, 0, false);
    InputIdent id_combo = ident_of(&g_dev[COMBO]);
    expect_int("present mask: P1 (pad A) P2 (combo)", gamepad_player_mask(&gm), 0x3);
    expect_bool("P2 has a keyboard", gamepad_player(&gm, 1)->keyboard_connected, true);
    expect_bool("any-device: a keyboard connected", st.keyboard_connected, true);
    GamepadDevice dv[8];
    int n = gamepad_devices(&gm, dv, 8);
    expect_int("devices: pad A + the combo (plain mouse unlisted)", n, 2);
    if (n == 2) {
        expect_str("devices[1] is the combo", dv[1].name, "BT Keyboard 5.1");
        expect_bool("the combo is a keyboard", dv[1].keyboard, true);
        expect_int("the combo's slot", dv[1].slot, 1);
        expect_bool("the combo's ident", input_ident_equal(&dv[1].ident, &id_combo), true);
    }
    deliver(&g_dev[COMBO], EV_KEY, KEY_ENTER, 1);
    gamepad_poll(&gm, &st, 0, 0, false);
    expect_bool("P2: the combo's ENTER is ACTION", p_held(&gm, 1, BTN_ID_ACTION), true);
    expect_bool("P1: the combo's ENTER not held", p_held(&gm, 0, BTN_ID_ACTION), false);
    expect_bool("any-device sees the combo's ACTION", st.buttons[BTN_ID_ACTION].held, true);
    deliver(&g_dev[COMBO], EV_KEY, KEY_ENTER, 0);
    gamepad_poll(&gm, &st, 0, 0, false);
    expect_bool("P2: ACTION released (no second latch)", p_held(&gm, 1, BTN_ID_ACTION), false);
    expect_bool("any-device: ACTION released", st.buttons[BTN_ID_ACTION].held, false);

    printf("\n11. pinning the combo moves it with no rescan\n");
    expect_int("pin the combo to P4 returns 3", gamepad_slot_pin(&gm, &id_combo, 3, NULL), 3);
    press(&g_dev[COMBO], KEY_ENTER, true);   /* stays in EVIOCGKEY for the rescan */
    gamepad_poll(&gm, &st, 0, 0, false);
    expect_bool("P4: the combo's ENTER", p_held(&gm, 3, BTN_ID_ACTION), true);
    expect_bool("P2: the combo's ENTER not held", p_held(&gm, 1, BTN_ID_ACTION), false);
    expect_int("present mask: P1 P4", gamepad_player_mask(&gm), 0x9);
    n = gamepad_devices(&gm, dv, 8);
    expect_int("devices: still two", n, 2);
    if (n == 2) {
        expect_int("the combo's slot after the pin", dv[1].slot, 3);
        expect_bool("the combo pinned", dv[1].pinned, true);
    }
    gamepad_rescan(&gm);
    gamepad_poll(&gm, &st, 0, 0, false);
    expect_bool("after a rescan P4 still has the combo",
                gamepad_player(&gm, 3)->keyboard_connected, true);
    expect_bool("P4: held ENTER seeded into its slot", p_held(&gm, 3, BTN_ID_ACTION), true);
    gamepad_close(&gm);
    unlink(path);
}

int main(void) {
    printf("gamepad slots: one player per device, any-device merge kept\n");
    GamepadManager gm; InputState st;
    devs_reset();
    gamepad_init(&gm);
    memset(&st, 0, sizeof(st));
    gamepad_poll(&gm, &st, 0, 0, false);

    printf("\n1. two pads and a two-node keyboard, each its own player\n");
    expect_bool("any-device: a pad connected", st.gamepad_connected, true);
    expect_int("present mask: P1 P2 P3", gamepad_player_mask(&gm), 0x7);
    expect_bool("P1 has a pad", gamepad_player(&gm, 0)->gamepad_connected, true);
    expect_bool("P2 has a pad", gamepad_player(&gm, 1)->gamepad_connected, true);
    expect_bool("P3 has the keyboard", gamepad_player(&gm, 2)->keyboard_connected, true);
    expect_bool("P3 has no pad", gamepad_player(&gm, 2)->gamepad_connected, false);
    expect_bool("P4 is empty", gamepad_player(&gm, 3)->gamepad_connected ||
                               gamepad_player(&gm, 3)->keyboard_connected, false);

    press(&g_dev[PAD_A], BTN_SOUTH, true);
    press(&g_dev[PAD_B], BTN_EAST, true);
    deliver(&g_dev[KBD1], EV_KEY, KEY_ENTER, 1);   /* the keyboard's SECOND node */
    gamepad_poll(&gm, &st, 0, 0, false);
    expect_bool("P1: pad A's JUMP held", p_held(&gm, 0, BTN_ID_JUMP), true);
    expect_bool("P1: pad A's JUMP pressed edge", gamepad_player(&gm, 0)->buttons[BTN_ID_JUMP].pressed, true);
    expect_bool("P1: pad B's RUN not held", p_held(&gm, 0, BTN_ID_RUN), false);
    expect_bool("P2: pad B's RUN held", p_held(&gm, 1, BTN_ID_RUN), true);
    expect_bool("P2: pad A's JUMP not held", p_held(&gm, 1, BTN_ID_JUMP), false);
    expect_bool("P3: the keyboard's ENTER is ACTION", p_held(&gm, 2, BTN_ID_ACTION), true);
    expect_bool("P1: the keyboard's ENTER not held", p_held(&gm, 0, BTN_ID_ACTION), false);
    expect_bool("any-device sees JUMP", st.buttons[BTN_ID_JUMP].held, true);
    expect_bool("any-device sees RUN", st.buttons[BTN_ID_RUN].held, true);
    expect_bool("any-device sees ACTION", st.buttons[BTN_ID_ACTION].held, true);
    expect_bool("any-device JUMP pressed edge", st.buttons[BTN_ID_JUMP].pressed, true);

    deliver(&g_dev[PAD_B], EV_ABS, ABS_X, -32768);
    gamepad_poll(&gm, &st, 0, 0, false);
    expect_int("P2: stick X", gamepad_player(&gm, 1)->axis_lx, -1000);
    expect_int("P1: stick X untouched", gamepad_player(&gm, 0)->axis_lx, 0);
    expect_bool("P2: stick LEFT", p_held(&gm, 1, BTN_ID_LEFT), true);
    expect_bool("P1: no stick LEFT", p_held(&gm, 0, BTN_ID_LEFT), false);
    expect_bool("any-device sees stick LEFT", st.buttons[BTN_ID_LEFT].held, true);
    expect_int("any-device stick X", st.axis_lx, -1000);
    expect_bool("P1: no repeated press edge", gamepad_player(&gm, 0)->buttons[BTN_ID_JUMP].pressed, false);

    printf("\n2. pad A unplugged: pad B stays P2 and keeps its held RUN\n");
    g_dev[PAD_A].present = false;
    gamepad_rescan(&gm);
    gamepad_poll(&gm, &st, 0, 0, false);
    expect_int("present mask: P2 P3", gamepad_player_mask(&gm), 0x6);
    expect_bool("P1 has no pad", gamepad_player(&gm, 0)->gamepad_connected, false);
    expect_bool("P1: JUMP released", p_held(&gm, 0, BTN_ID_JUMP), false);
    expect_bool("P1: JUMP release edge", gamepad_player(&gm, 0)->buttons[BTN_ID_JUMP].released, true);
    expect_bool("P2: RUN still held (seeded per slot)", p_held(&gm, 1, BTN_ID_RUN), true);
    expect_bool("P2: no spurious release edge", gamepad_player(&gm, 1)->buttons[BTN_ID_RUN].released, false);
    expect_bool("any-device: a pad still connected", st.gamepad_connected, true);

    printf("\n3. a new pad takes the free slot, not A's reserved one\n");
    g_dev[PAD_C].present = true;
    gamepad_rescan(&gm);
    gamepad_poll(&gm, &st, 0, 0, false);
    expect_int("present mask: P2 P3 P4", gamepad_player_mask(&gm), 0xe);
    expect_bool("P4 has pad C", gamepad_player(&gm, 3)->gamepad_connected, true);

    printf("\n4. pad A back, LAST in the scan order: P1 again\n");
    g_dev[PAD_A].present = true;
    int order[DEV_COUNT] = { PAD_C, KBD1, PAD_B, KBD0, PAD_A, COMBO, MOUSE0 };
    memcpy(g_order, order, sizeof(order));
    gamepad_rescan(&gm);
    gamepad_poll(&gm, &st, 0, 0, false);
    expect_int("present mask: all four", gamepad_player_mask(&gm), 0xf);
    press(&g_dev[PAD_A], BTN_WEST, true);
    press(&g_dev[PAD_C], BTN_START, true);
    gamepad_poll(&gm, &st, 0, 0, false);
    expect_bool("P1: pad A's ACTION", p_held(&gm, 0, BTN_ID_ACTION), true);
    expect_bool("P4: pad C's PAUSE", p_held(&gm, 3, BTN_ID_PAUSE), true);
    expect_bool("P4: pad A's ACTION not held", p_held(&gm, 3, BTN_ID_ACTION), false);
    expect_bool("P2: still pad B's RUN", p_held(&gm, 1, BTN_ID_RUN), true);
    expect_bool("P1: pad B's RUN not held", p_held(&gm, 0, BTN_ID_RUN), false);
    expect_bool("any-device sees PAUSE", st.buttons[BTN_ID_PAUSE].held, true);
    expect_bool("any-device sees ACTION", st.buttons[BTN_ID_ACTION].held, true);

    printf("\n5. out of range, and app exit clears the reservations\n");
    expect_bool("slot 4 is empty", gamepad_player(&gm, 4)->gamepad_connected, false);
    expect_bool("slot -1 is empty", gamepad_player(&gm, -1)->buttons[BTN_ID_RUN].held, false);
    gamepad_close(&gm);
    devs_reset();
    g_dev[PAD_A].present = false;            /* B first, with A's reservation gone */
    gamepad_init(&gm);
    gamepad_poll(&gm, &st, 0, 0, false);
    expect_bool("after re-init pad B is P1", gamepad_player(&gm, 0)->gamepad_connected, true);
    gamepad_close(&gm);

    pin_tests();
    combo_tests();

    printf("\n%s (%d failure%s)\n", fails ? "FAILED" : "PASSED",
           fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
