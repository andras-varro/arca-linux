/* log_page.c — control_panel's Log page: the unit's logs, read-only, at the panel.
 *
 * Exposed only as cp_log_page (cp_page.h).  Three tabs: the syslog
 * (/home/root/log/messages), the respawn log (which app exited, and how) and
 * the kernel ring buffer, which comes from klogctl(), not from running dmesg.
 *
 * Any component can add a tab of its own by installing an item file in
 * /opt/arca/logs.d (log_sources.h); the extra tabs sit in a second row, which is
 * absent when there are none, and read exactly like the syslog tab.
 *
 * Each tab is read once, on entering the page, on a tab change and on REFRESH —
 * never per frame.  A file is read from its last LOG_TAIL_BYTES only (seek from
 * the end), the ring buffer likewise, and at most LOG_MAX_LINES of the newest
 * lines are kept; the page says so when it cut anything.  The first line of a
 * tail is dropped, since it may start mid-line.  A line wider than the text
 * area is TRUNCATED, not wrapped: one log line stays one screen row, so the
 * scroll position counts lines.  The page opens at the newest line.
 *
 * ERRORS ONLY keeps the lines log_is_error() (log_filter.c) accepts.
 *
 * Input: the panel's keyboard and pad move a focus ring over the widgets this
 * page lists (cp_page.h), so the scroll controls are buttons — UP, DOWN, PGUP,
 * PGDN, TOP, END in a strip beside the text — and a key or pad press on one is
 * a tap.  A finger holding one repeats it, and dragging inside the text scrolls
 * it.  Nothing here writes, deletes or exports.
 */
#include "cp_page.h"
#include "cp_ui.h"
#include "log_filter.h"
#include "log_sources.h"
#include "../common/common.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/klog.h>
#include <sys/stat.h>
#include <unistd.h>

#define LOG_TAIL_BYTES   (256 * 1024)
#define LOG_MAX_LINES    4000
#define LOG_LINE_H       10         /* a scale-1 row: 8 px glyph + 2 */
#define LOG_CHAR_W       6          /* fb_draw_text advances 6 px per char at scale 1 (5 px glyph + 1) */
#define LOG_BTN_H        34
#define LOG_GAP          4
#define LOG_STRIP_W      64
#define LOG_STATUS_H     12
#define LOG_REPEAT_FIRST_MS 400
#define LOG_REPEAT_MS    100

#define KLOG_READ_ALL    3          /* <sys/klog.h> has no names for these */
#define KLOG_SIZE_BUFFER 10

enum { TAB_SYSLOG, TAB_RESPAWN, TAB_KERNEL, TAB_N };
static const struct { const char *label; const char *path; } tabs[TAB_N] = {
    { "SYSLOG",  "/home/root/log/messages" },
    { "RESPAWN", "/var/log/arca/respawn.log" },
    { "KERNEL",  NULL },                            /* klogctl */
};

enum { SC_TOP, SC_PGUP, SC_UP, SC_DOWN, SC_PGDN, SC_END, SC_N };
static const char *const sc_label[SC_N] = { "TOP", "PGUP", "UP", "DOWN", "PGDN", "END" };

/* ── State ──────────────────────────────────────────────────────────────── */

static char    buf[LOG_TAIL_BYTES + 1];
static int     blen;
static bool    cut_tail;            /* the log is longer than what was read */
static const char *read_err;        /* NULL, or why the tab shows nothing */
static LogLine lines[LOG_MAX_LINES];
static int     nlines;
static int     vis[LOG_MAX_LINES];  /* indices into lines[] that are shown */
static int     nvis;
static int     top;                 /* vis[] index of the first row drawn */
static int     tab;
static bool    errors_only;

static LogSource extra[LOGSRC_MAX]; /* tabs TAB_N.. from /opt/arca/logs.d */
static int     nextra;
static bool    sources_scanned;

static Button  tab_btn[TAB_N + LOGSRC_MAX], err_btn, refresh_btn, sc_btn[SC_N];
static int     area_x, area_y, area_w, area_h, rows, status_y;

static int     repeat_btn = -1;
static uint32_t repeat_next;
static bool    prev_touching, drag_active;
static int     drag_y;

/* ── Reading ────────────────────────────────────────────────────────────── */

/* The last LOG_TAIL_BYTES of a file into buf. */
static void read_file_tail(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) { read_err = errno == ENOENT ? "NO SUCH LOG ON THIS UNIT" : "LOG NOT READABLE"; return; }
    struct stat st;
    off_t start = 0;
    if (fstat(fd, &st) == 0 && st.st_size > LOG_TAIL_BYTES) {
        start = st.st_size - LOG_TAIL_BYTES;
        cut_tail = true;
    }
    if (start > 0 && lseek(fd, start, SEEK_SET) < 0) { close(fd); read_err = "LOG NOT READABLE"; return; }
    int got = 0;
    while (got < LOG_TAIL_BYTES) {
        ssize_t r = read(fd, buf + got, (size_t)(LOG_TAIL_BYTES - got));
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        got += (int)r;
    }
    close(fd);
    blen = got;
}

static void read_kernel(void) {
    int size = klogctl(KLOG_SIZE_BUFFER, NULL, 0);
    if (size <= 0) { read_err = "KERNEL LOG NOT READABLE"; return; }
    int want = size > LOG_TAIL_BYTES ? LOG_TAIL_BYTES : size;
    if (size > LOG_TAIL_BYTES) cut_tail = true;
    int got = klogctl(KLOG_READ_ALL, buf, want);
    if (got < 0) { read_err = "KERNEL LOG NOT READABLE"; return; }
    blen = got;
}

static void build_vis(void) {
    nvis = 0;
    for (int i = 0; i < nlines; i++)
        if (!errors_only || log_is_error(buf + lines[i].off, lines[i].len))
            vis[nvis++] = i;
}

/* Rescans the open tab and lands on its newest line. */
static void load_tab(void) {
    blen = 0; cut_tail = false; read_err = NULL; nlines = 0;
    if (tab >= TAB_N)          read_file_tail(extra[tab - TAB_N].path);
    else if (tabs[tab].path)   read_file_tail(tabs[tab].path);
    else                       read_kernel();
    if (blen > 0)
        nlines = log_split_tail(buf, blen, cut_tail, lines, LOG_MAX_LINES);
    build_vis();
    top = log_clamp_top(nvis, nvis, rows);
}

static void refresh_buttons(void) {
    for (int i = 0; i < TAB_N + nextra; i++)
        button_set_colors(&tab_btn[i], i == tab ? RGB(0, 90, 160) : RGB(80, 80, 80),
                          COLOR_WHITE, BTN_COLOR_HIGHLIGHT);
    button_set_text(&err_btn, errors_only ? "ERRORS ONLY: ON" : "ERRORS ONLY: OFF");
    button_set_colors(&err_btn, errors_only ? RGB(170, 60, 40) : RGB(80, 80, 80),
                      COLOR_WHITE, BTN_COLOR_HIGHLIGHT);
}

/* ── Page hooks ─────────────────────────────────────────────────────────── */

static void log_page_load(const Config *cfg) { (void)cfg; }

static void log_page_layout(void) {
    if (!sources_scanned) {
        nextra = logsrc_scan(LOGSRC_DIR, extra, LOGSRC_MAX);
        sources_scanned = true;
    }
    int y = CONTENT_Y + LOG_GAP;
    int tw = (CONTENT_WIDTH - (TAB_N - 1) * LOG_GAP) / TAB_N;
    for (int i = 0; i < TAB_N; i++)
        button_init_full(&tab_btn[i], CONTENT_LEFT + i * (tw + LOG_GAP), y, tw, LOG_BTN_H,
                         tabs[i].label, RGB(80, 80, 80), COLOR_WHITE, BTN_COLOR_HIGHLIGHT, 2);
    y += LOG_BTN_H + LOG_GAP;
    if (nextra > 0) {                   /* the second row, only when a component added a log */
        int ew = (CONTENT_WIDTH - (nextra - 1) * LOG_GAP) / nextra;
        for (int i = 0; i < nextra; i++)
            button_init_full(&tab_btn[TAB_N + i], CONTENT_LEFT + i * (ew + LOG_GAP), y, ew,
                             LOG_BTN_H, extra[i].name, RGB(80, 80, 80), COLOR_WHITE,
                             BTN_COLOR_HIGHLIGHT, 2);
        y += LOG_BTN_H + LOG_GAP;
    }
    int bw = (CONTENT_WIDTH - LOG_GAP) / 2;
    button_init_full(&err_btn, CONTENT_LEFT, y, bw, LOG_BTN_H, "ERRORS ONLY: OFF",
                     RGB(80, 80, 80), COLOR_WHITE, BTN_COLOR_HIGHLIGHT, 2);
    button_init_full(&refresh_btn, CONTENT_LEFT + bw + LOG_GAP, y, CONTENT_WIDTH - bw - LOG_GAP,
                     LOG_BTN_H, "REFRESH", RGB(0, 110, 60), COLOR_WHITE, BTN_COLOR_HIGHLIGHT, 2);
    y += LOG_BTN_H + LOG_GAP;

    area_x = CONTENT_LEFT;
    area_y = y;
    area_w = CONTENT_WIDTH - LOG_STRIP_W - LOG_GAP;
    status_y = CONTENT_Y + CONTENT_H - LOG_STATUS_H;
    area_h = status_y - LOG_GAP - area_y;
    rows = (area_h - 6) / LOG_LINE_H;
    if (rows < 1) rows = 1;
    int sh = area_h / SC_N;
    for (int i = 0; i < SC_N; i++)
        button_init_full(&sc_btn[i], CONTENT_RIGHT - LOG_STRIP_W, area_y + i * sh,
                         LOG_STRIP_W, sh - 2, sc_label[i], RGB(60, 60, 90), COLOR_WHITE,
                         BTN_COLOR_HIGHLIGHT, 1);
    refresh_buttons();
    top = log_clamp_top(top, nvis, rows);

    /* The receipt: the text area and the strip must sit inside the content
     * rectangle, which comes from a per-unit touch inset. */
    {
        int bottom = status_y + LOG_STATUS_H - CONTENT_Y;
        const char *verdict = bottom > CONTENT_H ? "PAST CONTENT BOTTOM"
                            : area_h < 4 * LOG_LINE_H ? "TEXT AREA TOO SHORT" : "fits";
        printf("control_panel: log stack %s — bottom +%d of CONTENT_H %d, right %d of "
               "CONTENT_RIGHT %d, %d rows x %d chars (safe %dx%d, %s)\n",
               verdict, bottom, CONTENT_H, CONTENT_RIGHT, CONTENT_RIGHT, rows, (area_w - 8) / LOG_CHAR_W,
               SCREEN_SAFE_WIDTH, SCREEN_SAFE_HEIGHT,
               CONTENT_WIDTH < 600 ? "portrait" : "landscape");
    }
}

static void log_page_enter(void) {
    repeat_btn = -1; drag_active = false; prev_touching = false;
    load_tab();
}

static void log_page_draw(Framebuffer *fb) {
    for (int i = 0; i < TAB_N + nextra; i++) button_draw(fb, &tab_btn[i]);
    button_draw(fb, &err_btn);
    button_draw(fb, &refresh_btn);
    for (int i = 0; i < SC_N; i++) button_draw(fb, &sc_btn[i]);

    fb_fill_rect(fb, area_x, area_y, area_w, area_h, RGB(8, 8, 14));
    fb_draw_rect(fb, area_x, area_y, area_w, area_h, RGB(70, 70, 90));

    int maxc = (area_w - 8) / LOG_CHAR_W;
    if (maxc > 200) maxc = 200;
    char tmp[208];
    if (nvis == 0) {
        const char *msg = read_err ? read_err
                        : errors_only && nlines > 0 ? "NO ERROR LINES"
                        : "THE LOG IS EMPTY";
        fb_draw_text(fb, area_x + 6, area_y + 6, msg, read_err ? COLOR_ORANGE : COLOR_LABEL, 1);
    }
    for (int r = 0; r < rows && top + r < nvis; r++) {
        const LogLine *ll = &lines[vis[top + r]];
        const char *p = buf + ll->off;
        int n = ll->len, skip = log_prefix_len(p, n);
        int err = log_is_error(p, n);
        p += skip; n -= skip;
        if (n > maxc) n = maxc;
        memcpy(tmp, p, (size_t)n);
        tmp[n] = '\0';
        for (int i = 0; i < n; i++)         /* the font has no controls */
            if ((unsigned char)tmp[i] < 32 || (unsigned char)tmp[i] > 126) tmp[i] = '?';
        fb_draw_text(fb, area_x + 4, area_y + 4 + r * LOG_LINE_H, tmp,
                     err ? RGB(255, 130, 110) : RGB(200, 200, 200), 1);
    }

    char st[160];
    if (nvis > 0) {
        int last = top + rows < nvis ? top + rows : nvis;
        snprintf(st, sizeof(st), "%d-%d OF %d%s%s", top + 1, last, nvis,
                 errors_only ? " ERROR LINES" : " LINES",
                 cut_tail ? "  (NEWEST 256 KB)" : "");
    } else {
        snprintf(st, sizeof(st), "0 LINES");
    }
    fb_draw_text(fb, CONTENT_LEFT, status_y, st, COLOR_LABEL, 1);
}

/* ── Input ──────────────────────────────────────────────────────────────── */

/* Moves the window for scroll button i; true if it moved. */
static bool scroll_by(int i) {
    int before = top;
    int page = rows > 1 ? rows - 1 : 1;
    switch (i) {
    case SC_TOP:  top = 0; break;
    case SC_PGUP: top -= page; break;
    case SC_UP:   top -= 1; break;
    case SC_DOWN: top += 1; break;
    case SC_PGDN: top += page; break;
    case SC_END:  top = nvis; break;
    }
    top = log_clamp_top(top, nvis, rows);
    return top != before;
}

static CpPageResult log_page_input(Config *cfg, int tx, int ty, bool touching, uint32_t now) {
    (void)cfg;
    CpPageResult res = CP_PAGE_IDLE;

    for (int i = 0; i < TAB_N + nextra; i++)
        if (button_update(&tab_btn[i], tx, ty, touching, now)) {
            tab = i;
            load_tab();
            refresh_buttons();
            res = CP_PAGE_REDRAW;
        }
    if (button_update(&err_btn, tx, ty, touching, now)) {
        errors_only = !errors_only;
        build_vis();
        top = log_clamp_top(nvis, nvis, rows);      /* back to the newest line */
        refresh_buttons();
        res = CP_PAGE_REDRAW;
    }
    if (button_update(&refresh_btn, tx, ty, touching, now)) {
        load_tab();
        res = CP_PAGE_REDRAW;
    }

    bool on_strip = false;
    for (int i = 0; i < SC_N; i++) {
        if (button_update(&sc_btn[i], tx, ty, touching, now)) {
            repeat_btn = i;
            repeat_next = now + LOG_REPEAT_FIRST_MS;
            if (scroll_by(i)) res = CP_PAGE_REDRAW;
        } else if (touching && repeat_btn == i && button_is_touched(&sc_btn[i], tx, ty)
                   && (int32_t)(now - repeat_next) >= 0) {
            repeat_next = now + LOG_REPEAT_MS;
            if (scroll_by(i)) res = CP_PAGE_REDRAW;
        }
        if (touching && button_is_touched(&sc_btn[i], tx, ty)) on_strip = true;
    }
    if (!touching) repeat_btn = -1;

    /* Drag inside the text: dragging down shows older lines. */
    bool in_area = touching && tx >= area_x && tx < area_x + area_w &&
                   ty >= area_y && ty < area_y + area_h;
    if (touching && !prev_touching && in_area && !on_strip) {
        drag_active = true;
        drag_y = ty;
    } else if (!touching) {
        drag_active = false;
    } else if (drag_active) {
        int d = (ty - drag_y) / LOG_LINE_H;
        if (d != 0) {
            int before = top;
            top = log_clamp_top(top - d, nvis, rows);
            drag_y += d * LOG_LINE_H;
            if (top != before) res = CP_PAGE_REDRAW;
        }
    }
    prev_touching = touching;
    return res;
}

static int log_page_focusables(UiRect *out, int max) {
    int n = 0;
    for (int i = 0; i < TAB_N + nextra; i++) n = focus_add_button(out, n, max, &tab_btn[i]);
    n = focus_add_button(out, n, max, &err_btn);
    n = focus_add_button(out, n, max, &refresh_btn);
    for (int i = 0; i < SC_N; i++) n = focus_add_button(out, n, max, &sc_btn[i]);
    return n;
}

const CpPage cp_log_page = {
    .name       = "Log",
    .icon       = "cp_log",
    .load       = log_page_load,
    .layout     = log_page_layout,
    .enter      = log_page_enter,
    .draw       = log_page_draw,
    .input      = log_page_input,
    .focusables = log_page_focusables,
};
