/*
 * App Launcher for Arca
 *
 * Visual grid launcher that scans the .app manifest files in /opt/arca/apps
 * and displays them as icon tiles.  Acts as the system shell — respawned
 * by the init script when an app exits.
 *
 * Manifest format (key=value text, one per line):
 *   name=Snake
 *   exec=/opt/games/snake
 *   icon=/opt/arca/icons/snake.ppm   (optional — PPM P6 format)
 *   args=fb,touch                           (optional — default: fb,touch)
 *
 * Recognized args values:
 *   fb,touch  — pass framebuffer and touch device paths (default)
 *   fb        — pass framebuffer device path only
 *   touch     — pass touch device path only
 *   none      — launch with no arguments
 *
 * Grid: dynamic layout — 3×2 in landscape, 2×3 in portrait, with pagination.
 *
 * The grid's exit X (top right) does not exit: the init script would only
 * respawn us.  It opens the Shutdown / Reboot / Cancel dialog, which is the
 * device's one home for powering down or restarting.
 */

#include "common/framebuffer.h"
#include "common/touch_input.h"
#include "common/common.h"
#include "common/icon_grid.h"
#include "common/logger.h"
#include "common/gamepad.h"
#include "common/pointer.h"
#include "common/hardware.h"
#include "common/config.h"
#include "common/status_line.h"
#include "common/top_bar.h"
#include "common/product.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <signal.h>
#include <errno.h>
#include <time.h>

/* ── Limits ─────────────────────────────────────────────────────────────── */

#define MAX_APPS        24
#define APPS_DIR        "/opt/arca/apps"

/* Post-launch cooldown: ignore ALL input (gamepad, touch, mouse) for this
 * many milliseconds after returning from a child process.  This prevents
 * auto-relaunch caused by stale/repeat key events, resistive touch noise,
 * or race conditions in the drain loop.  500 ms is long enough for any
 * held key to stop generating repeats, but short enough to feel instant. */
#define LAUNCH_COOLDOWN_MS 500

/* ── Grid geometry: common/icon_grid.c, shared with control_panel's home screen ──
 * The grid starts under the top bar (common/top_bar.h): the message on the left
 * (the tap target), the title, CPU and clock, and the exit X.  The bar is
 * flush with the top edge and reserves TOP_BAR_GRID_RESERVE under the touch
 * inset, so its buttons stay pressable on any unit. */
/* The title is the product name (common/product.h), asked at draw time. */
static IconGrid grid;

/* ── The MUSIC / EFFECTS toggles are NOT here ────────────────────────────────
 * They were, as a band between the last tile row and the page dots, and they
 * moved to control_panel's Audio page under the `AUDIO ENABLED` master they are
 * subordinate to.  Two reasons, in order: a games menu carrying settings widgets
 * is a games menu doing a settings tab's job, and this screen was then the only
 * games-side WRITER of two keys every game reads — so the launcher had to
 * re-read the file before each write to avoid reverting whatever control_panel
 * had set in the meantime.  One writer removes both problems.
 *
 * ⚠️ **Do not put them back here, and do not put them in a game's pause modal
 * either.** `MODAL_MAX_BUTTONS` is 4 and brick_breaker already uses all four;
 * `ModalDialog` has no widget slot; and a toggle that takes effect mid-run needs
 * a live setter for `Audio.music_on` (read once by `audio_init()`), a bed
 * stop/resume in the middle of a run, and seven writers of one key.
 */

/* ── The power dialog behind the exit X ──────────────────────────────────────
 * Three stacked ModalDialog buttons.  No message line: with three buttons the
 * stack starts at the height ModalDialog draws a message, so one would collide. */
enum { PWR_SHUTDOWN = 0, PWR_REBOOT = 1, PWR_CANCEL = 2 };
static ModalDialog power_dialog;
static bool exit_pressed;        /* the current touch went down on the X */
static int  power_press = -1;    /* dialog button the current touch went down on */
static int  home_press = -1;     /* item the current touch went down on, -1 none */
static bool msg_pressed;         /* the current touch went down on the status message */
/* Returned by handle_touch() and handle_gamepad_input() for "open the page that
 * fixes the shown message"; -2 is the exit X, so this is -3. */
#define TOUCH_MESSAGE   (-3)

/* ── Colours ────────────────────────────────────────────────────────────── */

#define BG_COLOR        RGB(25, 25, 35)

/* ── Argument-passing modes ─────────────────────────────────────────────── */

typedef enum {
    ARG_NONE     = 0,
    ARG_FB       = 1,
    ARG_TOUCH    = 2,
    ARG_FB_TOUCH = 3
} ArgMode;

/* ── App entry ──────────────────────────────────────────────────────────── */

typedef struct {
    char      name[64];
    char      exec_path[256];
    char      icon_path[256];
    ArgMode   args;
    uint32_t *icon_pixels;      /* Loaded & scaled to ICON_GRID_ICON_SIZE², or NULL */
    uint32_t  icon_color;       /* Auto-assigned colour for letter tile   */
} AppEntry;

/* ── Launcher state ─────────────────────────────────────────────────────── */

typedef struct {
    AppEntry    apps[MAX_APPS];
    int         app_count;
    int         current_page;
    int         total_pages;
    IconGridFocus focus;            /* the keyboard/pad selection and its ring,
                                       the model shared with control_panel */
    Framebuffer fb;
    TouchInput  touch;
    GamepadManager gamepad;
    InputState  input;
    Pointer     pointer;            /* the mouse arrow; a click is a touch */
    uint32_t    last_launch_return_ms;  /* Timestamp of last child-exit for cooldown */
    Logger      logger;
    bool        needs_redraw;       /* Dirty flag — skip rendering when false */
    /* The top bar (top_bar.h): the message on the left, CPU and clock on the
     * right, re-read once a second by status_refresh(). */
    TopBar      bar;
    StatusCtx   status_ctx;
    const StatusMsg *msg;           /* the message shown, NULL = none */
} Launcher;

/* Prints the layout receipts ("launcher: safe …", "launcher: bar …") — see
 * icon_grid_layout() and top_bar_receipt(). */
static void compute_grid_layout(Launcher *l) {
    top_bar_layout(&l->bar);
    top_bar_receipt(&l->bar, "launcher");
    icon_grid_layout(&grid, &l->fb, TOP_BAR_GRID_RESERVE, "launcher");
}

/* The bar's message, as a tap target: its text plus padding, full bar height. */
static bool message_hit(const Launcher *l, int x, int y) {
    return l->msg && top_bar_hit(&l->bar.L, x, y) == TB_MSG;
}

/* Re-read the bar's inputs if a second has passed.  Returns true when what
 * it draws changed (the right-hand text, or the message). */
static bool status_refresh(Launcher *l, uint32_t now_ms, bool *info_only) {
    if (info_only) *info_only = false;
    if (!top_bar_info_due(&l->bar, now_ms)) return false;
    bool text_changed = top_bar_info_sample(&l->bar);

    l->status_ctx.calibrated = (access("/etc/touch_calibration.conf", F_OK) == 0);
    const StatusMsg *msg = status_msg_pick(&l->status_ctx);

    bool changed = text_changed || msg != l->msg;
    if (info_only) *info_only = text_changed && msg == l->msg;   /* the CPU / clock text alone: a rectangle repaint will do */
    l->msg = msg;
    l->bar.message = msg ? msg->text : NULL;
    top_bar_layout(&l->bar);       /* the message moves the title's room */
    return changed;
}

static void status_init(Launcher *l) {
    status_refresh(l, get_time_ms(), NULL);
}

/* Open the page that fixes the shown message. */
static void launch_message(Launcher *l, const char *fb_dev, const char *touch_dev);

/* ════════════════════════════════════════════════════════════════════════ */
/*  Manifest parsing                                                       */
/* ════════════════════════════════════════════════════════════════════════ */

static void trim_trailing(char *s) {
    int len = (int)strlen(s);
    while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r' ||
                       s[len - 1] == ' '  || s[len - 1] == '\t'))
        s[--len] = '\0';
}

static ArgMode parse_args(const char *s) {
    if (!s || !*s)                return ARG_FB_TOUCH;
    if (strcmp(s, "none") == 0)   return ARG_NONE;
    ArgMode m = ARG_NONE;
    if (strstr(s, "fb"))    m |= ARG_FB;
    if (strstr(s, "touch")) m |= ARG_TOUCH;
    return m ? m : ARG_FB_TOUCH;
}

static int load_manifest(const char *path, AppEntry *app) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;

    memset(app, 0, sizeof(*app));
    app->args = ARG_FB_TOUCH;

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        trim_trailing(line);
        if (line[0] == '#' || line[0] == '\0') continue;

        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        const char *key = line;
        const char *val = eq + 1;

        if (strcmp(key, "name") == 0)
            strncpy(app->name, val, sizeof(app->name) - 1);
        else if (strcmp(key, "exec") == 0)
            strncpy(app->exec_path, val, sizeof(app->exec_path) - 1);
        else if (strcmp(key, "icon") == 0)
            strncpy(app->icon_path, val, sizeof(app->icon_path) - 1);
        else if (strcmp(key, "args") == 0)
            app->args = parse_args(val);
    }
    fclose(f);

    /* Must have both name and exec */
    if (!app->name[0] || !app->exec_path[0]) return -1;

    /* Verify executable exists and is runnable */
    struct stat st;
    if (stat(app->exec_path, &st) != 0 || !(st.st_mode & S_IXUSR)) return -1;

    app->icon_color = icon_grid_letter_color(app->name);
    return 0;
}

static void load_icon(AppEntry *app) {
    app->icon_pixels = icon_grid_load_icon(app->icon_path);
}

/* ════════════════════════════════════════════════════════════════════════ */
/*  App scanning                                                           */
/* ════════════════════════════════════════════════════════════════════════ */

static void free_icons(Launcher *l) {
    for (int i = 0; i < l->app_count; i++) {
        free(l->apps[i].icon_pixels);
        l->apps[i].icon_pixels = NULL;
    }
}

static int scan_apps(Launcher *l) {
    free_icons(l);
    l->app_count = 0;

    DIR *dir = opendir(APPS_DIR);
    if (!dir) {
        LOG_WARN(&l->logger, "Cannot open %s", APPS_DIR);
        l->total_pages = 1;
        return 0;
    }

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL && l->app_count < MAX_APPS) {
        const char *name = ent->d_name;
        int len = (int)strlen(name);
        if (len < 5 || strcmp(name + len - 4, ".app") != 0) continue;

        char path[512];
        snprintf(path, sizeof(path), "%s/%s", APPS_DIR, name);

        AppEntry app;
        if (load_manifest(path, &app) == 0) {
            load_icon(&app);
            l->apps[l->app_count++] = app;
            LOG_INFO(&l->logger, "Loaded: %-16s -> %s", app.name, app.exec_path);
        } else {
            LOG_WARN(&l->logger, "Skipped: %s", path);
        }
    }
    closedir(dir);

    /* Sort alphabetically by display name */
    for (int i = 0; i < l->app_count - 1; i++)
        for (int j = i + 1; j < l->app_count; j++)
            if (strcasecmp(l->apps[i].name, l->apps[j].name) > 0) {
                AppEntry tmp = l->apps[i];
                l->apps[i]  = l->apps[j];
                l->apps[j]  = tmp;
            }

    l->total_pages = icon_grid_pages(&grid, l->app_count);
    if (l->current_page >= l->total_pages) l->current_page = l->total_pages - 1;

    return l->app_count;
}

/* ════════════════════════════════════════════════════════════════════════ */
/*  Drawing                                                                */
/* ════════════════════════════════════════════════════════════════════════ */

/* Never the highlighted tile look: focus is the ring alone, as on the
 * Control Panel home. */
static void draw_tile(Framebuffer *fb, const AppEntry *app, int tx, int ty) {
    icon_grid_draw_tile(fb, &grid, tx, ty, app->name, app->icon_pixels,
                        app->icon_color, false);
}

static void draw_launcher(Launcher *l) {
    fb_clear(&l->fb, BG_COLOR);

    /* The top bar, flush with the top edge however large the touch inset is:
     * message left (the tap target), the title, CPU and clock, and the exit X.
     * The keyboard rings on the message and on the exit X are drawn inside
     * their targets, so the top edge of the screen does not clip them. */
    l->bar.title = product_name();
    top_bar_draw(&l->fb, &l->bar);

    /* Tiles for current page */
    int start = l->current_page * grid.per_page;
    int count = l->app_count - start;
    if (count > grid.per_page) count = grid.per_page;

    for (int i = 0; i < count; i++) {
        int tx, ty;
        icon_grid_tile_xy(&grid, i, &tx, &ty);
        int abs_idx = start + i;
        draw_tile(&l->fb, &l->apps[abs_idx], tx, ty);
        if (l->focus.shown && abs_idx == l->focus.sel)
            icon_grid_draw_selection(&l->fb, &grid, tx, ty);
    }

    /* Empty-state message */
    if (l->app_count == 0) {
        text_draw_centered(&l->fb, l->fb.width / 2, 220,
                           "No apps installed", RGB(150, 150, 150), 3);
        text_draw_centered(&l->fb, l->fb.width / 2, 260,
                           "Deploy apps with build-and-deploy.sh",
                           RGB(100, 100, 100), 2);
    }

    /* Page arrows and dots */
    icon_grid_draw_paging(&l->fb, &grid, l->current_page, l->total_pages);

    /* Focus on the bar: the message or the X; the bar draws its own ring. */
    if (l->focus.shown) {
        if (l->focus.sel == ICON_GRID_NAV_MSG)       top_bar_draw_focus(&l->fb, &l->bar.L, TB_MSG);
        else if (l->focus.sel == ICON_GRID_NAV_EXIT) top_bar_draw_focus(&l->fb, &l->bar.L, TB_EXIT);
    }

    /* The dialog highlights its focused button itself, always. */
    modal_dialog_draw(&power_dialog, &l->fb);

    fb_swap(&l->fb);
}

/* ════════════════════════════════════════════════════════════════════════ */
/*  Touch handling                                                         */
/* ════════════════════════════════════════════════════════════════════════ */

/* The tile under (x,y) on the shown page, as an absolute index, or -1. */
static int tile_at(Launcher *l, int x, int y) {
    int start = l->current_page * grid.per_page;
    int count = l->app_count - start;
    if (count > grid.per_page) count = grid.per_page;
    int i = icon_grid_hit(&grid, count, x, y);
    return i >= 0 ? start + i : -1;
}

/* A tile acts on RELEASE, and only over the tile it went down on, as on the
 * Control Panel home — so a finger can slide off to abort, and the child app
 * starts with no finger on the panel.  The X is checked first (it sits inside
 * the right page band); the page-flip bands act on the press.
 *  Returns:  >= 0   app index to launch
 *            -1     nothing (a page flip included)
 *            -2     the exit X, released over it
 *            TOUCH_MESSAGE  the bar's message, released over it */
static int handle_touch(Launcher *l, const TouchState *ts) {
    if (ts->pressed) {
        exit_pressed  = top_bar_hit(&l->bar.L, ts->x, ts->y) == TB_EXIT;
        msg_pressed   = !exit_pressed && message_hit(l, ts->x, ts->y);
        home_press    = (exit_pressed || msg_pressed) ? -1 : tile_at(l, ts->x, ts->y);
        if (!exit_pressed && !msg_pressed && home_press < 0)
            l->current_page += icon_grid_page_hit(ts->x, l->current_page,
                                                  l->total_pages);
    }
    /* No else: a quick tap delivers press and release in the same poll. */
    if (!ts->released) return -1;
    if (msg_pressed) {                 /* released over it, as a tile is */
        msg_pressed = false;
        return message_hit(l, ts->x, ts->y) ? TOUCH_MESSAGE : -1;
    }
    if (exit_pressed) {
        exit_pressed = false;
        return top_bar_hit(&l->bar.L, ts->x, ts->y) == TB_EXIT ? -2 : -1;
    }
    int p = home_press;
    home_press = -1;
    return (p >= 0 && tile_at(l, ts->x, ts->y) == p) ? p : -1;
}

/* ════════════════════════════════════════════════════════════════════════ */
/*  Gamepad / keyboard navigation                                          */
/* ════════════════════════════════════════════════════════════════════════ */

static void open_power_dialog(void) {
    modal_dialog_set_focus(&power_dialog, PWR_CANCEL);   /* the safe default */
    power_press = -1;
    modal_dialog_show(&power_dialog);
}

/* The ring and the selection are icon_grid_focus_frame()'s, the model the
 * Control Panel home uses too; touch_press is this frame's real press.
 *  Returns:  >= 0  app index to launch
 *            -1    nothing (navigation only, or no input)
 */
static int handle_gamepad_input(Launcher *l, bool touch_press) {
    InputState *inp = &l->input;

    /* BACK (Select / Backspace) and PAUSE (Start / Escape) mean "leave" in every
     * app; here leaving is the power dialog, as the X is. */
    if (inp->buttons[BTN_ID_BACK].pressed || inp->buttons[BTN_ID_PAUSE].pressed) {
        open_power_dialog();
        return -1;
    }

    const ButtonState *b = inp->buttons;
    int dir = icon_grid_focus_dir(b[BTN_ID_UP].pressed, b[BTN_ID_DOWN].pressed,
                                  b[BTN_ID_LEFT].pressed, b[BTN_ID_RIGHT].pressed);
    bool enter = b[BTN_ID_JUMP].pressed || b[BTN_ID_ACTION].pressed;

    /* The bar's message is a stop in the focus model beside the X (left of it);
     * whether it is showing is passed in each frame. */
    int act = icon_grid_focus_frame(&grid, l->app_count, &l->current_page,
                                    &l->focus, touch_press, dir, enter,
                                    l->msg != NULL);
    if (act == ICON_GRID_NAV_MSG) return TOUCH_MESSAGE;   /* as a tap on it */
    if (act == ICON_GRID_NAV_EXIT) {             /* the X: as a tap on it */
        open_power_dialog();
        return -1;
    }
    return act >= 0 && act < l->app_count ? act : -1;
}

/* ════════════════════════════════════════════════════════════════════════ */
/*  App launching                                                          */
/* ════════════════════════════════════════════════════════════════════════ */

/* `page`, when non-NULL, is passed as the last argument (control_panel opens
 * that page).  `app` may point into l->apps: it is not used after the final
 * re-scan, which is the last thing this function does. */
static void launch_app(Launcher *l, const AppEntry *app, const char *page,
                       const char *fb_dev, const char *touch_dev)
{
    LOG_INFO(&l->logger, "Launching: %s (%s)", app->name, app->exec_path);

    /* Fade out launcher before switching to app */
    fb_fade_out(&l->fb);

    /* Release framebuffer, touch, and gamepad so child gets exclusive access */
    gamepad_close(&l->gamepad);
    touch_close(&l->touch);
    fb_close(&l->fb);

    pid_t pid = fork();
    if (pid == 0) {
        /* ── Child process ────────────────────────────────────────── */
        /* argv[0] is the executable path, NOT app->name.  Passing the manifest's
         * display name made /proc/<pid>/cmdline read "VNC Client" while the
         * binary was /opt/vnc_client/vnc_client — which cost a session to
         * diagnose, because busybox `ps w` shows only processes with a TTY and
         * the cmdline is then the only thing left to walk.  (`comm` was always
         * right: the kernel takes it from the file being executed.)  Nothing
         * reads argv[0] except the usage text of three CLI tools. */
        const char *av[5];
        int n = 0;
        av[n++] = app->exec_path;
        switch (app->args) {
        case ARG_NONE:
            break;
        case ARG_FB:
            av[n++] = fb_dev;
            break;
        case ARG_TOUCH:
            av[n++] = touch_dev;
            break;
        case ARG_FB_TOUCH:
        default:
            av[n++] = fb_dev;
            av[n++] = touch_dev;
            break;
        }
        if (page) av[n++] = page;
        av[n] = NULL;
        execv(app->exec_path, (char *const *)av);
        perror("execl failed");
        _exit(1);
    } else if (pid > 0) {
        /* ── Parent: wait for app to finish (EINTR-safe) ──────────── */
        int status;
        pid_t ret;
        do {
            ret = waitpid(pid, &status, 0);
        } while (ret == -1 && errno == EINTR);

        if (ret > 0) {
            if (WIFEXITED(status))
                LOG_INFO(&l->logger, "%s exited (code %d)", app->name, WEXITSTATUS(status));
            else if (WIFSIGNALED(status))
                LOG_WARN(&l->logger, "%s killed by signal %d", app->name, WTERMSIG(status));
            else
                LOG_WARN(&l->logger, "%s exited (raw status %d)", app->name, status);
        } else {
            LOG_WARN(&l->logger, "waitpid failed: %s", strerror(errno));
        }
    } else {
        perror("fork failed");
    }

    /* Re-acquire framebuffer (restore 32bpp in case child left 16bpp) */
    fb_set_bpp(fb_dev, 32);
    if (fb_init(&l->fb, fb_dev) != 0)
        LOG_ERROR(&l->logger, "Failed to re-initialise framebuffer");

    /* Recompute grid layout (screen dimensions may differ after child) */
    compute_grid_layout(l);

    /* Re-acquire touch */
    if (touch_init(&l->touch, touch_dev) == 0) {
        touch_set_screen_size(&l->touch, l->fb.width, l->fb.height);
        touch_drain_events(&l->touch);  /* Discard stale events from child */
    }

    /* Re-init gamepad after child exits */
    gamepad_init(&l->gamepad);

    /* Wait-for-release drain: prevent auto-relaunch from held Enter/A.
     *
     * gamepad_init() has just cleared the manager, including the latched key
     * levels.  If the launch button (Enter or A) is still physically held,
     * evdev key-repeat events keep arriving, and the first REAL poll would see
     * a fresh press edge and immediately relaunch the app.  So poll until the
     * launch buttons read released (or a 2-second safety timeout expires).
     *
     * Since B2, held is a pure output of gamepad_poll() computed from state
     * inside the manager, so the InputState used here is arbitrary — it does
     * not have to be the same one the main loop uses. */
    {
        InputState drain;
        memset(&drain, 0, sizeof(drain));
        int safety = 0;
        do {
            gamepad_poll(&l->gamepad, &drain, 0, 0, false);
            usleep(16000);  /* ~60 fps */
            safety++;
        } while ((drain.buttons[BTN_ID_JUMP].held ||
                  drain.buttons[BTN_ID_ACTION].held) && safety < 120);
    }
    /* Zero the main input state so no stale held flags carry over, and drop
     * the click that launched the child (pointer.h, pointer_drain). */
    memset(&l->input, 0, sizeof(l->input));
    pointer_drain(&l->pointer);

    /* Record return time — main loop will ignore ALL input for
     * LAUNCH_COOLDOWN_MS after this, as defense-in-depth against
     * stale events from any source (keyboard, touch, mouse). */
    l->last_launch_return_ms = get_time_ms();

    /* The child may have fixed what the bar names (calibration), and the clock
     * has moved: read it all again on the next frame. */
    top_bar_info_invalidate(&l->bar);

    /* Re-scan manifests in case new apps were deployed while app ran */
    LOG_INFO(&l->logger, "Re-scanning apps...");
    scan_apps(l);
}

static void launch_message(Launcher *l, const char *fb_dev, const char *touch_dev) {
    if (!l->msg) return;
    AppEntry app;
    memset(&app, 0, sizeof(app));
    snprintf(app.name, sizeof(app.name), "%s", l->msg->text);
    snprintf(app.exec_path, sizeof(app.exec_path), "%s", l->msg->exec);
    app.args = ARG_FB_TOUCH;
    struct stat st;
    if (stat(app.exec_path, &st) != 0 || !(st.st_mode & S_IXUSR)) {
        LOG_WARN(&l->logger, "Message action not runnable: %s", app.exec_path);
        return;
    }
    launch_app(l, &app, l->msg->page, fb_dev, touch_dev);
    l->needs_redraw = true;
}

/* ════════════════════════════════════════════════════════════════════════ */
/*  Shutdown / reboot                                                      */
/* ════════════════════════════════════════════════════════════════════════ */

/* The one home of the shutdown screen's words.
 *
 * ⚠️ Nothing this process draws can say "safe now": it is killed before the
 * disks are. So the kernel's own end state is the signal, and it differs by
 * kernel (both measured): the vendor kernel HALTS with the panel bright WHITE and
 * the backlight on; our 4.14.52 image powers the board off, panel DARK. */
static const char *const shutdown_hint[] = {
    "WHEN THE SCREEN TURNS WHITE OR DARK,",
    "IT IS SAFE TO UNPLUG.",
};

static void draw_power_screen(Framebuffer *fb, int action, bool failed) {
    int cx = SCREEN_VISIBLE_LEFT + SCREEN_VISIBLE_WIDTH / 2;
    int cy = SCREEN_VISIBLE_TOP + SCREEN_VISIBLE_HEIGHT / 2;
    fb_clear(fb, COLOR_BLACK);
    text_draw_centered(fb, cx, cy - 50,
                       action == PWR_SHUTDOWN ? "SHUTTING DOWN..." : "REBOOTING...",
                       COLOR_YELLOW, 4);
    if (action == PWR_SHUTDOWN)
        for (int i = 0; i < (int)(sizeof(shutdown_hint) / sizeof(shutdown_hint[0])); i++)
            text_draw_centered(fb, cx, cy + 10 + i * 28, shutdown_hint[i], COLOR_WHITE, 2);
    if (failed)
        text_draw_centered(fb, cx, cy + 90, "COMMAND FAILED", COLOR_RED, 2);
    fb_swap(fb);
}

static void execute_power_action(Launcher *l, int action) {
    LOG_INFO(&l->logger, "%s requested", action == PWR_SHUTDOWN ? "Shutdown" : "Reboot");
    hw_leds_off();
    draw_power_screen(&l->fb, action, false);

    sync();
    /* poweroff, not `shutdown -h now`: BusyBox on our own root has no shutdown
     * applet (measured), and the vendor's sysvinit has poweroff too. */
    int rc = (action == PWR_SHUTDOWN) ? system("poweroff") : system("reboot");
    if (rc == 0) {
        /* Wait to be killed, never exit: the respawn wrapper would start a fresh
         * launcher whose grid replaces this screen for the rest of the shutdown.
         * A SIGTERM only sets quit_flag, which nothing reads from here. */
        for (;;) pause();
    }

    /* Say so rather than sit on a lying "SHUTTING DOWN..." — then back to the grid. */
    LOG_ERROR(&l->logger, "power command failed (rc=%d)", rc);
    draw_power_screen(&l->fb, action, true);
    sleep(5);
    touch_drain_events(&l->touch);
    pointer_drain(&l->pointer);
    l->last_launch_return_ms = get_time_ms();   /* the post-launch input cooldown */
}

static void choose_power(Launcher *l, int idx) {
    modal_dialog_hide(&power_dialog);
    power_press = -1;
    if (idx == PWR_SHUTDOWN || idx == PWR_REBOOT)
        execute_power_action(l, idx);
}

static int power_button_at(int x, int y) {
    for (int i = 0; i < power_dialog.button_count; i++)
        if (button_is_touched(&power_dialog.buttons[i], x, y)) return i;
    return -1;
}

/* While the dialog is up it takes ALL input. A touch acts on RELEASE, and only
 * on the button it went down on, so a finger can slide off SHUT DOWN to abort.
 * A mouse click arrives here as a touch (pointer_update).  The focused button
 * is the dialog's own (modal_dialog_focus_step), drawn highlighted always. */
static void handle_power_dialog(Launcher *l, const TouchState *ts) {
    InputState *inp = &l->input;

    if (ts->pressed) power_press = power_button_at(ts->x, ts->y);
    /* No else: a quick tap delivers press and release in the same poll. */
    if (ts->released && power_press >= 0) {
        int p = power_press;
        power_press = -1;
        if (power_button_at(ts->x, ts->y) == p) { choose_power(l, p); return; }
    }

    const ButtonState *b = inp->buttons;
    int dir = icon_grid_focus_dir(b[BTN_ID_UP].pressed, b[BTN_ID_DOWN].pressed,
                                  b[BTN_ID_LEFT].pressed, b[BTN_ID_RIGHT].pressed);
    if (dir >= 0) modal_dialog_focus_step(&power_dialog, (UiDir)dir);
    if (b[BTN_ID_BACK].pressed || b[BTN_ID_PAUSE].pressed)
        choose_power(l, PWR_CANCEL);
    else if (b[BTN_ID_JUMP].pressed || b[BTN_ID_ACTION].pressed)
        choose_power(l, power_dialog.focus);
}

/* ════════════════════════════════════════════════════════════════════════ */
/*  Main                                                                   */
/* ════════════════════════════════════════════════════════════════════════ */

/* Graceful SIGTERM handling for clean shutdown from init script */
static volatile sig_atomic_t quit_flag = 0;
static void on_sigterm(int sig) { (void)sig; quit_flag = 1; }

int main(int argc, char *argv[]) {
    /* ⚠️ **stdout is a FILE at boot, so it is block-buffered and the receipt below
     * does not arrive.** `/etc/init.d/arca-app` redirects us into
     * /var/log/arca/app_stdout.log; glibc then buffers 4 KB and the
     * compute_grid_layout() receipt — the one thing that has to be read BEFORE a
     * screenshot is trusted — sits in that buffer until the app exits or the
     * buffer fills.  Measured 2026-08-22 on `.188`: the log ended mid-word in
     * "Touch-safe area: lo" with the receipt nowhere in it.  `common/logger.c`
     * line-buffers its OWN file and so was never affected, which is what makes
     * this look like a missing printf rather than a buffering one. */
    setvbuf(stdout, NULL, _IOLBF, 0);

    const char *fb_dev    = "/dev/fb0";
    const char *touch_dev = "/dev/input/touchscreen0";
    if (argc > 1) fb_dev    = argv[1];
    if (argc > 2) touch_dev = argv[2];

    printf("%s App Launcher\n", product_name());
    printf("=======================\n");

    /* Singleton guard — exit immediately if another instance is running */
    int lock_fd = acquire_instance_lock("app_launcher");
    if (lock_fd < 0) {
        fprintf(stderr, "app_launcher: another instance is already running\n");
        return 1;
    }

    signal(SIGTERM, on_sigterm);
    signal(SIGINT,  on_sigterm);

    Launcher launcher;
    memset(&launcher, 0, sizeof(launcher));

    /* Initialize logger */
    logger_init(&launcher.logger, "app_launcher", LOG_LEVEL_INFO, true);

    /* Turn off LEDs — they stay yellow after boot from the firmware */
    hw_leds_off();
    LOG_INFO(&launcher.logger, "LEDs turned off");

    /* Ensure 32bpp — a previous process (e.g. ScummVM) may have left 16bpp */
    fb_set_bpp(fb_dev, 32);

    /* Framebuffer */
    if (fb_init(&launcher.fb, fb_dev) != 0) {
        LOG_ERROR(&launcher.logger, "Failed to initialise framebuffer");
        logger_close(&launcher.logger);
        return 1;
    }

    /* Touch */
    if (touch_init(&launcher.touch, touch_dev) != 0) {
        LOG_ERROR(&launcher.logger, "Failed to initialise touch input");
        fb_close(&launcher.fb);
        logger_close(&launcher.logger);
        return 1;
    }
    touch_set_screen_size(&launcher.touch,
                          launcher.fb.width, launcher.fb.height);

    /* Gamepad / keyboard / mouse */
    gamepad_init(&launcher.gamepad);
    memset(&launcher.input, 0, sizeof(launcher.input));
    launcher.last_launch_return_ms = 0;
    icon_grid_focus_init(&launcher.focus);  /* no ring until a key is pressed */
    top_bar_init(&launcher.bar, false, true);   /* no back: the exit X opens the power dialog */
    launcher.needs_redraw = true;  /* Force initial frame draw */

    /* Compute grid layout based on screen dimensions */
    compute_grid_layout(&launcher);
    pointer_init(&launcher.pointer);   /* after touch_init: SCREEN_SAFE_* */

    status_init(&launcher);   /* first reading: the bar is right on the first frame */

    modal_dialog_init(&power_dialog, "SHUT DOWN OR REBOOT?", NULL, 3);
    modal_dialog_set_button(&power_dialog, PWR_SHUTDOWN, "SHUT DOWN", BTN_COLOR_DANGER, COLOR_WHITE);
    modal_dialog_set_button(&power_dialog, PWR_REBOOT,   "REBOOT",    BTN_COLOR_WARNING, COLOR_WHITE);
    modal_dialog_set_button(&power_dialog, PWR_CANCEL,   "CANCEL",    RGB(100, 100, 100), COLOR_WHITE);

    /* Scan for installed apps */
    int count = scan_apps(&launcher);
    LOG_INFO(&launcher.logger, "Found %d app(s), %d page(s)", count, launcher.total_pages);

    /* ── Main loop — polling-based, with dirty-flag rendering ───── */
    while (!quit_flag) {
        /* Save visual state for dirty detection */
        IconGridFocus old_sel = launcher.focus;
        int old_page     = launcher.current_page;
        int old_count    = launcher.app_count;
        bool old_dialog  = modal_dialog_is_active(&power_dialog);
        int  old_focus   = power_dialog.focus;

        /* Poll touch (non-blocking) */
        touch_poll(&launcher.touch);
        TouchState ts = touch_get_state(&launcher.touch);

        /* Poll gamepad / keyboard / mouse */
        gamepad_poll(&launcher.gamepad, &launcher.input,
                     ts.x, ts.y, ts.pressed);

        /* Hot-plug check: rescans only when /dev/input changed or a device
         * went away.  The re-init after a child exits resets its baseline. */
        uint32_t now = get_time_ms();
        gamepad_tick(&launcher.gamepad, now);

        /* ── Post-launch cooldown ──────────────────────────────────────
         * After returning from a child process, ignore ALL input for
         * LAUNCH_COOLDOWN_MS.  This is the primary defense against
         * auto-relaunch caused by:
         *   (a) Keyboard repeat events slipping through the drain loop
         *   (b) Resistive touchscreen noise generating a false press
         *   (c) Mouse button bounce after returning from child
         * The drain loop in launch_app() is kept as additional safety. */
        if (launcher.last_launch_return_ms &&
            (now - launcher.last_launch_return_ms) < LAUNCH_COOLDOWN_MS) {
            /* Still in cooldown — draw once if needed, skip input */
            if (launcher.needs_redraw) {
                draw_launcher(&launcher);
                launcher.needs_redraw = false;
                pointer_invalidate(&launcher.pointer);
                pointer_paint(&launcher.pointer, &launcher.fb);
            }
            usleep(FRAME_DELAY_ACTIVE_US);
            continue;
        }

        /* The mouse: motion moves the arrow, a left click becomes this
         * frame's touch at it — so everything below, the dialog included,
         * takes it as a tap and a click hides the ring as a finger does.
         * Only the real finger and the nav keys hide the arrow. */
        {
            const ButtonState *kb = launcher.input.buttons;
            bool nav = kb[BTN_ID_UP].pressed   || kb[BTN_ID_DOWN].pressed  ||
                       kb[BTN_ID_LEFT].pressed || kb[BTN_ID_RIGHT].pressed ||
                       kb[BTN_ID_JUMP].pressed || kb[BTN_ID_ACTION].pressed ||
                       kb[BTN_ID_BACK].pressed || kb[BTN_ID_PAUSE].pressed;
            pointer_update(&launcher.pointer, &launcher.input, ts.pressed,
                           nav, &ts);
        }

        if (modal_dialog_is_active(&power_dialog)) {
            /* Closing it (or a failed command's screen) repaints via old_dialog. */
            handle_power_dialog(&launcher, &ts);
        } else {
            /* Touch first: a page it flips this frame is the page the focus
             * model then re-anchors the selection onto.  A mouse click is in
             * ts already (pointer_update above). */
            if (ts.pressed)
                LOG_DEBUG(&launcher.logger, "Touch: (%d, %d)", ts.x, ts.y);
            int result = handle_touch(&launcher, &ts);
            if (result == -2) {
                open_power_dialog();             /* with no finger on the panel */
            } else if (result >= 0) {
                /* Back from the child, the selection is the tile that was used. */
                icon_grid_focus_land(&grid, &launcher.focus,
                                     &launcher.current_page, result);
                launch_app(&launcher, &launcher.apps[result], NULL, fb_dev, touch_dev);
                launcher.needs_redraw = true;  /* State changed after launch return */
            } else if (result == TOUCH_MESSAGE) {
                launch_message(&launcher, fb_dev, touch_dev);
            }

            /* Handle gamepad / keyboard navigation */
            int gp_result = handle_gamepad_input(&launcher, ts.pressed);
            if (gp_result >= 0) {
                launch_app(&launcher, &launcher.apps[gp_result], NULL, fb_dev, touch_dev);
                launcher.needs_redraw = true;  /* State changed after launch return */
            } else if (gp_result == TOUCH_MESSAGE) {
                launch_message(&launcher, fb_dev, touch_dev);
            }
        }

        /* The bar: once a second.  The CPU / clock text alone is patched into
         * the drawing buffer and its rectangle presented (top_bar_info_present);
         * a changed message, a pending frame or the power dialog over the bar
         * draw the whole frame.  The pointer is put back over the rectangle below. */
        bool info_only = false;
        if (status_refresh(&launcher, get_time_ms(), &info_only)) {
            if (info_only && !launcher.needs_redraw && !modal_dialog_is_active(&power_dialog) &&
                top_bar_info_present(&launcher.fb, &launcher.bar, NULL))
                pointer_invalidate(&launcher.pointer);
            else
                launcher.needs_redraw = true;
        }

        /* Detect any visual state changes from input handling */
        if (modal_dialog_is_active(&power_dialog) != old_dialog ||
            power_dialog.focus != old_focus ||
            launcher.focus.sel   != old_sel.sel   ||
            launcher.focus.shown != old_sel.shown ||
            launcher.current_page != old_page     ||
            launcher.app_count    != old_count) {
            launcher.needs_redraw = true;
        }

        /* Conditional rendering — only redraw when UI state has changed */
        bool drew_frame = false;
        if (launcher.needs_redraw) {
            draw_launcher(&launcher);
            launcher.needs_redraw = false;
            drew_frame = true;
            pointer_invalidate(&launcher.pointer);   /* the frame lacks it */
        }
        /* The arrow alone, with fb_swap_rect(): a still mouse draws nothing,
         * a moving one keeps the loop at the active rate. */
        if (pointer_paint(&launcher.pointer, &launcher.fb))
            drew_frame = true;
        fb_overtemp_poll(&launcher.fb);   /* an idle screen presents nothing */

        /* Adaptive sleep — longer idle sleep reduces CPU usage significantly.
         * ~30 fps when actively redrawing; ~10 fps polling when idle. */
        if (drew_frame) {
            usleep(FRAME_DELAY_ACTIVE_US);
        } else {
            usleep(FRAME_DELAY_IDLE_US);
        }
    }

    /* Cleanup */
    LOG_INFO(&launcher.logger, "Shutting down launcher");
    logger_close(&launcher.logger);
    free_icons(&launcher);
    fb_clear(&launcher.fb, COLOR_BLACK);
    fb_swap(&launcher.fb);
    gamepad_close(&launcher.gamepad);
    touch_close(&launcher.touch);
    fb_close(&launcher.fb);
    return 0;
}
