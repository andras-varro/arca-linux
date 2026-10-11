/* ssh_page.c — control_panel's SSH page: who may log in over the network, and
 * what this unit is called there.
 *
 * Exposed only as cp_ssh_page (cp_page.h).  Four settings, read on entry and
 * after every change:
 *
 *   MODE        OFF / KEY ONLY / KEY+PASS.  A password mode never turns key
 *               login off (sys_ssh_set_mode() rewrites an active
 *               PubkeyAuthentication no to yes).  OFF is the marker file
 *               /etc/ssh/sshd_off plus a stopped sshd; the init script honours
 *               the marker at boot.  Leaving OFF removes the marker and starts
 *               sshd; KEY ONLY <-> KEY+PASS rewrites sshd_config and sends the
 *               listener SIGHUP, so open sessions survive.
 *   ROOT LOGIN  PermitRootLogin prohibit-password <-> yes.  Separate from MODE:
 *               a password only reaches root when both allow it.
 *   PASSWORD    root's password, typed on the on-screen keyboard (twice) in
 *               run_fullscreen and hashed by BusyBox `mkpasswd -m sha512`, fed on
 *               its stdin; the hash replaces the root line's password field in
 *               /etc/shadow by temp file and rename.  The password is never
 *               logged, printed or put on a command line, and is wiped after use.
 *   HOST NAME   typed on the keyboard, confirmed, then applied at once by
 *               /usr/sbin/set-hostname, whose one-line stderr is shown on refusal.
 *
 * Every change is confirmed and passes sys_ssh_refuse_change() first (the lockout guard:
 * a key mode needs a plausible authorized key, a password mode or root login yes
 * needs a password).  sshd_config is written as sshd_config.new, fsynced, checked
 * with `sshd -t -f`, then renamed over the original.
 */
#include "cp_page.h"
#include "cp_ui.h"
#include "cp_exec.h"
#include "sys_settings.h"
#include "../common/common.h"
#include "../common/keyboard.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define SSHD_CONFIG      "/etc/ssh/sshd_config"
#define SSHD_BIN         "/usr/sbin/sshd"
#define SSHD_INIT        "/etc/init.d/sshd"
#define SSHD_PIDFILE     "/var/run/sshd.pid"
#define SSHD_OFF_MARKER  "/etc/ssh/sshd_off"
#define AUTH_KEYS        "/home/root/.ssh/authorized_keys"
#define SHADOW           "/etc/shadow"
#define MKPASSWD_BIN     "/usr/bin/mkpasswd"
#define SET_HOSTNAME_BIN "/usr/sbin/set-hostname"
#define SSH_BUF          16384          /* a sshd_config is a few KB; a longer one is refused */
#define SHADOW_BUF       8192
#define PW_MIN           6
#define PW_MAX           32
#define HOST_MAX         63
#define NET_REFRESH_MS   2000

/* ── State ──────────────────────────────────────────────────────────────── */

static SshMode      mode = SSH_MODE_UNKNOWN;     /* in force (OFF from the marker) */
static SshMode      cfg_mode = SSH_MODE_UNKNOWN; /* what sshd_config says */
static SshRootLogin root = SSH_ROOT_UNKNOWN;
static bool         has_key, has_pw;
static char         host[HOST_MAX + 1];
static char         ip[20];
static uint32_t     sampled_ms;

/* What the open confirmation will apply. */
static SshMode      pend_mode;
static SshRootLogin pend_root;
static bool         pend_is_root;       /* the change is the root login, not the mode */
static char         pend_host[HOST_MAX + 1];

/* What input() queued for run_fullscreen. */
static bool         pw_queued, host_queued;

/* Reads a whole file into buf (NUL-terminated); the length, or -1 if it cannot
 * be opened or does not fit (a cut file must never be rewritten). */
static int read_whole(const char *path, char *buf, size_t cap) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    size_t n = fread(buf, 1, cap - 1, f);
    bool more = fgetc(f) != EOF;
    fclose(f);
    if (more) return -1;
    buf[n] = '\0';
    return (int)n;
}

static void wipe(void *p, size_t n) {
    volatile unsigned char *v = p;
    while (n--) *v++ = 0;
}

static void read_ssh(void) {
    static char cfg[SSH_BUF], keys[4096], shadow[SHADOW_BUF];
    bool ok = read_whole(SSHD_CONFIG, cfg, sizeof(cfg)) >= 0;
    cfg_mode = ok ? sys_ssh_mode(cfg) : SSH_MODE_UNKNOWN;
    root = ok ? sys_ssh_root_login(cfg) : SSH_ROOT_UNKNOWN;
    mode = sys_ssh_effective(cfg_mode, access(SSHD_OFF_MARKER, F_OK) == 0);
    has_key = read_whole(AUTH_KEYS, keys, sizeof(keys)) >= 0 && sys_authkeys_plausible(keys) &&
              sys_authkeys_safe(AUTH_KEYS, 0);
    has_pw = read_whole(SHADOW, shadow, sizeof(shadow)) >= 0 && sys_shadow_root_has_password(shadow);
    wipe(shadow, sizeof(shadow));
}

/* The first up, non-loopback, non-link-local IPv4 address. */
static void read_net(void) {
    char hn[HOST_MAX + 1];
    if (gethostname(hn, sizeof(hn)) == 0) { hn[HOST_MAX] = '\0'; snprintf(host, sizeof(host), "%s", hn); }
    else snprintf(host, sizeof(host), "UNKNOWN");
    snprintf(ip, sizeof(ip), "NO IPV4");
    struct ifaddrs *ifa0;
    if (getifaddrs(&ifa0) != 0) return;
    for (struct ifaddrs *a = ifa0; a; a = a->ifa_next) {
        if (!a->ifa_addr || a->ifa_addr->sa_family != AF_INET) continue;
        if ((a->ifa_flags & IFF_LOOPBACK) || !(a->ifa_flags & IFF_UP)) continue;
        struct in_addr ad = ((struct sockaddr_in *)a->ifa_addr)->sin_addr;
        if ((ntohl(ad.s_addr) >> 16) == 0xA9FE) continue;            /* 169.254/16 */
        inet_ntop(AF_INET, &ad, ip, sizeof(ip));
        break;
    }
    freeifaddrs(ifa0);
}

/* ── Layout ─────────────────────────────────────────────────────────────── */

#define SSH_HEADER_H   26
#define SSH_ROW_H      28           /* an info row without a button */
#define SSH_BAND_H     36           /* an info row with a button at its right */
#define SSH_BTN_H      34
#define SSH_MODE_BTN_H 40
#define SSH_GAP         6
#define SSH_BTN_W     104
#define SSH_VALUE_GAP   8
#define SSH_MODE_N      3

static const struct { SshMode mode; const char *label; } mode_choice[SSH_MODE_N] = {
    { SSH_MODE_OFF,      "OFF" },
    { SSH_MODE_KEY_ONLY, "KEY ONLY" },
    { SSH_MODE_PASSWORD, "KEY+PASS" },
};
#define NOTE_PW_ROOT  "PASSWORD NEEDS ROOT LOGIN: ALLOW, ELSE ONLY KEYS WORK"
#define NOTE_OFF      "SSH IS OFF, AND STAYS OFF AFTER A REBOOT"

static Button mode_btn[SSH_MODE_N], root_btn, pw_btn, host_btn;
static int sec_ssh_y, mode_y, root_y, pw_y, sec_net_y, host_y, hostval_y, ip_y, note_y;
static int vx;                      /* the value column: past the widest label */

static const char *mode_name(SshMode m) {
    return m == SSH_MODE_OFF ? "OFF" : m == SSH_MODE_KEY_ONLY ? "KEY ONLY"
         : m == SSH_MODE_PASSWORD ? "KEY + PASSWORD" : "UNKNOWN";
}

static uint32_t mode_color(SshMode m) {
    return m == SSH_MODE_OFF ? COLOR_LABEL : m == SSH_MODE_KEY_ONLY ? COLOR_GREEN
         : m == SSH_MODE_PASSWORD ? COLOR_YELLOW : COLOR_ORANGE;
}

static const char *root_name(SshRootLogin r) {
    return r == SSH_ROOT_KEY ? "KEY ONLY" : r == SSH_ROOT_PASSWORD ? "PASSWORD" : "UNKNOWN";
}

/* The buttons that show state: the chosen mode green, the others grey; each of
 * ROOT LOGIN and PASSWORD offers what a tap does. */
static void refresh_buttons(void) {
    for (int i = 0; i < SSH_MODE_N; i++)
        button_set_colors(&mode_btn[i], mode_choice[i].mode == mode ? RGB(0, 130, 70) : RGB(80, 80, 80),
                          COLOR_WHITE, BTN_COLOR_HIGHLIGHT);
    button_set_text(&root_btn, root == SSH_ROOT_PASSWORD ? "KEY ONLY" : "ALLOW");
    button_set_text(&pw_btn, has_pw ? "CHANGE" : "SET");
}

static void ssh_page_load(const Config *cfg) {
    (void)cfg;
    read_ssh();
    read_net();
}

static void ssh_page_enter(void) {
    read_ssh();
    read_net();
    refresh_buttons();
    sampled_ms = get_time_ms();
}

static void ssh_page_layout(void) {
    int y = CONTENT_Y + 6;
    vx = CONTENT_LEFT + 10 + text_measure_width("ROOT LOGIN:", 2) + SSH_VALUE_GAP;

    sec_ssh_y = y;  y += SSH_HEADER_H;
    mode_y = y;     y += SSH_ROW_H;
    int bw = (CONTENT_WIDTH - (SSH_MODE_N - 1) * SSH_GAP) / SSH_MODE_N;
    for (int i = 0; i < SSH_MODE_N; i++)
        button_init_full(&mode_btn[i], CONTENT_LEFT + i * (bw + SSH_GAP), y, bw, SSH_MODE_BTN_H,
                         mode_choice[i].label, RGB(80, 80, 80), COLOR_WHITE, BTN_COLOR_HIGHLIGHT, 2);
    y += SSH_MODE_BTN_H + SSH_GAP;
    root_y = y;
    button_init_full(&root_btn, CONTENT_RIGHT - SSH_BTN_W, y, SSH_BTN_W, SSH_BTN_H, "ALLOW",
                     RGB(0, 110, 60), COLOR_WHITE, BTN_COLOR_HIGHLIGHT, 2);
    y += SSH_BAND_H;
    pw_y = y;
    button_init_full(&pw_btn, CONTENT_RIGHT - SSH_BTN_W, y, SSH_BTN_W, SSH_BTN_H, "SET",
                     RGB(0, 110, 60), COLOR_WHITE, BTN_COLOR_HIGHLIGHT, 2);
    y += SSH_BAND_H;
    note_y = y;     y += 8 + SSH_GAP;               /* one scale-1 line */
    y += SSH_GAP;
    sec_net_y = y;  y += SSH_HEADER_H;
    host_y = y;
    button_init_full(&host_btn, CONTENT_RIGHT - SSH_BTN_W, y, SSH_BTN_W, SSH_BTN_H, "RENAME",
                     RGB(0, 110, 60), COLOR_WHITE, BTN_COLOR_HIGHLIGHT, 2);
    y += SSH_BAND_H;
    hostval_y = y;  y += SSH_ROW_H;                 /* the name on its own line: it can be 63 long */
    ip_y = y;       y += SSH_ROW_H;
    refresh_buttons();

    /* ⚠️ THE RECEIPT, in the settings stack's shape.  Everything hangs off
     * CONTENT_Y, which comes from a per-unit touch inset, so a button pushed past
     * the touchable rect looks fine in a screenshot and is dead to a finger.  The
     * bottom is the IP row; the right edge is the widest of the buttons and the
     * values and notes, each measured at its widest string as drawn.  A value
     * that would run under its button is a clash; the host name is cut by
     * fit_value() at CONTENT_RIGHT, and the receipt counts it. */
    {
        char cut[160];
        int bottom = y - CONTENT_Y;
        int right = root_btn.x + root_btn.width;
        const char *clash = NULL;
        int w = vx + text_measure_width("KEY + PASSWORD", 2);
        if (w > right) right = w;
        w = vx + text_measure_width("PASSWORD", 2);
        if (w + 8 > root_btn.x) clash = "ROOT LOGIN VALUE";
        w = vx + text_measure_width("NOT SET", 2);
        if (w + 8 > pw_btn.x) clash = "PASSWORD VALUE";
        w = CONTENT_LEFT + 10 + text_measure_width("HOST NAME:", 2);
        if (w + 8 > host_btn.x) clash = "HOST LABEL";
        w = vx + text_measure_width("255.255.255.255", 2);
        if (w > right) right = w;
        w = CONTENT_LEFT + text_measure_width(NOTE_PW_ROOT, 1);
        if (w > right) right = w;
        w = CONTENT_LEFT + text_measure_width(NOTE_OFF, 1);
        if (w > right) right = w;
        for (int i = 0; i < SSH_MODE_N; i++)
            if (text_measure_width(mode_choice[i].label, 2) + 8 > mode_btn[i].width) clash = "MODE BUTTON";
        if (text_measure_width("RENAME", 2) + 8 > host_btn.width) clash = "RENAME BUTTON";
        int clipped = fit_value(host, CONTENT_LEFT + 10, 2, cut, sizeof(cut)) ? 1 : 0;
        w = CONTENT_LEFT + 10 + text_measure_width(cut, 2);
        if (w > right) right = w;
        const char *verdict = bottom > CONTENT_H     ? "⚠ PAST CONTENT BOTTOM"
                            : right  > CONTENT_RIGHT ? "⚠ PAST CONTENT RIGHT"
                            : clash                  ? "⚠ OVERLAPS A BUTTON"
                            : "fits";
        printf("control_panel: ssh stack %s — bottom +%d of CONTENT_H %d, "
               "right %d of CONTENT_RIGHT %d, %d host name cut%s%s (safe %dx%d, %s)\n",
               verdict, bottom, CONTENT_H, right, CONTENT_RIGHT, clipped,
               clash ? ", clash " : "", clash ? clash : "",
               SCREEN_SAFE_WIDTH, SCREEN_SAFE_HEIGHT,
               CONTENT_WIDTH < 600 ? "portrait" : "landscape");
    }
}

/* ── Draw ───────────────────────────────────────────────────────────────── */

static void ssh_page_draw(Framebuffer *fb) {
    char cut[160];
    draw_section_header(fb, sec_ssh_y, "SSH ACCESS");
    draw_info_row_at(fb, mode_y, "MODE:", mode_name(mode), mode_color(mode), vx);
    for (int i = 0; i < SSH_MODE_N; i++) button_draw(fb, &mode_btn[i]);

    draw_info_row_at(fb, root_y + 4, "ROOT LOGIN:", root_name(root),
                     root == SSH_ROOT_PASSWORD ? COLOR_YELLOW : root == SSH_ROOT_KEY ? COLOR_GREEN : COLOR_ORANGE, vx);
    button_draw(fb, &root_btn);
    draw_info_row_at(fb, pw_y + 4, "PASSWORD:", has_pw ? "SET" : "NOT SET",
                     has_pw ? COLOR_GREEN : COLOR_LABEL, vx);
    button_draw(fb, &pw_btn);
    if (mode == SSH_MODE_OFF)
        fb_draw_text(fb, CONTENT_LEFT, note_y, NOTE_OFF, COLOR_LABEL, 1);
    else if (mode == SSH_MODE_PASSWORD && root != SSH_ROOT_PASSWORD)
        fb_draw_text(fb, CONTENT_LEFT, note_y, NOTE_PW_ROOT, COLOR_YELLOW, 1);

    draw_section_header(fb, sec_net_y, "THIS UNIT");
    fb_draw_text(fb, CONTENT_LEFT + 10, host_y + 8, "HOST NAME:", COLOR_LABEL, 2);
    button_draw(fb, &host_btn);
    fit_value(host, CONTENT_LEFT + 10, 2, cut, sizeof(cut));
    fb_draw_text(fb, CONTENT_LEFT + 10, hostval_y, cut, COLOR_WHITE, 2);
    draw_info_row_at(fb, ip_y, "IP:", ip, strcmp(ip, "NO IPV4") ? COLOR_WHITE : COLOR_ORANGE, vx);
}

/* ── Applying ───────────────────────────────────────────────────────────── */

static bool run_cmd(char *const argv[]) {
    return cp_exec(argv, NULL, NULL, 0, NULL, 0) == 0;
}

/* Writes data to path (created 0600 then chmod'ed to perm), fsynced. */
static bool write_file(const char *path, const char *data, size_t len, mode_t perm) {
    unlink(path);
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) return false;
    bool ok = write(fd, data, len) == (ssize_t)len && fchmod(fd, perm) == 0 && fsync(fd) == 0;
    ok = (close(fd) == 0) && ok;
    if (!ok) unlink(path);
    return ok;
}

static mode_t mode_of(const char *path, mode_t dflt) {
    struct stat st;
    return stat(path, &st) == 0 ? (st.st_mode & 0777) : dflt;
}

/* The pid of the live sshd listener named in its pid file, or 0.  The pid must
 * be a live sshd (its cmdline says so) before anything is signalled. */
static long sshd_pid(void) {
    char line[32], path[48], cmd[160];
    long pid = 0;
    if (read_file_line(SSHD_PIDFILE, line, sizeof(line)) != 0 ||
        sscanf(line, "%ld", &pid) != 1 || pid <= 1) return 0;
    snprintf(path, sizeof(path), "/proc/%ld/cmdline", pid);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    size_t n = fread(cmd, 1, sizeof(cmd) - 1, f);
    fclose(f);
    cmd[n] = '\0';
    return strstr(cmd, "sshd") != NULL ? pid : 0;
}

/* SIGHUP the listener, as /etc/init.d/sshd reload does: it re-execs itself and
 * re-reads the config; open sessions are separate processes and stay.  False
 * when none is running. */
static bool reload_sshd(void) {
    long pid = sshd_pid();
    return pid > 0 && kill((pid_t)pid, SIGHUP) == 0;
}

/* Applies pend_mode and pend_root.  pend_mode UNKNOWN means "leave the mode
 * alone" (a root-login change).  Every failure before the sshd_config rename
 * leaves the old file untouched. */
static void apply_ssh(Config *cfg) {
    (void)cfg;
    static char cur[SSH_BUF], a[SSH_BUF + 128], b[SSH_BUF + 256];
    const char *tmp = SSHD_CONFIG ".new";

    if (read_whole(SSHD_CONFIG, cur, sizeof(cur)) < 0) { cp_status("SSHD CONFIG UNREADABLE, NOT CHANGED", false); return; }
    const char *src = cur;
    if (pend_mode == SSH_MODE_KEY_ONLY || pend_mode == SSH_MODE_PASSWORD) {
        if (sys_ssh_set_mode(src, pend_mode, a, sizeof(a)) < 0) { cp_status("SSHD CONFIG TOO LARGE, NOT CHANGED", false); return; }
        src = a;
    }
    if (pend_root != SSH_ROOT_UNKNOWN) {
        if (sys_ssh_set_root_login(src, pend_root, b, sizeof(b)) < 0) { cp_status("SSHD CONFIG TOO LARGE, NOT CHANGED", false); return; }
        src = b;
    }
    if (strcmp(src, cur) != 0) {
        if (!write_file(tmp, src, strlen(src), mode_of(SSHD_CONFIG, 0644))) { cp_status("SSHD CONFIG WRITE FAILED, NOT CHANGED", false); return; }
        char *const test_argv[] = { SSHD_BIN, "-t", "-f", (char *)tmp, NULL };
        if (!run_cmd(test_argv)) { unlink(tmp); cp_status("SSHD REJECTED THE CONFIG, NOT CHANGED", false); return; }
        if (rename(tmp, SSHD_CONFIG) != 0) { unlink(tmp); cp_status("SSHD CONFIG RENAME FAILED, NOT CHANGED", false); return; }
    }

    bool ok = true;
    char *const stop_argv[] = { SSHD_INIT, "stop", NULL };
    char *const start_argv[] = { SSHD_INIT, "start", NULL };
    if (pend_mode == SSH_MODE_OFF) {
        /* The marker first: if the stop fails, a reboot still leaves sshd off. */
        int fd = open(SSHD_OFF_MARKER, O_WRONLY | O_CREAT, 0644);
        bool marked = fd >= 0 && fsync(fd) == 0;
        if (fd >= 0) close(fd);
        if (!marked) { cp_status("OFF MARKER WRITE FAILED, SSHD LEFT RUNNING", false); read_ssh(); refresh_buttons(); return; }
        run_cmd(stop_argv);
        ok = sshd_pid() == 0;
    } else if (pend_mode == SSH_MODE_UNKNOWN && access(SSHD_OFF_MARKER, F_OK) == 0) {
        /* a root-login change while OFF: the file is written, nothing runs */
    } else {
        if (unlink(SSHD_OFF_MARKER) != 0 && access(SSHD_OFF_MARKER, F_OK) == 0) {
            cp_status("OFF MARKER NOT REMOVED, SSHD NOT STARTED", false);
            read_ssh(); refresh_buttons();
            return;
        }
        ok = reload_sshd() || run_cmd(start_argv);
    }
    read_ssh();
    refresh_buttons();
    if (!ok) { cp_status("FILE CHANGED, SSHD RELOAD FAILED (REBOOT APPLIES)", false); return; }
    cp_status(pend_is_root ? (pend_root == SSH_ROOT_PASSWORD ? "ROOT LOGIN: PASSWORD ALLOWED" : "ROOT LOGIN: KEY ONLY")
              : pend_mode == SSH_MODE_OFF ? "SSH OFF"
              : pend_mode == SSH_MODE_KEY_ONLY ? "SSH: KEY ONLY" : "SSH: KEY + PASSWORD", true);
}

/* Sets root's password: mkpasswd hashes it from its stdin, the hash replaces the
 * root line's field in /etc/shadow.  Returns NULL, or a short reason.  pw is not
 * copied anywhere but the pipe. */
static const char *set_root_password(const char *pw) {
    static char shadow[SHADOW_BUF], next[SHADOW_BUF + 512];
    char hash[300];
    const char *tmp = SHADOW ".new";
    char line[PW_MAX + 2];
    snprintf(line, sizeof(line), "%s\n", pw);
    char *const argv[] = { "mkpasswd", "-m", "sha512", NULL };
    int r = access(MKPASSWD_BIN, X_OK) == 0 ? cp_exec(argv, line, hash, sizeof(hash), NULL, 0) : -2;
    wipe(line, sizeof(line));
    if (r == -2) { wipe(hash, sizeof(hash)); return "MKPASSWD NOT INSTALLED"; }
    char *nl = strchr(hash, '\n');
    if (nl) *nl = '\0';
    if (r != 0 || !sys_hash_plausible(hash)) { wipe(hash, sizeof(hash)); return "HASHING FAILED, PASSWORD UNCHANGED"; }
    if (read_whole(SHADOW, shadow, sizeof(shadow)) < 0) { wipe(hash, sizeof(hash)); return "SHADOW UNREADABLE, NOT CHANGED"; }
    int len = sys_shadow_set_root(shadow, hash, next, sizeof(next));
    wipe(shadow, sizeof(shadow));
    wipe(hash, sizeof(hash));
    if (len < 0) return "NO ROOT LINE IN SHADOW, NOT CHANGED";
    bool ok = write_file(tmp, next, (size_t)len, mode_of(SHADOW, 0600));
    wipe(next, sizeof(next));
    if (!ok) return "SHADOW WRITE FAILED, NOT CHANGED";
    if (rename(tmp, SHADOW) != 0) { unlink(tmp); return "SHADOW RENAME FAILED, NOT CHANGED"; }
    return NULL;
}

static void apply_host(Config *cfg) {
    (void)cfg;
    char err[160], *nl;
    char *const argv[] = { SET_HOSTNAME_BIN, pend_host, NULL };
    if (access(SET_HOSTNAME_BIN, X_OK) != 0) { cp_status("SET-HOSTNAME NOT INSTALLED", false); return; }
    int r = cp_exec(argv, NULL, NULL, 0, err, sizeof(err));
    if (r != 0) {
        if ((nl = strchr(err, '\n'))) *nl = '\0';
        char msg[64];
        for (char *c = err; *c; c++) *c = (char)toupper((unsigned char)*c);
        snprintf(msg, sizeof(msg), "%.50s", err[0] ? err : "SET-HOSTNAME FAILED");
        cp_status(msg, false);
        return;
    }
    read_net();
    char msg[64];
    snprintf(msg, sizeof(msg), "HOST NAME SET: %.40s", host);
    cp_status(msg, true);
}

/* ── Input ──────────────────────────────────────────────────────────────── */

static void ask_mode(SshMode target) {
    read_ssh();     /* a key or password can arrive over SSH while the page is open */
    if (target == mode) { cp_status("ALREADY SET", true); return; }
    /* a root login sshd refuses outright (PermitRootLogin no) is judged as key only */
    SshRootLogin r = root == SSH_ROOT_UNKNOWN ? SSH_ROOT_KEY : root;
    const char *why = target == SSH_MODE_OFF ? NULL
                    : sys_ssh_refuse_change(mode, r, target, r, has_key, has_pw);
    if (why) {
        char msg[64];
        snprintf(msg, sizeof(msg), "REFUSED: %s", why);
        cp_status(msg, false);
        return;
    }
    pend_mode = target;
    pend_root = SSH_ROOT_UNKNOWN;       /* the file's root login is left as it is */
    pend_is_root = false;
    if (target == SSH_MODE_OFF)
        cp_confirm("SSH OFF", "SSHD STOPS NOW AND STAYS OFF\nOPEN SESSIONS ARE KEPT", "TURN OFF", apply_ssh);
    else if (target == SSH_MODE_KEY_ONLY)
        cp_confirm("SSH KEY ONLY", "PASSWORD LOGINS ARE REFUSED\nOPEN SESSIONS ARE KEPT", "APPLY", apply_ssh);
    else
        cp_confirm("SSH KEY + PASSWORD", "KEY LOGIN STAYS ON\nOPEN SESSIONS ARE KEPT", "ALLOW", apply_ssh);
}

static void ask_root(void) {
    read_ssh();
    SshRootLogin target = root == SSH_ROOT_PASSWORD ? SSH_ROOT_KEY : SSH_ROOT_PASSWORD;
    /* judged against the mode in force; OFF or an unreadable mode only has the
     * root-login rule to meet, the mode's own is met when it is chosen */
    SshMode m = (mode == SSH_MODE_UNKNOWN) ? SSH_MODE_OFF : mode;
    SshRootLogin r = root == SSH_ROOT_UNKNOWN ? SSH_ROOT_KEY : root;
    const char *why = sys_ssh_refuse_change(mode, r, m, target, has_key, has_pw);
    if (why) {
        char msg[64];
        snprintf(msg, sizeof(msg), "REFUSED: %s", why);
        cp_status(msg, false);
        return;
    }
    pend_mode = SSH_MODE_UNKNOWN;       /* the mode and the service are left alone */
    pend_root = target;
    pend_is_root = true;
    if (target == SSH_ROOT_PASSWORD)
        cp_confirm("ALLOW ROOT PASSWORD", "ROOT MAY LOG IN BY PASSWORD\nIF THE MODE ALLOWS IT", "ALLOW", apply_ssh);
    else
        cp_confirm("ROOT LOGIN KEY ONLY", "ROOT NEEDS A KEY TO LOG IN", "APPLY", apply_ssh);
}

static CpPageResult ssh_page_input(Config *cfg, int tx, int ty, bool touching, uint32_t now) {
    (void)cfg;
    for (int i = 0; i < SSH_MODE_N; i++)
        if (button_update(&mode_btn[i], tx, ty, touching, now)) {
            ask_mode(mode_choice[i].mode);
            return CP_PAGE_REDRAW;
        }
    if (button_update(&root_btn, tx, ty, touching, now)) {
        ask_root();
        return CP_PAGE_REDRAW;
    }
    if (button_update(&pw_btn, tx, ty, touching, now)) {
        pw_queued = true;
        return CP_PAGE_FULLSCREEN;
    }
    if (button_update(&host_btn, tx, ty, touching, now)) {
        host_queued = true;
        return CP_PAGE_FULLSCREEN;
    }
    if (now - sampled_ms >= NET_REFRESH_MS) {
        sampled_ms = now;
        char old_ip[sizeof(ip)], old_host[sizeof(host)];
        memcpy(old_ip, ip, sizeof(ip));
        memcpy(old_host, host, sizeof(host));
        read_net();
        if (memcmp(old_ip, ip, sizeof(ip)) != 0 || memcmp(old_host, host, sizeof(host)) != 0)
            return CP_PAGE_REDRAW;
    }
    return CP_PAGE_IDLE;
}

/* ── Full-screen: the keyboard ──────────────────────────────────────────── */

static void run_password(Framebuffer *fb, TouchInput *touch) {
    char a[PW_MAX + 1] = "", b[PW_MAX + 1] = "";
    if (keyboard_enter(fb, touch, cp_gamepad(), "ROOT PASSWORD", a, PW_MAX, KB_LAYOUT_FULL) != KB_RESULT_OK) goto done;
    if ((int)strlen(a) < PW_MIN) { cp_status("PASSWORD TOO SHORT, MIN 6", false); goto done; }
    if (keyboard_enter(fb, touch, cp_gamepad(), "TYPE IT AGAIN", b, PW_MAX, KB_LAYOUT_FULL) != KB_RESULT_OK) goto done;
    if (strcmp(a, b) != 0) { cp_status("PASSWORDS DIFFER, NOT CHANGED", false); goto done; }
    const char *why = set_root_password(a);
    if (why) cp_status(why, false);
    else cp_status("ROOT PASSWORD SET", true);
    read_ssh();
    refresh_buttons();
done:
    wipe(a, sizeof(a));
    wipe(b, sizeof(b));
}

static void run_host(Framebuffer *fb, TouchInput *touch) {
    char name[HOST_MAX + 1];
    snprintf(name, sizeof(name), "%s", host);
    if (keyboard_enter(fb, touch, cp_gamepad(), "HOST NAME", name, HOST_MAX, KB_LAYOUT_ALPHANUM) != KB_RESULT_OK) return;
    for (char *c = name; *c; c++) *c = (char)tolower((unsigned char)*c);
    if (strcmp(name, host) == 0) { cp_status("HOST NAME UNCHANGED", false); return; }
    if (!sys_hostname_valid(name)) {
        cp_status("INVALID: LETTERS, DIGITS, - ; NO DOTS", false);
        return;
    }
    snprintf(pend_host, sizeof(pend_host), "%s", name);
    char msg[96];
    snprintf(msg, sizeof(msg), "%.30s\nTHE NETWORK NAME CHANGES AT ONCE", name);
    cp_confirm("RENAME THIS UNIT", msg, "RENAME", apply_host);
}

static void ssh_page_run_fullscreen(Framebuffer *fb, TouchInput *touch) {
    if (pw_queued)   { pw_queued = false;   run_password(fb, touch); }
    if (host_queued) { host_queued = false; run_host(fb, touch); }
}

/* Keyboard focus: every control, in reading order. */
static int ssh_page_focusables(UiRect *out, int max) {
    int n = 0;
    for (int i = 0; i < SSH_MODE_N; i++) n = focus_add_button(out, n, max, &mode_btn[i]);
    n = focus_add_button(out, n, max, &root_btn);
    n = focus_add_button(out, n, max, &pw_btn);
    return focus_add_button(out, n, max, &host_btn);
}

const CpPage cp_ssh_page = {
    .name           = "SSH",
    .icon           = "cp_ssh",
    .load           = ssh_page_load,
    .layout         = ssh_page_layout,
    .enter          = ssh_page_enter,
    .draw           = ssh_page_draw,
    .input          = ssh_page_input,
    .run_fullscreen = ssh_page_run_fullscreen,
    .focusables     = ssh_page_focusables,
};
