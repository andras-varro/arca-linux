/*
 * retro_picker — the "Retro" station front end: system list -> game list ->
 * runs the libretro frontend `retro` on the chosen game and comes back to the
 * game list when it exits.
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
 * Input on every screen: touch (tap a row, drag to scroll, BACK button),
 * keyboard and pad (Up/Down/PgUp/PgDn move, Enter/Space/A/X open,
 * Esc/Backspace/B/Start/Select back).
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
#include "../common/list_view.h"

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
    int  ngames;
} System;

/* file is empty for the nogame entry: retro is started with no content. */
typedef struct { char file[600]; char title[256]; } Game;

static System  g_sys[MAX_SYSTEMS];
static int     g_nsys;
static Game   *g_games;
static int     g_ngames;
static int     g_cur_sys = -1;
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

/* Fill g_games from s->roms: each matching file, and each subfolder holding
 * one (titled by the folder, see pick_in_folder); returns the count. */
static int scan_games(const System *s, int keep)
{
    if (!keep) free_games();
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
        copy_str(arr[n].file, sizeof arr[n].file, full);
        copy_str(arr[n].title, sizeof arr[n].title, e->d_name);
        char *dot = strrchr(arr[n].title, '.');
        if (dot && !folder) *dot = 0;
        n++;
    }
    closedir(d);
    if (n > 1) qsort(arr, (size_t)n, sizeof *arr, cmp_game);
    if (keep) { free(arr); return n; }
    g_games = arr;
    g_ngames = n;
    return n;
}

/* Put the nogame entry first in g_games (after scan_games, which fills the rest). */
static void add_nogame(const System *s)
{
    if (!s->nogame[0]) return;
    Game *arr = realloc(g_games, (size_t)(g_ngames + 1) * sizeof *arr);
    if (!arr) return;
    memmove(arr + 1, arr, (size_t)g_ngames * sizeof *arr);
    arr[0].file[0] = 0;
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
        s.ngames = scan_games(&s, 1) + (s.nogame[0] ? 1 : 0);
        g_sys[g_nsys++] = s;
    }
    closedir(d);
    qsort(g_sys, (size_t)g_nsys, sizeof g_sys[0], cmp_sys);
}

/* ── list sources ───────────────────────────────────────────────────────── */

static int  sys_count(void *c)  { (void)c; return g_nsys; }
static int  game_count(void *c) { (void)c; return g_ngames; }

static char g_detail[MAX_SYSTEMS][16];

static void sys_row(void *c, int i, LvRow *out)
{
    (void)c;
    out->title = g_sys[i].name;
    snprintf(g_detail[i], sizeof g_detail[i], "%d", g_sys[i].ngames);
    out->detail = g_detail[i];
    out->glyph = LV_GLYPH_FOLDER;
    if (!g_sys[i].ngames) out->flags = LV_ROW_DIM;
}

static void game_row(void *c, int i, LvRow *out)
{
    (void)c;
    out->title = g_games[i].title;
    out->glyph = LV_GLYPH_FILE;
}

/* ── launching ──────────────────────────────────────────────────────────── */

typedef struct { Framebuffer fb; TouchInput touch; GamepadManager gp; } Dev;

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

/* ── screens ────────────────────────────────────────────────────────────── */

#define ROW_H 48
#define COLOR_BG RGB(20, 20, 30)

enum { SCR_SYSTEMS, SCR_GAMES };

static void draw_screen(Dev *d, int scr, ListView *lv, Button *back, uint32_t now)
{
    Framebuffer *fb = &d->fb;
    fb_clear(fb, COLOR_BG);
    int left = SCREEN_SAFE_LEFT + 8, top = SCREEN_SAFE_TOP + 6;
    fb_draw_text(fb, left, top, "RETRO", COLOR_WHITE, 3);
    if (scr == SCR_GAMES) {
        char t[80];
        snprintf(t, sizeof t, "%s", g_sys[g_cur_sys].name);
        fb_draw_text(fb, left + 6 * 3 * 6 + 20, top + 4, t, RGB(120, 200, 255), 2);
    } else {
        fb_draw_text(fb, left + 6 * 3 * 6 + 20, top + 4, "CHOOSE A SYSTEM", RGB(120, 200, 255), 2);
    }
    lv_sync(lv);
    lv_draw(fb, lv, scr == SCR_SYSTEMS ? "NO SYSTEMS INSTALLED" : "NO GAMES FOUND");
    button_draw(fb, back);
    const char *hint = scr == SCR_GAMES ? "ENTER/A: PLAY   ESC/B: BACK" : "ENTER/A: OPEN   ESC/B: EXIT";
    int hx = back->x + back->width + 14, hy = back->y + back->height / 2 - 4;
    if (g_msg[0] && now - g_msg_ms < 8000) fb_draw_text(fb, hx, hy, g_msg, COLOR_ORANGE, 1);
    else fb_draw_text(fb, hx, hy, hint, RGB(130, 130, 150), 1);
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

    load_systems();
    printf("retro_picker: %d system(s) from %s/systems\n", g_nsys, g_data);

    int bar_h = 44;
    UiRect rect = { SCREEN_SAFE_LEFT + 8, SCREEN_SAFE_TOP + 44,
                    SCREEN_SAFE_WIDTH - 16, SCREEN_SAFE_HEIGHT - 44 - bar_h - 10 };
    ListView lv_sys, lv_games;
    LvSource ss = { sys_count, sys_row, NULL }, gs = { game_count, game_row, NULL };
    lv_init(&lv_sys, ss, rect, ROW_H);
    lv_init(&lv_games, gs, rect, ROW_H);
    lv_sys.active = lv_games.active = true;
    lv_sys.tap2_any = lv_games.tap2_any = true;

    Button back;
    button_init_full(&back, rect.x, SCREEN_SAFE_BOTTOM - bar_h - 2, 110, bar_h, "EXIT",
                     BTN_COLOR_SECONDARY, COLOR_WHITE, BTN_COLOR_HIGHLIGHT, 2);

    int scr = SCR_SYSTEMS;
    bool redraw = true, back_press = false;
    uint32_t cooldown_until = 0;
    InputState in;
    memset(&in, 0, sizeof in);

    while (g_running) {
        uint32_t now = get_time_ms();
        ListView *lv = scr == SCR_SYSTEMS ? &lv_sys : &lv_games;

        touch_poll(&dev.touch);
        TouchState ts = touch_get_state(&dev.touch);
        gamepad_poll(&dev.gp, &in, 0, 0, false);
        gamepad_tick(&dev.gp, now);
        const ButtonState *bt = in.buttons;

        if (now < cooldown_until) { usleep(16000); continue; }

        bool go_back = bt[BTN_ID_BACK].pressed || bt[BTN_ID_RUN].pressed ||
                       bt[BTN_ID_CANCEL].pressed || bt[BTN_ID_PAUSE].pressed;
        bool in_back = ts.x >= back.x && ts.x < back.x + back.width &&
                       ts.y >= back.y && ts.y < back.y + back.height;
        if (ts.pressed && in_back) back_press = true;
        if (ts.released) { if (back_press && in_back) go_back = true; back_press = false; }

        LvEvent ev = lv_input(lv, &ts, &in, now);
        if (ev != LV_EV_NONE) redraw = true;
        bool open = ev == LV_EV_TAP || ev == LV_EV_ACTIVATE || bt[BTN_ID_JUMP].pressed;
        if (lv_needs_frames(lv) || go_back) redraw = true;

        if (go_back) {
            if (scr == SCR_GAMES) {
                scr = SCR_SYSTEMS;
                button_init_full(&back, back.x, back.y, back.width, back.height, "EXIT",
                                 BTN_COLOR_SECONDARY, COLOR_WHITE, BTN_COLOR_HIGHLIGHT, 2);
                g_msg[0] = 0;
            } else {
                break;
            }
        } else if (open && lv->count > 0) {
            int i = lv->cursor;
            if (scr == SCR_SYSTEMS && i >= 0 && i < g_nsys) {
                g_cur_sys = i;
                scan_games(&g_sys[i], 0);
                add_nogame(&g_sys[i]);
                g_sys[i].ngames = g_ngames;
                lv_reset(&lv_games);
                scr = SCR_GAMES;
                button_init_full(&back, back.x, back.y, back.width, back.height, "BACK",
                                 BTN_COLOR_SECONDARY, COLOR_WHITE, BTN_COLOR_HIGHLIGHT, 2);
                g_msg[0] = 0;
            } else if (scr == SCR_GAMES && i >= 0 && i < g_ngames) {
                run_game(&dev, &g_sys[g_cur_sys], &g_games[i]);
                memset(&in, 0, sizeof in);
                cooldown_until = get_time_ms() + RETURN_COOLDOWN_MS;
                back_press = false;
            }
            redraw = true;
        }

        static uint32_t last_draw;
        if (now - last_draw > 1000) redraw = true;      /* something else may have taken the panel */
        if (redraw) {
            last_draw = now;
            draw_screen(&dev, scr, scr == SCR_SYSTEMS ? &lv_sys : &lv_games, &back, now);
            redraw = false;
        }
        usleep(lv_needs_frames(lv) ? 8000 : 16000);
    }

    free_games();
    lv_free(&lv_sys);
    lv_free(&lv_games);
    hw_leds_off();
    hw_reload_config();
    hw_set_backlight(100);
    fb_clear(&dev.fb, COLOR_BLACK);
    fb_swap(&dev.fb);
    dev_close(&dev);
    return 0;
}
