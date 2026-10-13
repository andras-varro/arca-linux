/* Host-side regression for the device-announcement volume in gamepad.c.
 *
 * gamepad_rescan() closes every device and re-opens it, on a 5 s timer in all
 * nine apps.  scan_devices() prints "found gamepad ..." inside an `fd < 0`
 * branch that the close it was just handed always makes true, so an unchanged
 * pad is re-announced on every tick — measured on .188 as 1720 identical lines
 * in one session, in the log that a no-microphone audio verification has to
 * read.  What must be true instead: announce a CHANGE, keep the first
 * announcement, and say so once when a device goes away.
 *
 * WARNING: this test needs a real evdev node, because the defect lives in the
 * open/classify path and a regular file cannot classify as a gamepad — the
 * EVIOCGBIT ioctls fail on one.  It therefore synthesises a pad through
 * /dev/uinput, which on this dev host is root-only.  Without it the test
 * SKIPS and says so; it does not pass quietly, because a pass here would mean
 * the announcement behaviour was never measured at all.  To actually reach it,
 * run THIS test as root rather than the whole gate:
 *
 *   wsl.exe -u root -e bash -lc "cd /mnt/c/work/arca/native_apps && \
 *       gcc -Wall -Wextra -Wno-unused-parameter -I common \
 *       -o build/gamepad_announce_test tests/gamepad_announce_test.c \
 *       common/gamepad.c common/input_slots.c common/input_scan.c common/framebuffer.c common/hardware.c \
 *       common/config.c common/touch_input.c -lm && ./build/gamepad_announce_test"
 *
 * Measured 2026-09-08: the whole gate under `wsl.exe -u root` does NOT reach
 * phase 2 on this host.  a host suite exits 2 as root, and
 * phase 2 returns without a word while any harness error stands; phase 3 then
 * fails too, because git refuses a repo it sees as dubiously owned.  Both are
 * artefacts of the shell, not of the repo.
 *
 * Build (this line is the CTEST_ROWS row in tests/run-all.sh):
 *   cd native_apps && gcc -Wall -Wextra -Wno-unused-parameter -I common \
 *       -o build/gamepad_announce_test tests/gamepad_announce_test.c \
 *       common/gamepad.c common/input_slots.c common/input_scan.c common/framebuffer.c common/hardware.c \
 *       common/config.c common/touch_input.c -lm
 *
 * Measured against the pre-fix source, three runs, identical each time:
 * 4 passed, 2 failed.  The two that failed are the defect and its missing other
 * half.  The four that already passed are not evidence of anything on their
 * own, and two of them are deliberately controls against OVER-suppression:
 * "a still-absent pad is not reported again" is vacuous pre-fix (nothing was
 * ever reported), and "a returning pad is announced again" passes pre-fix for
 * the wrong reason (pre-fix announces unconditionally).  They earn their keep
 * only after the fix, where a slot that is never cleared would break the second
 * and a slot cleared too eagerly would break the first.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <time.h>
#include "gamepad.h"

static int pass_n = 0, fail_n = 0;
static int uinput_fd = -1;
static char capture_path[64];

static void ok(const char *what) {
    printf("  \033[0;32mok\033[0m    %s\n", what);
    pass_n++;
}

static void bad(const char *what, const char *detail) {
    printf("  \033[0;31mFAIL\033[0m  %s -- %s\n", what, detail);
    fail_n++;
}

/* -- The synthetic pad --------------------------------------------------- */
/* Only the bits input_classify() tests: EV_ABS with ABS_X/ABS_Y plus EV_KEY
 * with BTN_SOUTH.  The name must not contain "panjit" in any case, which
 * input_classify() filters out. */
static const char *PAD_NAME = "RW Announce Test Pad";

static int pad_create(void) {
    struct uinput_user_dev dev;
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) return -1;

    if (ioctl(fd, UI_SET_EVBIT, EV_KEY) < 0) goto fail;
    if (ioctl(fd, UI_SET_EVBIT, EV_ABS) < 0) goto fail;
    if (ioctl(fd, UI_SET_EVBIT, EV_SYN) < 0) goto fail;
    if (ioctl(fd, UI_SET_KEYBIT, BTN_SOUTH) < 0) goto fail;
    if (ioctl(fd, UI_SET_ABSBIT, ABS_X) < 0) goto fail;
    if (ioctl(fd, UI_SET_ABSBIT, ABS_Y) < 0) goto fail;

    memset(&dev, 0, sizeof(dev));
    snprintf(dev.name, sizeof(dev.name), "%s", PAD_NAME);
    dev.id.bustype = BUS_USB;
    dev.id.vendor = 0x045e;
    dev.id.product = 0x028e;
    dev.id.version = 1;
    dev.absmin[ABS_X] = -32768;
    dev.absmax[ABS_X] = 32767;
    dev.absmin[ABS_Y] = -32768;
    dev.absmax[ABS_Y] = 32767;
    if (write(fd, &dev, sizeof(dev)) != (ssize_t)sizeof(dev)) goto fail;
    if (ioctl(fd, UI_DEV_CREATE) < 0) goto fail;
    return fd;
fail:
    close(fd);
    return -1;
}

static void pad_destroy(void) {
    if (uinput_fd < 0) return;
    ioctl(uinput_fd, UI_DEV_DESTROY);
    close(uinput_fd);
    uinput_fd = -1;
}

/* Is a node carrying PAD_NAME visible under /dev/input right now? */
static int pad_node_present(void) {
    char path[64];
    char name[128];
    int i;
    for (i = 0; i < 32; i++) {
        int fd;
        snprintf(path, sizeof(path), "/dev/input/event%d", i);
        fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;
        name[0] = '\0';
        ioctl(fd, EVIOCGNAME(sizeof(name)), name);
        close(fd);
        if (strcmp(name, PAD_NAME) == 0) return 1;
    }
    return 0;
}

/* UI_DEV_CREATE returns before the node is reliably reachable: measured on this
 * host as a first run that saw no /dev/input at all and graded two assertions
 * green having tested nothing.  Poll instead of assuming, in both directions —
 * a rescan issued while a destroyed pad is still visible measures nothing
 * either.  Returns 0 if the state never arrived. */
static int pad_wait_for(int want_present) {
    int tries;
    for (tries = 0; tries < 400; tries++) {   /* 400 x 5 ms = 2 s */
        struct timespec ts = { 0, 5 * 1000 * 1000 };
        if (pad_node_present() == want_present) return 1;
        nanosleep(&ts, NULL);
    }
    return 0;
}

static void cleanup(void) {
    pad_destroy();
    if (capture_path[0]) unlink(capture_path);
}

/* -- Counting what scan_devices printed ---------------------------------- */
/* gamepad.c writes these with printf(), not the Logger, so the only way to see
 * them is to hold stdout aside for the duration of the call. */
static int saved_stdout = -1;

static void capture_begin(void) {
    int fd;
    fflush(stdout);
    saved_stdout = dup(STDOUT_FILENO);
    fd = open(capture_path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        perror("capture open");
        cleanup();
        exit(2);
    }
    dup2(fd, STDOUT_FILENO);
    close(fd);
}

/* Returns the number of captured lines containing `needle`, and puts the whole
 * capture in `all` so a failure can name what was actually printed. */
static int capture_end(const char *needle, char *all, size_t all_sz) {
    FILE *f;
    char line[512];
    int n = 0;

    fflush(stdout);
    dup2(saved_stdout, STDOUT_FILENO);
    close(saved_stdout);
    saved_stdout = -1;

    all[0] = '\0';
    f = fopen(capture_path, "r");
    if (!f) return -1;
    while (fgets(line, sizeof(line), f)) {
        size_t used = strlen(all);
        if (used + strlen(line) + 1 < all_sz)
            memcpy(all + used, line, strlen(line) + 1);
        if (strstr(line, needle)) n++;
    }
    fclose(f);
    return n;
}

static void expect_count(const char *what, const char *needle, int want, int got,
                         const char *all) {
    char detail[768];
    if (got == want) {
        ok(what);
        return;
    }
    snprintf(detail, sizeof(detail),
             "wanted %d line(s) matching \"%s\", got %d; capture was: %s",
             want, needle, got, all[0] ? all : "(empty)");
    bad(what, detail);
}

int main(void) {
    char all[2048];
    GamepadManager gm;
    int n, rc;

    printf("gamepad announcement volume\n");
    snprintf(capture_path, sizeof(capture_path), "/tmp/arca_gp_announce_%d", (int)getpid());

    uinput_fd = pad_create();
    if (uinput_fd < 0) {
        printf("  \033[1;33mskip\033[0m  /dev/uinput unusable (%s) -- the announcement\n",
               strerror(errno));
        printf("        behaviour is UNMEASURED here.  It needs a real evdev node, so\n");
        printf("        run the gate as root:  wsl.exe -u root bash tests/run-all.sh\n");
        return 0;
    }

    /* 1. The first announcement is kept -- it is genuinely useful. */
    if (!pad_wait_for(1)) {
        printf("  the synthetic pad never became visible under /dev/input --\n");
        printf("  NOTHING below can be graded, so this is a harness error, not a\n");
        printf("  verdict on gamepad.c.\n");
        cleanup();
        return 2;
    }
    capture_begin();
    rc = gamepad_init(&gm);
    n = capture_end("found gamepad", all, sizeof(all));
    if (rc != 0)
        bad("gamepad_init() succeeded", "non-zero return");
    else
        ok("gamepad_init() succeeded");
    if (gm.gamepad_fd < 0) {
        printf("  the pad was visible but gamepad_init() did not bind it, so every\n");
        printf("  assertion below would pass on an empty capture.  Refusing to grade.\n");
        printf("  What init printed: %s\n", all[0] ? all : "(nothing)");
        gamepad_close(&gm);
        cleanup();
        return 2;
    }
    expect_count("init announces the pad once", "found gamepad", 1, n, all);

    /* 2. Two rescans of an UNCHANGED pad announce nothing.  This is the
     *    defect: pre-fix each rescan prints, because gamepad_close() has just
     *    forced the fd < 0 that guards the printf. */
    capture_begin();
    gamepad_rescan(&gm);
    gamepad_rescan(&gm);
    n = capture_end("found gamepad", all, sizeof(all));
    expect_count("two rescans of the same pad stay silent", "found gamepad", 0, n, all);

    /* 3. A pad going away is a change, and is said once. */
    pad_destroy();
    if (!pad_wait_for(0)) {
        printf("  the destroyed pad is still visible under /dev/input; a rescan now\n");
        printf("  would measure nothing.  Refusing to grade the unplug cases.\n");
        gamepad_close(&gm);
        cleanup();
        return 2;
    }
    capture_begin();
    gamepad_rescan(&gm);
    n = capture_end("no longer present", all, sizeof(all));
    expect_count("an unplugged pad is reported once", "no longer present", 1, n, all);

    capture_begin();
    gamepad_rescan(&gm);
    n = capture_end("no longer present", all, sizeof(all));
    expect_count("a still-absent pad is not reported again", "no longer present", 0, n, all);

    /* 4. A pad coming back re-announces, because step 3 cleared what was
     *    remembered.  Without that clearing this would stay silent and the
     *    operator would have no line saying the pad was usable again. */
    uinput_fd = pad_create();
    if (uinput_fd < 0) {
        bad("the pad could be recreated", "second /dev/uinput open failed");
    } else if (!pad_wait_for(1)) {
        bad("the pad could be recreated", "the second node never became visible");
    } else {
        capture_begin();
        gamepad_rescan(&gm);
        n = capture_end("found gamepad", all, sizeof(all));
        expect_count("a returning pad is announced again", "found gamepad", 1, n, all);
    }

    gamepad_close(&gm);
    cleanup();

    printf("\n  %d passed, %d failed\n", pass_n, fail_n);
    return fail_n == 0 ? 0 : 1;
}
