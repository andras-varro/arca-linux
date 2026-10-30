/*
 * retro_picker — the "Retro" station front end: one screen, one game list;
 * runs the libretro frontend `retro` on the chosen game and comes back to the
 * list when it exits.
 *
 * Screen: the common top bar (X exits) whose title is a drop-down: "All games"
 * plus one entry per installed system.  The list below shows the chosen view
 * (in All games each row carries its system name as a right-aligned detail).
 * The last view is kept in <data>/last-view and restored on the next start.
 *
 * Systems are DATA, not code: one INI-like file per system in
 * <data>/systems/<id>.sys (default data dir /home/root/data/retro):
 *
 *     name=Commodore 64            display name
 *     core=vice_x64_libretro.so    bare file name -> <data>/cores/<name>; a path with '/' is used as is
 *     exts=m3u,d64,t64,prg,crt,zip content extensions, comma separated, case-insensitive, no dot
 *                                  (a .m3u playlist is one game; the files it lists are hidden from the list)
 *     opts=key=value,key=value     optional core options, passed to retro as RETRO_OPTS
 *     roms=/some/dir               optional; default <data>/roms/<id>
 *                                  (a subfolder there is one game, named by the folder)
 *     video=sw|overlay             optional; passed as retro -v
 *     nogame=BASIC                 optional; a first list entry with this label that starts
 *                                  retro with no content ("-"), for cores that run without a game
 *
 * A system whose core file is missing is left out (not greyed).  Cores and
 * content are not part of any image.  Adding a system = dropping a .sys file.
 *
 * Input: touch (tap a row, drag to scroll, tap the title for the drop-down, X to
 * exit); keyboard and pad: Up/Down move (Up on the first row opens the drop-down),
 * Left/Right and PgUp/PgDn page, RB/LB or Tab/Shift+Tab cycle the views,
 * Enter/Space/A/X launch, Esc/Backspace/B/Start/Select close the drop-down if
 * open, else exit.  B never launches.
 *
 * Launch model mirrors app_launcher: release pad, touch and framebuffer, fork +
 * exec, waitpid, restore 32bpp, re-open everything, swallow the keys still held.
 *
 * Environment (test only): RETRO_DATA overrides the data dir, RETRO_BIN the
 * frontend binary (default /opt/games/retro).
 */
#include "../common/framebuffer.h"
#include "../common/touch_input.h"
#include "../common/hardware.h"
#include "../common/common.h"
#include "../common/gamepad.h"
#include "../common/pointer.h"
#include "../common/list_view.h"
#include "../common/top_bar.h"
#include "../common/top_pick.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define DEFAULT_DATA "/home/root/data/retro"
#define DEFAULT_BIN  "/opt/games/retro"
#define MAX_SYSTEMS  32
#define RETURN_COOLDOWN_MS 500

typedef struct {
    char id[32], name[48], core[256], corepath[400], exts[96], opts[256], roms[256], video[16];
    char nogame[48];

} System;

/* file is empty for the nogame entry: retro is started with no content. */
typedef struct { char file[600]; char title[256]; int sys; } Game;   /* sys: index into g_sys */

static System  g_sys[MAX_SYSTEMS];
static int     g_nsys;
static Game   *g_games;
static int     g_ngames;
static int     g_view;                 /* 0 = All games, n = g_sys[n - 1] */
static char    g_data[200] = DEFAULT_DATA;
static char    g_bin[200]  = DEFAULT_BIN;
static char    g_msg[80];
static uint32_t g_msg_ms;

static volatile sig_atomic_t g_running = 1;
static volatile pid_t g_child;

static void on_signal(int sig)
{
    (void)sig;
    g_running = 0;
    if (g_child > 0) kill(g_child, SIGTERM);   /* arca-app stop reaches the picker, not the grandchild */
}

/* ── system files ───────────────────────────────────────────────────────── */

static void copy_str(char *dst, size_t n, const char *s) { snprintf(dst, n, "%s", s); }

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n')) s[--n] = 0;
    return s;
}

/* Parse one .sys file into *s; returns 0 when it has the keys it needs. */
static int parse_sys(const char *path, const char *id, System *s)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    memset(s, 0, sizeof *s);
    copy_str(s->id, sizeof s->id, id);
    char line[512];
    while (fgets(line, sizeof line, f)) {
        char *l = trim(line);
        if (!*l || *l == '#') continue;
        char *eq = strchr(l, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = trim(l), *v = trim(eq + 1);
        if      (!strcmp(k, "name"))  copy_str(s->name,  sizeof s->name,  v);
        else if (!strcmp(k, "core"))  copy_str(s->core,  sizeof s->core,  v);
        else if (!strcmp(k, "exts"))  copy_str(s->exts,  sizeof s->exts,  v);
        else if (!strcmp(k, "opts"))  copy_str(s->opts,  sizeof s->opts,  v);
        else if (!strcmp(k, "roms"))  copy_str(s->roms,  sizeof s->roms,  v);
        else if (!strcmp(k, "video")) copy_str(s->video, sizeof s->video, v);
        else if (!strcmp(k, "nogame")) copy_str(s->nogame, sizeof s->nogame, v);
    }
    fclose(f);
    if (!s->core[0] || !s->exts[0]) return -1;
    if (!s->name[0]) copy_str(s->name, sizeof s->name, id);
    if (!s->roms[0]) snprintf(s->roms, sizeof s->roms, "%s/roms/%s", g_data, id);
    if (strchr(s->core, '/')) copy_str(s->corepath, sizeof s->corepath, s->core);
    else snprintf(s->corepath, sizeof s->corepath, "%s/cores/%s", g_data, s->core);
    return 0;
}

/* Which of the comma separated extensions does the file name end in?  1 for
 * the first, 2 for the second, ...; 0 for none. */
static int ext_rank(const char *exts, const char *name)
{
    const char *dot = strrchr(name, '.');
    if (!dot || !dot[1]) return 0;
    dot++;
    size_t dl = strlen(dot);
    for (int rank = 1; *exts; rank++) {
        size_t n = strcspn(exts, ",");
        if (n == dl && !strncasecmp(exts, dot, n)) return rank;
        exts += n;
        if (*exts) exts++;
    }
    return 0;
}

/* A game folder (e.g. CAULDRON/ holding CAULDRON.COM and its data files) is
 * one game.  Its program is the matching file named like the folder, else the
 * one whose extension comes first in exts, else the first by name.  Writes the
 * path into out; returns 0 if the folder holds no matching file. */
static int pick_in_folder(const char *exts, const char *dir, const char *folder,
                          char *out, size_t outsz)
{
    DIR *d = opendir(dir);
    if (!d) return 0;
    char best[256] = "";
    int best_score = 0;
    size_t fl = strlen(folder);
    struct dirent *e;
    while ((e = readdir(d))) {
        int rank = e->d_name[0] == '.' ? 0 : ext_rank(exts, e->d_name);
        if (!rank) continue;
        const char *dot = strrchr(e->d_name, '.');
        int named = (size_t)(dot - e->d_name) == fl && !strncasecmp(e->d_name, folder, fl);
        int score = (named ? 1000 : 0) + 100 - rank;
        if (score > best_score ||
            (score == best_score && strcasecmp(e->d_name, best) < 0)) {
            best_score = score;
            copy_str(best, sizeof best, e->d_name);
        }
    }
    closedir(d);
    if (!best_score) return 0;
    int k = snprintf(out, outsz, "%s/%s", dir, best);
    return k > 0 && (size_t)k < outsz;
}

static int cmp_game(const void *a, const void *b)
{
    return strcasecmp(((const Game *)a)->title, ((const Game *)b)->title);
}

static void free_games(void) { free(g_games); g_games = NULL; g_ngames = 0; }

/* Is `name` listed by a .m3u playlist in dir?  A playlist is one game with several
 * disks/sides; the disks it lists are not offered on their own (the playlist is).
 * Lines are matched by file name only, '#' lines and blanks are skipped. */
static int listed_by_m3u(const char *dir, const char *name)
{
    DIR *d = opendir(dir);
    if (!d) return 0;
    struct dirent *e;
    int hit = 0;
    while (!hit && (e = readdir(d))) {
        if (e->d_name[0] == '.' || !ext_rank("m3u", e->d_name) || !strcasecmp(e->d_name, name)) continue;
        char p[600], line[600];
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        FILE *f = fopen(p, "r");
        if (!f) continue;
        while (!hit && fgets(line, sizeof line, f)) {
            line[strcspn(line, "\r\n")] = 0;
            if (!line[0] || line[0] == '#') continue;
            char *b = line, *s;
            while ((s = strpbrk(b, "/\\"))) b = s + 1;
            hit = !strcmp(b, name);
        }
        fclose(f);
    }
    closedir(d);
    return hit;
}

/* Scan s->roms (system index sidx): each matching file, and each subfolder holding
 * one (titled by the folder, see pick_in_folder), sorted by title; returns the count and,
 * when out is not NULL, a malloc'd array in *out (the caller frees it). */
static int scan_games(const System *s, int sidx, Game **out)
{
    DIR *d = opendir(s->roms);
    if (!d) return 0;
    int cap = 0, n = 0;
    Game *arr = NULL;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char full[600];
        struct stat st;
        snprintf(full, sizeof full, "%s/%s", s->roms, e->d_name);
        if (stat(full, &st) != 0) continue;
        int folder = S_ISDIR(st.st_mode);
        if (folder) {
            char dir[600];
            copy_str(dir, sizeof dir, full);
            if (!pick_in_folder(s->exts, dir, e->d_name, full, sizeof full)) continue;
        } else if (!S_ISREG(st.st_mode) || !ext_rank(s->exts, e->d_name) ||
                   listed_by_m3u(s->roms, e->d_name)) {
            continue;
        }
        if (n == cap) {
            cap = cap ? cap * 2 : 32;
            Game *na = realloc(arr, (size_t)cap * sizeof *arr);
            if (!na) break;
            arr = na;
        }
        arr[n].sys = sidx;
        copy_str(arr[n].file, sizeof arr[n].file, full);
        copy_str(arr[n].title, sizeof arr[n].title, e->d_name);
        char *dot = strrchr(arr[n].title, '.');
        if (dot && !folder) *dot = 0;
        n++;
    }
    closedir(d);
    if (n > 1) qsort(arr, (size_t)n, sizeof *arr, cmp_game);
    if (!out) { free(arr); return n; }
    *out = arr;
    return n;
}

/* Put the nogame entry first in g_games (after scan_games, which fills the rest). */
static void add_nogame(const System *s, int sidx)
{
    if (!s->nogame[0]) return;
    Game *arr = realloc(g_games, (size_t)(g_ngames + 1) * sizeof *arr);
    if (!arr) return;
    memmove(arr + 1, arr, (size_t)g_ngames * sizeof *arr);
    arr[0].file[0] = 0;
    arr[0].sys = sidx;
    copy_str(arr[0].title, sizeof arr[0].title, s->nogame);
    g_games = arr;
    g_ngames++;
}

static int cmp_sys(const void *a, const void *b)
{
    return strcasecmp(((const System *)a)->name, ((const System *)b)->name);
}

static void load_systems(void)
{
    char dir[260];
    snprintf(dir, sizeof dir, "%s/systems", g_data);
    g_nsys = 0;
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && g_nsys < MAX_SYSTEMS) {
        size_t n = strlen(e->d_name);
        if (e->d_name[0] == '.' || n < 5 || strcmp(e->d_name + n - 4, ".sys")) continue;
        char id[32], path[600];
        if (n - 4 >= sizeof id) continue;
        memcpy(id, e->d_name, n - 4);
        id[n - 4] = 0;
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        System s;
        if (parse_sys(path, id, &s) != 0) continue;
        if (access(s.corepath, R_OK) != 0) continue;          /* no core: omitted, not greyed */
        g_sys[g_nsys++] = s;
    }
    closedir(d);
    qsort(g_sys, (size_t)g_nsys, sizeof g_sys[0], cmp_sys);
}

/* ── views ──────────────────────────────────────────────────────────────── */
/* View 0 is "All games"; view n >= 1 is g_sys[n - 1].  The choice is kept in
 * <data>/last-view (the system id, or "all") and restored on the next start. */

static int cmp_game_all(const void *a, const void *b)
{
    const Game *x = a, *y = b;
    int c = strcasecmp(x->title, y->title);
    return c ? c : strcasecmp(g_sys[x->sys].name, g_sys[y->sys].name);
}

/* Fill g_games for view v.  "nogame" entries (BASIC...) belong to their own system view only. */
static void build_view(int v)
{
    free_games();
    if (v == 0) {
        for (int i = 0; i < g_nsys; i++) {
            Game *a = NULL;
            int n = scan_games(&g_sys[i], i, &a);
            if (n > 0) {
                Game *na = realloc(g_games, (size_t)(g_ngames + n) * sizeof *na);
                if (!na) { free(a); break; }
                memcpy(na + g_ngames, a, (size_t)n * sizeof *a);
                g_games = na;
                g_ngames += n;
            }
            free(a);
        }
        if (g_ngames > 1) qsort(g_games, (size_t)g_ngames, sizeof *g_games, cmp_game_all);
    } else {
        g_ngames = scan_games(&g_sys[v - 1], v - 1, &g_games);
        add_nogame(&g_sys[v - 1], v - 1);
    }
}

static void view_path(char *p, size_t n) { snprintf(p, n, "%s/last-view", g_data); }

static void save_view(void)
{
    char p[260];
    view_path(p, sizeof p);
    FILE *f = fopen(p, "w");
    if (!f) return;
    fprintf(f, "%s\n", g_view > 0 ? g_sys[g_view - 1].id : "all");
    fclose(f);
}

static int load_view(void)
{
    char p[260], line[64] = "";
    view_path(p, sizeof p);
    FILE *f = fopen(p, "r");
    if (!f) return 0;
    if (!fgets(line, sizeof line, f)) line[0] = 0;
    fclose(f);
    char *id = trim(line);
    for (int i = 0; i < g_nsys; i++) if (!strcmp(id, g_sys[i].id)) return i + 1;
    return 0;
}

static const char *view_name(int v) { return v == 0 ? "All games" : g_sys[v - 1].name; }

/* ── list and drop-down sources ─────────────────────────────────────────── */

static int game_count(void *c) { (void)c; return g_ngames; }

static void game_row(void *c, int i, LvRow *out)
{
    (void)c;
    out->title = g_games[i].title;
    out->glyph = LV_GLYPH_FILE;
    if (g_view == 0) out->detail = g_sys[g_games[i].sys].name;   /* All games: say which system */
}

static int         pick_count(void *c) { (void)c; return g_nsys + 1; }
static const char *pick_name(void *c, int i) { (void)c; return view_name(i); }


/* ── launching ──────────────────────────────────────────────────────────── */

typedef struct { Framebuffer fb; TouchInput touch; GamepadManager gp; Pointer ptr; } Dev;

static const char *g_fb_dev = "/dev/fb0", *g_touch_dev = "/dev/input/touchscreen0";

static int dev_open(Dev *d)
{
    fb_set_bpp(g_fb_dev, 32);
    if (fb_init(&d->fb, g_fb_dev) < 0) return -1;
    if (touch_init(&d->touch, g_touch_dev) == 0) {
        touch_set_screen_size(&d->touch, d->fb.width, d->fb.height);
        touch_drain_events(&d->touch);
    }
    gamepad_init(&d->gp);
    pointer_init(&d->ptr);        /* after touch_init: SCREEN_SAFE_* */
    return 0;
}

static void dev_close(Dev *d)
{
    gamepad_close(&d->gp);
    touch_close(&d->touch);
    fb_close(&d->fb);
}

/* retro_video.c's ov_init enables the overlay plane on fb1 and sets omapdss
 * scale_mode to 1; ov_close undoes both, but only when retro exits through its
 * own cleanup. After a crash the saved state (the plane as it was, the previous
 * scale_mode) died with the child, so this undo cannot restore it. It does the
 * closest thing: disable the plane as it now stands (query, clear enabled, set),
 * and write scale_mode back to the stock 0 if the file exists. The plane memory
 * and vscreen are not restored. Same ioctl numbers and struct as retro_video.c. */
#define OMAP_IOW_P(nr, t)  _IOW('O', nr, t)
struct ofb_plane_p { uint32_t pos_x, pos_y; uint8_t enabled, channel_out, mirror, mem_idx;
    uint32_t out_width, out_height; uint32_t reserved2[12]; };
#define OMAPFB_SETUP_PLANE_P  OMAP_IOW_P(52, struct ofb_plane_p)
#define OMAPFB_QUERY_PLANE_P  OMAP_IOW_P(53, struct ofb_plane_p)
#define SCALE_MODE_PATH "/sys/module/omapdss/parameters/scale_mode"

static void retro_overlay_undo(const char *why)
{
    int fd1 = open("/dev/fb1", O_RDWR);
    if (fd1 >= 0) {
        struct ofb_plane_p pl;
        memset(&pl, 0, sizeof pl);
        if (ioctl(fd1, OMAPFB_QUERY_PLANE_P, &pl) == 0 && pl.enabled) {
            pl.enabled = 0;
            ioctl(fd1, OMAPFB_SETUP_PLANE_P, &pl);
        }
        close(fd1);
    }
    if (access(SCALE_MODE_PATH, F_OK) == 0) {
        FILE *f = fopen(SCALE_MODE_PATH, "w");
        if (f) { fputs("0", f); fclose(f); }
    }
    fprintf(stderr, "retro_picker: retro ended %s; overlay disabled, scale_mode reset to 0\n", why);
}

static void run_game(Dev *d, const System *s, const Game *g)
{
    dev_close(d);
    pid_t pid = fork();
    if (pid == 0) {
        char logp[260];
        snprintf(logp, sizeof logp, "%s/last.log", g_data);
        int fd = open(logp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); close(fd); }
        if (s->opts[0]) setenv("RETRO_OPTS", s->opts, 1);
        setenv("RETRO_SYSID", s->id, 1);                /* state files are kept per system */
        const char *av[6];
        int n = 0;
        av[n++] = g_bin;
        if (s->video[0]) { av[n++] = "-v"; av[n++] = s->video; }
        av[n++] = s->corepath;
        av[n++] = g->file[0] ? g->file : "-";           /* "-" = no content (retro_main) */
        av[n] = NULL;
        execv(g_bin, (char *const *)av);
        _exit(127);
    }
    int ok = 0;
    if (pid > 0) {
        g_child = pid;
        int st = 0;
        pid_t r;
        do { r = waitpid(pid, &st, 0); } while (r == -1 && errno == EINTR);
        g_child = 0;
        ok = r > 0 && WIFEXITED(st) && WEXITSTATUS(st) == 0;
        if (r > 0 && !ok) {
            char why[32];
            if (WIFSIGNALED(st)) snprintf(why, sizeof why, "after signal %d", WTERMSIG(st));
            else snprintf(why, sizeof why, "with status %d", WEXITSTATUS(st));
            retro_overlay_undo(why);
        }
    }
    if (dev_open(d) < 0) { g_running = 0; return; }
    if (!ok) { snprintf(g_msg, sizeof g_msg, "RETRO ENDED WITH AN ERROR - SEE LAST.LOG"); g_msg_ms = get_time_ms(); }
    /* Keys still held from the quit combo must not act here (app_launcher's drain). */
    InputState dr;
    memset(&dr, 0, sizeof dr);
    for (int i = 0; i < 120; i++) {
        gamepad_poll(&d->gp, &dr, 0, 0, false);
        usleep(16000);
        int any = 0;
        for (int b = 0; b < BTN_ID_COUNT; b++) any |= dr.buttons[b].held;
        if (!any && i > 3) break;
    }
}

/* ── screen ─────────────────────────────────────────────────────────────── */

#define ROW_H   48
#define MSG_H   18          /* the strip under the list for the error message */
#define COLOR_BG RGB(20, 20, 30)

static TopBar   g_bar;
static TopPick  g_pick;
static ListView g_lv;

static TpExt pick_ext(void)
{
    TpExt te = { SCREEN_SAFE_LEFT, SCREEN_SAFE_TOP, SCREEN_SAFE_RIGHT, SCREEN_SAFE_BOTTOM };
    return te;
}

/* Bar and list against the live screen: call after fb_init() + touch_init() and after every dev_open(). */
static void layout(void)
{
    g_bar.title = g_nsys ? view_name(g_view) : "RETRO";
    g_bar.title_extra = g_nsys ? TP_EXTRA : 0;
    top_bar_layout(&g_bar);
    int top = g_bar.L.bottom + 7, bot = SCREEN_SAFE_BOTTOM - MSG_H;
    g_lv.rect = (UiRect){ SCREEN_SAFE_LEFT + 8, top, SCREEN_SAFE_WIDTH - 16, bot - top };
}

static UiRect head_rect(void)
{
    layout();
    TpExt te = pick_ext();
    const TopBarLayout *L = &g_bar.L;
    return tp_head_rect(L->title_x, L->title_w, L->top, L->bottom, &te, true);
}

static void open_pick(void)
{
    if (!g_nsys) return;
    TpExt te = pick_ext();
    g_pick.sel = g_view;
    tp_open(&g_pick, head_rect(), &te);
}

/* The frame shown while the games are scanned (the file manager's "LOADING..."). */
static void loading_frame(Dev *d)
{
    fb_clear(&d->fb, COLOR_BG);
    text_draw_centered(&d->fb, d->fb.width / 2, d->fb.height / 2, "LOADING...", COLOR_WHITE, 3);
    fb_swap(&d->fb);
}

static void set_view(Dev *d, int v)
{
    loading_frame(d);
    g_view = v;
    build_view(v);
    lv_reset(&g_lv);
    g_msg[0] = 0;
    save_view();
    layout();
}

static void draw_screen(Dev *d, uint32_t now)
{
    Framebuffer *fb = &d->fb;
    fb_clear(fb, COLOR_BG);
    lv_sync(&g_lv);
    lv_draw(fb, &g_lv, g_nsys ? "NO GAMES FOUND" : "NO SYSTEMS INSTALLED");
    top_bar_draw(fb, &g_bar);
    if (g_nsys) tp_draw_arrow(fb, g_bar.L.title_x, g_bar.L.title_w, g_bar.L.top, g_bar.L.h);
    if (g_msg[0] && now - g_msg_ms < 8000)
        fb_draw_text(fb, SCREEN_SAFE_LEFT + 8, SCREEN_SAFE_BOTTOM - MSG_H + 6, g_msg, COLOR_ORANGE, 1);
    tp_draw(fb, &g_pick);
    fb_swap(fb);
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc > 1) g_fb_dev = argv[1];
    if (argc > 2) g_touch_dev = argv[2];
    if (getenv("RETRO_DATA")) copy_str(g_data, sizeof g_data, getenv("RETRO_DATA"));
    if (getenv("RETRO_BIN"))  copy_str(g_bin,  sizeof g_bin,  getenv("RETRO_BIN"));

    int lock_fd = acquire_instance_lock("retro_picker");
    if (lock_fd < 0) { fprintf(stderr, "retro_picker: another instance is already running\n"); return 1; }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    hw_init();
    hw_set_backlight(100);

    Dev dev;
    memset(&dev, 0, sizeof dev);
    if (dev_open(&dev) < 0) { fprintf(stderr, "retro_picker: cannot open framebuffer %s\n", g_fb_dev); return 1; }
    loading_frame(&dev);

    top_bar_init(&g_bar, false, true);
    g_bar.title_color = COLOR_WHITE;
    g_bar.title_cut = true;
    LvSource gs = { game_count, game_row, NULL };
    lv_init(&g_lv, gs, (UiRect){ 0, 0, 0, 0 }, ROW_H);
    g_lv.active = true;
    g_lv.tap2_any = true;
    TpSource ps = { pick_count, pick_name, NULL };
    tp_init(&g_pick, ps);

    load_systems();
    printf("retro_picker: %d system(s) from %s/systems\n", g_nsys, g_data);
    g_view = load_view();
    build_view(g_view);
    lv_reset(&g_lv);
    layout();

    bool redraw = true, exit_press = false, swallow_touch = false;
    uint32_t cooldown_until = 0;
    InputState in;
    memset(&in, 0, sizeof in);

    while (g_running) {
        uint32_t now = get_time_ms();

        touch_poll(&dev.touch);
        TouchState ts = touch_get_state(&dev.touch);
        gamepad_poll(&dev.gp, &in, 0, 0, false);
        gamepad_tick(&dev.gp, now);
        const ButtonState *bt = in.buttons;

        if (now < cooldown_until) { usleep(16000); continue; }

        /* The mouse: motion moves the arrow, a left click becomes this frame's
         * touch at it (pointer.h), so everything below takes it as a tap. */
        {
            bool nav = bt[BTN_ID_UP].pressed   || bt[BTN_ID_DOWN].pressed  ||
                       bt[BTN_ID_LEFT].pressed || bt[BTN_ID_RIGHT].pressed ||
                       bt[BTN_ID_JUMP].pressed || bt[BTN_ID_ACTION].pressed ||
                       bt[BTN_ID_BACK].pressed || bt[BTN_ID_PAUSE].pressed;
            pointer_update(&dev.ptr, &in, ts.pressed, nav, &ts);
        }

        if (top_bar_info_due(&g_bar, now) && top_bar_info_sample(&g_bar)) redraw = true;

        bool go_back = bt[BTN_ID_BACK].pressed || bt[BTN_ID_CANCEL].pressed || bt[BTN_ID_PAUSE].pressed;
        bool was_up = g_pick.up, open = false, quit = false;
        int  want_view = -1;

        if (was_up) {
            /* The drop-down owns every input (Back / Start close it). */
            TpEvent ev = tp_input(&g_pick, &ts, &in, now);
            if (ev != TP_NONE || ts.pressed || ts.released || ts.held ||
                bt[BTN_ID_UP].pressed || bt[BTN_ID_DOWN].pressed || bt[BTN_ID_JUMP].pressed || bt[BTN_ID_ACTION].pressed)
                redraw = true;
            if (ev == TP_PICKED) {
                int v = tp_take(&g_pick);
                if (v >= 0 && v <= g_nsys) want_view = v;
            }
            if (ev == TP_PICKED || ev == TP_CLOSED) swallow_touch = ts.held || ts.released;   /* the finger that closed it acts on nothing below */
            exit_press = false;
        } else {
            TouchState tz;
            memset(&tz, 0, sizeof tz);
            const TouchState *lts = &ts;
            if (swallow_touch) {
                if (!ts.held && !ts.released) swallow_touch = false;
                else lts = &tz;
            }
            if (lts == &ts) {
                TbHit h = top_bar_hit(&g_bar.L, ts.x, ts.y);
                if (ts.pressed) exit_press = h == TB_EXIT;
                if (ts.released) { if (exit_press && h == TB_EXIT) quit = true; exit_press = false; }
                if (g_nsys && !quit) {
                    InputState nokeys;
                    memset(&nokeys, 0, sizeof nokeys);
                    TpExt te = pick_ext();
                    g_pick.sel = g_view;
                    if (tp_head_input(&g_pick, head_rect(), &te, &ts, &nokeys) == TP_OPENED) {
                        open_pick();
                        redraw = true;
                    }
                }
            }
            if (!go_back && !quit && !g_pick.up) {
                if (g_nsys && bt[BTN_ID_UP].pressed && g_lv.cursor == 0) {
                    /* Up on the first row: the header drop-down */
                    open_pick();
                    redraw = true;
                } else {
                    int step = 0;
                    if (g_nsys && bt[BTN_ID_NEXT].pressed)
                        step = (bt[BTN_ID_RUN].held && !bt[BTN_ID_CANCEL].held) ? -1 : 1;   /* Tab / RB next, Shift+Tab previous */
                    if (g_nsys && bt[BTN_ID_PREV].pressed) step = -1;
                    if (step) want_view = (g_view + step + g_nsys + 1) % (g_nsys + 1);
                    if (bt[BTN_ID_LEFT].pressed)  { lv_key(&g_lv, LV_KEY_PGUP); redraw = true; }
                    if (bt[BTN_ID_RIGHT].pressed) { lv_key(&g_lv, LV_KEY_PGDN); redraw = true; }
                    LvEvent ev = lv_input(&g_lv, lts, &in, now);
                    if (ev != LV_EV_NONE) redraw = true;
                    open = ev == LV_EV_TAP || ev == LV_EV_ACTIVATE || bt[BTN_ID_JUMP].pressed;
                }
            }
        }
        if (lv_needs_frames(&g_lv) || go_back) redraw = true;

        if ((go_back && !was_up) || quit) break;      /* with the drop-down open, Back only closed it */
        if (want_view >= 0 && want_view != g_view) {
            set_view(&dev, want_view);
            redraw = true;
        } else if (open && g_lv.count > 0) {
            int i = g_lv.cursor;
            if (i >= 0 && i < g_ngames) {
                run_game(&dev, &g_sys[g_games[i].sys], &g_games[i]);
                memset(&in, 0, sizeof in);
                pointer_drain(&dev.ptr);      /* the click that launched it must not replay */
                cooldown_until = get_time_ms() + RETURN_COOLDOWN_MS;
                exit_press = false;
                top_bar_info_invalidate(&g_bar);
                layout();
            }
            redraw = true;
        }

        static uint32_t last_draw;
        if (now - last_draw > 1000) redraw = true;      /* something else may have taken the panel */
        if (redraw) {
            last_draw = now;
            draw_screen(&dev, now);
            pointer_invalidate(&dev.ptr);   /* the frame lacks the arrow */
            redraw = false;
        }
        bool moved = pointer_paint(&dev.ptr, &dev.fb);   /* the arrow alone, a moving mouse keeps the active rate */
        usleep(moved || lv_needs_frames(&g_lv) || g_pick.up ? 8000 : 16000);
    }

    free_games();
    lv_free(&g_lv);
    tp_free(&g_pick);
    hw_leds_off();
    hw_reload_config();
    hw_set_backlight(100);
    fb_clear(&dev.fb, COLOR_BLACK);
    fb_swap(&dev.fb);
    dev_close(&dev);
    return 0;
}
