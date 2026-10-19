/* backup_page.c — control_panel's Backup page: export the unit's settings and
 * scores to a USB stick, and import them back, choosing WHICH in the UI.
 *
 * Exposed only as cp_backup_page (cp_page.h).  What is state is not decided
 * here: /usr/sbin/arca-backup reads the item files in /opt/arca/backup.d and
 * this page lists what `arca-backup items` prints as a marked list_view (the
 * first Control Panel user of common/list_view).  Secret items start unmarked.
 *
 *   HOME      the unit's items; EXPORT TO USB, IMPORT FROM USB
 *   ARCHIVES  the stick's archives, newest first; a tap opens one
 *   IMPORT    that archive's items (secret ones, and unit-bound ones from
 *             another unit, start unmarked, with a warning); IMPORT SELECTED
 *   DONE      "IMPORTED n ITEMS"; REBOOT NOW only if the engine said so
 *
 * The stick is found the way file_manager does: `arca-storage mount`, parsed by
 * fm_parse_storage_line().  file_manager asks once per visit (and on RESCAN);
 * this page also watches /dev once a second on HOME (usb_disk_sig) and mounts again
 * only when a disk node appears; a vanished one just clears the stick.  Backups
 * live in <mount>/ludbox-backup.  Every
 * program runs through cp_exec() (no shell, argv built from ids that passed
 * bk_name_ok()).  A slow run is queued: the status line ("EXPORTING...") is
 * painted first, the run happens on the next input() call (the first load of
 * the items too).  Keyboard and pad
 * reach every visible row and button through focusables(); a row tap toggles
 * its mark (ARCHIVES: opens it).  Ids the importing unit does not know are left
 * out of the list and counted in a note.  The line parsing is backup_parse.c
 * (host test tests/backup_parse_test.c).
 */
#include "cp_page.h"
#include "cp_ui.h"
#include "cp_exec.h"
#include "backup_parse.h"
#include "../common/common.h"
#include "../common/list_view.h"
#include "../common/usb_watch.h"
#include "../file_manager/fm_core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define ENGINE        "/usr/sbin/arca-backup"
#define STORAGE       "/usr/sbin/arca-storage"
#define BACKUP_SUBDIR "ludbox-backup"
#define MAX_ITEMS     48
#define MAX_ARCH      32
#define OUT_BYTES     16384

typedef enum { V_HOME, V_ARCHIVES, V_IMPORT, V_DONE } View;
typedef enum { OP_NONE, OP_LOAD, OP_STICK, OP_EXPORT, OP_ARCHIVES, OP_CONTENTS, OP_IMPORT } Op;

/* ── State ──────────────────────────────────────────────────────────────── */

static View   view;
static Op     pending;
static bool   have_engine;

static BkItem home_items[MAX_ITEMS];  static int n_home;
static BkItem imp_items[MAX_ITEMS];   static int n_imp;
static int    n_skipped;              /* ids in the archive this unit does not know */
static char   arch[MAX_ARCH][BK_FILE_MAX];  static int n_arch;
static int    arch_sel;
static char   my_host[BK_HOST_MAX];
static char   imp_host[BK_HOST_MAX], imp_ver[BK_HOST_MAX], imp_date[BK_HOST_MAX];
static bool   imp_foreign;
static bool   imp_reboot_wanted;      /* the engine's "done reboot=" */
static int    done_n;

static bool   stick_ok;
static char   stick_mp[128];
static char   stick_note[64];         /* shown when there is no stick */
static unsigned stick_sig;            /* usb_disk_sig() when the stick was last looked for */
static uint32_t next_poll_ms;         /* HOME: when to look at /dev again */

static char   out_buf[OUT_BYTES];

static ListView lv;
static bool     lv_ready;
static char     detail_buf[8][24];
static int      row_i;

static Button btn_a, btn_b, btn_up, btn_dn;
static bool   btn_b_shown;
static int    hdr_y, info_y, list_y, btn_y;
static UiRect list_rect;
static bool   prev_touch;
static int    last_x, last_y;

#define ROW_H      30
#define BTN_H      44
#define SCROLL_W   44
#define INFO_PITCH 18

/* ── The list source ────────────────────────────────────────────────────── */

static const BkItem *cur_items(int *n) {
    if (view == V_IMPORT) { *n = n_imp; return imp_items; }
    *n = n_home;
    return home_items;
}

static int src_count(void *ctx) {
    (void)ctx;
    if (view == V_ARCHIVES) return n_arch;
    if (view == V_DONE) return 0;
    int n;
    cur_items(&n);
    return n;
}

static void src_row(void *ctx, int i, LvRow *out) {
    (void)ctx;
    int slot = row_i++ & 7;
    if (view == V_ARCHIVES) {
        if (i >= 0 && i < n_arch) { out->title = arch[i]; out->glyph = LV_GLYPH_FILE; }
        return;
    }
    int n;
    const BkItem *it = cur_items(&n);
    if (i < 0 || i >= n) return;
    /* Marked = list_view's bar and yellow text; the cursor is its own blue row
     * with an edge, so the name carries no prefix.  BLANK: no placeholder box. */
    out->title = it[i].name;
    out->glyph = LV_GLYPH_BLANK;
    snprintf(detail_buf[slot], sizeof detail_buf[slot], "%s%s%s",
             it[i].secret ? "SECRET " : "", it[i].unit ? "UNIT " : "",
             it[i].reboot ? "REBOOT" : "");
    out->detail = detail_buf[slot];
}

/* New data behind the list: counts re-read, cursor/scroll/marks cleared; then
 * the marks every item starts with (foreign = the archive is from another unit). */
static void list_reload(bool foreign) {
    if (!lv_ready) return;
    lv_reset(&lv);
    if (view == V_HOME || view == V_IMPORT) {
        int n;
        const BkItem *it = cur_items(&n);
        for (int i = 0; i < n; i++)
            if (bk_start_marked(&it[i], foreign)) { lv_move_to(&lv, i); lv_key(&lv, LV_KEY_MARK); }
        lv_move_to(&lv, 0);
    }
}

/* ── Engine and stick ───────────────────────────────────────────────────── */

/* Calls fn for each line of buf (which it cuts up). */
static void each_line(char *buf, void (*fn)(const char *line)) {
    for (char *l = buf; l && *l; ) {
        char *nl = strchr(l, '\n');
        if (nl) *nl = '\0';
        fn(l);
        l = nl ? nl + 1 : NULL;
    }
}

/* Runs the engine (argv[0] = ENGINE, NULL-ended); stdout into out_buf.  0 on
 * success; otherwise why holds the failure text, upper case. */
static int run_engine(char *const argv[], char *why, size_t why_n) {
    char err[256];
    out_buf[0] = err[0] = '\0';
    int rc = cp_exec(argv, NULL, out_buf, sizeof out_buf, err, sizeof err);
    if (rc == 0) return 0;
    if (!bk_last_error(out_buf, why, why_n) && !bk_last_error(err, why, why_n))
        snprintf(why, why_n, rc == 127 ? "NO ENGINE" : "ENGINE FAILED");
    for (char *p = why; *p; p++) if (*p >= 'a' && *p <= 'z') *p = (char)(*p - 32);
    return rc > 0 ? rc : 1;
}

static void line_item_home(const char *l) {
    if (n_home < MAX_ITEMS && bk_parse_item(l, &home_items[n_home])) n_home++;
}

static void load_items(void) {
    n_home = 0;
    have_engine = access(ENGINE, X_OK) == 0;
    if (!have_engine) return;
    char *const argv[] = { (char *)ENGINE, "items", NULL };
    char why[64];
    if (run_engine(argv, why, sizeof why) == 0) each_line(out_buf, line_item_home);
    else cp_status(why, false);
}

/* Mounts (if needed) and finds the first stick: stick_ok / stick_mp / stick_note. */
static void find_stick(void) {
    stick_sig = usb_disk_sig();
    stick_ok = false;
    stick_mp[0] = '\0';
    snprintf(stick_note, sizeof stick_note, "NO USB STICK");
    if (access(STORAGE, X_OK) != 0) {
        snprintf(stick_note, sizeof stick_note, "NO USB SUPPORT ON THIS SYSTEM");
        return;
    }
    char *const argv[] = { (char *)STORAGE, "mount", NULL };
    char err[64];
    out_buf[0] = '\0';
    if (cp_exec(argv, NULL, out_buf, sizeof out_buf, err, sizeof err) < 0) return;
    for (char *l = out_buf; l && *l; ) {
        char *nl = strchr(l, '\n');
        if (nl) *nl = '\0';
        FmStLine s = fm_parse_storage_line(l);
        if (!stick_ok && (s.kind == FM_ST_MOUNTED || s.kind == FM_ST_ALREADY)) {
            snprintf(stick_mp, sizeof stick_mp, "%s", s.mp);
            stick_ok = true;
        } else if (!stick_ok && s.kind == FM_ST_UNSUPPORTED) {
            snprintf(stick_note, sizeof stick_note, "STICK NOT SUPPORTED: USE FAT32");
        } else if (!stick_ok && s.kind == FM_ST_FAILED) {
            snprintf(stick_note, sizeof stick_note, "STICK CANNOT BE READ");
        }
        l = nl ? nl + 1 : NULL;
    }
}

static void backup_dir(char *out, size_t n) {
    snprintf(out, n, "%s/%s", stick_mp, BACKUP_SUBDIR);
}

/* ── Buttons and views ──────────────────────────────────────────────────── */

static void set_buttons(const char *a, uint32_t ca, const char *b, uint32_t cb) {
    btn_b_shown = b != NULL;
    int w = btn_b_shown ? (CONTENT_WIDTH - 8) / 2 : CONTENT_WIDTH;
    button_init_full(&btn_a, CONTENT_LEFT, btn_y, w, BTN_H, a, ca, COLOR_WHITE, BTN_COLOR_HIGHLIGHT, 2);
    if (b) button_init_full(&btn_b, CONTENT_RIGHT - w, btn_y, w, BTN_H, b, cb, COLOR_WHITE, BTN_COLOR_HIGHLIGHT, 2);
}

static void apply_view_buttons(void) {
    const uint32_t green = RGB(0, 110, 60), grey = RGB(80, 80, 80);
    switch (view) {
    case V_HOME:     set_buttons("EXPORT TO USB", green, "IMPORT FROM USB", RGB(0, 90, 150)); break;
    case V_ARCHIVES: set_buttons("CANCEL", grey, NULL, 0); break;
    case V_IMPORT:   set_buttons("IMPORT SELECTED", green, "CANCEL", grey); break;
    case V_DONE:
        if (imp_reboot_wanted) set_buttons("REBOOT NOW", RGB(170, 90, 0), "DONE", grey);
        else                   set_buttons("DONE", grey, NULL, 0);
        break;
    }
    if (view == V_HOME) {
        btn_a.disabled = !have_engine || n_home == 0;
        btn_b.disabled = !have_engine;
    } else if (view == V_IMPORT) {
        btn_a.disabled = lv_ready && lv_mark_count(&lv) == 0;
    }
}

static void go_view(View v) {
    view = v;
    list_reload(v == V_IMPORT ? imp_foreign : false);
    apply_view_buttons();
}

/* ── Page contract ──────────────────────────────────────────────────────── */

static void backup_load(const Config *cfg) {
    (void)cfg;
    if (gethostname(my_host, sizeof my_host) == 0) my_host[sizeof my_host - 1] = '\0';
    else my_host[0] = '\0';
}

static void backup_layout(void) {
    /* The marks survive a re-layout (the logical screen changed size). */
    bool keep[MAX_ITEMS] = { false };
    int nk = 0;
    if (lv_ready) {
        nk = lv.count < MAX_ITEMS ? lv.count : MAX_ITEMS;
        for (int i = 0; i < nk; i++) keep[i] = lv_is_marked(&lv, i);
        lv_free(&lv);
    }
    int y = CONTENT_Y + CONTENT_TOP_GAP;
    hdr_y = y;   y += 26;
    info_y = y;  y += 3 * INFO_PITCH + 4;
    list_y = y;
    btn_y = SCREEN_SAFE_BOTTOM - BTN_H - 2;
    int list_h = btn_y - 6 - list_y;
    int list_w = CONTENT_WIDTH - SCROLL_W - 6;
    list_rect = (UiRect){ CONTENT_LEFT, list_y, list_w, list_h };
    lv_init(&lv, (LvSource){ src_count, src_row, NULL }, list_rect, ROW_H);
    lv.active = true;
    lv_ready = true;
    if (nk > 0) {
        lv_reset(&lv);
        for (int i = 0; i < lv.count && i < nk; i++)
            if (keep[i]) { lv_move_to(&lv, i); lv_key(&lv, LV_KEY_MARK); }
        lv_move_to(&lv, 0);
    } else {
        list_reload(view == V_IMPORT ? imp_foreign : false);
    }

    int sh = list_h / 2 - 3;
    if (sh > BTN_H) sh = BTN_H;
    if (sh < 8) sh = 8;
    button_init_full(&btn_up, CONTENT_RIGHT - SCROLL_W, list_y, SCROLL_W, sh, "UP",
                     RGB(80, 80, 80), COLOR_WHITE, BTN_COLOR_HIGHLIGHT, 1);
    button_init_full(&btn_dn, CONTENT_RIGHT - SCROLL_W, list_y + list_h - sh, SCROLL_W, sh, "DN",
                     RGB(80, 80, 80), COLOR_WHITE, BTN_COLOR_HIGHLIGHT, 1);
    apply_view_buttons();

    /* ⚠️ THE RECEIPT: everything hangs off CONTENT_Y, which comes from a
     * per-unit touch inset, so what lies below the touchable edge looks fine in
     * a screenshot and is dead to a finger. */
    {
        int bottom = btn_y + BTN_H - CONTENT_Y;
        const char *verdict = bottom > CONTENT_H ? "⚠ PAST CONTENT BOTTOM"
                            : lv_visible_rows(&lv) < 3 ? "⚠ LIST UNDER 3 ROWS"
                            : "fits";
        printf("control_panel: backup stack %s — bottom +%d of CONTENT_H %d, list %dx%d = %d rows "
               "(safe %dx%d, %s)\n", verdict, bottom, CONTENT_H, list_rect.w, list_rect.h,
               lv_visible_rows(&lv), SCREEN_SAFE_WIDTH, SCREEN_SAFE_HEIGHT,
               CONTENT_WIDTH < 600 ? "portrait" : "landscape");
    }
}

static void backup_enter(void) {
    /* The slow part (items, mount) runs on the next input(), after a frame that
     * says READING... has been painted. */
    have_engine = access(ENGINE, X_OK) == 0;
    n_home = 0;
    go_view(V_HOME);
    pending = OP_LOAD;
    prev_touch = false;
    cp_status("READING...", true);
}

static void backup_leave(void) {
    pending = OP_NONE;
    view = V_HOME;
}

static bool backup_busy(void) { return pending != OP_NONE; }

/* ── Draw ───────────────────────────────────────────────────────────────── */

static void info_line(Framebuffer *fb, int row, const char *text, uint32_t color) {
    char t[96];
    fit_value(text, CONTENT_LEFT, 2, t, sizeof t);
    fb_draw_text(fb, CONTENT_LEFT, info_y + row * INFO_PITCH, t, color, 2);
}

static void backup_draw(Framebuffer *fb) {
    char s[128];
    switch (view) {
    case V_HOME:
        draw_section_header(fb, hdr_y, "BACK UP THIS UNIT");
        if (pending == OP_LOAD) {
            info_line(fb, 0, "READING...", COLOR_LABEL);
            break;
        }
        if (!have_engine) info_line(fb, 0, "NO BACKUP SUPPORT ON THIS SYSTEM", COLOR_YELLOW);
        else if (n_home == 0) info_line(fb, 0, "NOTHING TO BACK UP", COLOR_YELLOW);
        else {
            snprintf(s, sizeof s, "%d OF %d MARKED", lv_mark_count(&lv), n_home);
            info_line(fb, 0, s, COLOR_WHITE);
        }
        if (stick_ok) {
            snprintf(s, sizeof s, "STICK: %s", fm_base(stick_mp));
            info_line(fb, 1, s, COLOR_GREEN);
        } else {
            info_line(fb, 1, stick_note, COLOR_YELLOW);
        }
        info_line(fb, 2, "SECRET ITEMS START UNMARKED", COLOR_LABEL);
        break;
    case V_ARCHIVES:
        draw_section_header(fb, hdr_y, "CHOOSE A BACKUP");
        snprintf(s, sizeof s, "%d BACKUP%s ON THE STICK", n_arch, n_arch == 1 ? "" : "S");
        info_line(fb, 0, s, COLOR_WHITE);
        info_line(fb, 1, "TAP ONE TO SEE WHAT IT HOLDS", COLOR_LABEL);
        break;
    case V_IMPORT:
        draw_section_header(fb, hdr_y, "CHOOSE WHAT TO IMPORT");
        snprintf(s, sizeof s, "FROM %s  %s", imp_host, imp_date);
        info_line(fb, 0, s, COLOR_WHITE);
        snprintf(s, sizeof s, "%d OF %d MARKED", lv_mark_count(&lv), n_imp);
        info_line(fb, 1, s, COLOR_WHITE);
        if (imp_foreign) {
            info_line(fb, 2, "OTHER UNIT: UNIT ITEMS UNMARKED", COLOR_YELLOW);
        } else if (n_skipped) {
            snprintf(s, sizeof s, "%d UNKNOWN ITEM%s LEFT OUT", n_skipped, n_skipped == 1 ? "" : "S");
            info_line(fb, 2, s, COLOR_YELLOW);
        }
        break;
    case V_DONE:
        draw_section_header(fb, hdr_y, "IMPORT DONE");
        snprintf(s, sizeof s, "IMPORTED %d ITEM%s", done_n, done_n == 1 ? "" : "S");
        text_draw_centered(fb, (CONTENT_LEFT + CONTENT_RIGHT) / 2, list_y + 30, s, COLOR_GREEN, 3);
        if (imp_reboot_wanted)
            text_draw_centered(fb, (CONTENT_LEFT + CONTENT_RIGHT) / 2, list_y + 80,
                               "SOME TAKE EFFECT AFTER A REBOOT", COLOR_YELLOW, 2);
        break;
    }
    if (view != V_DONE) {
        lv_sync(&lv);
        lv_draw(fb, &lv, pending == OP_LOAD ? "READING..."
                       : view == V_ARCHIVES ? "NO BACKUPS ON THE STICK"
                       : have_engine ? "NOTHING TO LIST" : "NO BACKUP SUPPORT");
        if (lv.count > lv_visible_rows(&lv)) {
            button_draw(fb, &btn_up);
            button_draw(fb, &btn_dn);
        }
    }
    button_draw(fb, &btn_a);
    if (btn_b_shown) button_draw(fb, &btn_b);
}

/* ── Operations (run from input(), after the status was painted) ────────── */

static void line_archive(const char *l) {
    if (n_arch < MAX_ARCH && bk_parse_archive(l, arch[n_arch], sizeof arch[n_arch])) n_arch++;
}

/* One pass over `contents`: the info line, the items this unit knows, a count
 * of the ids it does not. */
static void line_contents(const char *l) {
    char h[BK_HOST_MAX], v[BK_HOST_MAX], d[BK_HOST_MAX], id[BK_ID_MAX];
    if (bk_parse_info(l, h, sizeof h, v, sizeof v, d, sizeof d)) {
        snprintf(imp_host, sizeof imp_host, "%s", h);
        snprintf(imp_ver, sizeof imp_ver, "%s", v);
        snprintf(imp_date, sizeof imp_date, "%s", d);
    } else if (bk_parse_unknown(l, id, sizeof id)) {
        n_skipped++;
    } else if (n_imp < MAX_ITEMS && bk_parse_item(l, &imp_items[n_imp])) {
        n_imp++;
    }
}

static int imported_seen;
static void line_imported(const char *l) {
    char id[BK_ID_MAX];
    bool rb;
    if (bk_parse_imported(l, id, sizeof id)) imported_seen++;
    else if (bk_parse_done(l, &rb)) imp_reboot_wanted = rb;
}

/* argv = engine, verb, target, then the marked ids of it[]; returns the id count. */
static int marked_argv(char *argv[], int cap, const char *verb, const char *target,
                       const BkItem *it) {
    int k = 0;
    argv[k++] = (char *)ENGINE;
    argv[k++] = (char *)verb;
    argv[k++] = (char *)target;
    for (int i = lv_marked_next(&lv, 0); i >= 0 && k < cap - 1; i = lv_marked_next(&lv, i + 1))
        if (bk_name_ok(it[i].id)) argv[k++] = (char *)it[i].id;
    argv[k] = NULL;
    return k - 3;
}

static void fail_status(const char *what, const char *why) {
    char msg[64];
    snprintf(msg, sizeof msg, "%s: %.*s", what, (int)(sizeof msg - strlen(what) - 3), why);
    cp_status(msg, false);
}

static char exported_path[160];
static void line_exported(const char *l) {
    if (!strncmp(l, "exported ", 9)) snprintf(exported_path, sizeof exported_path, "%s", l + 9);
}

static void do_export(void) {
    find_stick();
    if (!stick_ok) { cp_status(stick_note, false); return; }
    char dir[200], why[64];
    char *argv[MAX_ITEMS + 4];
    backup_dir(dir, sizeof dir);
    if (marked_argv(argv, MAX_ITEMS + 4, "export", dir, home_items) == 0) {
        cp_status("NOTHING MARKED", false);
        return;
    }
    if (run_engine(argv, why, sizeof why) != 0) { fail_status("EXPORT FAILED", why); return; }
    exported_path[0] = '\0';
    each_line(out_buf, line_exported);
    if (!exported_path[0]) { cp_status("EXPORT GAVE NO FILE NAME", false); return; }
    char msg[64];
    snprintf(msg, sizeof msg, "EXPORTED %.52s", fm_base(exported_path));
    cp_status(msg, true);
}

static void do_archives(void) {
    find_stick();
    if (!stick_ok) { cp_status(stick_note, false); return; }
    char dir[200], why[64];
    backup_dir(dir, sizeof dir);
    char *const argv[] = { (char *)ENGINE, "archives", dir, NULL };
    n_arch = 0;
    if (run_engine(argv, why, sizeof why) != 0) { fail_status("CANNOT LIST", why); return; }
    each_line(out_buf, line_archive);
    go_view(V_ARCHIVES);
    cp_status(n_arch ? "CHOOSE A BACKUP" : "NO BACKUPS ON THE STICK", n_arch > 0);
}

static void do_contents(void) {
    char dir[200], path[320], why[64];
    backup_dir(dir, sizeof dir);
    snprintf(path, sizeof path, "%s/%s", dir, arch[arch_sel]);
    char *const argv[] = { (char *)ENGINE, "contents", path, NULL };
    if (run_engine(argv, why, sizeof why) != 0) { fail_status("CANNOT READ IT", why); return; }
    n_imp = 0;
    n_skipped = 0;
    imp_host[0] = imp_ver[0] = imp_date[0] = '\0';
    each_line(out_buf, line_contents);
    imp_foreign = imp_host[0] && my_host[0] && strcmp(imp_host, my_host) != 0;
    if (n_imp == 0) { cp_status("NOTHING IN IT THIS UNIT KNOWS", false); return; }
    go_view(V_IMPORT);
    cp_status(imp_foreign ? "FROM ANOTHER UNIT" : "CHOOSE WHAT TO IMPORT", !imp_foreign);
}

static void do_import(void) {
    char dir[200], path[320], why[64];
    char *argv[MAX_ITEMS + 4];
    backup_dir(dir, sizeof dir);
    snprintf(path, sizeof path, "%s/%s", dir, arch[arch_sel]);
    if (marked_argv(argv, MAX_ITEMS + 4, "import", path, imp_items) == 0) {
        cp_status("NOTHING MARKED", false);
        return;
    }
    imported_seen = 0;
    imp_reboot_wanted = false;
    if (run_engine(argv, why, sizeof why) != 0) { fail_status("IMPORT FAILED", why); return; }
    each_line(out_buf, line_imported);
    done_n = imported_seen;
    char msg[40];
    snprintf(msg, sizeof msg, "IMPORTED %d ITEM%s", done_n, done_n == 1 ? "" : "S");
    go_view(V_DONE);
    cp_status(msg, true);
}

static void run_pending(void) {
    Op op = pending;
    pending = OP_NONE;
    switch (op) {
    case OP_LOAD:
        load_items();
        find_stick();
        go_view(V_HOME);
        next_poll_ms = 0;
        break;
    case OP_STICK:
        find_stick();
        cp_status(stick_ok ? "USB STICK READY" : stick_note, stick_ok);
        break;
    case OP_EXPORT:   do_export();   break;
    case OP_ARCHIVES: do_archives(); break;
    case OP_CONTENTS: do_contents(); break;
    case OP_IMPORT:   do_import();   break;
    case OP_NONE:     break;
    }
}

static void queue(Op op, const char *status) {
    pending = op;
    cp_status(status, true);
}

static void confirmed_import(Config *cfg) { (void)cfg; queue(OP_IMPORT, "IMPORTING..."); }

static void confirmed_reboot(Config *cfg) {
    (void)cfg;
    char *const argv[] = { "reboot", NULL };
    if (cp_exec(argv, NULL, NULL, 0, NULL, 0) != 0) cp_status("REBOOT FAILED", false);
}

/* ── Input ──────────────────────────────────────────────────────────────── */

static bool in_list(int x, int y) {
    return x >= list_rect.x && x < list_rect.x + list_rect.w &&
           y >= list_rect.y && y < list_rect.y + list_rect.h;
}

/* A row was tapped (or focused and activated). */
static CpPageResult row_tapped(int row) {
    if (view == V_ARCHIVES) {
        arch_sel = row;
        queue(OP_CONTENTS, "READING...");
        return CP_PAGE_REDRAW;
    }
    lv_move_to(&lv, row);
    lv_key(&lv, LV_KEY_MARK);
    apply_view_buttons();
    return CP_PAGE_REDRAW;
}

static CpPageResult backup_input(Config *cfg, int tx, int ty, bool touching, uint32_t now) {
    (void)cfg;
    if (pending != OP_NONE) {          /* the status line is on screen: do the slow part */
        run_pending();
        prev_touch = false;
        return CP_PAGE_REDRAW;
    }
    CpPageResult act = CP_PAGE_IDLE;
    /* HOME shows the stick: look at /dev once a second.  A new disk node is
     * mounted on the next call (status first); a vanished one clears the stick
     * at once.  The list, cursor and marks are not touched. */
    if (view == V_HOME && (int32_t)(now - next_poll_ms) >= 0) {
        next_poll_ms = now + 1000;
        unsigned sig = usb_disk_sig();
        if (sig != stick_sig) {
            if (sig == 0) {
                stick_sig = 0;
                stick_ok = false;
                snprintf(stick_note, sizeof stick_note, "NO USB STICK");
                act = CP_PAGE_REDRAW;
            } else {
                queue(OP_STICK, "READING USB...");
                return CP_PAGE_REDRAW;
            }
        }
    }
    int vis = lv_visible_rows(&lv);

    if (view != V_DONE) {
        if (touching && !prev_touch && in_list(tx, ty)) lv_drag_begin(&lv.drag, ty);
        if (touching) {
            last_x = tx;
            last_y = ty;
            if (lv.drag.active) {
                int d = lv_drag_move(&lv.drag, ty, ROW_H);
                if (d) { lv.top = lv_clamp_top(lv.top + d, lv.count, vis); act = CP_PAGE_REDRAW; }
            }
        } else if (prev_touch && lv.drag.active) {
            lv.drag.active = false;
            if (!lv.drag.dragging && last_x < list_rect.x + list_rect.w - LV_BORDER - LV_BAR_W) {
                int row = lv_hit_row(lv.top, last_y - list_rect.y - LV_BORDER, ROW_H, vis, lv.count);
                if (row >= 0) { prev_touch = touching; return row_tapped(row); }
            }
        }
        if (lv.count > vis) {
            if (button_update(&btn_up, tx, ty, touching, now)) { lv_key(&lv, LV_KEY_PGUP); act = CP_PAGE_REDRAW; }
            if (button_update(&btn_dn, tx, ty, touching, now)) { lv_key(&lv, LV_KEY_PGDN); act = CP_PAGE_REDRAW; }
        }
    }
    prev_touch = touching;

    bool a = button_update(&btn_a, tx, ty, touching, now);
    bool b = btn_b_shown && button_update(&btn_b, tx, ty, touching, now);
    switch (view) {
    case V_HOME:
        if (a) { queue(OP_EXPORT, "EXPORTING..."); act = CP_PAGE_REDRAW; }
        if (b) { queue(OP_ARCHIVES, "LOOKING FOR BACKUPS..."); act = CP_PAGE_REDRAW; }
        break;
    case V_ARCHIVES:
        if (a) { go_view(V_HOME); act = CP_PAGE_REDRAW; }
        break;
    case V_IMPORT:
        if (a) {
            char msg[80];
            int n = lv_mark_count(&lv);
            snprintf(msg, sizeof msg, "%d ITEM%s WILL BE REPLACED\nON THIS UNIT", n, n == 1 ? "" : "S");
            cp_confirm("IMPORT", msg, "IMPORT", confirmed_import);
            return CP_PAGE_IDLE;
        }
        if (b) { go_view(V_ARCHIVES); act = CP_PAGE_REDRAW; }
        break;
    case V_DONE:
        if (a && imp_reboot_wanted) {
            cp_confirm("REBOOT NOW", "THE UNIT RESTARTS", "REBOOT", confirmed_reboot);
            return CP_PAGE_IDLE;
        }
        if (a || b) { go_view(V_HOME); act = CP_PAGE_REDRAW; }
        break;
    }
    return act;
}

/* Visible rows (a tap on one toggles its mark), the scroll buttons while the
 * list scrolls, then the action buttons — each at the rect the hit-test uses. */
static int backup_focusables(UiRect *out, int max) {
    int n = 0;
    if (view != V_DONE && pending == OP_NONE) {
        int vis = lv_visible_rows(&lv);
        for (int i = 0; i < vis && lv.top + i < lv.count && n < max; i++)
            out[n++] = (UiRect){ list_rect.x + LV_BORDER,
                                 list_rect.y + LV_BORDER + i * ROW_H,
                                 list_rect.w - 2 * LV_BORDER - LV_BAR_W, ROW_H };
        if (lv.count > vis) {
            n = focus_add_button(out, n, max, &btn_up);
            n = focus_add_button(out, n, max, &btn_dn);
        }
    }
    n = focus_add_button(out, n, max, &btn_a);
    if (btn_b_shown) n = focus_add_button(out, n, max, &btn_b);
    return n;
}

const CpPage cp_backup_page = {
    .name       = "Backup",
    .icon       = "cp_backup",
    .load       = backup_load,
    .layout     = backup_layout,
    .enter      = backup_enter,
    .leave      = backup_leave,
    .draw       = backup_draw,
    .input      = backup_input,
    .busy       = backup_busy,
    .focusables = backup_focusables,
};
