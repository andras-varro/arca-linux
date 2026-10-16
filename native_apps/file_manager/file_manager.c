/*
 * file_manager — a two-pane, Norton-Commander-style file manager.
 *
 * Two ListViews (common/list_view.c) side by side, each in a header-labelled
 * frame; an action bar underneath works on the marked rows of the active pane
 * (or the cursor row) into the other pane's folder.  Browsing is confined to
 * a few ROOTS: the data folder, the ScummVM games folder, the audio folder and
 * every mounted USB stick (mounted by /usr/sbin/arca-storage).  The logic that
 * has no screen in it lives in fm_core.c, under a host test.
 *
 * Copy, move and delete are an FmJob advanced from the main loop in small
 * steps (a time budget per frame), behind a progress dialog with CANCEL; there
 * is no thread, so the UI, the pad and the touch panel stay live while a stick
 * is written.
 *
 * Input: touch (tap a row, tap the cursor row again to open it, drag to
 * scroll, tap a header to switch pane or open the root list), keyboard (Tab,
 * arrows, Enter, Backspace, F2/F3/F5-F10, Insert, Delete, PgUp/PgDn/Home/End,
 * keypad + and -), pad (D-pad, A mark, X open, B the action bar, Select up,
 * Start exit).  gamepad.c turns a keyboard into nine abstract buttons and has
 * no function keys, so the function keys are read from the keyboard nodes
 * here, the way terminal.c reads its keys.
 */
#include "../common/framebuffer.h"
#include "../common/touch_input.h"
#include "../common/hardware.h"
#include "../common/common.h"
#include "../common/gamepad.h"
#include "../common/keyboard.h"
#include "../common/list_view.h"
#include "../common/pointer.h"
#include "../common/top_bar.h"
#include "../common/ui_flow.h"
#include "fm_core.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define STORAGE_HELPER  "/usr/sbin/arca-storage"
#define DATA_DIR        "/home/root/data"
#define GAMES_DIR       "/home/root/data/games"     /* where scummvm-arca/README.md tells users to put game folders */
#define AUDIO_DIR       "/opt/sound"                /* the effect clips and music beds (native_apps/build-and-deploy.sh) */

#define ROW_H      40
#define HDR_H      26
#define BTN_H      38
#define GAP        4
#define STATUS_H   14
#define JOB_BUDGET_MS 12     /* of work per frame while a job runs */

#define COLOR_BG        RGB(20, 20, 30)
#define COLOR_HDR       RGB(34, 34, 50)
#define COLOR_HDR_ON    RGB(40, 70, 120)
#define COLOR_BTN       RGB(55, 65, 95)

static volatile bool running = true;
static void signal_handler(int sig) { (void)sig; running = false; }

/* ── roots ───────────────────────────────────────────────────────────────── */

typedef struct {
    char label[64];
    char path[128];
    char detail[24];     /* free space, shown right-aligned in the picker */
    char note[96];       /* what a dimmed row says when opened */
    bool usb;
    bool dim;
} Root;

#define MAX_ROOTS 16
static Root roots[MAX_ROOTS];
static int  n_roots;
static bool have_helper;

static Root *root_add(const char *label, const char *path, bool usb, bool dim, const char *note) {
    if (n_roots >= MAX_ROOTS) return NULL;
    Root *r = &roots[n_roots++];
    memset(r, 0, sizeof *r);
    snprintf(r->label, sizeof r->label, "%s", label);
    snprintf(r->path, sizeof r->path, "%s", path ? path : "");
    snprintf(r->note, sizeof r->note, "%s", note ? note : "");
    r->usb = usb;
    r->dim = dim;
    long long fr;
    if (!dim && path && fm_space(path, &fr, NULL)) {
        char sz[16];
        fm_format_size(fr, sz, sizeof sz);
        snprintf(r->detail, sizeof r->detail, "%s FREE", sz);
    }
    return r;
}

static bool is_dir_path(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

/* Run `arca-storage <verb> [arg]` and parse its stdout.  -1: no helper. */
static int run_storage(const char *verb, const char *arg, FmStLine *out, int max) {
    if (access(STORAGE_HELPER, X_OK) != 0) return -1;
    char cmd[256];
    if (arg) snprintf(cmd, sizeof cmd, "%s %s %s 2>/dev/null", STORAGE_HELPER, verb, arg);
    else     snprintf(cmd, sizeof cmd, "%s %s 2>/dev/null", STORAGE_HELPER, verb);
    FILE *f = popen(cmd, "r");
    if (!f) return -1;
    char line[256];
    int n = 0;
    while (fgets(line, sizeof line, f)) {
        FmStLine l = fm_parse_storage_line(line);
        if (l.kind != FM_ST_NONE && n < max) out[n++] = l;
    }
    pclose(f);
    return n;
}

/* Rebuild the root list; mounts any stick that is plugged in and not yet mounted. */
static int refresh_roots(void) {
    n_roots = 0;
    if (is_dir_path(DATA_DIR)) root_add("DATA", DATA_DIR, false, false, NULL);
    if (!is_dir_path(GAMES_DIR)) (void)mkdir(GAMES_DIR, 0755);
    if (is_dir_path(GAMES_DIR)) root_add("GAMES", GAMES_DIR, false, false, NULL);
    if (is_dir_path(AUDIO_DIR)) root_add("AUDIO", AUDIO_DIR, false, false, NULL);

    FmStLine lines[16];
    int n = run_storage("mount", NULL, lines, 16);
    have_helper = n >= 0;
    int sticks = 0;
    for (int i = 0; i < n; i++) {
        char label[64], note[96];
        FmStLine *l = &lines[i];
        switch (l->kind) {
        case FM_ST_MOUNTED:
        case FM_ST_ALREADY:
            snprintf(label, sizeof label, "USB %s", fm_base(l->mp));
            root_add(label, l->mp, true, false, NULL);
            sticks++;
            break;
        case FM_ST_UNSUPPORTED:
            snprintf(label, sizeof label, "USB (%s: not supported)",
                     strcmp(l->extra, "unknown") ? l->extra : "unknown format");
            snprintf(note, sizeof note, "%s IS NOT SUPPORTED. FORMAT THE STICK AS FAT32.", l->extra);
            root_add(label, "", true, true, note);
            break;
        case FM_ST_FAILED:
            snprintf(label, sizeof label, "USB (cannot be read)");
            snprintf(note, sizeof note, "%.80s", l->extra);
            root_add(label, "", true, true, note);
            break;
        default: break;
        }
    }
    return sticks;
}

/* ── panes ───────────────────────────────────────────────────────────────── */

typedef enum { PM_DIR, PM_ROOTS } PaneMode;

typedef struct {
    ListView lv;
    PaneMode mode;
    char     root[FM_PATH_MAX];
    char     root_label[64];
    bool     root_usb;
    char     path[FM_PATH_MAX];
    FmDir    dir;
    bool     has_up;
    FmSort   sort;
    char     free_txt[24];
    char     detail[24];       /* the row being drawn */
    UiRect   hdr;
} Pane;

static Pane P[2];
static int  act;               /* the active pane */

static int pane_count(void *ctx) {
    Pane *p = ctx;
    return p->mode == PM_ROOTS ? n_roots : (p->has_up ? 1 : 0) + p->dir.n;
}

static FmEntry *entry_at(Pane *p, int row) {
    if (p->mode != PM_DIR) return NULL;
    int i = row - (p->has_up ? 1 : 0);
    return (i >= 0 && i < p->dir.n) ? &p->dir.e[i] : NULL;
}

static void pane_row(void *ctx, int i, LvRow *out) {
    Pane *p = ctx;
    if (p->mode == PM_ROOTS) {
        if (i < 0 || i >= n_roots) return;
        out->title = roots[i].label;
        out->detail = roots[i].detail;
        out->glyph = LV_GLYPH_FOLDER;
        if (roots[i].dim) out->flags |= LV_ROW_DIM;
        return;
    }
    if (p->has_up && i == 0) {
        out->title = "..";
        out->detail = strcmp(p->path, p->root) ? "" : "ROOTS";
        out->glyph = LV_GLYPH_UP;
        return;
    }
    FmEntry *e = entry_at(p, i);
    if (!e) return;
    out->title = e->name;
    fm_entry_detail(e, p->detail, sizeof p->detail);
    out->detail = p->detail;
    out->glyph = e->type == FM_T_DIR ? LV_GLYPH_FOLDER : LV_GLYPH_FILE;
    if (e->type == FM_T_LINK || e->type == FM_T_OTHER) out->flags |= LV_ROW_DIM;
}

/* ── the status line ─────────────────────────────────────────────────────── */

static char     status[128];
static uint32_t status_color;
static uint32_t status_ms;
static bool     status_dirty;

static void set_status(uint32_t color, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(status, sizeof status, fmt, ap);
    va_end(ap);
    status_color = color;
    status_ms = get_time_ms();
    status_dirty = true;
}

#define ST_OK    RGB(110, 230, 130)
#define ST_WARN  RGB(255, 190, 70)

/* ── pane navigation ─────────────────────────────────────────────────────── */

static void pane_open_roots(Pane *p) {
    char cur[FM_PATH_MAX];
    snprintf(cur, sizeof cur, "%s", p->root);
    p->mode = PM_ROOTS;
    p->has_up = false;
    fm_dir_free(&p->dir);
    p->free_txt[0] = '\0';
    lv_reset(&p->lv);
    for (int i = 0; i < n_roots; i++)
        if (cur[0] && !strcmp(roots[i].path, cur)) { lv_move_to(&p->lv, i); break; }
}

/* Read p->path.  On failure step to the parent (or the root list) with a message. */
static void pane_load(Pane *p, const char *select_name, int fallback_cursor) {
    for (;;) {
        FmDir d;
        int rc = fm_dir_read(p->path, p->sort, &d);
        if (rc == 0) {
            fm_dir_free(&p->dir);
            p->dir = d;
            break;
        }
        set_status(ST_WARN, "CANNOT OPEN %.60s: %s", fm_base(p->path), strerror(-rc));
        if (!strcmp(p->path, p->root) || !fm_path_parent(p->path, p->path, sizeof p->path) ||
            !fm_path_within(p->root, p->path)) {
            pane_open_roots(p);
            return;
        }
    }
    p->mode = PM_DIR;
    p->has_up = true;
    long long fr = 0;
    if (fm_space(p->path, &fr, NULL)) {
        char sz[16];
        fm_format_size(fr, sz, sizeof sz);
        snprintf(p->free_txt, sizeof p->free_txt, "%s FREE", sz);
    } else {
        p->free_txt[0] = '\0';
    }
    lv_reset(&p->lv);
    int idx = select_name ? fm_dir_find(&p->dir, select_name) : -1;
    if (idx >= 0) lv_move_to(&p->lv, idx + 1);
    else if (fallback_cursor > 0) lv_move_to(&p->lv, fallback_cursor < p->lv.count ? fallback_cursor : p->lv.count - 1);
}

static void pane_reload(Pane *p) {
    if (p->mode != PM_DIR) return;
    char keep[FM_NAME_MAX] = "";
    FmEntry *e = entry_at(p, p->lv.cursor);
    if (e) snprintf(keep, sizeof keep, "%s", e->name);
    pane_load(p, keep[0] ? keep : NULL, p->lv.cursor);
}

static void pane_enter_root(Pane *p, int idx) {
    const Root *r = &roots[idx];
    snprintf(p->root, sizeof p->root, "%s", r->path);
    snprintf(p->root_label, sizeof p->root_label, "%s", r->label);
    p->root_usb = r->usb;
    snprintf(p->path, sizeof p->path, "%s", r->path);
    pane_load(p, NULL, 0);
}

static void pane_up(Pane *p) {
    if (p->mode != PM_DIR) return;
    if (!strcmp(p->path, p->root)) { pane_open_roots(p); return; }
    char came[FM_NAME_MAX];
    snprintf(came, sizeof came, "%s", fm_base(p->path));
    char parent[FM_PATH_MAX];
    if (!fm_path_parent(p->path, parent, sizeof parent) || !fm_path_within(p->root, parent)) {
        pane_open_roots(p);
        return;
    }
    snprintf(p->path, sizeof p->path, "%s", parent);
    pane_load(p, came, 0);
}

static void pane_descend(Pane *p, const char *name) {
    char child[FM_PATH_MAX];
    if (!fm_path_join(p->path, name, child, sizeof child) || !fm_path_within(p->root, child)) {
        set_status(ST_WARN, "CANNOT OPEN THAT");
        return;
    }
    snprintf(p->path, sizeof p->path, "%s", child);
    pane_load(p, NULL, 0);        /* an unreadable folder falls back to its parent, with a message */
}

static void pane_activate(Pane *p) {
    int idx = p->lv.cursor;
    if (idx < 0) return;
    if (p->mode == PM_ROOTS) {
        if (idx >= n_roots) return;
        if (roots[idx].dim) { set_status(ST_WARN, "%s", roots[idx].note); return; }
        pane_enter_root(p, idx);
        return;
    }
    if (p->has_up && idx == 0) { pane_up(p); return; }
    FmEntry *e = entry_at(p, idx);
    if (!e) return;
    if (e->type == FM_T_DIR) { pane_descend(p, e->name); return; }
    char sz[16];
    fm_format_size(e->size, sz, sizeof sz);
    set_status(COLOR_WHITE, "%.80s  %s", e->name, e->type == FM_T_FILE ? sz : "(NOT A PLAIN FILE)");
}

static void pane_sort_next(Pane *p) {
    p->sort = (FmSort)((p->sort + 1) % FM_SORT_COUNT);
    pane_reload(p);
    set_status(COLOR_WHITE, "SORT BY %s%s", fm_sort_label(p->sort),
               p->sort == FM_SORT_SIZE ? " (LARGEST FIRST)" : p->sort == FM_SORT_DATE ? " (NEWEST FIRST)" : "");
}

/* After the root list changed: a pane inside a root that is gone goes back to the list. */
static void panes_revalidate(void) {
    for (int k = 0; k < 2; k++) {
        Pane *p = &P[k];
        if (p->mode == PM_ROOTS) { lv_reset(&p->lv); continue; }
        bool found = false;
        for (int i = 0; i < n_roots; i++)
            if (!roots[i].dim && !strcmp(roots[i].path, p->root)) { found = true; break; }
        if (!found) {
            if (p->root_usb) set_status(ST_WARN, "THE STICK IS GONE");
            pane_open_roots(p);
        } else {
            pane_reload(p);
        }
    }
}

/* ── layout and widgets ──────────────────────────────────────────────────── */

typedef enum {
    B_REN, B_COPY, B_MOVE, B_MKDIR, B_DEL, B_MARK,
    B_ALL, B_SORT, B_EJECT, B_SCAN, B_EXIT, B_COUNT
} BarBtn;
#define BAR_ROW1 6      /* buttons on the first bar row; the rest are on the second */

typedef enum {
    A_NONE, A_REN, A_COPY, A_MOVE, A_MKDIR, A_DEL, A_MARK, A_ALL, A_SORT,
    A_EJECT, A_SCAN, A_EXIT, A_SWITCH, A_UP, A_CLEAR
} Act;

static const char *const btn_label[B_COUNT] = {
    "RENAME", "COPY", "MOVE", "MKDIR", "DELETE", "MARK",
    "ALL", "SORT", "EJECT", "RESCAN", "EXIT"
};
static const Act btn_act[B_COUNT] = {
    A_REN, A_COPY, A_MOVE, A_MKDIR, A_DEL, A_MARK, A_ALL, A_SORT, A_EJECT, A_SCAN, A_EXIT
};

static Button  btn[B_COUNT];
static TopBar  bar;
static int     status_y;
static int     bar_focus = -1;      /* the action-bar button the pad or keyboard is on */

static Framebuffer    *g_fb;
static TouchInput     *g_touch;
static GamepadManager *g_gp;

static void set_active(int k) {
    act = k;
    P[0].lv.active = k == 0;
    P[1].lv.active = k == 1;
}

/* Everything below the top bar, from SCREEN_SAFE_* so every target is pressable. */
static void layout(Framebuffer *fb) {
    top_bar_layout(&bar);
    int left = SCREEN_SAFE_LEFT, w = SCREEN_SAFE_WIDTH;
    int top = TOP_BAR_BOTTOM + 1 + GAP;
    int row2_y = SCREEN_SAFE_BOTTOM - BTN_H - 2;
    int row1_y = row2_y - BTN_H - GAP;
    status_y = row1_y - STATUS_H - GAP;
    int pane_bottom = status_y - GAP;

    for (int k = 0; k < 2; k++) {
        int x, y, pw, ph;
        if (fb->portrait_mode) {                 /* narrow screen: stack the panes */
            ph = (pane_bottom - top - GAP) / 2;
            x = left; y = top + k * (ph + GAP); pw = w;
        } else {
            pw = (w - GAP) / 2;
            ph = pane_bottom - top;
            x = left + k * (pw + GAP); y = top;
        }
        P[k].hdr = (UiRect){ x, y, pw, HDR_H };
        LvSource src = { pane_count, pane_row, &P[k] };
        lv_init(&P[k].lv, src, (UiRect){ x, y + HDR_H, pw, ph - HDR_H }, ROW_H);
    }

    for (int row = 0; row < 2; row++) {
        int first = row ? BAR_ROW1 : 0, n = row ? B_COUNT - BAR_ROW1 : BAR_ROW1;
        int bw = (w - (n - 1) * GAP) / n, y = row ? row2_y : row1_y;
        int scale = 2;
        for (int i = 0; i < n; i++)
            if (!ui_label_fits(btn_label[first + i], 2, bw)) scale = 1;
        for (int i = 0; i < n; i++) {
            int b = first + i;
            uint32_t bg = b == B_DEL ? BTN_COLOR_DANGER : b == B_EXIT ? BTN_EXIT_COLOR : COLOR_BTN;
            button_init_full(&btn[b], left + i * (bw + GAP), y, bw, BTN_H, btn_label[b],
                             bg, COLOR_WHITE, BTN_COLOR_HIGHLIGHT, scale);
        }
    }
}

/* ── dialogs ─────────────────────────────────────────────────────────────── */

typedef enum { D_NONE, D_INFO, D_DELETE, D_CONFLICT, D_ERROR, D_ERROR_FATAL, D_PROGRESS } DlgKind;
static ModalDialog dlg;
static DlgKind     dlg_kind;

static void dlg_show(DlgKind k, int focus) {
    dlg_kind = k;
    modal_dialog_set_focus(&dlg, focus);
    modal_dialog_show(&dlg);
}

static void dlg_info(const char *title, const char *msg) {
    modal_dialog_init(&dlg, title, msg, 1);
    modal_dialog_set_button(&dlg, 0, "OK", BTN_COLOR_PRIMARY, COLOR_WHITE);
    dlg_show(D_INFO, 0);
}

/* ── the job ─────────────────────────────────────────────────────────────── */

static FmJob job;
static bool  job_active;

static void show_progress(void) {
    modal_dialog_init(&dlg, job.op == FM_OP_COPY ? "COPYING" : job.op == FM_OP_MOVE ? "MOVING" : "DELETING", "", 1);
    modal_dialog_set_button(&dlg, 0, "CANCEL", BTN_COLOR_DANGER, COLOR_WHITE);
    dlg_show(D_PROGRESS, -1);
}

static void update_progress(void) {
    char nm[96], l2[48] = "", l3[64];
    lv_fit(nm, sizeof nm, fm_base(fm_job_current(&job)), 380, 2);
    if (job.in_fd >= 0 && job.cur_total > 0) {
        char a[16], b[16];
        fm_format_size(job.cur_done, a, sizeof a);
        fm_format_size(job.cur_total, b, sizeof b);
        snprintf(l2, sizeof l2, "%s OF %s", a, b);
    }
    char t[16];
    fm_format_size(job.bytes_done, t, sizeof t);
    if (job.op == FM_OP_DELETE) snprintf(l3, sizeof l3, "%d DELETED", job.files_done);
    else                        snprintf(l3, sizeof l3, "TOTAL %s  %d FILES", t, job.files_done);
    snprintf(dlg.message, sizeof dlg.message, "%.70s\n%.20s\n%.30s", nm, l2, l3);
}

static void open_conflict(void) {
    struct stat so, dn;
    char nm[96], a[16] = "?", b[16] = "?", msg[128];
    lv_fit(nm, sizeof nm, fm_base(job.pend_dst), 380, 2);
    if (lstat(job.pend_src, &so) == 0) fm_format_size((long long)so.st_size, a, sizeof a);
    if (lstat(job.pend_dst, &dn) == 0) fm_format_size((long long)dn.st_size, b, sizeof b);
    snprintf(msg, sizeof msg, "%.70s\nNEW %.10s   OLD %.10s", nm, a, b);
    modal_dialog_init(&dlg, "FILE EXISTS", msg, 4);
    modal_dialog_set_button(&dlg, 0, "OVERWRITE", BTN_COLOR_WARNING, COLOR_BLACK);
    modal_dialog_set_button(&dlg, 1, "SKIP", BTN_COLOR_SECONDARY, COLOR_WHITE);
    modal_dialog_set_button(&dlg, 2, "ALL", BTN_COLOR_WARNING, COLOR_BLACK);
    modal_dialog_set_button(&dlg, 3, "CANCEL", BTN_COLOR_DANGER, COLOR_WHITE);
    dlg.dialog_height = 360;       /* the stacked buttons start under the message */
    dlg_show(D_CONFLICT, 1);
}

static void open_error(void) {
    /* err_msg is "what\npath\nreason" (the reason is absent when there is no errno) */
    char what[64] = "", path[FM_PATH_MAX] = "", why[64] = "", fit[96], msg[128];
    const char *s = job.err_msg, *nl;
    if ((nl = strchr(s, '\n')) != NULL) {
        snprintf(what, sizeof what, "%.*s", (int)(nl - s), s);
        s = nl + 1;
        if ((nl = strchr(s, '\n')) != NULL) {
            snprintf(path, sizeof path, "%.*s", (int)(nl - s), s);
            snprintf(why, sizeof why, "%s", nl + 1);
        } else {
            snprintf(path, sizeof path, "%s", s);
        }
    } else {
        snprintf(what, sizeof what, "%.60s", s);
    }
    lv_fit(fit, sizeof fit, fm_base(path), 380, 2);
    snprintf(msg, sizeof msg, "%.45s\n%.45s\n%.30s", what, fit, why);
    if (job.err_fatal) {
        modal_dialog_init(&dlg, "STOPPED", msg, 1);
        modal_dialog_set_button(&dlg, 0, "OK", BTN_COLOR_DANGER, COLOR_WHITE);
        dlg.dialog_height = 230;
        dlg_show(D_ERROR_FATAL, 0);
    } else {
        modal_dialog_init(&dlg, "ERROR", msg, 2);
        modal_dialog_set_button(&dlg, 0, "SKIP", BTN_COLOR_SECONDARY, COLOR_WHITE);
        modal_dialog_set_button(&dlg, 1, "CANCEL", BTN_COLOR_DANGER, COLOR_WHITE);
        dlg.dialog_height = 240;
        dlg_show(D_ERROR, 0);
    }
}

static void finish_job(FmJobState st) {
    FmOp op = job.op;
    int files = job.files_done, skipped = job.skipped, errors = job.errors;
    long long bytes = job.bytes_done;
    fm_job_free(&job);
    job_active = false;
    modal_dialog_hide(&dlg);
    dlg_kind = D_NONE;
    pane_reload(&P[0]);
    pane_reload(&P[1]);
    char sz[16], tail[48] = "";
    fm_format_size(bytes, sz, sizeof sz);
    if (skipped || errors) snprintf(tail, sizeof tail, "  (%d SKIPPED, %d FAILED)", skipped, errors);
    const char *verb = op == FM_OP_COPY ? "COPIED" : op == FM_OP_MOVE ? "MOVED" : "DELETED";
    if (st == FM_J_CANCELLED)
        set_status(ST_WARN, "CANCELLED AFTER %d FILE%s", files, files == 1 ? "" : "S");
    else if (op == FM_OP_DELETE)
        set_status(errors ? ST_WARN : ST_OK, "DELETED %d ITEM%s%s", files, files == 1 ? "" : "S", tail);
    else
        set_status(errors ? ST_WARN : ST_OK, "%s %d FILE%s (%s)%s", verb, files, files == 1 ? "" : "S", sz, tail);
}

/* One frame of the job: as much work as fits the time budget. */
static void job_frame(void) {
    uint32_t t0 = get_time_ms();
    FmJobState st;
    do st = fm_job_step(&job, FM_CHUNK); while (st == FM_J_RUN && get_time_ms() - t0 < JOB_BUDGET_MS);
    switch (st) {
    case FM_J_RUN:       update_progress(); break;
    case FM_J_CONFLICT:  open_conflict(); break;
    case FM_J_ERROR:     open_error(); break;
    default:             finish_job(st); break;
    }
}

static void dialog_answered(int a) {
    switch (dlg_kind) {
    case D_DELETE:
        if (a == 0) { job_active = true; show_progress(); update_progress(); }
        else        { fm_job_free(&job); dlg_kind = D_NONE; }
        break;
    case D_PROGRESS:
        fm_job_cancel(&job);
        finish_job(FM_J_CANCELLED);
        break;
    case D_CONFLICT:
        if (a == 3) { fm_job_cancel(&job); finish_job(FM_J_CANCELLED); break; }
        fm_job_decide(&job, a == 0 ? FM_DECIDE_OVERWRITE : a == 1 ? FM_DECIDE_SKIP : FM_DECIDE_OVERWRITE_ALL);
        show_progress();
        break;
    case D_ERROR:
        if (a == 1) { fm_job_ack_error(&job, true); finish_job(FM_J_CANCELLED); break; }
        fm_job_ack_error(&job, false);
        show_progress();
        break;
    case D_ERROR_FATAL:
        fm_job_ack_error(&job, true);
        finish_job(FM_J_CANCELLED);
        break;
    default:
        dlg_kind = D_NONE;
        break;
    }
}

/* Which button Esc / Start / Select answers for the open dialog. */
static int dialog_cancel_idx(void) {
    switch (dlg_kind) {
    case D_DELETE:   return 1;
    case D_CONFLICT: return 3;
    case D_ERROR:    return 1;
    default:         return 0;
    }
}

/* ── physical keyboard (function keys and friends) ───────────────────────── */

#define RK_MAX 8
static InputNode    rk_nodes[RK_MAX];
static int          rk_n;
static InputSigGate rk_gate;
static int          rk_q[32];
static int          rk_qn;

static void rk_scan(void) {
    static const int cap[INPUT_KIND_COUNT] = { [INPUT_KIND_KEYBOARD] = 4, [INPUT_KIND_MOUSE] = 4 };
    int before = rk_n;
    input_sig_gate_baseline(&rk_gate, input_node_sig());
    rk_n = input_scan(rk_nodes, rk_n, RK_MAX, cap);
    for (int i = rk_n - 1; i >= before; i--)         /* a mouse is no use here; a keyboard+touchpad combo is a keyboard */
        if (rk_nodes[i].kind != INPUT_KIND_KEYBOARD &&
            !(rk_nodes[i].kind == INPUT_KIND_MOUSE && rk_nodes[i].keys))
            rk_n = input_scan_drop(rk_nodes, rk_n, i);
}

static void rk_poll(uint32_t now) {
    if (input_sig_gate_poll(&rk_gate, now)) rk_scan();
    for (int i = 0; i < rk_n; i++) {
        InputSynDrop sd = { false, false };
        struct input_event ev;
        ssize_t r;
        while ((r = read(rk_nodes[i].fd, &ev, sizeof ev)) == (ssize_t)sizeof ev) {
            if (input_syn_drop_skip(&sd, &ev)) continue;
            if (ev.type != EV_KEY) continue;
            bool paging = ev.code == KEY_PAGEUP || ev.code == KEY_PAGEDOWN;
            if ((ev.value == 1 || (ev.value == 2 && paging)) && rk_qn < (int)(sizeof rk_q / sizeof rk_q[0]))
                rk_q[rk_qn++] = ev.code;
        }
        if (r < 0 && errno == ENODEV) { rk_n = input_scan_drop(rk_nodes, rk_n, i); i--; }
    }
}

static bool rk_take(int code) {
    for (int i = 0; i < rk_qn; i++)
        if (rk_q[i] == code) {
            memmove(&rk_q[i], &rk_q[i + 1], (size_t)(rk_qn - i - 1) * sizeof rk_q[0]);
            rk_qn--;
            return true;
        }
    return false;
}

/* After a blocking screen (the on-screen keyboard) or while a dialog owns the input. */
static void rk_drain(uint32_t now) {
    rk_poll(now);
    rk_qn = 0;
}

/* ── drawing ─────────────────────────────────────────────────────────────── */

/* src cut from the LEFT with "..." to fit max_w px: a path's end matters most. */
static void tail_fit(char *dest, size_t size, const char *src, int max_w, int scale) {
    if (text_measure_width(src, scale) <= max_w) { snprintf(dest, size, "%s", src); return; }
    for (const char *s = src; *s; s++) {
        snprintf(dest, size, "...%.*s", (int)size - 4, s);
        if (text_measure_width(dest, scale) <= max_w) return;
    }
    snprintf(dest, size, "...");
}

static void draw_pane(Framebuffer *fb, Pane *p, bool on) {
    UiRect h = p->hdr;
    fb_fill_rect(fb, h.x, h.y, h.w, h.h, on ? COLOR_HDR_ON : COLOR_HDR);
    char rt[48], lt[FM_PATH_MAX + 80], cut[FM_PATH_MAX + 80];
    if (p->mode == PM_DIR) {
        const char *rel = fm_path_within(p->root, p->path) ? p->path + strlen(p->root) : "";
        snprintf(rt, sizeof rt, "%s  %s", fm_sort_label(p->sort), p->free_txt);
        snprintf(lt, sizeof lt, "%s%s", p->root_label, rel);
    } else {
        snprintf(rt, sizeof rt, "FOLDERS");
        snprintf(lt, sizeof lt, "OPEN A FOLDER");
    }
    int rw = text_measure_width(rt, 1);
    fb_draw_text(fb, h.x + h.w - 8 - rw, h.y + (h.h - text_measure_height(1)) / 2, rt, RGB(200, 200, 215), 1);
    tail_fit(cut, sizeof cut, lt, h.w - 24 - rw, 2);
    fb_draw_text(fb, h.x + 8, h.y + (h.h - text_measure_height(2)) / 2, cut, COLOR_WHITE, 2);
    lv_draw(fb, &p->lv, "EMPTY");
}

static void draw_all(Framebuffer *fb, const InputState *in) {
    fb_clear(fb, COLOR_BG);
    bar.title = "FILES";
    bar.title_color = COLOR_WHITE;
    bar.title_scale = 3;
    bar.title_cut = false;
    top_bar_draw(fb, &bar);

    for (int k = 0; k < 2; k++) draw_pane(fb, &P[k], k == act);

    int sw = SCREEN_SAFE_WIDTH - 8;
    if (status[0]) {
        char cut[128];
        lv_fit(cut, sizeof cut, status, sw, 2);
        fb_draw_text(fb, SCREEN_SAFE_LEFT + 4, status_y, cut, status_color, 2);
    } else {
        const char *hint = in && in->keyboard_connected
            ? "TAB PANE  F2 REN  F3 SORT  F5 COPY  F6 MOVE  F7 MKDIR  F8 DEL  INS MARK  F9 RESCAN  F10 EXIT"
            : in && in->gamepad_connected
            ? "PAD: DPAD MOVE / PANE   A MARK   X OPEN   SELECT UP   B BUTTON BAR   START EXIT"
            : "TAP A ROW, TAP AGAIN TO OPEN   TAP A HEADER FOR THE FOLDER LIST   DRAG TO SCROLL";
        fb_draw_text(fb, SCREEN_SAFE_LEFT + 4, status_y + 3, hint, RGB(130, 130, 150), 1);
    }

    for (int i = 0; i < B_COUNT; i++) button_draw(fb, &btn[i]);
    if (bar_focus >= 0) {
        const Button *b = &btn[bar_focus];
        fb_draw_rect(fb, b->x - 2, b->y - 2, b->width + 4, b->height + 4, BTN_COLOR_HIGHLIGHT);
        fb_draw_rect(fb, b->x - 3, b->y - 3, b->width + 6, b->height + 6, BTN_COLOR_HIGHLIGHT);
    }
    modal_dialog_draw(&dlg, fb);
}

/* A message over the current frame, for a call that blocks for a second. */
static void busy_banner(const char *msg) {
    draw_all(g_fb, NULL);
    int w = text_measure_width(msg, 2) + 40, h = 50;
    int x = (g_fb->width - w) / 2, y = (g_fb->height - h) / 2;
    fb_fill_rect(g_fb, x, y, w, h, RGB(30, 30, 45));
    fb_draw_rect(g_fb, x, y, w, h, COLOR_WHITE);
    text_draw_centered(g_fb, g_fb->width / 2, y + h / 2, msg, COLOR_YELLOW, 2);
    fb_swap(g_fb);
}
/* ── actions ─────────────────────────────────────────────────────────────── */

static Pane *AP(void) { return &P[act]; }
static Pane *OP(void) { return &P[1 - act]; }

/* The marked names of the active pane, else the cursor row.  Returns the count;
 * the pointers belong to the pane's directory listing. */
static int selection(Pane *p, const char **names, int max) {
    int n = 0;
    for (int i = lv_marked_next(&p->lv, 0); i >= 0 && n < max; i = lv_marked_next(&p->lv, i + 1)) {
        FmEntry *e = entry_at(p, i);
        if (e) names[n++] = e->name;
    }
    if (n == 0) {
        FmEntry *e = entry_at(p, p->lv.cursor);
        if (e) names[n++] = e->name;
    }
    return n;
}

static void start_transfer(FmOp op) {
    Pane *s = AP(), *d = OP();
    if (s->mode != PM_DIR) { set_status(ST_WARN, "OPEN A FOLDER FIRST"); return; }
    if (op != FM_OP_DELETE && d->mode != PM_DIR) { set_status(ST_WARN, "OPEN A FOLDER IN THE OTHER PANE FIRST"); return; }
    const char **names = malloc((size_t)(s->dir.n + 1) * sizeof *names);
    if (!names) { set_status(ST_WARN, "OUT OF MEMORY"); return; }
    int n = selection(s, names, s->dir.n);
    if (n == 0) { free(names); set_status(ST_WARN, "NOTHING SELECTED"); return; }
    char err[160];
    int rc = fm_job_init(&job, op, s->path, op == FM_OP_DELETE ? NULL : d->path, names, n, err, sizeof err);
    char first[96];
    lv_fit(first, sizeof first, names[0], 380, 2);
    free(names);
    if (rc < 0) { dlg_info(op == FM_OP_DELETE ? "CANNOT DELETE" : "CANNOT DO THAT", err); return; }
    if (op != FM_OP_DELETE) { job_active = true; show_progress(); update_progress(); return; }

    char msg[128];
    if (n == 1) snprintf(msg, sizeof msg, "%s\nCANNOT BE UNDONE", first);
    else        snprintf(msg, sizeof msg, "%d ITEMS\nCANNOT BE UNDONE", n);
    modal_dialog_init(&dlg, "DELETE?", msg, 2);
    modal_dialog_set_button(&dlg, 0, "DELETE", BTN_COLOR_DANGER, COLOR_WHITE);
    modal_dialog_set_button(&dlg, 1, "CANCEL", BTN_COLOR_SECONDARY, COLOR_WHITE);
    dlg_show(D_DELETE, 1);
}

/* Blocking on-screen keyboard; true when the user confirmed a non-empty text. */
static bool ask_name(const char *title, char *buf) {
    int rc = keyboard_enter(g_fb, g_touch, g_gp, title, buf, FM_NAME_MAX - 1, KB_LAYOUT_FULL);
    touch_poll(g_touch);                   /* the tap that closed it must not reach the lists */
    rk_drain(get_time_ms());
    return rc == KB_RESULT_OK && buf[0];
}

static void do_rename(void) {
    Pane *p = AP();
    FmEntry *e = entry_at(p, p->lv.cursor);
    if (!e) { set_status(ST_WARN, "SELECT AN ITEM TO RENAME"); return; }
    char old[FM_NAME_MAX], buf[FM_NAME_MAX];
    snprintf(old, sizeof old, "%s", e->name);
    snprintf(buf, sizeof buf, "%s", e->name);
    if (!ask_name("RENAME", buf) || !strcmp(buf, old)) return;
    int rc = fm_rename(p->path, old, buf);
    if (rc) { dlg_info("RENAME FAILED", rc == EEXIST ? "THAT NAME IS ALREADY IN USE" : rc == EINVAL ? "THAT IS NOT A VALID NAME" : strerror(rc)); return; }
    pane_load(p, buf, 0);
    if (OP()->mode == PM_DIR) pane_reload(OP());
    set_status(ST_OK, "RENAMED");
}

static void do_mkdir(void) {
    Pane *p = AP();
    if (p->mode != PM_DIR) { set_status(ST_WARN, "OPEN A FOLDER FIRST"); return; }
    char buf[FM_NAME_MAX] = "";
    if (!ask_name("NEW FOLDER", buf)) return;
    int rc = fm_mkdir(p->path, buf);
    if (rc) { dlg_info("CANNOT CREATE", rc == EEXIST ? "THAT NAME IS ALREADY IN USE" : rc == EINVAL ? "THAT IS NOT A VALID NAME" : strerror(rc)); return; }
    pane_load(p, buf, 0);
    if (OP()->mode == PM_DIR) pane_reload(OP());
    set_status(ST_OK, "FOLDER CREATED");
}

static void mark_next(void) {
    Pane *p = AP();
    if (p->mode != PM_DIR) return;
    if (p->has_up && p->lv.cursor == 0) { lv_key(&p->lv, LV_KEY_DOWN); return; }   /* ".." is never marked */
    lv_key(&p->lv, LV_KEY_MARK_NEXT);
}

static void mark_all_toggle(void) {
    Pane *p = AP();
    if (p->mode != PM_DIR || p->dir.n == 0) return;
    if (lv_mark_count(&p->lv) >= p->dir.n) { lv_key(&p->lv, LV_KEY_MARK_CLEAR); return; }
    lv_key(&p->lv, LV_KEY_MARK_ALL);
    if (p->has_up && lv_is_marked(&p->lv, 0)) {      /* take ".." out again */
        int cur = p->lv.cursor;
        lv_move_to(&p->lv, 0);
        lv_key(&p->lv, LV_KEY_MARK);
        lv_move_to(&p->lv, cur);
    }
}

static void roots_remove(const char *path) {
    for (int i = 0; i < n_roots; i++)
        if (!strcmp(roots[i].path, path)) {
            memmove(&roots[i], &roots[i + 1], (size_t)(n_roots - i - 1) * sizeof roots[0]);
            n_roots--;
            return;
        }
}

static void do_rescan(void) {
    busy_banner("LOOKING FOR USB STICKS...");
    int sticks = refresh_roots();
    panes_revalidate();
    if (!have_helper) set_status(ST_WARN, "NO USB SUPPORT ON THIS SYSTEM");
    else set_status(ST_OK, "%d USB STICK%s READY", sticks, sticks == 1 ? "" : "S");
}

static void do_eject(void) {
    Pane *p = AP();
    if (p->mode != PM_DIR || !p->root_usb) { set_status(ST_WARN, "EJECT NEEDS A PANE ON A USB STICK"); return; }
    char mp[FM_PATH_MAX];
    snprintf(mp, sizeof mp, "%s", p->root);
    busy_banner("WRITING TO THE STICK...");
    FmStLine out[4];
    int n = run_storage("eject", mp, out, 4);
    bool ejected = false, busy = false;
    for (int i = 0; i < n; i++) {
        if (out[i].kind == FM_ST_EJECTED && !strcmp(out[i].mp, mp)) ejected = true;
        if (out[i].kind == FM_ST_BUSY) busy = true;
    }
    if (ejected) {
        roots_remove(mp);
        panes_revalidate();
        set_status(ST_OK, "EJECTED - SAFE TO REMOVE THE STICK");
    } else if (busy) {
        dlg_info("STICK IS BUSY", "SOMETHING IS STILL USING IT.\nTRY AGAIN IN A MOMENT.");
    } else {
        set_status(ST_WARN, n < 0 ? "NO USB SUPPORT ON THIS SYSTEM" : "EJECT FAILED");
    }
}

static void do_action(Act a) {
    switch (a) {
    case A_REN:    do_rename(); break;
    case A_COPY:   start_transfer(FM_OP_COPY); break;
    case A_MOVE:   start_transfer(FM_OP_MOVE); break;
    case A_DEL:    start_transfer(FM_OP_DELETE); break;
    case A_MKDIR:  do_mkdir(); break;
    case A_MARK:   mark_next(); break;
    case A_ALL:    mark_all_toggle(); break;
    case A_CLEAR:  if (AP()->mode == PM_DIR) lv_key(&AP()->lv, LV_KEY_MARK_CLEAR); break;
    case A_SORT:   if (AP()->mode == PM_DIR) pane_sort_next(AP()); break;
    case A_EJECT:  do_eject(); break;
    case A_SCAN:   do_rescan(); break;
    case A_EXIT:   running = false; break;
    case A_SWITCH: set_active(1 - act); break;
    case A_UP:     pane_up(AP()); break;
    case A_NONE:   break;
    }
}

/* The next enabled bar button from `from` in direction `step` (wraps). */
static int bar_next(int from, int step) {
    int i = from;
    for (int n = 0; n < B_COUNT; n++) {
        i = (i + step + B_COUNT) % B_COUNT;
        if (!btn[i].disabled) return i;
    }
    return from;
}

/* ── main ────────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
    setvbuf(stdout, NULL, _IOLBF, 0);       /* at boot stdout is a log file */

    const char *fb_device = (argc > 1) ? argv[1] : "/dev/fb0";
    const char *touch_device = (argc > 2) ? argv[2] : "/dev/input/touchscreen0";

    int lock_fd = acquire_instance_lock("file_manager");
    if (lock_fd < 0) {
        fprintf(stderr, "file_manager: another instance is already running\n");
        return 1;
    }

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGPIPE, SIG_IGN);

    hw_init();
    hw_set_backlight(100);

    fb_set_bpp(fb_device, 32);
    Framebuffer fb;
    if (fb_init(&fb, fb_device) < 0) {
        fprintf(stderr, "file_manager: cannot open framebuffer %s\n", fb_device);
        return 1;
    }
    TouchInput touch;
    if (touch_init(&touch, touch_device) < 0) {
        fprintf(stderr, "file_manager: cannot open touch %s\n", touch_device);
        fb_close(&fb);
        return 1;
    }
    touch_set_screen_size(&touch, fb.width, fb.height);

    GamepadManager gp;
    gamepad_init(&gp);
    g_fb = &fb; g_touch = &touch; g_gp = &gp;

    Pointer ptr;
    pointer_init(&ptr);
    top_bar_init(&bar, false, true);
    memset(P, 0, sizeof P);
    memset(&dlg, 0, sizeof dlg);
    layout(&fb);
    rk_scan();

    int sticks = refresh_roots();
    printf("file_manager: %d root(s), %d stick(s), helper %s\n", n_roots, sticks, have_helper ? "present" : "absent");
    for (int k = 0; k < 2; k++) { P[k].sort = FM_SORT_NAME; pane_open_roots(&P[k]); }
    set_active(0);
    if (n_roots > 0) pane_enter_root(&P[0], 0);
    if (n_roots > 0) {
        int r = n_roots > 1 ? 1 : 0;
        for (int i = 0; i < n_roots; i++)
            if (roots[i].usb && !roots[i].dim) { r = i; break; }
        pane_enter_root(&P[1], r);
    }
    set_status(have_helper ? COLOR_WHITE : ST_WARN, have_helper ? "%d USB STICK%s" : "NO USB SUPPORT ON THIS SYSTEM (NO HELPER)", sticks, sticks == 1 ? "" : "S");

    bool needs_redraw = true, prev_eject_off = true;
    int  hdr_press = -1;
    bool exit_press = false;
    uint32_t last_job_draw = 0;
    InputState in;
    memset(&in, 0, sizeof in);

    while (running) {
        uint32_t now = get_time_ms();

        if (status[0] && now - status_ms > 6000) { status[0] = '\0'; needs_redraw = true; }
        if (status_dirty) { needs_redraw = true; status_dirty = false; }
        if (top_bar_info_due(&bar, now) && top_bar_info_sample(&bar)) needs_redraw = true;

        bool eject_off = !(P[act].mode == PM_DIR && P[act].root_usb);
        btn[B_EJECT].disabled = eject_off;
        if (eject_off != prev_eject_off) { prev_eject_off = eject_off; needs_redraw = true; }
        if (bar_focus >= 0 && btn[bar_focus].disabled) bar_focus = bar_next(bar_focus, 1);

        if (needs_redraw) {
            draw_all(&fb, &in);
            fb_swap(&fb);
            needs_redraw = false;
            last_job_draw = now;
            pointer_invalidate(&ptr);
            pointer_paint(&ptr, &fb);
        }

        touch_poll(&touch);
        TouchState ts = touch_get_state(&touch);
        gamepad_poll(&gp, &in, 0, 0, false);
        gamepad_tick(&gp, now);
        rk_poll(now);
        const ButtonState *bt = in.buttons;
        bool navkey = false;
        for (int i = 0; i < BTN_ID_COUNT; i++) if (bt[i].pressed) navkey = true;
        pointer_update(&ptr, &in, ts.pressed, navkey || rk_qn > 0, &ts);
        bool touching = ts.pressed || ts.held;
        if (ts.pressed || ts.released || navkey || rk_qn > 0) needs_redraw = true;

        /* A frame that answered a dialog does nothing else: the key or tap that
         * closed it must not also act on the lists. */
        bool had_dialog = dlg.active;
        if (had_dialog) {
            rk_qn = 0;
            int a = modal_dialog_input(&dlg, &in, dialog_cancel_idx());
            if (a < 0) a = modal_dialog_update(&dlg, ts.x, ts.y, touching, now);
            if (a >= 0) dialog_answered(a);
            needs_redraw = true;
        }

        if (job_active && (!dlg.active || dlg_kind == D_PROGRESS)) {
            job_frame();
            if (now - last_job_draw >= 100 || !job_active || dlg_kind != D_PROGRESS) needs_redraw = true;
        } else if (!had_dialog && !dlg.active) {
            Act action = A_NONE;

            /* the touch targets: action bar, top-bar exit, pane headers */
            for (int i = 0; i < B_COUNT; i++)
                if (button_check_tap(&btn[i], &ts, now)) action = btn_act[i];
            if (ts.pressed) {
                exit_press = top_bar_hit(&bar.L, ts.x, ts.y) == TB_EXIT;
                hdr_press = -1;
                for (int k = 0; k < 2; k++)
                    if (ts.x >= P[k].hdr.x && ts.x < P[k].hdr.x + P[k].hdr.w &&
                        ts.y >= P[k].hdr.y && ts.y < P[k].hdr.y + P[k].hdr.h) hdr_press = k;
            }
            if (ts.released) {
                if (exit_press && top_bar_hit(&bar.L, ts.x, ts.y) == TB_EXIT) running = false;
                if (hdr_press >= 0 && ts.x >= P[hdr_press].hdr.x && ts.x < P[hdr_press].hdr.x + P[hdr_press].hdr.w &&
                    ts.y >= P[hdr_press].hdr.y && ts.y < P[hdr_press].hdr.y + P[hdr_press].hdr.h) {
                    if (hdr_press == act) pane_open_roots(&P[act]);      /* the active header opens the folder list */
                    else set_active(hdr_press);
                }
                exit_press = false;
                hdr_press = -1;
            }

            /* the lists: touch on both, keys on the active one */
            for (int k = 0; k < 2; k++) {
                LvEvent ev = lv_input(&P[k].lv, &ts, (k == act && bar_focus < 0) ? &in : NULL, now);
                if (ev == LV_EV_TAP && k != act) set_active(k);
                else if (ev == LV_EV_ACTIVATE) pane_activate(&P[k]);
                if (ev != LV_EV_NONE) needs_redraw = true;
            }

            /* keyboard */
            if (rk_take(KEY_TAB))      action = A_SWITCH;
            if (rk_take(KEY_F2))       action = A_REN;
            if (rk_take(KEY_F3))       action = A_SORT;
            if (rk_take(KEY_F5))       action = A_COPY;
            if (rk_take(KEY_F6))       action = A_MOVE;
            if (rk_take(KEY_F7))       action = A_MKDIR;
            if (rk_take(KEY_F8) || rk_take(KEY_DELETE)) action = A_DEL;
            if (rk_take(KEY_F9))       action = A_SCAN;
            if (rk_take(KEY_F10))      action = A_EXIT;
            if (rk_take(KEY_INSERT))   action = A_MARK;
            if (rk_take(KEY_KPPLUS))   action = A_ALL;
            if (rk_take(KEY_KPMINUS))  action = A_CLEAR;
            struct { int code; LvKey key; } nav[] = {
                { KEY_PAGEUP, LV_KEY_PGUP }, { KEY_PAGEDOWN, LV_KEY_PGDN },
                { KEY_HOME, LV_KEY_HOME }, { KEY_END, LV_KEY_END },
            };
            for (unsigned i = 0; i < sizeof nav / sizeof nav[0]; i++)
                while (rk_take(nav[i].code)) lv_key(&P[act].lv, nav[i].key);
            rk_qn = 0;

            /* pad and the abstract keys */
            if (bar_focus < 0) {
                if (bt[BTN_ID_LEFT].pressed && act != 0)  action = A_SWITCH;
                if (bt[BTN_ID_RIGHT].pressed && act != 1) action = A_SWITCH;
                if (bt[BTN_ID_JUMP].pressed)  action = A_MARK;
                if (bt[BTN_ID_BACK].pressed)  action = A_UP;
                if (bt[BTN_ID_PAUSE].pressed) action = A_EXIT;
                if (bt[BTN_ID_RUN].pressed)   bar_focus = bar_next(-1, 1);
            } else {
                if (bt[BTN_ID_LEFT].pressed)  bar_focus = bar_next(bar_focus, -1);
                if (bt[BTN_ID_RIGHT].pressed) bar_focus = bar_next(bar_focus, 1);
                if (bt[BTN_ID_UP].pressed && bar_focus >= BAR_ROW1)  bar_focus -= BAR_ROW1;
                if (bt[BTN_ID_DOWN].pressed && bar_focus < BAR_ROW1) bar_focus = bar_focus + BAR_ROW1 < B_COUNT ? bar_focus + BAR_ROW1 : B_COUNT - 1;
                if (btn[bar_focus].disabled) bar_focus = bar_next(bar_focus, 1);
                if (bt[BTN_ID_JUMP].pressed || bt[BTN_ID_ACTION].pressed) { action = btn_act[bar_focus]; bar_focus = -1; }
                else if (bt[BTN_ID_RUN].pressed || bt[BTN_ID_BACK].pressed || bt[BTN_ID_PAUSE].pressed) bar_focus = -1;
            }
            if (action != A_NONE) { do_action(action); needs_redraw = true; }
        }

        if (button_take_dirty()) needs_redraw = true;

        bool ptr_moved = !needs_redraw && pointer_paint(&ptr, &fb);
        fb_overtemp_poll(&fb);
        usleep((needs_redraw || job_active || ptr_moved) ? FRAME_DELAY_ACTIVE_US : FRAME_DELAY_IDLE_US);
    }

    if (job_active) { fm_job_cancel(&job); fm_job_free(&job); }
    for (int k = 0; k < 2; k++) { fm_dir_free(&P[k].dir); lv_free(&P[k].lv); }
    hw_leds_off();
    hw_reload_config();
    hw_set_backlight(100);
    fb_clear(&fb, COLOR_BLACK);
    fb_swap(&fb);
    gamepad_close(&gp);
    touch_close(&touch);
    fb_close(&fb);
    return 0;
}
