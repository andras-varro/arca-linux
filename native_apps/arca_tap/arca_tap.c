/*
 * arca_tap — tap, hold, swipe and drag the touch panel from a script.
 *
 *   arca_tap start            create the virtual touchscreen, point
 *                             /dev/input/touchscreen0 at it, restart the app
 *   arca_tap tap X Y          X Y are LOGICAL screen pixels (a screenshot's)
 *   arca_tap hold X Y MS
 *   arca_tap swipe X1 Y1 X2 Y2 [MS]
 *   arca_tap drag X1 Y1 X2 Y2 HOLD [MS]   press, stay still HOLD ms, then travel
 *   arca_tap status | stop
 *
 * Why a daemon and a re-pointed link: every app opens exactly ONE touch node,
 * /dev/input/touchscreen0, once at startup (touch_init); none scans for another
 * and none reopens. So the virtual device has to be the thing that path names
 * before the app starts, and it has to outlive each command. While it runs the
 * real panel is not read by new apps; `stop` (or REMOTE TAP off in the Control
 * Panel) puts the link back and restarts the app.
 *
 * Gate: the setting "remote_tap" (common/remote_tap.h), off by default, set
 * only on the Control Panel's Input page. When it is off nothing here runs,
 * and a running daemon destroys its device within a second of it turning off.
 *
 * Coordinates: logical -> panel (+ bezel origin) -> raw, the last step by
 * touch_calib_raw_for_panel(), which bisects the production axis map. Landscape
 * only (refused when /opt/games/portrait.mode exists).
 */
#include "common/config.h"
#include "common/framebuffer.h"
#include "common/remote_tap.h"
#include "common/touch_calib.h"
#include "common/touch_input.h"

#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <linux/input.h>
#include <linux/uinput.h>

#ifndef UI_GET_SYSNAME
#define UI_GET_SYSNAME(len) _IOC(_IOC_READ, UINPUT_IOCTL_BASE, 44, len)
#endif

#define LINK_PATH   "/dev/input/touchscreen0"
#define FIFO_PATH   "/var/run/arca-tap.fifo"
#define PID_PATH    "/var/run/arca-tap.pid"
#define ORIG_PATH   "/var/run/arca-tap.orig"
#define READY_PATH  "/var/run/arca-tap.ready"
#define LOG_PATH    "/tmp/arca-tap.log"
#define DAEMON_COPY "/var/run/arca-tapd"
#define APP_INIT    "/etc/init.d/arca-app"
#define RAW_MAX     4095

static volatile sig_atomic_t g_stop;
static void on_signal(int s) { (void)s; g_stop = 1; }

static int fail(const char *msg) {
    fprintf(stderr, "arca_tap: %s\n", msg);
    return 1;
}

static bool enabled_now(void) {
    Config cfg;
    config_init(&cfg);
    config_load(&cfg);
    return remote_tap_enabled(&cfg);
}

static int read_pid(void) {
    FILE *f = fopen(PID_PATH, "r");
    int pid = 0;
    if (f) { if (fscanf(f, "%d", &pid) != 1) pid = 0; fclose(f); }
    return (pid > 1 && kill(pid, 0) == 0) ? pid : 0;
}

/* ---- the symlink ---------------------------------------------------------- */

static int swap_link(const char *target) {
    char tmp[] = LINK_PATH ".arca-tap";
    unlink(tmp);
    if (symlink(target, tmp) != 0) return -1;
    return rename(tmp, LINK_PATH);
}

/* Put the real panel's node back. Safe to call when nothing was attached. */
static void restore_link(void) {
    char t[128];
    FILE *f = fopen(ORIG_PATH, "r");
    if (!f) return;
    if (fgets(t, sizeof(t), f)) {
        t[strcspn(t, "\r\n")] = '\0';
        if (t[0]) swap_link(t);
    }
    fclose(f);
    unlink(ORIG_PATH);
}

static void restart_app(void) {
    if (access(APP_INIT, X_OK) == 0)
        if (system(APP_INIT " restart >/dev/null 2>&1") != 0) { /* reported by the caller's screenshot */ }
}

/* ---- the geometry --------------------------------------------------------- */

typedef struct {
    TouchInput t;
    int view_x, view_y;
} Geo;

static int geo_load(Geo *g) {
    if (access("/opt/games/portrait.mode", F_OK) == 0) return -1;
    memset(g, 0, sizeof(*g));
    g->t.fd = -1;
    g->t.panel_width = 800;  g->t.panel_height = 480;
    g->t.raw_min_x = g->t.raw_min_y = 0;
    g->t.raw_max_x = g->t.raw_max_y = RAW_MAX;
    fb_load_bezel();
    g->view_x = screen_bezel_left;
    g->view_y = screen_bezel_top;
    g->t.calib.bezel_top = screen_bezel_top;  g->t.calib.bezel_bottom = screen_bezel_bottom;
    g->t.calib.bezel_left = screen_bezel_left; g->t.calib.bezel_right = screen_bezel_right;
    touch_load_calibration(&g->t, "/etc/touch_calibration.conf");   /* absent: linear */
    return 0;
}

static int axis_raw(int panel, int v0, int k_lo, int k_hi, int v1, int dim, bool *exact) {
    int lo = v0 < v1 ? v0 : v1, hi = v0 < v1 ? v1 : v0;
    if (lo < 0) lo = 0;
    if (hi > RAW_MAX) hi = RAW_MAX;
    return touch_calib_raw_for_panel(panel, v0, k_lo, k_hi, v1, dim, lo, hi, exact);
}

/* Logical pixel -> raw reading. Returns 0, or -1 for a pixel off the panel. */
static int to_raw(const Geo *g, int lx, int ly, int *rx, int *ry, bool *exact) {
    int px = lx + g->view_x, py = ly + g->view_y;
    if (px < 0 || px >= 800 || py < 0 || py >= 480) return -1;
    bool ex = true, ey = true;
    const TouchInput *t = &g->t;
    *rx = axis_raw(px, t->raw_min_x, t->raw_knot_lo_x, t->raw_knot_hi_x, t->raw_max_x, 800, &ex);
    *ry = axis_raw(py, t->raw_min_y, t->raw_knot_lo_y, t->raw_knot_hi_y, t->raw_max_y, 480, &ey);
    if (exact) *exact = ex && ey;
    return 0;
}

/* ---- the virtual device --------------------------------------------------- */

static void emit(int fd, int type, int code, int value) {
    struct input_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = type; ev.code = code; ev.value = value;
    if (write(fd, &ev, sizeof(ev)) != (ssize_t)sizeof(ev)) { /* device gone; the next command says so */ }
}

static void move(int fd, int rx, int ry) {
    emit(fd, EV_ABS, ABS_X, rx);
    emit(fd, EV_ABS, ABS_Y, ry);
    emit(fd, EV_SYN, SYN_REPORT, 0);
}

/* The order apps depend on: coordinates, then BTN_TOUCH, then the frame. */
static void press(int fd, int rx, int ry) {
    emit(fd, EV_ABS, ABS_X, rx);
    emit(fd, EV_ABS, ABS_Y, ry);
    emit(fd, EV_KEY, BTN_TOUCH, 1);
    emit(fd, EV_SYN, SYN_REPORT, 0);
}

static void lift(int fd) {
    emit(fd, EV_KEY, BTN_TOUCH, 0);
    emit(fd, EV_SYN, SYN_REPORT, 0);
}

static int uinput_create(char *node, size_t node_len) {
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) fd = open("/dev/input/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) return -1;
    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    ioctl(fd, UI_SET_EVBIT, EV_ABS);
    ioctl(fd, UI_SET_EVBIT, EV_SYN);
    ioctl(fd, UI_SET_KEYBIT, BTN_TOUCH);
    ioctl(fd, UI_SET_ABSBIT, ABS_X);
    ioctl(fd, UI_SET_ABSBIT, ABS_Y);
    ioctl(fd, UI_SET_PROPBIT, INPUT_PROP_DIRECT);

    struct uinput_user_dev ud;
    memset(&ud, 0, sizeof(ud));
    /* "panjit" keeps input_scan from ever classing it as a pad or mouse. */
    snprintf(ud.name, sizeof(ud.name), "panjit arca-tap virtual touch");
    ud.id.bustype = BUS_VIRTUAL;
    ud.absmin[ABS_X] = 0; ud.absmax[ABS_X] = RAW_MAX;
    ud.absmin[ABS_Y] = 0; ud.absmax[ABS_Y] = RAW_MAX;
    if (write(fd, &ud, sizeof(ud)) != (ssize_t)sizeof(ud) ||
        ioctl(fd, UI_DEV_CREATE) < 0) {
        close(fd);
        return -1;
    }

    /* Which eventN did the kernel give it: UI_GET_SYSNAME names the inputN, and
     * /sys/devices/virtual/input/inputN/eventM is the node. */
    char sys[64] = "";
    node[0] = '\0';
    if (ioctl(fd, UI_GET_SYSNAME(sizeof(sys)), sys) < 0) sys[0] = '\0';
    for (int tries = 0; sys[0] && tries < 30 && !node[0]; tries++) {
        char dir[128];
        snprintf(dir, sizeof(dir), "/sys/devices/virtual/input/%s", sys);
        DIR *d = opendir(dir);
        struct dirent *e;
        while (d && (e = readdir(d)) != NULL)
            if (strncmp(e->d_name, "event", 5) == 0)
                snprintf(node, node_len, "%s", e->d_name);
        if (d) closedir(d);
        if (!node[0]) usleep(100000);
    }
    if (!node[0]) { ioctl(fd, UI_DEV_DESTROY); close(fd); return -1; }
    return fd;
}

/* ---- commands the daemon runs ---------------------------------------------- */

static void run_cmd(int fd, const Geo *g, const RemoteTapCmd *c) {
    int rx, ry, rx2, ry2;
    if (to_raw(g, c->x1, c->y1, &rx, &ry, NULL) != 0) {
        fprintf(stderr, "arca_tap: (%d,%d) is off the panel\n", c->x1, c->y1);
        return;
    }
    if ((c->kind == RTAP_CMD_SWIPE || c->kind == RTAP_CMD_DRAG) && to_raw(g, c->x2, c->y2, &rx2, &ry2, NULL) != 0) {
        fprintf(stderr, "arca_tap: (%d,%d) is off the panel\n", c->x2, c->y2);
        return;
    }
    if (c->kind == RTAP_CMD_SWIPE || c->kind == RTAP_CMD_DRAG) {
        int n = remote_tap_swipe_steps(c->ms);
        press(fd, rx, ry);
        if (c->kind == RTAP_CMD_DRAG) usleep((useconds_t)c->hold_ms * 1000);   /* still, so the app sees a hold */
        for (int i = 1; i <= n; i++) {
            int lx = remote_tap_lerp(c->x1, c->x2, i, n);
            int ly = remote_tap_lerp(c->y1, c->y2, i, n);
            int mx, my;
            if (to_raw(g, lx, ly, &mx, &my, NULL) == 0) move(fd, mx, my);
            usleep(RTAP_STEP_MS * 1000);
        }
        lift(fd);
    } else {
        press(fd, rx, ry);
        usleep((useconds_t)c->ms * 1000);
        lift(fd);
    }
    usleep(60000);   /* let the app see the release before the next command */
}

static int daemon_main(void) {
    Geo g;
    if (geo_load(&g) != 0) return fail("portrait mode is not supported");
    if (!enabled_now()) return fail("REMOTE TAP is off (Control Panel > Input)");

    char node[32];
    int ufd = uinput_create(node, sizeof(node));
    if (ufd < 0) return fail("cannot create the virtual touchscreen (/dev/uinput missing or uinput.ko not loaded)");

    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    signal(SIGHUP, SIG_IGN);

    /* Remember the real node once; a stale file from a crash is still right. */
    if (access(ORIG_PATH, F_OK) != 0) {
        char t[128];
        ssize_t n = readlink(LINK_PATH, t, sizeof(t) - 1);
        FILE *f = fopen(ORIG_PATH, "w");
        if (f) { if (n > 0) { t[n] = '\0'; fputs(t, f); } fclose(f); }
    }
    unlink(FIFO_PATH);
    if (mkfifo(FIFO_PATH, 0600) != 0) { ioctl(ufd, UI_DEV_DESTROY); close(ufd); return fail("mkfifo failed"); }
    int ffd = open(FIFO_PATH, O_RDWR | O_NONBLOCK);

    usleep(1500000);          /* udev links the new node itself; ours goes last */
    if (swap_link(node) != 0) fprintf(stderr, "arca_tap: cannot re-point %s\n", LINK_PATH);
    FILE *pf = fopen(PID_PATH, "w");
    if (pf) { fprintf(pf, "%d\n", (int)getpid()); fclose(pf); }
    FILE *rf = fopen(READY_PATH, "w");
    if (rf) { fprintf(rf, "%s\n", node); fclose(rf); }
    fprintf(stderr, "arca_tap: virtual touch on /dev/input/%s, %s -> %s\n", node, LINK_PATH, node);

    char buf[256];
    size_t have = 0;
    while (!g_stop) {
        struct pollfd p = { .fd = ffd, .events = POLLIN };
        int pr = poll(&p, 1, 1000);
        if (pr == 0 || (pr < 0 && errno == EINTR)) {
            if (!enabled_now()) { fprintf(stderr, "arca_tap: REMOTE TAP turned off\n"); break; }
            continue;
        }
        if (pr < 0) break;
        ssize_t n = read(ffd, buf + have, sizeof(buf) - 1 - have);
        if (n <= 0) continue;
        have += (size_t)n;
        buf[have] = '\0';
        char *nl;
        char *line = buf;
        while ((nl = strchr(line, '\n')) != NULL) {
            *nl = '\0';
            RemoteTapCmd c;
            if (!enabled_now()) { g_stop = 1; break; }
            geo_load(&g);     /* a recalibration since start is honoured */
            if (remote_tap_parse(line, &c)) run_cmd(ufd, &g, &c);
            else fprintf(stderr, "arca_tap: bad command '%s'\n", line);
            line = nl + 1;
        }
        have = strlen(line);
        memmove(buf, line, have + 1);
        if (have >= sizeof(buf) - 1) have = 0;
    }

    restore_link();
    ioctl(ufd, UI_DEV_DESTROY);
    close(ufd);
    unlink(FIFO_PATH); unlink(PID_PATH); unlink(READY_PATH);
    /* Apps opened the virtual node; with it gone their touch is dead, so they
     * must reopen the real one. */
    restart_app();
    return 0;
}

/* ---- the front end ---------------------------------------------------------- */

/* Does any process hold /dev/input/<node> open? arca-app restart returns before
 * the app has run touch_init, and a tap sent before that reaches nobody. */
static bool node_in_use(const char *node) {
    char want[64], path[300], t[64];
    snprintf(want, sizeof(want), "/dev/input/%s", node);
    DIR *pd = opendir("/proc");
    struct dirent *p;
    bool found = false;
    while (pd && !found && (p = readdir(pd)) != NULL) {
        if (p->d_name[0] < '0' || p->d_name[0] > '9') continue;
        snprintf(path, sizeof(path), "/proc/%s/fd", p->d_name);
        DIR *fd = opendir(path);
        struct dirent *f;
        while (fd && !found && (f = readdir(fd)) != NULL) {
            snprintf(path, sizeof(path), "/proc/%s/fd/%s", p->d_name, f->d_name);
            ssize_t n = readlink(path, t, sizeof(t) - 1);
            if (n > 0) { t[n] = '\0'; found = strcmp(t, want) == 0; }
        }
        if (fd) closedir(fd);
    }
    if (pd) closedir(pd);
    return found;
}

static int copy_self(void) {
    int in = open("/proc/self/exe", O_RDONLY);
    unlink(DAEMON_COPY);      /* a daemon still exiting runs it: O_TRUNC would be ETXTBSY */
    int out = open(DAEMON_COPY, O_WRONLY | O_CREAT | O_TRUNC, 0755);
    char b[4096];
    ssize_t n;
    if (in < 0 || out < 0) return -1;
    while ((n = read(in, b, sizeof(b))) > 0)
        if (write(out, b, (size_t)n) != n) return -1;
    close(in); close(out);
    chmod(DAEMON_COPY, 0755);
    return 0;
}

static int cmd_start(void) {
    if (!enabled_now()) return fail("REMOTE TAP is off: switch it on in Control Panel > Input first");
    if (read_pid()) return fail("already running (arca_tap status)");
    if (access("/dev/uinput", F_OK) != 0 && access("/dev/input/uinput", F_OK) != 0)
        return fail("/dev/uinput is absent: uinput.ko comes with the bluetooth component");
    unlink(READY_PATH);
    pid_t pid = fork();
    if (pid < 0) return fail("fork failed");
    if (pid == 0) {
        setsid();
        int fd = open(LOG_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        int nul = open("/dev/null", O_RDONLY);
        if (nul >= 0) dup2(nul, 0);
        if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); }
        execl(DAEMON_COPY, DAEMON_COPY, "daemon", (char *)NULL);
        _exit(127);
    }
    for (int i = 0; i < 100; i++) {          /* up to 10 s */
        if (access(READY_PATH, F_OK) == 0) {
            char node[32] = "";
            FILE *rf = fopen(READY_PATH, "r");
            if (rf) { if (fscanf(rf, "%31s", node) != 1) node[0] = '\0'; fclose(rf); }
            restart_app();
            for (int j = 0; j < 150 && node[0] && !node_in_use(node); j++) usleep(100000);
            if (node[0] && !node_in_use(node)) {
                fprintf(stderr, "arca_tap: running, but no app opened /dev/input/%s within 15 s\n", node);
                return 1;
            }
            printf("arca_tap: running; app restarted on the virtual touchscreen\n");
            return 0;
        }
        usleep(100000);
    }
    fprintf(stderr, "arca_tap: daemon did not come up; see %s\n", LOG_PATH);
    return 1;
}

static int cmd_stop(void) {
    int pid = read_pid();
    if (pid) {
        kill(pid, SIGTERM);       /* the daemon restores the link and restarts the app */
        /* Wait for the process, not its PID file: that goes before the app
         * restart, which takes several seconds. */
        for (int i = 0; i < 200 && kill(pid, 0) == 0; i++) usleep(100000);
        if (kill(pid, 0) == 0) return fail("daemon still running after 20 s; see " LOG_PATH);
        printf("arca_tap: stopped; app restarted on the real panel\n");
        return 0;
    }
    if (access(ORIG_PATH, F_OK) == 0) {     /* a crashed daemon left the link behind */
        restore_link();
        restart_app();
        printf("arca_tap: not running; link restored\n");
        return 0;
    }
    printf("arca_tap: not running\n");
    return 0;
}

static int cmd_send(int argc, char **argv) {
    char line[160];
    int n = 0;
    for (int i = 1; i < argc; i++)
        n += snprintf(line + n, sizeof(line) - (size_t)n, "%s%s", i > 1 ? " " : "", argv[i]);
    RemoteTapCmd c;
    if (n >= (int)sizeof(line) - 2 || !remote_tap_parse(line, &c))
        return fail("usage: tap X Y | hold X Y MS | swipe X1 Y1 X2 Y2 [MS] | drag X1 Y1 X2 Y2 HOLD [MS]  (logical pixels)");
    if (!enabled_now()) return fail("REMOTE TAP is off: switch it on in Control Panel > Input");
    if (!read_pid()) return fail("not running: arca_tap start");
    Geo g;
    int rx, ry;
    if (geo_load(&g) != 0) return fail("portrait mode is not supported");
    if (to_raw(&g, c.x1, c.y1, &rx, &ry, NULL) != 0) return fail("coordinate is off the panel");
    int fd = open(FIFO_PATH, O_WRONLY | O_NONBLOCK);
    if (fd < 0) return fail("cannot reach the daemon (" FIFO_PATH ")");
    line[n++] = '\n';
    if (write(fd, line, (size_t)n) != n) { close(fd); return fail("write to the daemon failed"); }
    close(fd);
    return 0;   /* queued: the daemon needs ~(ms + 60) to play it out */
}

/* arca-app stop kills everything running out of /opt/games, and start and stop
 * both end in an app restart, so they re-exec from the copy outside it first:
 * run from /opt/games, the restart would kill this process too.
 * Compared by inode, not by path: /var/run is a symlink to volatile/run, so
 * /proc/self/exe never spells DAEMON_COPY, and a string compare made the copy
 * re-stage itself (ETXTBSY) on start and exec itself forever on stop. */
static bool running_from_copy(void) {
    struct stat self, copy;
    if (stat("/proc/self/exe", &self) != 0 || stat(DAEMON_COPY, &copy) != 0) return false;
    return self.st_dev == copy.st_dev && self.st_ino == copy.st_ino;
}

int main(int argc, char **argv) {
    if (argc < 2) return fail("usage: arca_tap start | stop | status | tap X Y | hold X Y MS | swipe X1 Y1 X2 Y2 [MS] | drag X1 Y1 X2 Y2 HOLD [MS]");
    const char *v = argv[1];
    if (strcmp(v, "daemon") == 0) return daemon_main();
    if ((strcmp(v, "start") == 0 || strcmp(v, "stop") == 0) && !running_from_copy()) {
        if (strcmp(v, "start") == 0) {
            if (!enabled_now()) return fail("REMOTE TAP is off: switch it on in Control Panel > Input first");
            if (read_pid()) return fail("already running (arca_tap status)");
            if (copy_self() != 0) return fail("cannot stage the daemon in /var/run");
        }
        if (access(DAEMON_COPY, X_OK) == 0) execv(DAEMON_COPY, argv);
        if (strcmp(v, "start") == 0) return fail("cannot run the daemon copy in /var/run");
    }
    if (strcmp(v, "start") == 0)  return cmd_start();
    if (strcmp(v, "stop") == 0)   return cmd_stop();
    if (strcmp(v, "status") == 0) {
        printf("remote_tap setting: %s\ndaemon: %s\n", enabled_now() ? "on" : "off",
               read_pid() ? "running" : "not running");
        return 0;
    }
    return cmd_send(argc, argv);
}
