/*
 * terminal -- a text terminal: /bin/login in a pty, drawn with libvterm.
 *
 * It runs login, not a shell: access is exactly what the accounts on the unit
 * grant, so with a locked root nobody gets in, and this app carries no security
 * logic of its own.  Leaving the session (the shell exits, login is abandoned
 * with Ctrl-D at its prompt) returns to the launcher.
 *
 * Output: the pty is read without blocking and fed to libvterm; only the cells
 * libvterm reports damaged are redrawn, through framebuffer.c (6 x 9 px a cell
 * at font scale 1, 12 x 18 at scale 2) and presented with fb_swap_rect().
 *
 * Input, three sources into one pty:
 *   - a USB / Bluetooth keyboard, read here from its evdev nodes (gamepad.c
 *     reduces a keyboard to nine buttons and has no letters); common/term_keymap.h turns
 *     a key code into the character or special key, libvterm into its bytes;
 *   - the toolbar along the bottom edge (touch): EXIT, KBD (the on-screen
 *     keyboard, one line at a time), CTRL (sticky, applies to the next key),
 *     ESC, TAB, ENT, ^C, the four arrows and FONT (6 x 9 or 12 x 18 cells);
 *   - a pad: D-pad = arrows, A = Enter, B = Escape, X = Tab, Start = the
 *     on-screen keyboard, Select tapped = Ctrl-C, Select held 1 s = leave.
 *
 * Run on device (the launcher passes the first two):
 *   /opt/games/terminal /dev/fb0 /dev/input/touchscreen0
 *
 * Build: native_apps/build-and-deploy.sh.  libvterm is linked static from
 * arm-deps-softfp (build-libvterm.sh); no other app sees it.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <vterm.h>

#include "../common/common.h"
#include "../common/framebuffer.h"
#include "../common/gamepad.h"
#include "../common/hardware.h"
#include "../common/input_scan.h"
#include "../common/keyboard.h"
#include "../common/touch_input.h"
#include "control_panel/sys_settings.h"
#include "../common/term_keymap.h"

#define MAX_ROWS   100
#define MAX_COLS   200
#define MAX_KBD    8
#define OUT_BUF    4096
#define DRAW_MIN_MS 33          /* at most ~30 presents a second */
#define EXIT_HOLD_MS 1000       /* pad Select held this long leaves */
#define QUICK_EXIT_MS 3000      /* a session shorter than this is reported, not silent */
#define TERM_NAME  "xterm"

#define COL_FG  RGB(204, 204, 204)
#define COL_BG  RGB(0, 0, 0)

static volatile bool running = true;
static void sig_handler(int s) { (void)s; running = false; }

/* ── terminal state ──────────────────────────────────────────────────────── */

typedef struct {
    VTerm       *vt;
    VTermScreen *scr;
    int rows, cols;
    int scale, cw, ch;          /* font scale and cell size in px */
    int ox, oy;                 /* grid origin */
    int tb_y, tb_h;             /* toolbar band */

    /* damaged columns per row; dmin > dmax means clean */
    int dmin[MAX_ROWS], dmax[MAX_ROWS];
    bool need_full;             /* repaint everything, present with fb_swap() */
    bool tb_dirty;              /* repaint the toolbar */

    VTermPos cur;
    bool cur_visible;

    int   master;               /* pty master, -1 once closed */
    pid_t child;                /* -1: none or reaped */
    uint32_t started_ms, gone_ms;
    bool  child_gone;           /* pty hung up */
    bool  waiting_exit;         /* the session ended badly: show why, leave on a key */

    char obuf[OUT_BUF];         /* bytes libvterm wants sent to the child */
    int  olen;
} Term;

static Term T;

static void mark(int row, int c0, int c1) {
    if (row < 0 || row >= T.rows) return;
    if (c0 < 0) c0 = 0;
    if (c1 >= T.cols) c1 = T.cols - 1;
    if (c0 > c1) return;
    if (T.dmin[row] > T.dmax[row]) { T.dmin[row] = c0; T.dmax[row] = c1; return; }
    if (c0 < T.dmin[row]) T.dmin[row] = c0;
    if (c1 > T.dmax[row]) T.dmax[row] = c1;
}

static void mark_all(void) {
    for (int r = 0; r < MAX_ROWS; r++) { T.dmin[r] = 1; T.dmax[r] = 0; }
    for (int r = 0; r < T.rows; r++) mark(r, 0, T.cols - 1);
}

static bool any_damage(void) {
    for (int r = 0; r < T.rows; r++)
        if (T.dmin[r] <= T.dmax[r]) return true;
    return false;
}

/* ── libvterm callbacks ──────────────────────────────────────────────────── */

static int cb_damage(VTermRect r, void *u) {
    (void)u;
    for (int row = r.start_row; row < r.end_row; row++) mark(row, r.start_col, r.end_col - 1);
    return 1;
}

static int cb_movecursor(VTermPos pos, VTermPos old, int visible, void *u) {
    (void)u;
    mark(old.row, old.col, old.col);
    mark(pos.row, pos.col, pos.col);
    T.cur = pos;
    T.cur_visible = visible != 0;
    return 1;
}

static int cb_settermprop(VTermProp prop, VTermValue *val, void *u) {
    (void)u;
    if (prop == VTERM_PROP_CURSORVISIBLE) {
        T.cur_visible = val->boolean != 0;
        mark(T.cur.row, T.cur.col, T.cur.col);
    }
    return 1;
}

static int cb_bell(void *u) { (void)u; return 1; }
static int cb_resize(int rows, int cols, void *u) { (void)rows; (void)cols; (void)u; return 1; }

static const VTermScreenCallbacks screen_cbs = {
    .damage = cb_damage,
    .movecursor = cb_movecursor,
    .settermprop = cb_settermprop,
    .bell = cb_bell,
    .resize = cb_resize,
};

/* Bytes the emulator wants the child to receive: key sequences and its replies
 * to queries (cursor position, device attributes -- vi and top ask). */
static void flush_out(void) {
    while (T.olen > 0 && T.master >= 0) {
        ssize_t n = write(T.master, T.obuf, (size_t)T.olen);
        if (n > 0) {
            memmove(T.obuf, T.obuf + n, (size_t)(T.olen - n));
            T.olen -= (int)n;
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else {
            break;          /* EAGAIN: the child is not reading; keep it for the next pass */
        }
    }
}

static void on_output(const char *s, size_t len, void *u) {
    (void)u;
    if (T.master < 0) return;
    if ((size_t)T.olen + len > OUT_BUF) flush_out();
    if ((size_t)T.olen + len > OUT_BUF) return;    /* a stuck child: drop rather than grow */
    memcpy(T.obuf + T.olen, s, len);
    T.olen += (int)len;
}

/* A line for the operator, written into the screen as if the child had printed it. */
static void say(const char *text) {
    vterm_input_write(T.vt, text, strlen(text));
    vterm_screen_flush_damage(T.scr);
}

/* ── layout ──────────────────────────────────────────────────────────────── */

static void layout(void) {
    T.cw = 6 * T.scale;
    T.ch = 9 * T.scale;
    T.tb_h = BTN_MENU_HEIGHT;
    T.tb_y = SCREEN_SAFE_BOTTOM - T.tb_h;
    T.ox = SCREEN_VISIBLE_LEFT;
    T.oy = SCREEN_VISIBLE_TOP;
    int rows = (T.tb_y - 2 - T.oy) / T.ch;
    int cols = SCREEN_VISIBLE_WIDTH / T.cw;
    if (rows < 2) rows = 2;
    if (cols < 10) cols = 10;
    if (rows > MAX_ROWS) rows = MAX_ROWS;
    if (cols > MAX_COLS) cols = MAX_COLS;
    T.rows = rows;
    T.cols = cols;
}

static void set_pty_size(void) {
    if (T.master < 0) return;
    struct winsize ws;
    memset(&ws, 0, sizeof ws);
    ws.ws_row = (unsigned short)T.rows;
    ws.ws_col = (unsigned short)T.cols;
    ioctl(T.master, TIOCSWINSZ, &ws);
}

/* ── the pty and its child ───────────────────────────────────────────────── */

/* BusyBox login answers a locked root ("root:*" in /etc/shadow, the image's
 * state until a password is set) with "bad salt".  Say what to do instead; other
 * accounts may still log in, so login runs regardless. */
static void hint_if_root_locked(void) {
    static char shadow[8192];
    int fd = open("/etc/shadow", O_RDONLY);
    if (fd < 0) return;
    ssize_t n = read(fd, shadow, sizeof shadow - 1);
    close(fd);
    if (n <= 0) return;
    shadow[n] = '\0';
    if (!sys_shadow_root_has_password(shadow))
        say("root has no password yet. Set one in Control Panel > SSH,\r\n"
            "then log in as root. (\"bad salt\" means exactly this.)\r\n\r\n");
    memset(shadow, 0, sizeof shadow);
}

/* 0 on success; -1 with errno set, and a line already written to the screen. */
static int start_session(void) {
    int m = posix_openpt(O_RDWR | O_NOCTTY);
    if (m < 0 || grantpt(m) < 0 || unlockpt(m) < 0) {
        int e = errno;
        if (m >= 0) close(m);
        char msg[96];
        snprintf(msg, sizeof msg, "terminal: no pty (%s)\r\n", strerror(e));
        say(msg);
        return -1;
    }
    char *sn = ptsname(m);
    if (!sn) { close(m); say("terminal: no pty name\r\n"); return -1; }
    char slave[64];
    snprintf(slave, sizeof slave, "%s", sn);

    struct winsize ws;
    memset(&ws, 0, sizeof ws);
    ws.ws_row = (unsigned short)T.rows;
    ws.ws_col = (unsigned short)T.cols;
    ioctl(m, TIOCSWINSZ, &ws);

    hint_if_root_locked();
    pid_t pid = fork();
    if (pid < 0) {
        int e = errno;
        close(m);
        char msg[96];
        snprintf(msg, sizeof msg, "terminal: fork failed (%s)\r\n", strerror(e));
        say(msg);
        return -1;
    }
    if (pid == 0) {
        /* The child: a new session whose controlling terminal is the pty slave. */
        setsid();
        int s = open(slave, O_RDWR);
        if (s < 0) _exit(126);
        ioctl(s, TIOCSCTTY, 0);
        dup2(s, 0); dup2(s, 1); dup2(s, 2);
        for (int fd = 3; fd < 1024; fd++) close(fd);   /* framebuffer, input, the instance lock */
        signal(SIGPIPE, SIG_DFL);
        signal(SIGINT, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        setenv("TERM", TERM_NAME, 1);
        execl("/bin/login", "login", (char *)NULL);
        _exit(127);
    }
    int fl = fcntl(m, F_GETFL, 0);
    fcntl(m, F_SETFL, fl | O_NONBLOCK);
    T.master = m;
    T.child = pid;
    T.started_ms = get_time_ms();
    printf("terminal: login pid %d on %s, %dx%d\n", (int)pid, slave, T.cols, T.rows);
    return 0;
}

static void stop_session(void) {
    if (T.child > 0) {
        kill(-T.child, SIGHUP);
        if (T.master >= 0) { close(T.master); T.master = -1; }
        for (int i = 0; i < 20; i++) {
            if (waitpid(T.child, NULL, WNOHANG) == T.child) { T.child = -1; break; }
            poll(NULL, 0, 15);
        }
        if (T.child > 0) {
            kill(-T.child, SIGKILL);
            waitpid(T.child, NULL, 0);
            T.child = -1;
        }
    }
    if (T.master >= 0) { close(T.master); T.master = -1; }
}

/* Read what the child printed.  True if anything arrived. */
static bool pump_pty(void) {
    if (T.master < 0) return false;
    char buf[4096];
    int total = 0;
    for (;;) {
        ssize_t n = read(T.master, buf, sizeof buf);
        if (n > 0) {
            vterm_input_write(T.vt, buf, (size_t)n);
            total += (int)n;
            if (total >= 65536) break;      /* let the loop draw and poll input */
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && errno == EAGAIN) break;
        /* 0, or EIO once the slave side is closed: the session is over */
        T.child_gone = true;
        T.gone_ms = get_time_ms();
        close(T.master);
        T.master = -1;
        break;
    }
    if (total) vterm_screen_flush_damage(T.scr);
    return total > 0;
}

/* Collect the child once it is gone and decide whether to leave.  It is gone
 * when the pty hangs up, or when waitpid says so first (a background job still
 * holding the slave keeps the pty open after the session leader has exited). */
static void reap_child(uint32_t now) {
    if (T.child <= 0) return;
    int st = 0;
    pid_t r = waitpid(T.child, &st, WNOHANG);
    if (r == 0) {
        if (T.child_gone && now - T.gone_ms > 1000) kill(-T.child, SIGKILL);
        return;
    }
    if (!T.child_gone) {
        pump_pty();                         /* its last words */
        T.child_gone = true;
        T.gone_ms = now;
        if (T.master >= 0) { close(T.master); T.master = -1; }
    }
    T.child = -1;
    bool clean = (r > 0) && WIFEXITED(st) && WEXITSTATUS(st) == 0;
    bool quick = (now - T.started_ms) < QUICK_EXIT_MS;
    printf("terminal: session ended, status 0x%x after %u ms\n", (unsigned)st, (unsigned)(now - T.started_ms));
    if (clean && !quick) { running = false; return; }
    char msg[128];
    if (r > 0 && WIFEXITED(st))
        snprintf(msg, sizeof msg, "\r\n[login exited, status %d -- press a key or EXIT]\r\n", WEXITSTATUS(st));
    else
        snprintf(msg, sizeof msg, "\r\n[session ended -- press a key or EXIT]\r\n");
    say(msg);
    T.waiting_exit = true;
}

/* ── keys into the terminal ──────────────────────────────────────────────── */

static VTermKey vkey_for(int key) {
    switch (key) {
    case TKEY_ENTER:     return VTERM_KEY_ENTER;
    case TKEY_TAB:       return VTERM_KEY_TAB;
    case TKEY_BACKSPACE: return VTERM_KEY_BACKSPACE;
    case TKEY_ESCAPE:    return VTERM_KEY_ESCAPE;
    case TKEY_UP:        return VTERM_KEY_UP;
    case TKEY_DOWN:      return VTERM_KEY_DOWN;
    case TKEY_LEFT:      return VTERM_KEY_LEFT;
    case TKEY_RIGHT:     return VTERM_KEY_RIGHT;
    case TKEY_INS:       return VTERM_KEY_INS;
    case TKEY_DEL:       return VTERM_KEY_DEL;
    case TKEY_HOME:      return VTERM_KEY_HOME;
    case TKEY_END:       return VTERM_KEY_END;
    case TKEY_PAGEUP:    return VTERM_KEY_PAGEUP;
    case TKEY_PAGEDOWN:  return VTERM_KEY_PAGEDOWN;
    default:
        if (key >= TKEY_F1 && key < TKEY_F1 + 12)
            return (VTermKey)VTERM_KEY_FUNCTION(key - TKEY_F1 + 1);
        return VTERM_KEY_NONE;
    }
}

static void send_out(TermKeyOut o) {
    if (o.kind == TK_CHAR) {
        vterm_keyboard_unichar(T.vt, o.ch, (VTermModifier)o.mods);
    } else if (o.kind == TK_KEY) {
        VTermKey k = vkey_for(o.key);
        if (k != VTERM_KEY_NONE) vterm_keyboard_key(T.vt, k, (VTermModifier)o.mods);
    }
    flush_out();
}

static void send_special(int tkey, unsigned mods) {
    TermKeyOut o = { TK_KEY, 0, tkey, mods };
    send_out(o);
}

static void send_char(uint32_t ch, unsigned mods) {
    TermKeyOut o = { TK_CHAR, ch, 0, mods };
    send_out(o);
}

/* The sticky CTRL button: applies to the next key sent from the toolbar, the
 * on-screen keyboard, a pad or a physical keyboard, then clears. */
static bool sticky_ctrl;

static unsigned take_sticky(void) {
    unsigned m = sticky_ctrl ? TMOD_CTRL : 0;
    if (sticky_ctrl) { sticky_ctrl = false; T.tb_dirty = true; }
    return m;
}

/* ── physical keyboards ──────────────────────────────────────────────────── */

typedef struct {
    InputNode    nodes[MAX_KBD];
    unsigned     mods[MAX_KBD];   /* TMOD_* held on each node, index-aligned */
    int          n;
    bool         caps;
    InputSigGate gate;
} Kbd;

static Kbd K;

static void kbd_sync_mods(int i) {
    unsigned long keys[INPUT_SCAN_NLONGS(KEY_MAX + 1)];
    memset(keys, 0, sizeof keys);
    if (ioctl(K.nodes[i].fd, EVIOCGKEY(sizeof keys), keys) < 0) return;
    unsigned m = 0;
    static const int codes[] = { KEY_LEFTSHIFT, KEY_RIGHTSHIFT, KEY_LEFTCTRL, KEY_RIGHTCTRL,
                                 KEY_LEFTALT, KEY_RIGHTALT };
    for (unsigned c = 0; c < sizeof codes / sizeof codes[0]; c++)
        if (input_caps_test(keys, codes[c])) m |= term_mod_bit(codes[c]);
    K.mods[i] = m;
}

static void kbd_drop(int i) {
    K.n = input_scan_drop(K.nodes, K.n, i);
    memmove(&K.mods[i], &K.mods[i + 1], (size_t)(K.n - i) * sizeof K.mods[0]);
}

static void kbd_scan(void) {
    static const int cap[INPUT_KIND_COUNT] = {
        [INPUT_KIND_KEYBOARD] = 4, [INPUT_KIND_MOUSE] = 4,
    };
    int before = K.n;
    input_sig_gate_baseline(&K.gate, input_node_sig());
    K.n = input_scan(K.nodes, K.n, MAX_KBD, cap);
    for (int i = K.n - 1; i >= before; i--) {
        /* a mouse is of no use here, a keyboard+touchpad combo (a mouse node that
         * carries keys) is a keyboard */
        if (K.nodes[i].kind != INPUT_KIND_KEYBOARD &&
            !(K.nodes[i].kind == INPUT_KIND_MOUSE && K.nodes[i].keys)) {
            kbd_drop(i);
        }
    }
    for (int i = before; i < K.n; i++) {
        K.mods[i] = 0;
        kbd_sync_mods(i);
        printf("terminal: keyboard %s (%s)\n", K.nodes[i].path, K.nodes[i].name);
    }
}

static unsigned kbd_mods(void) {
    unsigned m = 0;
    for (int i = 0; i < K.n; i++) m |= K.mods[i];
    return m;
}

static void kbd_key(int i, int code, int value) {
    unsigned bit = term_mod_bit(code);
    if (bit) {
        if (value == 0) K.mods[i] &= ~bit;
        else            K.mods[i] |= bit;
        return;
    }
    if (value == 0) return;
    if (T.waiting_exit) { running = false; return; }
    if (code == KEY_CAPSLOCK) { if (value == 1) K.caps = !K.caps; return; }
    TermKeyOut o = term_keymap(code, kbd_mods() | take_sticky(), K.caps);
    if (o.kind != TK_NONE) send_out(o);     /* value 2 is the kernel's autorepeat */
}

static void kbd_poll(void) {
    for (int i = 0; i < K.n; i++) {
        InputSynDrop sd = { false, false };
        struct input_event ev;
        ssize_t r;
        while ((r = read(K.nodes[i].fd, &ev, sizeof ev)) == (ssize_t)sizeof ev) {
            if (input_syn_drop_skip(&sd, &ev)) continue;
            if (ev.type == EV_KEY) kbd_key(i, ev.code, ev.value);
        }
        if (r < 0 && errno == ENODEV) { kbd_drop(i); i--; continue; }
        if (sd.resync) kbd_sync_mods(i);    /* a release may have been dropped with the overflow */
    }
}

/* ── drawing ─────────────────────────────────────────────────────────────── */

static uint32_t brighten(uint32_t c) {
    int r = (c >> 16) & 0xff, g = (c >> 8) & 0xff, b = c & 0xff;
    r += (255 - r) / 3; g += (255 - g) / 3; b += (255 - b) / 3;
    return RGB(r, g, b);
}

static uint32_t mix4(uint32_t fg, uint32_t bg, int k) {      /* k/4 of fg over bg */
    int fr = (fg >> 16) & 0xff, fgc = (fg >> 8) & 0xff, fb = fg & 0xff;
    int br = (bg >> 16) & 0xff, bgc = (bg >> 8) & 0xff, bb = bg & 0xff;
    return RGB((fr * k + br * (4 - k)) >> 2, (fgc * k + bgc * (4 - k)) >> 2, (fb * k + bb * (4 - k)) >> 2);
}

enum { BL = 1, BR = 2, BU = 4, BD = 8 };

/* Which arms a box-drawing character has.  Light, heavy and double all draw as
 * one weight: the point is that a table or a frame joins up, not its style. */
static int box_mask(uint32_t cp) {
    switch (cp) {
    case 0x2500: case 0x2501: case 0x2550: return BL | BR;
    case 0x2502: case 0x2503: case 0x2551: return BU | BD;
    case 0x250C ... 0x250F: case 0x2554: case 0x256D: return BR | BD;
    case 0x2510 ... 0x2513: case 0x2557: case 0x256E: return BL | BD;
    case 0x2514 ... 0x2517: case 0x255A: case 0x2570: return BR | BU;
    case 0x2518 ... 0x251B: case 0x255D: case 0x256F: return BL | BU;
    case 0x251C ... 0x2523: case 0x2560: return BU | BD | BR;
    case 0x2524 ... 0x252B: case 0x2563: return BU | BD | BL;
    case 0x252C ... 0x2533: case 0x2566: return BL | BR | BD;
    case 0x2534 ... 0x253B: case 0x2569: return BL | BR | BU;
    case 0x253C ... 0x254B: case 0x256C: return BL | BR | BU | BD;
    default: return 0;
    }
}

static void draw_box(Framebuffer *fb, int mask, int x, int y, uint32_t fg) {
    int t = T.scale;
    int cx = x + T.cw / 2, cy = y + T.ch / 2;
    if (mask & BL) fb_fill_rect(fb, x, cy, cx - x + t, t, fg);
    if (mask & BR) fb_fill_rect(fb, cx, cy, x + T.cw - cx, t, fg);
    if (mask & BU) fb_fill_rect(fb, cx, y, t, cy - y + t, fg);
    if (mask & BD) fb_fill_rect(fb, cx, cy, t, y + T.ch - cy, fg);
}

/* ASCII stand-in for a character the 5x7 font does not have. */
static char ascii_for(uint32_t cp) {
    if (cp >= 32 && cp < 127) return (char)cp;
    switch (cp) {
    case 0x00A0: return ' ';
    case 0x00B7: case 0x2022: case 0x2219: return '.';
    case 0x2018: case 0x2019: return '\'';
    case 0x201C: case 0x201D: return '"';
    case 0x2013: case 0x2014: return '-';
    case 0x2026: return '.';
    default: return '?';
    }
}

static uint32_t rgb_of(VTermColor c, bool is_fg) {
    if (is_fg ? VTERM_COLOR_IS_DEFAULT_FG(&c) : VTERM_COLOR_IS_DEFAULT_BG(&c))
        return is_fg ? COL_FG : COL_BG;
    vterm_screen_convert_color_to_rgb(T.scr, &c);
    return RGB(c.rgb.red, c.rgb.green, c.rgb.blue);
}

static void draw_cell(Framebuffer *fb, int row, int col) {
    VTermScreenCell cell;
    VTermPos p = { row, col };
    if (!vterm_screen_get_cell(T.scr, p, &cell)) return;
    if (cell.chars[0] == (uint32_t)-1) return;          /* second half of a wide character */

    uint32_t fg = rgb_of(cell.fg, true), bg = rgb_of(cell.bg, false);
    if (cell.attrs.bold) fg = brighten(fg);
    bool cursor = T.cur_visible && row == T.cur.row && col == T.cur.col;
    if (cell.attrs.reverse != cursor) { uint32_t t = fg; fg = bg; bg = t; }

    int w = cell.width > 1 ? 2 : 1;
    int x = T.ox + col * T.cw, y = T.oy + row * T.ch;
    fb_fill_rect(fb, x, y, T.cw * w, T.ch, bg);

    uint32_t cp = cell.chars[0];
    if (cp != 0 && cp != ' ') {
        int mask = box_mask(cp);
        if (mask) {
            draw_box(fb, mask, x, y, fg);
        } else if (cp == 0x2588) {
            fb_fill_rect(fb, x, y, T.cw, T.ch, fg);
        } else if (cp == 0x2580) {
            fb_fill_rect(fb, x, y, T.cw, T.ch / 2, fg);
        } else if (cp == 0x2584) {
            fb_fill_rect(fb, x, y + T.ch / 2, T.cw, T.ch - T.ch / 2, fg);
        } else if (cp == 0x258C) {
            fb_fill_rect(fb, x, y, T.cw / 2, T.ch, fg);
        } else if (cp == 0x2590) {
            fb_fill_rect(fb, x + T.cw / 2, y, T.cw - T.cw / 2, T.ch, fg);
        } else if (cp >= 0x2591 && cp <= 0x2593) {
            fb_fill_rect(fb, x, y, T.cw, T.ch, mix4(fg, bg, (int)(cp - 0x2590)));
        } else {
            char s[2] = { ascii_for(cp), 0 };
            if (s[0] != ' ') fb_draw_text(fb, x, y + T.scale, s, fg, T.scale);
        }
    }
    if (cell.attrs.underline)
        fb_fill_rect(fb, x, y + 8 * T.scale, T.cw * w, T.scale, fg);
}

/* ── toolbar ─────────────────────────────────────────────────────────────── */

enum { B_EXIT, B_KBD, B_CTRL, B_ESC, B_TAB, B_ENT, B_INT, B_LEFT, B_UP, B_DOWN, B_RIGHT, B_FONT, B_COUNT };
static const char *const btn_label[B_COUNT] = {
    "EXIT", "KBD", "CTRL", "ESC", "TAB", "ENT", "^C", "<", "^", "v", ">", "FONT"
};
static Button btns[B_COUNT];

static void tb_build(void) {
    int gap = 3;
    int w = (SCREEN_SAFE_WIDTH - gap * (B_COUNT - 1)) / B_COUNT;
    for (int i = 0; i < B_COUNT; i++) {
        uint32_t bg = (i == B_EXIT) ? BTN_EXIT_COLOR : BTN_COLOR_SECONDARY;
        button_init_full(&btns[i], SCREEN_SAFE_LEFT + i * (w + gap), T.tb_y, w, T.tb_h,
                         btn_label[i], bg, COLOR_WHITE, BTN_HIGHLIGHT_COLOR, 2);
    }
}

static void tb_draw(Framebuffer *fb) {
    fb_fill_rect(fb, 0, T.tb_y - 2, fb->width, fb->height - (T.tb_y - 2), COLOR_BLACK);
    btns[B_CTRL].bg_color = sticky_ctrl ? BTN_COLOR_WARNING : BTN_COLOR_SECONDARY;
    for (int i = 0; i < B_COUNT; i++) button_draw(fb, &btns[i]);
}

/* ── frame ───────────────────────────────────────────────────────────────── */

typedef struct { int x0, y0, x1, y1; bool any; } PxRect;

static void px_add(PxRect *r, int x, int y, int w, int h) {
    if (!r->any) { r->x0 = x; r->y0 = y; r->x1 = x + w; r->y1 = y + h; r->any = true; return; }
    if (x < r->x0) r->x0 = x;
    if (y < r->y0) r->y0 = y;
    if (x + w > r->x1) r->x1 = x + w;
    if (y + h > r->y1) r->y1 = y + h;
}

static void draw_frame(Framebuffer *fb) {
    PxRect dirty = { 0, 0, 0, 0, false };
    bool full = T.need_full;

    if (full) fb_clear(fb, COLOR_BLACK);
    if (full) mark_all();

    for (int r = 0; r < T.rows; r++) {
        if (T.dmin[r] > T.dmax[r]) continue;
        for (int c = T.dmin[r]; c <= T.dmax[r]; c++) draw_cell(fb, r, c);
        px_add(&dirty, T.ox + T.dmin[r] * T.cw, T.oy + r * T.ch,
               (T.dmax[r] - T.dmin[r] + 1) * T.cw, T.ch);
        T.dmin[r] = 1; T.dmax[r] = 0;
    }
    if (full || T.tb_dirty) {
        tb_draw(fb);
        px_add(&dirty, 0, T.tb_y - 2, fb->width, fb->height - (T.tb_y - 2));
    }
    T.need_full = false;
    T.tb_dirty = false;

    if (full) {
        fb_swap(fb);
    } else if (dirty.any) {
        /* A wide character at the right edge can reach past the surface. */
        if (dirty.x1 > (int)fb->width)  dirty.x1 = (int)fb->width;
        if (dirty.y1 > (int)fb->height) dirty.y1 = (int)fb->height;
        fb_swap_rect(fb, dirty.x0, dirty.y0, dirty.x1 - dirty.x0, dirty.y1 - dirty.y0);
    }
}

/* ── font scale ──────────────────────────────────────────────────────────── */

static void apply_scale(int scale) {
    T.scale = scale;
    layout();
    tb_build();
    vterm_set_size(T.vt, T.rows, T.cols);
    set_pty_size();
    if (T.cur.row >= T.rows) T.cur.row = T.rows - 1;
    if (T.cur.col >= T.cols) T.cur.col = T.cols - 1;
    T.need_full = true;
}

/* ── pad ─────────────────────────────────────────────────────────────────── */

typedef struct { uint32_t next_ms; bool held; } Repeat;

/* Fire on the press, then again after 400 ms and every 90 ms while held. */
static bool repeat_step(Repeat *r, bool pressed, bool held, uint32_t now) {
    if (!held) { r->held = false; return false; }
    if (pressed || !r->held) { r->held = true; r->next_ms = now + 400; return true; }
    if (now >= r->next_ms) { r->next_ms = now + 90; return true; }
    return false;
}

/* The abstract buttons of every PAD slot -- not the keyboards, whose Backspace
 * and Esc gamepad.c also reports as BACK and PAUSE. */
static void pad_state(const GamepadManager *gm, bool pressed[BTN_ID_COUNT], bool held[BTN_ID_COUNT]) {
    GamepadDevice devs[8];
    int n = gamepad_devices(gm, devs, 8);
    memset(pressed, 0, BTN_ID_COUNT * sizeof(bool));
    memset(held, 0, BTN_ID_COUNT * sizeof(bool));
    for (int i = 0; i < n; i++) {
        if (devs[i].keyboard || devs[i].slot < 0) continue;
        const InputState *p = gamepad_player(gm, devs[i].slot);
        for (int b = 0; b < BTN_ID_COUNT; b++) {
            if (p->buttons[b].pressed) pressed[b] = true;
            if (p->buttons[b].held) held[b] = true;
        }
    }
}

/* ── on-screen keyboard ──────────────────────────────────────────────────── */

static void type_line(Framebuffer *fb, TouchInput *touch, GamepadManager *gp) {
    char buf[96] = "";
    if (keyboard_enter(fb, touch, gp, "TYPE TO THE TERMINAL", buf, (int)sizeof(buf) - 1,
                       KB_LAYOUT_FULL) != KB_RESULT_OK)
        return;
    unsigned mods = take_sticky();
    for (const char *s = buf; *s; s++) {
        send_char((unsigned char)*s, mods);
        mods = 0;                       /* CTRL applies to the first character only */
    }
}

/* ── main ────────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
    /* Line-buffer stdout first: at boot it is a log file (native_apps/CLAUDE.md). */
    setvbuf(stdout, NULL, _IOLBF, 0);

    const char *fb_device = (argc > 1) ? argv[1] : "/dev/fb0";
    const char *touch_device = (argc > 2) ? argv[2] : "/dev/input/touchscreen0";

    int lock_fd = acquire_instance_lock("terminal");
    if (lock_fd < 0) {
        fprintf(stderr, "terminal: another instance is already running\n");
        return 1;
    }

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
    signal(SIGPIPE, SIG_IGN);

    hw_init();
    hw_set_backlight(100);

    fb_set_bpp(fb_device, 32);
    Framebuffer fb;
    if (fb_init(&fb, fb_device) < 0) {
        fprintf(stderr, "terminal: cannot open framebuffer %s\n", fb_device);
        return 1;
    }
    TouchInput touch;
    if (touch_init(&touch, touch_device) < 0) {
        fprintf(stderr, "terminal: cannot open touch %s\n", touch_device);
        fb_close(&fb);
        return 1;
    }
    touch_set_screen_size(&touch, fb.width, fb.height);

    GamepadManager gp;
    gamepad_init(&gp);

    /* Emulator, at the font scale 1 geometry. */
    memset(&T, 0, sizeof T);
    T.master = -1;
    T.child = -1;
    T.scale = 1;
    layout();
    tb_build();
    mark_all();
    T.vt = vterm_new(T.rows, T.cols);
    vterm_set_utf8(T.vt, 1);
    T.scr = vterm_obtain_screen(T.vt);
    vterm_screen_enable_altscreen(T.scr, 1);
    vterm_screen_set_damage_merge(T.scr, VTERM_DAMAGE_ROW);
    vterm_screen_set_callbacks(T.scr, &screen_cbs, NULL);
    vterm_output_set_callback(T.vt, on_output, NULL);
    vterm_screen_reset(T.scr, 1);
    T.cur_visible = true;
    T.need_full = true;

    kbd_scan();
    if (start_session() < 0) T.waiting_exit = true;

    Repeat rep[BTN_ID_COUNT];
    memset(rep, 0, sizeof rep);
    uint32_t last_draw = 0, select_since = 0, ignore_until = 0;
    bool select_down = false;

    while (running) {
        uint32_t now = get_time_ms();

        touch_poll(&touch);
        TouchState ts = touch_get_state(&touch);
        InputState in;
        memset(&in, 0, sizeof in);
        gamepad_poll(&gp, &in, ts.x, ts.y, ts.held);
        gamepad_tick(&gp, now);
        fb_overtemp_poll(&fb);

        if (input_sig_gate_poll(&K.gate, now)) kbd_scan();
        kbd_poll();

        if (now >= ignore_until) {
            /* toolbar: ask every button every frame (native_apps/CLAUDE.md, button_check_tap) */
            bool tap[B_COUNT];
            for (int i = 0; i < B_COUNT; i++) tap[i] = button_check_tap(&btns[i], &ts, now);
            if (button_take_dirty()) T.tb_dirty = true;

            if (tap[B_EXIT]) running = false;
            else if (T.waiting_exit) {
                for (int i = 0; i < B_COUNT; i++) if (tap[i]) running = false;
            } else {
                if (tap[B_KBD]) {
                    type_line(&fb, &touch, &gp);
                    T.need_full = true;
                    ignore_until = get_time_ms() + 300;   /* the OK tap must not land on a button */
                }
                if (tap[B_CTRL]) { sticky_ctrl = !sticky_ctrl; T.tb_dirty = true; }
                if (tap[B_ESC])   send_special(TKEY_ESCAPE, take_sticky());
                if (tap[B_TAB])   send_special(TKEY_TAB, take_sticky());
                if (tap[B_ENT])   send_special(TKEY_ENTER, take_sticky());
                if (tap[B_INT])   { take_sticky(); send_char('c', TMOD_CTRL); }
                if (tap[B_LEFT])  send_special(TKEY_LEFT, take_sticky());
                if (tap[B_UP])    send_special(TKEY_UP, take_sticky());
                if (tap[B_DOWN])  send_special(TKEY_DOWN, take_sticky());
                if (tap[B_RIGHT]) send_special(TKEY_RIGHT, take_sticky());
                if (tap[B_FONT])  apply_scale(T.scale == 1 ? 2 : 1);
            }

            bool pp[BTN_ID_COUNT], ph[BTN_ID_COUNT];
            pad_state(&gp, pp, ph);
            if (T.waiting_exit) {
                for (int b = 0; b < BTN_ID_COUNT; b++) if (pp[b]) running = false;
            } else {
                if (repeat_step(&rep[BTN_ID_UP],    pp[BTN_ID_UP],    ph[BTN_ID_UP],    now)) send_special(TKEY_UP, 0);
                if (repeat_step(&rep[BTN_ID_DOWN],  pp[BTN_ID_DOWN],  ph[BTN_ID_DOWN],  now)) send_special(TKEY_DOWN, 0);
                if (repeat_step(&rep[BTN_ID_LEFT],  pp[BTN_ID_LEFT],  ph[BTN_ID_LEFT],  now)) send_special(TKEY_LEFT, 0);
                if (repeat_step(&rep[BTN_ID_RIGHT], pp[BTN_ID_RIGHT], ph[BTN_ID_RIGHT], now)) send_special(TKEY_RIGHT, 0);
                if (pp[BTN_ID_JUMP])   send_special(TKEY_ENTER, take_sticky());
                if (pp[BTN_ID_RUN])    send_special(TKEY_ESCAPE, take_sticky());
                if (pp[BTN_ID_ACTION]) send_special(TKEY_TAB, take_sticky());
                if (pp[BTN_ID_PAUSE]) {
                    type_line(&fb, &touch, &gp);
                    T.need_full = true;
                    ignore_until = get_time_ms() + 300;
                }
            }
            /* Select: a tap is Ctrl-C, a 1 s hold leaves. */
            if (ph[BTN_ID_BACK]) {
                if (!select_down) { select_down = true; select_since = now; }
                else if (now - select_since >= EXIT_HOLD_MS) running = false;
            } else if (select_down) {
                select_down = false;
                if (!T.waiting_exit && now - select_since < EXIT_HOLD_MS) send_char('c', TMOD_CTRL);
            }
        }

        if (pump_pty()) { /* damage already marked by the callbacks */ }
        reap_child(now);
        flush_out();

        bool dirty = T.need_full || T.tb_dirty || any_damage();
        if (dirty && now - last_draw >= DRAW_MIN_MS) {
            draw_frame(&fb);
            last_draw = now;
            dirty = false;
        }

        /* The one wait: wakes at once for output or a key, else every 50 ms so touch,
         * the pad and the hot-plug check keep running; 10 ms while a frame is owed. */
        struct pollfd pf[1 + MAX_KBD];
        int np = 0;
        if (T.master >= 0) { pf[np].fd = T.master; pf[np].events = POLLIN; pf[np].revents = 0; np++; }
        for (int i = 0; i < K.n; i++) { pf[np].fd = K.nodes[i].fd; pf[np].events = POLLIN; pf[np].revents = 0; np++; }
        poll(pf, (nfds_t)np, dirty ? 10 : 50);
    }

    stop_session();
    vterm_free(T.vt);
    for (int i = K.n - 1; i >= 0; i--) kbd_drop(i);

    hw_leds_off();
    hw_set_backlight(100);
    gamepad_close(&gp);
    touch_close(&touch);
    fb_clear(&fb, COLOR_BLACK);
    fb_swap(&fb);
    fb_close(&fb);
    close(lock_fd);
    return 0;
}
