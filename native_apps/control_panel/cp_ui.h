/* cp_ui.h — what control_panel's page modules share with control_panel.c.
 *
 * control_panel.c owns the page bar and the home grid; every page (led_page.c,
 * monitor_page.c, info_page.c, network_page.c, usb_page.c, input_page.c,
 * audio_page.c, display_page.c, bluetooth_page.c, ssh_page.c) lays itself out in the same content rectangle
 * and draws with the same helpers.
 * This header is the one home for that rectangle and those helpers, so a moved
 * page cannot carry a second copy of either.  The helper bodies stay in control_panel.c.
 *
 * Everything here is derived from SCREEN_SAFE_*, so it is only correct after
 * fb_init() and touch_init(), and a layout computed from it must be re-run
 * after any fb_set_bezel() — which is what rebuild_ui() is for.
 */
#ifndef CP_UI_H
#define CP_UI_H

#include "../common/framebuffer.h"
#include "../common/touch_input.h"
#include "../common/top_bar.h"
#include <stdbool.h>
#include <stddef.h>

/* The devices the panel opens, and the two it hands a child tool on its
 * command line — the same pair app_launcher passes for a manifest's fb,touch. */
#define FB_DEVICE         "/dev/fb0"
#define TOUCH_DEVICE      "/dev/input/touchscreen0"
/* The touch curve and bezel the wizard writes and the touch tests load. */
#define CALIB_FILE        "/etc/touch_calibration.conf"

/* The content rectangle: the safe area below the top bar (common/top_bar.h) and
 * its rule.  The bar is tall enough that this never starts lower than it did
 * under the old 44 px page bar, so a page that fitted still fits. */
#define TAB_DIVIDER_H     2
#define CONTENT_Y         (TOP_BAR_BOTTOM + TAB_DIVIDER_H)
#define CONTENT_H         (SCREEN_SAFE_BOTTOM - CONTENT_Y)
#define CONTENT_LEFT      (SCREEN_SAFE_LEFT + 10)
#define CONTENT_RIGHT     (SCREEN_SAFE_RIGHT - 10)
#define CONTENT_WIDTH     (CONTENT_RIGHT - CONTENT_LEFT)

/* draw_brightness_bar()'s track: the landscape width, and the height it
 * always draws at. */
#define BAR_WIDTH  300
#define BAR_HEIGHT 20

/* Above this many logical pixels, a touch inset stops looking like the panel's
 * saturation band and starts looking like a bad calibration. RW09 measures ~17;
 * 24 leaves headroom for panel variation without hiding a real fault.  Read by
 * the calibration wizard's report and the Display page's TOUCHABLE row. */
#define DISP_INSET_SUSPECT 24

#define COLOR_LABEL       RGB(180, 180, 180)
#define COLOR_BG          RGB(20, 20, 30)      /* the panel's background */
#define COLOR_DISABLED    RGB(120, 120, 120)   /* a control switched off */

/* A section title at y, with a rule running to CONTENT_RIGHT — or, for a
 * header heading one column of a two-column page, to `right`. */
void draw_section_header(Framebuffer *fb, int y, const char *title);
void draw_section_header_to(Framebuffer *fb, int y, const char *title, int right);

/* A value/min..max fill bar with its percentage printed to the right; drawn
 * grey when !active (a setting that is currently switched off). */
void draw_brightness_bar(Framebuffer *fb, int x, int y, int value,
                         int min_val, int max_val, int bar_width, bool active);

/* A used/total bar, green -> warn -> critical by fill, its percentage centred
 * inside; label (if non-empty) is drawn 18 px above it. */
void draw_usage_bar(Framebuffer *fb, int x, int y, int width, int height,
                    unsigned long used, unsigned long total, const char *label);

/* src drawn from x at scale, cut with ".." so it ends by CONTENT_RIGHT, into
 * out; true if it had to be cut (what a page's receipt counts). */
bool fit_value(const char *src, int x, int scale, char *out, size_t len);

/* A label at the left of the content and its value in a column at x 270
 * (150 in portrait); returns the next row's y (28 lower). */
int draw_info_row(Framebuffer *fb, int y, const char *label,
                  const char *value, uint32_t value_color);
/* The same row with the value at a caller's column (absolute x). */
int draw_info_row_at(Framebuffer *fb, int y, const char *label,
                     const char *value, uint32_t value_color, int value_x);

/* The first line of a file, newline stripped; 0, or -1 with buf empty. */
int read_file_line(const char *path, char *buf, size_t len);

/* A full-screen tester's frame: title, status, optional progress (-1 = none),
 * "TOUCH TO RETURN".  Swaps the framebuffer itself. */
void draw_test_screen(Framebuffer *fb, const char *title,
                      const char *status, int progress);

/* One poll; true on a press, with its position — or on Esc/Backspace (or a
 * pad's Start/Select), with x = y = -1, so every full-screen test that waits
 * on it also leaves by keyboard. */
bool check_touch(TouchInput *touch, int *x, int *y);

/* One pad-layer poll: true when Esc/Backspace or Start/Select was just pressed.
 * For full-screen loops that wait on something other than check_touch(). */
bool cp_key_back(void);

/* False once SIGINT/SIGTERM asked the panel to quit: a full-screen loop that
 * runs until the operator leaves it checks this, so a stop is not held up. */
bool cp_running(void);

#endif
