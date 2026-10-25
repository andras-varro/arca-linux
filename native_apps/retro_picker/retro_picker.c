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
 *     exts=d64,t64,prg,crt,zip     content extensions, comma separated, case-insensitive, no dot
 *     opts=key=value,key=value     optional core options, passed to retro as RETRO_OPTS
 *     roms=/some/dir               optional; default <data>/roms/<id>
 *     video=sw|overlay             optional; passed as retro -v
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
    int  ngames;
} System;

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
    }
    fclose(f);
    if (!s->core[0] || !s->exts[0]) return -1;
    if (!s->name[0]) copy_str(s->name, sizeof s->name, id);
    if (!s->roms[0]) snprintf(s->roms, sizeof s->roms, "%s/roms/%s", g_data, id);
    if (strchr(s->core, '/')) copy_str(s->corepath, sizeof s->corepath, s->core);
    else snprintf(s->corepath, sizeof s->corepath, "%s/cores/%s", g_data, s->core);
    return 0;
}

/* Does the file name end in one of the comma separated extensions? */
static int ext_match(const char *exts, const char *name)
{
    const char *dot = strrchr(name, '.');
    if (!dot || !dot[1]) return 0;
    dot++;
    size_t dl = strlen(dot);
    while (*exts) {
        size_t n = strcspn(exts, ",");
        if (n == dl && !strncasecmp(exts, dot, n)) return 1;
        exts += n;
        if (*exts) exts++;
    }
    return 0;
}

static int cmp_game(const void *a, const void *b)
{
    return strcasecmp(((const Game *)a)->title, ((const Game *)b)->title);
}

static void free_games(void) { free(g_games); g_games = NULL; g_ngames = 0; }

/* Fill g_games from s->roms (flat directory); returns the count. */
static int scan_games(const System *s, int keep)
{
    if (!keep) free_games();
    DIR *d = opendir(s->roms);
    if (!d) return 0;
    int cap = 0, n = 0;
    Game *arr = NULL;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.' || !ext_match(s->exts, e->d_name)) continue;
        char full[600];
        struct stat st;
        snprintf(full, sizeof full, "%s/%s", s->roms, e->d_name);
        if (stat(full, &st) != 0 || !S_ISREG(st.st_mode)) continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 32;
            Game *na = realloc(arr, (size_t)cap * sizeof *arr);
            if (!na) break;
            arr = na;
        }
        copy_str(arr[n].file, sizeof arr[n].file, full);
        copy_str(arr[n].title, sizeof arr[n].title, e->d_name);
        char *dot = strrchr(arr[n].title, '.');
        if (dot) *dot = 0;
        n++;
    }
    closedir(d);
    if (n > 1) qsort(arr, (size_t)n, sizeof *arr, cmp_game);
    if (keep) { free(arr); return n; }
    g_games = arr;
    g_ngames = n;
    return n;
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
        s.ngames = scan_games(&s, 1);
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
        const char *av[6];
        int n = 0;
        av[n++] = g_bin;
        if (s->video[0]) { av[n++] = "-v"; av[n++] = s->video; }
        av[n++] = s->corepath;
        av[n++] = g->file;
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
