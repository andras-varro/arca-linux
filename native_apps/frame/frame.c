/*
 * frame — the picture frame.  A library of panel-sized JPEG copies in /home/root/data/pictures/,
 * filled from a USB stick or from a Jellyfin photo library (the originals are not kept), shown as a
 * slideshow.
 *
 *   frame                    start menu: START SLIDESHOW, INTERVAL, ORDER, IMPORT FROM USB, FROM JELLYFIN, EXIT
 *   frame --import <dir>     import <dir> without the stick lookup or the notice, then the menu
 *   frame --session <conf>   read the saved Jellyfin sign-in from <conf> (default /opt/ludfin/ludfin.conf);
 *                            the file is only read, this app never signs in
 *
 * The decode and the stored format are frame_core.c; the file key and the walk order are
 * frame_logic.c; the Jellyfin wire strings and reply parsers are frame_jf.c (host tests for all three).
 * Each import or download runs in a forked child so this process keeps polling input and redrawing; the
 * child never touches the framebuffer.  The server listings (libraries, albums, photos) run in this process,
 * polled between frames, and Back cancels them.
 */

#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include "../common/framebuffer.h"
#include "../common/touch_input.h"
#include "../common/hardware.h"
#include "../common/common.h"
#include "../common/gamepad.h"
#include "../common/pointer.h"
#include "../common/start_menu.h"
#include "../common/config.h"
#include "frame_core.h"
#include "frame_logic.h"
#include "frame_jf.h"
#include "../common/http.h"
#include "../common/list_view.h"
#include "../jf_shared/jf_login.h"
#include "../jf_shared/jf_url.h"

#define LIB_DIR        "/home/root/data/pictures"
#define CONF_PATH      "/home/root/data/frame.conf"
#define STORAGE_TOOL   "/usr/sbin/arca-storage mount 2>/dev/null"
#define MAX_FILES      5000
#define MAX_DEPTH      4
#define MAX_MOUNTS     4
#define HINT_MS        3000u
#define SESSION_PATH   "/opt/ludfin/ludfin.conf"
#define JF_CLIENT      "Ludfin"       /* the name the saved token was issued to (the server keys it on the device id) */
#define JF_PAGE        100
#define JF_MAX_ALBUMS  1000
#define JF_MAX_PHOTOS  2000
#define JF_MAX_DEPTH   6
#define JF_IMG_MAX     (3u * 1024u * 1024u)
#define JF_LIST_MAX    (512u * 1024u)

static volatile sig_atomic_t g_running = 1;
static volatile pid_t        g_child;

static void on_signal(int sig)
{
    (void)sig;
    g_running = 0;
    if (g_child > 0) kill(g_child, SIGKILL);
}

typedef struct { Framebuffer fb; TouchInput touch; GamepadManager gp; Pointer ptr; } Dev;

static Dev    g_dev;
static Config g_cfg;
static const char *g_fb_dev = "/dev/fb0", *g_touch_dev = "/dev/input/touchscreen0";

static const char *const INTERVAL_NAMES[] = { "5 SEC", "10 SEC", "20 SEC", "30 SEC", "60 SEC" };
static const int         INTERVAL_S[]     = { 5, 10, 20, 30, 60 };
#define N_INTERVALS 5
static const char *const ORDER_NAMES[] = { "SEQUENTIAL", "SHUFFLE" };
static const char *const FILL_NAMES[]  = { "OFF", "LANDSCAPE", "ALL" };

/* ── input and screens ──────────────────────────────────────────────────── */

static InputState g_in;
static TouchState g_ts;

static void poll_input(uint32_t now)
{
    touch_poll(&g_dev.touch);
    g_ts = touch_get_state(&g_dev.touch);
    gamepad_poll(&g_dev.gp, &g_in, 0, 0, false);
    gamepad_tick(&g_dev.gp, now);
    const ButtonState *bt = g_in.buttons;
    bool nav = bt[BTN_ID_UP].pressed   || bt[BTN_ID_DOWN].pressed  ||
               bt[BTN_ID_LEFT].pressed || bt[BTN_ID_RIGHT].pressed ||
               bt[BTN_ID_JUMP].pressed || bt[BTN_ID_ACTION].pressed ||
               bt[BTN_ID_BACK].pressed || bt[BTN_ID_PAUSE].pressed;
    pointer_update(&g_dev.ptr, &g_in, g_ts.pressed, nav, &g_ts);
}

static bool back_pressed(void)
{
    const ButtonState *bt = g_in.buttons;
    return bt[BTN_ID_BACK].pressed || bt[BTN_ID_PAUSE].pressed || bt[BTN_ID_CANCEL].pressed;
}

static void present(void)
{
    fb_swap(&g_dev.fb);
    pointer_invalidate(&g_dev.ptr);      /* the frame lacks the arrow */
}

typedef void (*MenuHook)(StartMenu *m);

/* Run one StartMenu until an entry is activated (its index) or Back (SM_EXIT). */
static int menu_run(StartMenu *m, MenuHook each)
{
    bool redraw = true;
    while (g_running) {
        uint32_t now = get_time_ms();
        poll_input(now);
        bool ping = false;
        int r = start_menu_step(m, &g_in, g_ts.x, g_ts.y, g_ts.held || g_ts.pressed,
                                gamepad_player_mask(&g_dev.gp), now, &ping);
        if (each) each(m);
        if (r != SM_NONE) return r;
        if (start_menu_needs_redraw(m) || g_ts.pressed || g_ts.released) redraw = true;
        if (redraw) {
            start_menu_draw(m, &g_dev.fb);
            present();
            redraw = false;
        }
        bool moved = pointer_paint(&g_dev.ptr, &g_dev.fb);
        usleep(moved ? 8000 : 16000);
    }
    return SM_EXIT;
}

/* A one-button message screen. */
static void message_screen(const char *title, const char *text)
{
    StartMenu m;
    start_menu_init(&m, title, text, get_time_ms());
    start_menu_add_action(&m, "OK");
    menu_run(&m, NULL);
}

static void draw_centered(const char *text, int y, int scale, uint32_t color)
{
    int w = text_measure_width(text, scale);
    fb_draw_text(&g_dev.fb, SCREEN_VISIBLE_LEFT + (SCREEN_VISIBLE_WIDTH - w) / 2, y, text, color, scale);
}

/* ── library ────────────────────────────────────────────────────────────── */

static void make_lib_dir(void)
{
    mkdir("/home/root", 0755);
    mkdir("/home/root/data", 0755);
    mkdir(LIB_DIR, 0755);
}

typedef struct { char src[300]; char key[100]; char id[33]; } Item;   /* id: the server item of a download (src is then its name) */

static Item *g_items;
static int   g_nitems, g_nskipped, g_ncandidates;

static void scan_dir(const char *dir, int depth)
{
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && g_ncandidates < MAX_FILES && g_running) {
        if (e->d_name[0] == '.') continue;
        char path[300];
        if (snprintf(path, sizeof path, "%s/%s", dir, e->d_name) >= (int)sizeof path) continue;
        struct stat st;
        if (stat(path, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            if (depth < MAX_DEPTH) scan_dir(path, depth + 1);
            continue;
        }
        if (!S_ISREG(st.st_mode) || !frame_is_image_name(e->d_name)) continue;
        g_ncandidates++;
        char key[100], dst[220];
        if (frame_key(path, (long)st.st_size, key, sizeof key) < 0) continue;
        snprintf(dst, sizeof dst, "%s/%s", LIB_DIR, key);
        struct stat ds;
        if (stat(dst, &ds) == 0) { g_nskipped++; continue; }
        snprintf(g_items[g_nitems].src, sizeof g_items[g_nitems].src, "%s", path);
        snprintf(g_items[g_nitems].key, sizeof g_items[g_nitems].key, "%s", key);
        g_nitems++;
    }
    closedir(d);
}

/* ── Jellyfin: session and requests ─────────────────────────────────────── */

static const char *g_session = SESSION_PATH;
static bool        g_job_jf;                  /* import_items runs the Jellyfin fetch, not frame_import */
static JfLogin     g_jl;
static char        g_host[JF_HOST_MAX + 1], g_auth[512];
static int         g_port;
static bool        g_tls;

/* Read the saved sign-in (never written here).  False when there is none usable. */
static bool jf_session_load(void)
{
    memset(&g_jl, 0, sizeof g_jl);
    if (jf_login_load(&g_jl, g_session, "") != 0 || !g_jl.server[0] || !g_jl.user_id[0] || !g_jl.token[0]) return false;
    if (jf_parse_server(g_jl.server, g_host, sizeof g_host, &g_port, &g_tls) != JF_SERVER_OK) return false;
    return jf_auth_header(g_auth, sizeof g_auth, JF_CLIENT, JF_CLIENT, g_jl.device_id, g_jl.token) > 0;
}

enum { JG_OK = 0, JG_FAIL, JG_CANCEL };

/* One GET into buf.  ui: poll input and repaint the pointer, and cancel on Back.  *status = the HTTP status. */
static int jf_http(const char *path, const char *accept, char *buf, size_t cap, size_t *len,
                   int idle_ms, int total_ms, bool ui, int *status)
{
    char hdr[600];
    snprintf(hdr, sizeof hdr, "%s%s", g_auth, accept);
    HttpReq r;
    http_init(&r);
    *status = 0;
    *len = 0;
    http_begin(&r, g_host, g_port, g_tls, "GET", path, hdr, NULL, 0, HTTP_BUFFER, buf, cap);
    r.timeout_ms = idle_ms;
    r.total_timeout_ms = total_ms;
    int res = JG_FAIL;
    for (;;) {
        HttpPoll p = http_poll(&r);
        if (p == HTTP_DONE) { *status = http_status(&r); *len = http_body_len(&r); res = JG_OK; break; }
        if (p == HTTP_ERROR) break;
        if (ui) {
            poll_input(get_time_ms());
            if (!g_running || back_pressed()) { res = JG_CANCEL; break; }
            pointer_paint(&g_dev.ptr, &g_dev.fb);
        }
        usleep(ui ? 20000 : 5000);
    }
    http_end(&r);
    memset(hdr, 0, sizeof hdr);
    return res;
}

/* A listing GET in the parent: LOADING screen once, outcomes mapped to a message.  True = body in buf. */
static bool jf_get(const char *path, char *buf, size_t cap, size_t *len)
{
    fb_clear(&g_dev.fb, COLOR_BLACK);
    draw_centered("LOADING...", SCREEN_VISIBLE_TOP + SCREEN_VISIBLE_HEIGHT / 2 - 10, 3, COLOR_WHITE);
    present();
    int st = 0;
    int r = jf_http(path, "Accept: application/json\r\n", buf, cap - 1, len, 8000, 30000, true, &st);
    if (r == JG_CANCEL) return false;
    if (r != JG_OK) message_screen("CANNOT REACH THE JELLYFIN SERVER", "CHECK THE SERVER AND THE NETWORK.");
    else if (st == 401 || st == 403) message_screen("JELLYFIN REFUSED THE SIGN-IN", "SIGN IN AGAIN WITH THE JELLYFIN APP");
    else if (st != 200) { char t[48]; snprintf(t, sizeof t, "JELLYFIN ANSWERED %d", st); message_screen(t, "THE SERVER DID NOT LIST IT."); }
    else { buf[*len] = 0; return true; }
    return false;
}

/* Child: fetch one photo (server-resized to the panel) into dst.  0 = stored. */
static int jf_fetch_one(const char *id, const char *key, const char *dst)
{
    char *buf = malloc(JF_IMG_MAX + 1);
    if (!buf) return 1;
    char path[300], dl[240];
    snprintf(dl, sizeof dl, "%s.dl", dst);
    int rc = 1;
    for (int attempt = 0; attempt < 2 && rc != 0; attempt++) {
        int n = attempt == 0 ? fj_path_image(path, sizeof path, id, SCREEN_VISIBLE_WIDTH, SCREEN_VISIBLE_HEIGHT)
                             : fj_path_original(path, sizeof path, id);
        if (n < 0) break;
        uint32_t t0 = get_time_ms();
        size_t len = 0;
        int st = 0;
        if (jf_http(path, "Accept: image/jpeg\r\n", buf, JF_IMG_MAX, &len, 8000, 60000, false, &st) != JG_OK) break;
        if (st != 200 || len == 0) continue;           /* 404 or not an image: once more with the original */
        FILE *f = fopen(dl, "wb");
        if (!f) break;
        bool ok = fwrite(buf, 1, len, f) == len;
        ok = (fclose(f) == 0) && ok;
        frame_info_t info;
        memset(&info, 0, sizeof info);
        if (ok && frame_store(dl, dst, SCREEN_VISIBLE_WIDTH, SCREEN_VISIBLE_HEIGHT, 0, &info) == 0) {
            printf("frame: jf: %s recv %dx%d %lu bytes %lu ms%s\n", key, info.src_w, info.src_h, (unsigned long)len,
                   (unsigned long)(get_time_ms() - t0), attempt ? " (original)" : "");
            rc = 0;
        }
        unlink(dl);
    }
    unlink(dl);
    free(buf);
    fflush(stdout);
    return rc;
}

static void draw_progress(const char *verb, int n, int m, const char *src, uint32_t now)
{
    Framebuffer *fb = &g_dev.fb;
    char line[64];
    fb_clear(fb, COLOR_BLACK);
    snprintf(line, sizeof line, "%s %d OF %d", verb, n, m);
    int cy = SCREEN_VISIBLE_TOP + SCREEN_VISIBLE_HEIGHT / 2;
    draw_centered(line, cy - 70, 3, COLOR_WHITE);
    int bw = SCREEN_VISIBLE_WIDTH * 3 / 5, bh = 24;
    int bx = SCREEN_VISIBLE_LEFT + (SCREEN_VISIBLE_WIDTH - bw) / 2;
    fb_draw_rect(fb, bx, cy - 12, bw, bh, COLOR_WHITE);
    int done = m > 0 ? (int)((long)(bw - 4) * (n - 1) / m) : 0;
    if (done > 0) fb_fill_rect(fb, bx + 2, cy - 10, done, bh - 4, RGB(60, 160, 255));
    /* file name, cut to fit */
    const char *base = strrchr(src, '/');
    base = base ? base + 1 : src;
    char nm[300];
    snprintf(nm, sizeof nm, "%s", base);
    while (nm[0] && text_measure_width(nm, 2) > SCREEN_VISIBLE_WIDTH - 40) nm[strlen(nm) - 1] = 0;
    draw_centered(nm, cy + 30, 2, COLOR_GRAY);
    draw_centered("TOUCH OR ESC TO CANCEL", SCREEN_VISIBLE_BOTTOM - 50, 2, COLOR_GRAY);
    (void)now;
    present();
}

/* Import (USB) or download (g_job_jf) everything pending in g_items.  Returns the done/failed counts. */
static void import_items(int *imported, int *failed, bool *cancelled)
{
    *imported = *failed = 0;
    *cancelled = false;
    make_lib_dir();
    uint32_t t0 = get_time_ms();
    for (int i = 0; i < g_nitems && !*cancelled && g_running; i++) {
        char dst[220], tmp[230];
        snprintf(dst, sizeof dst, "%s/%s", LIB_DIR, g_items[i].key);
        snprintf(tmp, sizeof tmp, "%s.tmp", dst);
        draw_progress(g_job_jf ? "DOWNLOADING" : "IMPORTING", i + 1, g_nitems, g_items[i].src, get_time_ms());
        fflush(NULL);
        pid_t pid = fork();
        if (pid < 0) { (*failed)++; continue; }
        if (pid == 0) {
            signal(SIGTERM, SIG_DFL);
            signal(SIGINT, SIG_DFL);
            int rc = g_job_jf ? jf_fetch_one(g_items[i].id, g_items[i].key, dst)
                             : frame_import(g_items[i].src, dst, SCREEN_VISIBLE_WIDTH, SCREEN_VISIBLE_HEIGHT, 0, NULL);
            _exit(rc == 0 ? 0 : 1);
        }
        g_child = pid;
        int st = 0;
        bool reaped = false;
        while (!reaped) {
            uint32_t now = get_time_ms();
            poll_input(now);
            if (g_running == 0 || back_pressed() || (g_ts.pressed && now - t0 > 400)) {
                kill(pid, SIGKILL);
                waitpid(pid, &st, 0);
                unlink(tmp);
                if (g_job_jf) { char dl[240]; snprintf(dl, sizeof dl, "%s.dl", dst); unlink(dl); }
                *cancelled = true;
                reaped = true;
                g_child = 0;
                break;
            }
            pid_t w = waitpid(pid, &st, WNOHANG);
            if (w == pid) {
                reaped = true;
                g_child = 0;
                if (WIFEXITED(st) && WEXITSTATUS(st) == 0) (*imported)++;
                else { (*failed)++; unlink(tmp); if (g_job_jf) { char dl[240]; snprintf(dl, sizeof dl, "%s.dl", dst); unlink(dl); } }
            } else if (w < 0 && errno != EINTR) {
                reaped = true;
                g_child = 0;
                (*failed)++;
            } else {
                pointer_paint(&g_dev.ptr, &g_dev.fb);
                usleep(30000);
            }
        }
    }
}

/* Scan `dirs`, import, show the result.  Used by the USB path and by --import. */
static void import_dirs(const char *const *dirs, int ndirs)
{
    Framebuffer *fb = &g_dev.fb;
    fb_clear(fb, COLOR_BLACK);
    draw_centered("LOOKING FOR PICTURES", SCREEN_VISIBLE_TOP + SCREEN_VISIBLE_HEIGHT / 2 - 10, 3, COLOR_WHITE);
    present();
    g_items = malloc(sizeof(Item) * MAX_FILES);
    if (!g_items) { message_screen("IMPORT", "OUT OF MEMORY"); return; }
    g_nitems = g_nskipped = g_ncandidates = 0;
    for (int i = 0; i < ndirs; i++) scan_dir(dirs[i], 0);
    int imported = 0, failed = 0;
    bool cancelled = false;
    if (g_nitems > 0) import_items(&imported, &failed, &cancelled);
    char text[160];
    snprintf(text, sizeof text, "%sIMPORTED %d\nSKIPPED %d (ALREADY IN THE LIBRARY)\nFAILED %d",
             cancelled ? "CANCELLED\n" : "", imported, g_nskipped, failed);
    printf("frame: import: imported %d skipped %d failed %d%s\n", imported, g_nskipped, failed,
           cancelled ? " (cancelled)" : "");
    free(g_items);
    g_items = NULL;
    if (g_running) message_screen("IMPORT DONE", text);
}

static void save_conf(void) { config_save(&g_cfg); }

/* The one-time quality notice.  False when the operator cancels. */
static bool quality_notice(void)
{
    if (config_get_int(&g_cfg, "notice_ack", 0) != 0) return true;
    StartMenu m;
    start_menu_init(&m, "BEFORE YOU IMPORT",
                    "PICTURES ARE STORED AT SCREEN RESOLUTION\n"
                    "WITH SOME QUALITY LOSS.\n"
                    "THE ORIGINALS ARE NOT KEPT.", get_time_ms());
    start_menu_add_action(&m, "CONTINUE");
    start_menu_add_action(&m, "CANCEL");
    if (menu_run(&m, NULL) != 0) return false;
    config_set_int(&g_cfg, "notice_ack", 1);
    save_conf();
    return true;
}

static void import_from_usb(void)
{
    if (!quality_notice()) return;
    static char mp[MAX_MOUNTS][64];
    const char *dirs[MAX_MOUNTS];
    int nd = 0;
    FILE *p = popen(STORAGE_TOOL, "r");
    if (p) {
        char line[200];
        while (fgets(line, sizeof line, p) && nd < MAX_MOUNTS) {
            char kind[16], dir[64];
            if (sscanf(line, "%15s %63s", kind, dir) == 2 &&
                (strcmp(kind, "MOUNTED") == 0 || strcmp(kind, "ALREADY") == 0)) {
                snprintf(mp[nd], sizeof mp[nd], "%s", dir);
                dirs[nd] = mp[nd];
                nd++;
            }
        }
        pclose(p);
    }
    if (nd == 0) {
        message_screen("NO USB STICK FOUND", "INSERT A USB STICK WITH PICTURES\nAND TRY AGAIN.");
        return;
    }
    import_dirs(dirs, nd);
}

/* ── Jellyfin: albums and download ──────────────────────────────────────── */

/* List a parent's children (photos == false: albums only) or its photos, paged.  Returns the count kept
 * (-1 = failed or cancelled, message shown); *others = rows of the other kind seen. */
/* kind: 0 sub-albums (others counts the photos), 1 every photo below parent, 2 only the photos directly in parent. */
static int jf_list(const char *parent, int kind, FjItem *out, int cap, int *others)
{
    char *buf = malloc(JF_LIST_MAX);
    FjItem *page = malloc(sizeof(FjItem) * JF_PAGE);
    int kept = 0, total = 0;
    *others = 0;
    if (!buf || !page) { free(buf); free(page); message_screen("JELLYFIN", "OUT OF MEMORY"); return -1; }
    for (int start = 0; start == 0 || (start < total && kept < cap); start += JF_PAGE) {
        char path[400];
        size_t len;
        int pl = kind == 2 ? fj_path_loose(path, sizeof path, g_jl.user_id, parent, start, JF_PAGE)
               : kind == 1 ? fj_path_photos(path, sizeof path, g_jl.user_id, parent, start, JF_PAGE)
                        : fj_path_children(path, sizeof path, g_jl.user_id, parent, start, JF_PAGE);
        if (pl < 0) { message_screen("JELLYFIN", "BAD ITEM ID"); kept = -1; break; }
        if (!jf_get(path, buf, JF_LIST_MAX, &len)) { kept = -1; break; }
        int n = fj_parse_items(buf, len, page, JF_PAGE, &total);
        if (n < 0) { message_screen("UNEXPECTED ANSWER FROM THE SERVER", "TRY AGAIN LATER."); kept = -1; break; }
        for (int i = 0; i < n && kept < cap; i++) {
            if (page[i].kind == (kind ? FJ_PHOTO : FJ_ALBUM)) out[kept++] = page[i];
            else (*others)++;
        }
        if (n == 0) break;
    }
    free(buf);
    free(page);
    return kept;
}

typedef struct { const FjItem *it; int n; } AlbumSrc;
static int  album_count(void *c) { return ((AlbumSrc *)c)->n; }
static void album_row(void *c, int i, LvRow *o) { o->title = ((AlbumSrc *)c)->it[i].name; o->glyph = LV_GLYPH_FOLDER; }

/* The album list: touch, pad, keyboard and mouse.  Returns the row, or -1 for Back. */
static int album_list(const char *title, const FjItem *it, int n)
{
    Framebuffer *fb = &g_dev.fb;
    AlbumSrc as = { it, n };
    ListView lv;
    int top = SCREEN_VISIBLE_TOP + 44, bh = 44;
    UiRect back = { SCREEN_SAFE_LEFT + 10, SCREEN_SAFE_BOTTOM - bh - 4, 140, bh };
    lv_init(&lv, (LvSource){ album_count, album_row, &as },
            (UiRect){ SCREEN_SAFE_LEFT, top, SCREEN_SAFE_WIDTH, back.y - 8 - top }, 40);
    lv.active = true;
    int result = -1;
    bool redraw = true;
    while (g_running) {
        uint32_t now = get_time_ms();
        poll_input(now);
        lv_sync(&lv);
        LvEvent ev = lv_input(&lv, &g_ts, &g_in, now);
        if (back_pressed()) break;
        if (g_ts.released && g_ts.x >= back.x && g_ts.x < back.x + back.w && g_ts.y >= back.y && g_ts.y < back.y + back.h) break;
        if (ev == LV_EV_ACTIVATE && lv.cursor >= 0) { result = lv.cursor; break; }
        if (ev != LV_EV_NONE || g_ts.pressed || g_ts.released) redraw = true;
        if (redraw) {
            char t[64];
            fb_clear(fb, COLOR_BLACK);
            lv_fit(t, sizeof t, title, SCREEN_VISIBLE_WIDTH - 40, 3);
            fb_draw_text(fb, SCREEN_VISIBLE_LEFT + 20, SCREEN_VISIBLE_TOP + 10, t, COLOR_WHITE, 3);
            lv_draw(fb, &lv, "NO ALBUMS");
            fb_draw_rect(fb, back.x, back.y, back.w, back.h, COLOR_WHITE);
            fb_draw_text(fb, back.x + 30, back.y + 14, "BACK", COLOR_WHITE, 2);
            present();
            redraw = false;
        }
        bool moved = pointer_paint(&g_dev.ptr, fb);
        usleep(moved ? 8000 : 16000);
    }
    lv_free(&lv);
    return result;
}

/* Download every photo under `folder`; what the library already holds is skipped. */
static void jf_download(const FjItem *folder)
{
    FjItem *ph = malloc(sizeof(FjItem) * JF_MAX_PHOTOS);
    g_items = malloc(sizeof(Item) * JF_MAX_PHOTOS);
    if (!ph || !g_items) { message_screen("DOWNLOAD", "OUT OF MEMORY"); goto out; }
    int others, np = jf_list(folder->id, folder->loose ? 2 : 1, ph, JF_MAX_PHOTOS, &others);
    if (np < 0) goto out;
    if (np == 0) { message_screen("THIS ALBUM HAS NO PHOTOS", ""); goto out; }
    make_lib_dir();
    g_nitems = g_nskipped = 0;
    for (int i = 0; i < np; i++) {
        char key[48], dst[220];
        struct stat st;
        if (fj_key_from_id(key, sizeof key, ph[i].id) < 0) continue;
        snprintf(dst, sizeof dst, "%s/%s", LIB_DIR, key);
        if (stat(dst, &st) == 0) { g_nskipped++; continue; }
        snprintf(g_items[g_nitems].src, sizeof g_items[g_nitems].src, "%s", ph[i].name);
        snprintf(g_items[g_nitems].key, sizeof g_items[g_nitems].key, "%s", key);
        snprintf(g_items[g_nitems].id, sizeof g_items[g_nitems].id, "%s", ph[i].id);
        g_nitems++;
    }
    int done = 0, failed = 0;
    bool cancelled = false;
    g_job_jf = true;
    if (g_nitems > 0) import_items(&done, &failed, &cancelled);
    g_job_jf = false;
    char text[160];
    snprintf(text, sizeof text, "%sDOWNLOADED %d\nSKIPPED %d (ALREADY IN THE LIBRARY)\nFAILED %d",
             cancelled ? "CANCELLED\n" : "", done, g_nskipped, failed);
    printf("frame: jellyfin: downloaded %d skipped %d failed %d%s\n", done, g_nskipped, failed, cancelled ? " (cancelled)" : "");
    if (g_running) message_screen("DOWNLOAD DONE", text);
out:
    free(ph);
    free(g_items);
    g_items = NULL;
}

static void jf_browse(const FjItem *it, int n, const char *title, int depth);

/* Open one folder: its sub-albums as a list, or, with none, the offer to download its photos. */
static void jf_open(const FjItem *folder, int depth)
{
    FjItem *al = malloc(sizeof(FjItem) * JF_MAX_ALBUMS);
    if (!al) { message_screen("JELLYFIN", "OUT OF MEMORY"); return; }
    int others, n = jf_list(folder->id, 0, al, JF_MAX_ALBUMS, &others);
    if (n > 0) {
        n = fj_add_loose(al, n, JF_MAX_ALBUMS, folder, others);   /* photos lying loose in this folder become an album of their own */
        jf_browse(al, n, folder->name, depth);
    }
    else if (n == 0) {
        StartMenu m;
        start_menu_init(&m, folder->name, others > 0 ? "THIS ALBUM HOLDS PHOTOS." : "THIS ALBUM IS EMPTY.", get_time_ms());
        start_menu_add_action(&m, "DOWNLOAD ALL PHOTOS");
        start_menu_add_action(&m, "BACK");
        if (menu_run(&m, NULL) == 0) jf_download(folder);
    }
    free(al);
}

static void jf_browse(const FjItem *it, int n, const char *title, int depth)
{
    for (;;) {
        int row = album_list(title, it, n);
        if (row < 0 || !g_running) return;
        StartMenu m;
        start_menu_init(&m, it[row].name, NULL, get_time_ms());
        start_menu_add_action(&m, "DOWNLOAD ALL PHOTOS");
        int e_open = depth < JF_MAX_DEPTH && !it[row].loose ? start_menu_add_action(&m, "OPEN") : -1;
        start_menu_add_action(&m, "BACK");
        int r = menu_run(&m, NULL);
        if (r == 0) jf_download(&it[row]);
        else if (e_open >= 0 && r == e_open) jf_open(&it[row], depth + 1);
        memset(&g_in, 0, sizeof g_in);
        pointer_drain(&g_dev.ptr);
    }
}

static void from_jellyfin(void)
{
    if (!quality_notice()) return;
    if (!jf_session_load()) {
        message_screen("NOT SIGNED IN TO JELLYFIN", "SIGN IN WITH THE JELLYFIN APP FIRST.");
        return;
    }
    char path[200], *buf = malloc(JF_LIST_MAX);
    FjItem libs[16];
    size_t len;
    if (!buf) return;
    int n = -1;
    if (fj_path_views(path, sizeof path, g_jl.user_id) > 0 && jf_get(path, buf, JF_LIST_MAX, &len)) {
        n = fj_parse_libraries(buf, len, libs, 16);
        if (n < 0) message_screen("UNEXPECTED ANSWER FROM THE SERVER", "TRY AGAIN LATER.");
    }
    if (n > 0) {                       /* a library not typed "photos" counts only when it holds a Photo */
        int kept = 0;
        for (int i = 0; i < n; i++) {
            if (libs[i].probe) {
                FjItem one[1];
                int total = 0, pn = -1;
                if (fj_path_photos(path, sizeof path, g_jl.user_id, libs[i].id, 0, 1) > 0 && jf_get(path, buf, JF_LIST_MAX, &len))
                    pn = fj_parse_items(buf, len, one, 1, &total);
                printf("frame: jellyfin: library \"%s\" probe: %d photo(s)\n", libs[i].name, pn < 0 ? -1 : total);
                if (pn < 1) continue;
            }
            libs[kept++] = libs[i];
        }
        n = kept;
    }
    free(buf);
    if (n == 0) message_screen("NO PHOTO LIBRARY ON THE SERVER", "");
    else if (n == 1) jf_open(&libs[0], 1);
    else if (n > 1) jf_browse(libs, n, "PHOTO LIBRARIES", 1);
    memset(g_auth, 0, sizeof g_auth);
    memset(&g_jl, 0, sizeof g_jl);
}

/* ── slideshow ──────────────────────────────────────────────────────────── */

typedef struct { char **name; int n; } Lib;

static int cmp_names(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static void lib_load(Lib *l)
{
    l->name = NULL;
    l->n = 0;
    DIR *d = opendir(LIB_DIR);
    if (!d) return;
    int cap = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        size_t len = strlen(e->d_name);
        if (len < 5 || e->d_name[0] == '.' || strcasecmp(e->d_name + len - 4, ".jpg") != 0) continue;
        if (l->n == cap) {
            cap = cap ? cap * 2 : 64;
            char **nn = realloc(l->name, sizeof(char *) * cap);
            if (!nn) break;
            l->name = nn;
        }
        l->name[l->n] = strdup(e->d_name);
        if (l->name[l->n]) l->n++;
    }
    closedir(d);
    if (l->n > 1) qsort(l->name, l->n, sizeof(char *), cmp_names);
}

static void lib_free(Lib *l)
{
    for (int i = 0; i < l->n; i++) free(l->name[i]);
    free(l->name);
    l->name = NULL;
    l->n = 0;
}

static int lib_count(void)
{
    Lib l;
    lib_load(&l);
    int n = l.n;
    lib_free(&l);
    return n;
}

static int g_fill;                        /* FRAME_FILL_*: the start menu's FILL choice, read by slot_load */

typedef struct { uint32_t *px; int pic; } Slot;

static bool slot_load(const Lib *l, Slot *s, int pic)
{
    char path[300];
    s->pic = -1;
    snprintf(path, sizeof path, "%s/%s", LIB_DIR, l->name[pic]);
    Framebuffer *fb = &g_dev.fb;
    memset(s->px, 0, (size_t)fb->width * fb->height * 4);
    uint32_t t0 = get_time_ms();
    frame_info_t fi;
    if (frame_load_mode(path, s->px, fb->width, fb->height, fb->width, g_fill, &fi) != 0) return false;
    printf("frame: slide %s %dx%d %s %lu ms\n", l->name[pic], fi.out_w, fi.out_h,
           frame_fill_wanted(g_fill, fi.out_w, fi.out_h, fb->width, fb->height) ? "fill" : "fit", (unsigned long)(get_time_ms() - t0));
    s->pic = pic;
    return true;
}

static void blit(const Slot *s, bool hint)
{
    Framebuffer *fb = &g_dev.fb;
    memcpy(fb->back_buffer, s->px, (size_t)fb->width * fb->height * 4);
    if (hint) {
        const char *t = "TAP CENTRE FOR THE MENU, ESC TO EXIT";
        int w = text_measure_width(t, 2);
        int x = SCREEN_VISIBLE_LEFT + (SCREEN_VISIBLE_WIDTH - w) / 2;
        int y = SCREEN_VISIBLE_BOTTOM - 34;
        fb_fill_rect(fb, x - 8, y - 6, w + 16, 28, COLOR_BLACK);
        fb_draw_text(fb, x, y, t, COLOR_WHITE, 2);
    }
    present();
}

enum { SL_RESUME = 0, SL_DELETE, SL_LEAVE };

/* The slideshow's menu (centre tap, Tab / RB / Home): resume, delete the picture on screen (after a confirm), or
 * leave.  Esc resumes; in the confirm it goes back to the menu. */
static int slide_menu(void)
{
    for (;;) {
        StartMenu m;
        start_menu_init(&m, "PICTURE", NULL, get_time_ms());
        start_menu_add_action(&m, "RESUME");
        start_menu_add_action(&m, "DELETE THIS PICTURE");
        start_menu_add_action(&m, "EXIT SLIDESHOW");
        int r = menu_run(&m, NULL);
        if (r == 2) return SL_LEAVE;
        if (r != 1) return SL_RESUME;
        StartMenu c;
        start_menu_init(&c, "DELETE THIS PICTURE?", "IT CANNOT BE UNDONE.", get_time_ms());
        start_menu_add_action(&c, "CANCEL");
        start_menu_add_action(&c, "DELETE");
        if (menu_run(&c, NULL) == 1) return SL_DELETE;
    }
}

static void slideshow(int interval_s, bool shuffle)
{
    Lib lib;
    lib_load(&lib);
    if (lib.n == 0) { lib_free(&lib); return; }
    Framebuffer *fb = &g_dev.fb;
    size_t bytes = (size_t)fb->width * fb->height * 4;
    Slot slot[2];
    slot[0].px = malloc(bytes);
    slot[1].px = malloc(bytes);
    int *order = malloc(sizeof(int) * lib.n);
    if (!slot[0].px || !slot[1].px || !order) {
        free(slot[0].px); free(slot[1].px); free(order);
        lib_free(&lib);
        message_screen("PICTURES", "OUT OF MEMORY");
        return;
    }
    slot[0].pic = slot[1].pic = -1;
    FrameOrder ord;
    frame_order_init(&ord, order, lib.n, shuffle, get_time_ms());

    hw_blank_set_enabled(false);
    int cur = 0;                    /* slot on screen */
    int dir = 1;                    /* the way the last step went, for skipping bad files */
    bool need_show = true, need_preload = false, touch_down = false, show_hint = true, hint_on = false;
    uint32_t shown_at = 0, start = get_time_ms();
    uint32_t interval_ms = (uint32_t)interval_s * 1000u;
    int lastx = 0, fails = 0;
    bool leave = false, open_menu = false;

    while (g_running && !leave) {
        uint32_t now = get_time_ms();
        poll_input(now);
        const ButtonState *bt = g_in.buttons;
        int step = 0;
        if (back_pressed()) leave = true;
        if (bt[BTN_ID_RIGHT].pressed || bt[BTN_ID_JUMP].pressed || bt[BTN_ID_ACTION].pressed ||
            bt[BTN_ID_DOWN].pressed || bt[BTN_ID_PGDN].pressed) step = 1;
        if (bt[BTN_ID_LEFT].pressed || bt[BTN_ID_UP].pressed || bt[BTN_ID_PGUP].pressed) step = -1;
        if (bt[BTN_ID_NEXT].pressed || bt[BTN_ID_HOME].pressed) open_menu = true;
        if (g_ts.pressed || g_ts.held) { lastx = g_ts.x; if (g_ts.pressed) touch_down = true; }
        if (g_ts.released && touch_down) {
            touch_down = false;
            int w40 = SCREEN_SAFE_WIDTH * 2 / 5;
            if (lastx < SCREEN_SAFE_LEFT + w40) step = -1;
            else if (lastx >= SCREEN_SAFE_RIGHT - w40) step = 1;
            else open_menu = true;
        }
        if (leave) break;
        if (open_menu) {
            open_menu = false;
            int act = slide_menu();
            memset(&g_in, 0, sizeof g_in);
            pointer_drain(&g_dev.ptr);
            touch_down = false;
            if (act == SL_LEAVE) break;
            if (act == SL_DELETE) {
                int p = frame_order_cur(&ord);
                char path[300];
                snprintf(path, sizeof path, "%s/%s", LIB_DIR, lib.name[p]);
                printf("frame: delete %s: %s\n", lib.name[p], unlink(path) == 0 ? "ok" : "failed");
                lib_free(&lib);
                lib_load(&lib);
                if (lib.n == 0) break;
                frame_order_init(&ord, order, lib.n, shuffle, get_time_ms());
                if (!shuffle) ord.pos = p < lib.n ? p : 0;       /* the next picture moved into this place */
                slot[0].pic = slot[1].pic = -1;
                fails = 0;
            }
            need_show = true;
            continue;
        }
        if (!need_show && step == 0 && now - shown_at >= interval_ms) step = 1;
        if (step != 0) {
            if (step > 0) frame_order_next(&ord); else frame_order_prev(&ord);
            dir = step;
            need_show = true;
        }

        if (need_show) {
            int p = frame_order_cur(&ord);
            bool ok = true;
            if (slot[cur].pic != p) {
                if (slot[1 - cur].pic == p) cur = 1 - cur;
                else ok = slot_load(&lib, &slot[cur], p);
            }
            if (!ok) {
                printf("frame: cannot load %s\n", lib.name[p]);
                if (++fails >= lib.n) { message_screen("PICTURES", "NO PICTURE COULD BE LOADED"); break; }
                if (dir > 0) frame_order_next(&ord); else frame_order_prev(&ord);
                continue;
            }
            fails = 0;
            hint_on = show_hint && now - start < HINT_MS;
            blit(&slot[cur], hint_on);
            shown_at = get_time_ms();
            need_show = false;
            need_preload = true;
        } else if (hint_on && now - start >= HINT_MS) {
            hint_on = false;
            show_hint = false;
            blit(&slot[cur], false);
        } else if (need_preload) {
            need_preload = false;
            int q = frame_order_peek_next(&ord);
            if (q >= 0 && slot[1 - cur].pic != q) slot_load(&lib, &slot[1 - cur], q);
            continue;
        }
        bool moved = pointer_paint(&g_dev.ptr, &g_dev.fb);
        usleep(moved ? 8000 : 16000);
    }
    hw_blank_set_enabled(true);
    free(slot[0].px);
    free(slot[1].px);
    free(order);
    lib_free(&lib);
}

/* ── menus ──────────────────────────────────────────────────────────────── */

static int g_iv_idx, g_ord_idx, g_fill_idx, g_saved_iv = -1, g_saved_ord = -1, g_saved_fill = -1;

static void persist_hook(StartMenu *m)
{
    int iv = start_menu_value(m, g_iv_idx), od = start_menu_value(m, g_ord_idx), fl = start_menu_value(m, g_fill_idx);
    if (iv != g_saved_iv || od != g_saved_ord || fl != g_saved_fill) {
        g_saved_iv = iv;
        g_saved_ord = od;
        g_saved_fill = fl;
        config_set_int(&g_cfg, "fill", fl);
        config_set_int(&g_cfg, "interval_s", INTERVAL_S[iv]);
        config_set_int(&g_cfg, "shuffle", od);
        save_conf();
    }
}

static int interval_index(int s)
{
    for (int i = 0; i < N_INTERVALS; i++) if (INTERVAL_S[i] == s) return i;
    return 1;
}

static void empty_library(void)
{
    StartMenu m;
    start_menu_init(&m, "NO PICTURES YET", "IMPORT PICTURES FROM A USB STICK\nOR DOWNLOAD THEM FROM JELLYFIN.", get_time_ms());
    start_menu_add_action(&m, "IMPORT FROM USB");
    start_menu_add_action(&m, "FROM JELLYFIN");
    start_menu_add_action(&m, "BACK");
    int r = menu_run(&m, NULL);
    if (r == 0) import_from_usb();
    else if (r == 1) from_jellyfin();
}

/* DELETE ALL PICTURES: a confirm that shows the count, then every library picture is removed. */
static void delete_all(void)
{
    Lib l;
    lib_load(&l);
    int n = l.n;
    if (n == 0) { lib_free(&l); message_screen("NO PICTURES TO DELETE", ""); return; }
    char text[64];
    snprintf(text, sizeof text, "DELETE ALL %d PICTURES?", n);
    StartMenu c;
    start_menu_init(&c, text, "THEY CANNOT BE GOT BACK.", get_time_ms());
    start_menu_add_action(&c, "CANCEL");
    start_menu_add_action(&c, "DELETE ALL");
    int gone = 0;
    if (menu_run(&c, NULL) == 1) {
        for (int i = 0; i < n; i++) {
            char path[300];
            snprintf(path, sizeof path, "%s/%s", LIB_DIR, l.name[i]);
            if (unlink(path) == 0) gone++;
        }
        printf("frame: deleted %d of %d pictures\n", gone, n);
    }
    lib_free(&l);
}

int main(int argc, char *argv[])
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    const char *import_dir = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--import") == 0 && i + 1 < argc) import_dir = argv[++i];
        else if (strcmp(argv[i], "--session") == 0 && i + 1 < argc) g_session = argv[++i];
        else if (strcmp(argv[i], "--fb") == 0 && i + 1 < argc) g_fb_dev = argv[++i];
        else if (strcmp(argv[i], "--touch") == 0 && i + 1 < argc) g_touch_dev = argv[++i];
    }
    int lock_fd = acquire_instance_lock("frame");
    if (lock_fd < 0) return 1;
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    hw_init();
    hw_set_backlight(100);
    memset(&g_dev, 0, sizeof g_dev);
    fb_set_bpp(g_fb_dev, 32);
    if (fb_init(&g_dev.fb, g_fb_dev) < 0) { fprintf(stderr, "frame: cannot open framebuffer %s\n", g_fb_dev); return 1; }
    if (touch_init(&g_dev.touch, g_touch_dev) == 0) {
        touch_set_screen_size(&g_dev.touch, g_dev.fb.width, g_dev.fb.height);
        touch_drain_events(&g_dev.touch);
    }
    gamepad_init(&g_dev.gp);
    pointer_init(&g_dev.ptr);           /* after touch_init: SCREEN_SAFE_* */
    memset(&g_in, 0, sizeof g_in);

    config_init_path(&g_cfg, CONF_PATH);
    config_load(&g_cfg);
    int iv = interval_index(config_get_int(&g_cfg, "interval_s", 10));
    int od = config_get_int(&g_cfg, "shuffle", 0) ? 1 : 0;
    g_saved_iv = iv;
    g_saved_ord = od;
    int fl = config_get_int(&g_cfg, "fill", 0);
    if (fl < 0 || fl >= FRAME_FILL_N) fl = 0;
    g_saved_fill = fl;

    if (import_dir) {
        const char *d[1] = { import_dir };
        import_dirs(d, 1);
    }

    StartMenu m;
    start_menu_init(&m, "PICTURES", NULL, get_time_ms());
    int e_start = start_menu_add_action(&m, "START SLIDESHOW");
    g_iv_idx    = start_menu_add_choice(&m, "INTERVAL", INTERVAL_NAMES, N_INTERVALS, iv);
    g_ord_idx   = start_menu_add_choice(&m, "ORDER", ORDER_NAMES, 2, od);
    g_fill_idx  = start_menu_add_choice(&m, "FILL", FILL_NAMES, FRAME_FILL_N, fl);
    int e_imp   = start_menu_add_action(&m, "IMPORT FROM USB");
    int e_jf    = start_menu_add_action(&m, "FROM JELLYFIN");
    int e_del   = start_menu_add_action(&m, "DELETE ALL PICTURES");
    int e_exit  = start_menu_add_action(&m, "EXIT");
    start_menu_select(&m, e_start, get_time_ms());

    while (g_running) {
        int r = menu_run(&m, persist_hook);
        if (r == SM_EXIT || r == e_exit) break;
        if (r == e_start) {
            if (lib_count() == 0) empty_library();
            else { g_fill = start_menu_value(&m, g_fill_idx); slideshow(INTERVAL_S[start_menu_value(&m, g_iv_idx)], start_menu_value(&m, g_ord_idx) == 1); if (lib_count() == 0) empty_library(); }
        } else if (r == e_imp) {
            import_from_usb();
        } else if (r == e_del) {
            delete_all();
        } else if (r == e_jf) {
            from_jellyfin();
        }
        memset(&g_in, 0, sizeof g_in);
        pointer_drain(&g_dev.ptr);
        start_menu_reopen(&m, get_time_ms());
    }

    hw_blank_set_enabled(true);
    hw_leds_off();
    hw_set_backlight(100);
    fb_clear(&g_dev.fb, COLOR_BLACK);
    fb_swap(&g_dev.fb);
    gamepad_close(&g_dev.gp);
    touch_close(&g_dev.touch);
    fb_close(&g_dev.fb);
    return 0;
}
