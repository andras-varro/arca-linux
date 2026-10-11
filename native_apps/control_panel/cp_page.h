/* cp_page.h — the one interface a control_panel page implements.
 *
 * A page is a compiled-in applet: one module, one const CpPage, one row in
 * control_panel.c's home grid naming it.  control_panel.c holds no per-page
 * code — it opens a page from its tile, draws its title bar (BACK and the
 * page's name), and dispatches layout, draw, input and full-screen runs
 * through this struct.  The page owns its state, its widgets and its layout,
 * in the content rectangle cp_ui.h defines, drawn with cp_ui.h's helpers.
 *
 * Adding a page: a new .c exposing a const CpPage, its extern below, a row in
 * home_pages[], and the file on control_panel's line in build-and-deploy.sh.
 */
#ifndef CP_PAGE_H
#define CP_PAGE_H

#include "../common/framebuffer.h"
#include "../common/touch_input.h"
#include "../common/config.h"
#include "../common/ui_focus.h"
#include "../common/gamepad.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

typedef enum {
    CP_PAGE_IDLE,        /* nothing visible changed */
    CP_PAGE_REDRAW,      /* something visible changed: repaint the page */
    CP_PAGE_FULLSCREEN   /* input() queued a full-screen run: call run_fullscreen */
} CpPageResult;

typedef struct CpPage {
    const char *name;   /* home tile label AND page title — one name */
    const char *icon;   /* basename under /opt/roomwizard/icons/, no .ppm; NULL = letter tile */

    /* Once at startup, from the control panel's loaded Config. */
    void (*load)(const Config *cfg);

    /* Lay out from CONTENT_* / SCREEN_SAFE_* and print the page's
     * "control_panel: <page> stack ..." receipt.  Called from rebuild_ui(),
     * i.e. again whenever the logical screen changes. */
    void (*layout)(void);

    void (*enter)(void);                      /* optional (NULL) */
    void (*leave)(void);                      /* optional (NULL) */

    void (*draw)(Framebuffer *fb);

    /* cfg is the control panel's in-memory Config; a page that writes keys
     * keeps it in step, so a later whole-file save elsewhere cannot put back a
     * value the page replaced. */
    CpPageResult (*input)(Config *cfg, int tx, int ty, bool touching, uint32_t now);

    /* Optional.  Blocking, full-screen: runs whatever input() queued and cleans
     * up its own hardware.  The caller only drains touch and repaints after. */
    void (*run_fullscreen)(Framebuffer *fb, TouchInput *touch);

    /* Optional.  cp_reset_all_defaults() (below) calls it on every page,
     * AFTER config_clear(cfg). */
    void (*reset_defaults)(Config *cfg);

    /* Optional.  Asked once per main-loop iteration while the page is open:
     * true while it holds something input() must service at the active frame
     * rate (an open audio stream, which FRAME_DELAY_IDLE_US would starve), and
     * the loop then sleeps FRAME_DELAY_ACTIVE_US.  It reports live state —
     * never a constant true, which would spin a static page at full rate. */
    bool (*busy)(void);

    /* Optional.  Keyboard/pad focus: writes the rects of the widgets that take
     * a tap RIGHT NOW (enabled and drawn — an overlay's buttons only while it
     * is up) into out, at most max, and returns the count.  Asked fresh each
     * frame after input(), computed from the same values the page's hit-test
     * uses (button_rect / toggle_hit_rect), never from a cached copy.
     * Activation is a synthetic tap at the rect's centre through input(), so a
     * page needs no other keyboard code.  NULL = nothing to focus. */
    int (*focusables)(UiRect *out, int max);

    /* Optional.  Keyboard/pad LEFT/RIGHT on a widget that steps rather than
     * being stepped past (a Cycler): asked BEFORE the ring moves, only while
     * the ring is shown on one of the page's focusables.  idx is that
     * widget's index in the list focusables() wrote THIS frame (the panel's
     * own BACK is not counted); dir is -1 for LEFT, +1 for RIGHT — a
     * cycler_step() dir.  Return true when the page consumed it (stepped the
     * widget, kept cfg in step as input() does): the ring stays and the page
     * is repainted.  false = the ring moves as usual.  UP/DOWN never come
     * here.  NULL = arrows only ever move the ring. */
    bool (*focus_nudge)(Config *cfg, int idx, int dir);
} CpPage;

extern const CpPage cp_led_page;       /* led_page.c */
extern const CpPage cp_monitor_page;   /* monitor_page.c */
extern const CpPage cp_log_page;       /* log_page.c */
extern const CpPage cp_info_page;      /* info_page.c */
extern const CpPage cp_network_page;   /* network_page.c */
extern const CpPage cp_ssh_page;       /* ssh_page.c */
extern const CpPage cp_usb_page;       /* usb_page.c */
extern const CpPage cp_input_page;     /* input_page.c */
extern const CpPage cp_audio_page;     /* audio_page.c */
extern const CpPage cp_display_page;   /* display_page.c */
extern const CpPage cp_bluetooth_page; /* bluetooth_page.c */
extern const CpPage cp_system_page;    /* system_page.c */

/* The Audio page's OUT setting, for another page that offers a shortcut to it
 * (the Bluetooth page's USE FOR AUDIO).  One writer: cp_audio_set_output()
 * goes through audio_page.c's own persist, so the Audio page's on-screen
 * choice, the in-memory Config and the file stay one value.  choice is an
 * AudioOutChoice (common/audio_out.h); cp_audio_output() is the SAVED one.
 * bt_addr is the preferred headset (config audio_bt_addr), in force under
 * BLUETOOTH and AUTO and kept across OUT changes; a malformed or empty address
 * is saved as "" (unpinned).  cp_audio_bt_addr() is the SAVED pin. */
int         cp_audio_output(void);
const char *cp_audio_bt_addr(void);
void        cp_audio_set_output(Config *cfg, int choice, const char *bt_addr);

/* Implemented in control_panel.c, for pages. */

/* The panel's one GamepadManager (common/gamepad.h): opened at startup,
 * polled and rescanned by the main loop.  For a page that reads or changes
 * the player slots (the Input page's PLAYERS rows); a page never inits,
 * polls or closes it. */
GamepadManager *cp_gamepad(void);

/* Posts msg on the page's status line — it takes the title bar's place for a
 * few seconds, green when ok, orange when not.  The panel repaints for it and
 * once more when it expires; the page need not return CP_PAGE_REDRAW for it. */
void cp_status(const char *msg, bool ok);

/* Asks before doing something: the panel's one confirmation dialog, ok_text
 * and CANCEL side by side, drawn
 * over the whole screen.  While it is up it takes all input — the title bar's
 * BACK and the page's input() included.  OK calls on_ok with the panel's
 * Config and repaints the page; CANCEL does nothing.  Returns at once: call it
 * from input() and return CP_PAGE_IDLE, the dialog appearing repaints.
 * Common's ModalDialog fixes the box at 420x200 px, which fits the 421 px
 * portrait width; title is scale 3 (at most 22 characters to fit), message
 * scale 2 (at most 33 a line), and a '\n' in message starts a second line. */
typedef void (*CpConfirmFn)(Config *cfg);
void cp_confirm(const char *title, const char *message, const char *ok_text,
                CpConfirmFn on_ok);

/* The touch geometry tools, for a page's run_fullscreen().  CALIBRATE is the
 * full wizard (interior fit, reach, edges), EDGES its screen-edge steps alone,
 * DIAGNOSTIC hands the screen to /opt/games/touch_raw until it exits.  The
 * wizard is landscape-only: in portrait it says so for 3 s and changes
 * nothing.  Blocking.  Afterwards calibration and bezel are whatever the
 * device now holds, the outcome is posted with cp_status(), and every page is
 * laid out again (its layout() runs) because the logical screen may have
 * changed size. */
enum { CP_TOUCH_CALIBRATE, CP_TOUCH_EDGES, CP_TOUCH_DIAGNOSTIC };
void cp_run_touch_tool(Framebuffer *fb, TouchInput *touch, int mode);

/* Puts /etc/touch_calibration.conf back to the hardware range and the default
 * edges (after a backup beside it), applies that, posts the outcome with
 * cp_status() — saving needs root — and lays every page out again.  For a
 * cp_confirm() on_ok. */
void cp_reset_touch_geometry(void);

/* The global RESET DEFAULTS, pressed on the Information page.  First copies
 * the config file to a timestamped "<path>.bak-YYYYmmdd-HHMMSS" beside it; if
 * that copy fails nothing is reset and -1 comes back.  Otherwise it clears
 * cfg, calls every page's reset_defaults (the Display page's re-applies the
 * backlight), saves the cleared file and returns 0.  Either way msg (len bytes) gets
 * the line to show: "BACKUP: <path>", a note when there was no file to copy
 * (nothing to lose, so it resets), or "RESET FAILED: BACKUP <reason>".
 * /etc/touch_calibration.conf is not touched. */
int cp_reset_all_defaults(Config *cfg, char *msg, size_t len);

#endif
