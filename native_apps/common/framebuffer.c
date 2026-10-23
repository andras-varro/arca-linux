#include "framebuffer.h"
#include "hardware.h"
#include "font.h"
#include "utf8.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/fb.h>
#include <linux/kd.h>
#include <sys/stat.h>
#include <time.h>
#include "overtemp.h"

#define LOGGER_LIB_CLIENT
#include "logger.h"

// Level of fb_init()'s routine reports. app_launcher closes and re-inits the
// framebuffer around every child, so only the process's first fb_init() reports
// at INFO and every later one at DEBUG. Warnings are unaffected.
static bool     fb_announced;
static LogLevel fb_note = LOG_LEVEL_INFO;

// Runtime bezel margins (pixels hidden by the plastic bezel)
int screen_bezel_top    = FB_BEZEL_TOP_DEFAULT;
int screen_bezel_bottom = FB_BEZEL_BOTTOM_DEFAULT;
int screen_bezel_left   = FB_BEZEL_LEFT_DEFAULT;
int screen_bezel_right  = FB_BEZEL_RIGHT_DEFAULT;

// Panel and viewport geometry. The defaults form an IDENTITY viewport so any
// code that reads these before fb_init() gets plain panel coordinates.
int screen_panel_width  = 800;
int screen_panel_height = 480;
int screen_view_x = 0;
int screen_view_y = 0;

// The panel's own resolution — see the header. Defaults match the surface
// defaults above, so the UI scale is a 1:1 no-op before fb_init() has run.
int screen_true_panel_width  = 800;
int screen_true_panel_height = 480;

// Logical (visible) screen dimensions
int screen_base_width  = 800;
int screen_base_height = 480;

// Visible-but-not-touchable band at each logical edge. Zero until touch_input.c
// measures it, so an app that never initialises touch treats the whole logical
// screen as safe — which is also the right answer on an uncalibrated device.
int screen_touch_inset_top    = 0;
int screen_touch_inset_bottom = 0;
int screen_touch_inset_left   = 0;
int screen_touch_inset_right  = 0;

static int clamp_inset(const char *side, int v, int max) {
    if (v < 0) return 0;
    if (v > max) {
        // Loud, because the alternative is every UI in the system quietly
        // shrinking to fit a broken calibration.
        LIB_LOG(LOG_LEVEL_WARN, "Touch inset: %s %d px exceeds the %d px limit — clamped. "
               "The calibration is almost certainly wrong; recalibrate.",
               side, v, max);
        return max;
    }
    return v;
}

// The cap is a FRACTION of the panel — "~10% of the short axis" — so it converts
// with the surface like any other design pixel. Left at a flat 48 it would be
// ~10% of 480 rows but 20% of a 240-row surface, i.e. twice as permissive exactly
// where there is half the screen to lose. The insets themselves arrive already in
// surface pixels, measured by touch_input.c through the live map, so only the cap
// needed converting.
void fb_set_touch_inset(int top, int bottom, int left, int right) {
    int max_y = fb_ui_px_y(FB_TOUCH_INSET_MAX);
    int max_x = fb_ui_px_x(FB_TOUCH_INSET_MAX);

    screen_touch_inset_top    = clamp_inset("top", top, max_y);
    screen_touch_inset_bottom = clamp_inset("bottom", bottom, max_y);
    screen_touch_inset_left   = clamp_inset("left", left, max_x);
    screen_touch_inset_right  = clamp_inset("right", right, max_x);
}

void fb_load_bezel(void) {
    // No file, or no margin line in it, means "not configured" → defaults.
    // A margin line that IS present is honoured exactly, zeros included.
    screen_bezel_top    = FB_BEZEL_TOP_DEFAULT;
    screen_bezel_bottom = FB_BEZEL_BOTTOM_DEFAULT;
    screen_bezel_left   = FB_BEZEL_LEFT_DEFAULT;
    screen_bezel_right  = FB_BEZEL_RIGHT_DEFAULT;

    FILE *f = fopen("/etc/touch_calibration.conf", "r");
    if (!f) {
        LIB_LOG(fb_note, "Bezel: no calibration file — using defaults T=%d B=%d L=%d R=%d",
               screen_bezel_top, screen_bezel_bottom,
               screen_bezel_left, screen_bezel_right);
        return;
    }

    char line[256];
    int data_lines = 0;
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        data_lines++;
        if (data_lines == 2) {
            int t, b, l, r;
            if (sscanf(line, "%d %d %d %d", &t, &b, &l, &r) == 4) {
                screen_bezel_top    = t;
                screen_bezel_bottom = b;
                screen_bezel_left   = l;
                screen_bezel_right  = r;
            }
            break;
        }
    }
    fclose(f);

    LIB_LOG(fb_note, "Bezel: margins T=%d B=%d L=%d R=%d from /etc/touch_calibration.conf",
           screen_bezel_top, screen_bezel_bottom,
           screen_bezel_left, screen_bezel_right);
}

// The panel's own resolution, from the display's mode timings. Deliberately NOT
// any framebuffer's geometry: a scaling DSS overlay presents a SMALLER surface
// upscaled to fill the panel, so the two are different numbers exactly when this
// matters, and fb0's size is not a stand-in for the panel's. Timings format is
// "<pixclock>,<xres>/<hfp>/<hbp>/<hsw>,<yres>/<vfp>/<vbp>/<vsw>".
// Returns 0 and writes *pw/*ph on success; on any failure returns -1 and leaves
// them untouched, and the caller then treats the surface as the panel (1:1).
// Absent off-device, which is why the arithmetic below is a separate function.
static int fb_read_panel_size(int *pw, int *ph) {
    FILE *f = fopen("/sys/devices/platform/omapdss/display0/timings", "r");
    if (!f) return -1;

    char line[128];
    if (!fgets(line, sizeof(line), f)) { fclose(f); return -1; }
    fclose(f);

    long pixclock;
    int x, y;
    if (sscanf(line, "%ld,%d/%*d/%*d/%*d,%d/", &pixclock, &x, &y) != 3) return -1;
    if (x < 16 || y < 16) return -1;

    *pw = x;
    *ph = y;
    return 0;
}

// Convert the bezel margins from PANEL pixels into SURFACE pixels. Pure, and
// public, because it is the part that can be wrong and the sysfs read above
// cannot run on a host: see native_apps/tests/bezel_scale_test.c.
//
// The margins name pixels the plastic bezel physically covers, so they are panel
// pixels. Subtracting them from a surface that a scaling overlay will upscale
// spends them at the upscale factor — measured on a 400x240 surface upscaled 2x,
// where T=15 ate 30 of the panel's 480 rows and left a band visible below the
// bezel. Rounds DOWN, so the drawn area runs slightly under the bezel rather
// than stopping short of it: an overlap is invisible, a gap is not.
// A panel dimension of 0, or one equal to the surface, is left 1:1.
void fb_scale_bezel_to_surface(int panel_w, int panel_h, int surf_w, int surf_h,
                               int *top, int *bottom, int *left, int *right) {
    if (panel_w > 0 && surf_w > 0 && panel_w != surf_w) {
        *left  = *left  * surf_w / panel_w;
        *right = *right * surf_w / panel_w;
    }
    if (panel_h > 0 && surf_h > 0 && panel_h != surf_h) {
        *top    = *top    * surf_h / panel_h;
        *bottom = *bottom * surf_h / panel_h;
    }
}

// Convert one DESIGN pixel count into surface pixels — the pure half of the UI
// scale, contract in the header. Rounds DOWN like the bezel conversion above, and
// an equal or unreadable panel dimension is an exact 1:1 no-op.
//
// It never rounds a positive input away to nothing: a button scaled to zero
// pixels is invisible AND untappable, which is a worse failure than one pixel
// short, and the caller cannot tell the two apart afterwards. The floor is at 1
// rather than at a finger width on purpose — this function does not know whether
// the pixels it was handed are a tap target or a one-pixel gridline.
int fb_scale_ui_px(int panel_dim, int surf_dim, int px) {
    if (panel_dim <= 0 || surf_dim <= 0 || panel_dim == surf_dim) return px;

    int scaled = px * surf_dim / panel_dim;
    if (px > 0 && scaled < 1) scaled = 1;
    return scaled;
}

int fb_ui_px_x(int px) {
    return fb_scale_ui_px(screen_true_panel_width, screen_panel_width, px);
}

int fb_ui_px_y(int px) {
    return fb_scale_ui_px(screen_true_panel_height, screen_panel_height, px);
}

// Recompute the logical surface from the SURFACE dims + current bezel margins
// and publish the result to both fb and the globals. surf_w/surf_h are the
// framebuffer's own xres/yres, which equal the panel's only when nothing is
// scaling this node. Margins that would leave less than a sixteenth of an axis
// usable are rejected as a typo/misconfiguration.
//
// The globals keep their published meaning: SURFACE space throughout, which is
// what indexes the framebuffer mmap and what touch_input.c maps onto. Touch
// needs no scale conversion — see the header note on fb_scale_bezel_to_surface.
static int fb_apply_viewport(Framebuffer *fb, int surf_w, int surf_h) {
    int t = screen_bezel_top,  b = screen_bezel_bottom;
    int l = screen_bezel_left, r = screen_bezel_right;

    int panel_w = surf_w, panel_h = surf_h;
    if (fb_read_panel_size(&panel_w, &panel_h) == 0) {
        // The timings are physical; the app's orientation may be rotated.
        if (fb->portrait_mode) { int s = panel_w; panel_w = panel_h; panel_h = s; }
        fb_scale_bezel_to_surface(panel_w, panel_h, surf_w, surf_h, &t, &b, &l, &r);
    }

    int lw = surf_w - l - r;
    int lh = surf_h - t - b;

    if (screen_bezel_top < 0 || screen_bezel_bottom < 0 ||
        screen_bezel_left < 0 || screen_bezel_right < 0 ||
        lw < surf_w / 16 || lh < surf_h / 16) {
        return -1;
    }

    fb->view_x = l;
    fb->view_y = t;
    fb->width  = (uint32_t)lw;
    fb->height = (uint32_t)lh;

    // Deliberately the SURFACE dims, not the panel dims read above: every
    // reader of these two treats them as the space fb->buffer is indexed in,
    // and touch_input.c maps into that space. Publishing the true panel here
    // would change their meaning on a scaled node and silently move touch.
    // ⚠️ No host test can catch that change. Off-device fb_read_panel_size()
    // fails, so panel == surf and the two candidates are the same number; a
    // group asserting it would be a vacuous pass. This comment is the guard,
    // and touch_map_test.c group J is what would then start failing on a device.
    screen_panel_width  = surf_w;
    screen_panel_height = surf_h;

    // The true panel, from the timings, in its own pair — the UI scale needs the
    // ratio between the two, and panel_w/panel_h die with this function. Equal to
    // the surface whenever nothing scales this node, and when the timings could
    // not be read at all, so fb_ui_px_*() is then an exact no-op.
    screen_true_panel_width  = panel_w;
    screen_true_panel_height = panel_h;
    screen_view_x       = fb->view_x;
    screen_view_y       = fb->view_y;
    screen_base_width   = lw;
    screen_base_height  = lh;
    return 0;
}

// Paint the whole panel black, so the bezel bands are clean and nothing from a
// previously running app survives outside the logical surface.
static void fb_black_panel(Framebuffer *fb) {
    if (fb->buffer != MAP_FAILED && fb->buffer != NULL)
        memset(fb->buffer, 0, fb->screen_size);
}

int fb_set_bezel(Framebuffer *fb, int top, int bottom, int left, int right) {
    int old_t = screen_bezel_top,  old_b = screen_bezel_bottom;
    int old_l = screen_bezel_left, old_r = screen_bezel_right;

    screen_bezel_top    = top;
    screen_bezel_bottom = bottom;
    screen_bezel_left   = left;
    screen_bezel_right  = right;

    int panel_w = fb->portrait_mode ? (int)fb->phys_height : (int)fb->phys_width;
    int panel_h = fb->portrait_mode ? (int)fb->phys_width  : (int)fb->phys_height;

    if (fb_apply_viewport(fb, panel_w, panel_h) < 0) {
        screen_bezel_top    = old_t;
        screen_bezel_bottom = old_b;
        screen_bezel_left   = old_l;
        screen_bezel_right  = old_r;
        fb_apply_viewport(fb, panel_w, panel_h);
        return -1;
    }

    size_t new_size = (size_t)fb->width * fb->height * fb->bytes_per_pixel;
    if (new_size != fb->back_buffer_size) {
        uint32_t *nb = (uint32_t *)realloc(fb->back_buffer, new_size);
        if (nb == NULL) {
            screen_bezel_top    = old_t;
            screen_bezel_bottom = old_b;
            screen_bezel_left   = old_l;
            screen_bezel_right  = old_r;
            fb_apply_viewport(fb, panel_w, panel_h);
            return -1;
        }
        fb->back_buffer = nb;
        fb->back_buffer_size = new_size;
    }
    memset(fb->back_buffer, 0, fb->back_buffer_size);
    fb_black_panel(fb);
    return 0;
}

bool fb_is_portrait_mode(void) {
    return access("/opt/games/portrait.mode", F_OK) == 0;
}

int fb_set_bpp(const char *device, int bpp) {
    int fd = open(device, O_RDWR);
    if (fd < 0) return -1;

    struct fb_var_screeninfo vinfo;
    if (ioctl(fd, FBIOGET_VSCREENINFO, &vinfo) < 0) {
        close(fd);
        return -1;
    }

    vinfo.bits_per_pixel = bpp;
    int ret = ioctl(fd, FBIOPUT_VSCREENINFO, &vinfo);
    close(fd);
    return ret;
}

// ---------------------------------------------------------------------------
// Pixel format dispatch
//
// /dev/fb0's bpp is chosen at RUNTIME by whatever app called fb_set_bpp(): the
// native menus, tools and games run 32bpp XRGB8888; ScummVM and the VNC session
// run 16bpp RGB565 to halve write bandwidth on this memory-bound part. The back
// buffer is allocated as width * height * bytes_per_pixel, so a primitive that
// writes a uint32_t unconditionally overruns a 16bpp allocation by exactly 2x —
// which is what every primitive below used to do. It was reachable, not
// theoretical: no game asserted the bpp, so one launched over SSH after a 16bpp
// app inherited 16bpp and ran the full overflow. Both halves are fixed — the
// dispatch below, and an fb_set_bpp() call in front of every fb_init() — and
// tests/framebuffer_bpp_test.c is the host regression for this one.
//
// The API is unchanged: callers always pass 24-bit RGB888 and these four helpers
// are the only code that knows what the surface actually holds. fb_init() must
// keep accepting 16bpp — ScummVM legitimately drives this path.
// ---------------------------------------------------------------------------

#define FB_IS_16BPP(fb) ((fb)->bytes_per_pixel == 2)

static inline uint16_t fb_pack565(uint32_t c) {
    return (uint16_t)((((c >> 16) & 0xF8) << 8) |
                      (((c >>  8) & 0xFC) << 3) |
                      (( c        & 0xF8) >> 3));
}

// 5/6-bit channel back to 8 bits by replicating the high bits into the low ones.
// Exact at both ends (0 -> 0, max -> 255) and division-free, which matters here:
// the Cortex-A8 has no hardware divide, so a /31 would become a call into
// __aeabi_uidiv inside the alpha-blend inner loop.
static inline uint32_t fb_unpack565(uint16_t v) {
    uint32_t r = (v >> 11) & 0x1F, g = (v >> 5) & 0x3F, b = v & 0x1F;
    r = (r << 3) | (r >> 2);
    g = (g << 2) | (g >> 4);
    b = (b << 3) | (b >> 2);
    return (r << 16) | (g << 8) | b;
}

static inline void fb_store(void *target, size_t idx, uint32_t color, bool is16) {
    if (is16) ((uint16_t *)target)[idx] = fb_pack565(color);
    else      ((uint32_t *)target)[idx] = color;
}

static inline uint32_t fb_load(const void *target, size_t idx, bool is16) {
    if (is16) return fb_unpack565(((const uint16_t *)target)[idx]);
    return ((const uint32_t *)target)[idx] & 0x00FFFFFF;
}

// The surface every primitive writes into. Kept in one place so the
// double-buffering test is not repeated in a dozen functions.
static inline void *fb_target(Framebuffer *fb) {
    return fb->double_buffering ? (void *)fb->back_buffer : (void *)fb->buffer;
}

// On our kernel image the panel is also the boot console, and fbcon would draw every
// later printk over whatever an app has mmapped. KD_GRAPHICS stops fbcon drawing. It is
// set on every init, so a crashed or kill -9'ed predecessor is repaired, and fb_close()
// never puts KD_TEXT back: the launcher closes and re-inits around each child, and the
// console would flash in between. /dev/tty0 explicitly, because apps started from init
// have no controlling tty. On the vendor kernel, which has no console on the panel, it is
// a harmless no-op.
static void fb_console_graphics(void) {
    int tty = open("/dev/tty0", O_RDWR | O_CLOEXEC);
    if (tty < 0) return;
    ioctl(tty, KDSETMODE, KD_GRAPHICS);
    close(tty);
}

int fb_init(Framebuffer *fb, const char *device) {
    struct fb_var_screeninfo vinfo;
    struct fb_fix_screeninfo finfo;

    fb_console_graphics();

    fb_note = fb_announced ? LOG_LEVEL_DEBUG : LOG_LEVEL_INFO;
    // Load bezel margins from calibration config
    fb_load_bezel();

    fb->fd = open(device, O_RDWR);
    if (fb->fd == -1) {
        perror("Error opening framebuffer device");
        return -1;
    }
    
    if (ioctl(fb->fd, FBIOGET_FSCREENINFO, &finfo) == -1) {
        perror("Error reading fixed information");
        close(fb->fd);
        return -1;
    }
    
    if (ioctl(fb->fd, FBIOGET_VSCREENINFO, &vinfo) == -1) {
        perror("Error reading variable information");
        close(fb->fd);
        return -1;
    }
    
    fb->phys_width = vinfo.xres;
    fb->phys_height = vinfo.yres;
    fb->portrait_mode = fb_is_portrait_mode();

    // Panel dimensions in the app's orientation
    int panel_w, panel_h;
    if (fb->portrait_mode) {
        // Apps see swapped dimensions (e.g., 480x800 instead of 800x480)
        panel_w = fb->phys_height;
        panel_h = fb->phys_width;
        LIB_LOG(fb_note, "Portrait mode: physical %dx%d -> virtual %dx%d",
               fb->phys_width, fb->phys_height, panel_w, panel_h);

        // Rotate the bezel margins into the virtual coordinate system (90 CCW):
        // physical left -> virtual top, right -> bottom, top -> right, bottom -> left
        int pt = screen_bezel_top, pb = screen_bezel_bottom;
        int pl = screen_bezel_left, pr = screen_bezel_right;
        screen_bezel_top    = pl;
        screen_bezel_bottom = pr;
        screen_bezel_left   = pb;
        screen_bezel_right  = pt;

        LIB_LOG(fb_note, "Portrait mode: rotated bezel margins T=%d B=%d L=%d R=%d",
               screen_bezel_top, screen_bezel_bottom,
               screen_bezel_left, screen_bezel_right);
    } else {
        panel_w = fb->phys_width;
        panel_h = fb->phys_height;
    }

    // Shrink the drawing surface to the visible rectangle. Bad margins fall
    // back to no bezel rather than leaving the app with an unusable screen.
    if (fb_apply_viewport(fb, panel_w, panel_h) < 0) {
        fprintf(stderr, "Bezel margins T=%d B=%d L=%d R=%d unusable on a %dx%d panel"
                        " — ignoring them\n",
                screen_bezel_top, screen_bezel_bottom,
                screen_bezel_left, screen_bezel_right, panel_w, panel_h);
        screen_bezel_top = screen_bezel_bottom = 0;
        screen_bezel_left = screen_bezel_right = 0;
        fb_apply_viewport(fb, panel_w, panel_h);
    }

    fb->bytes_per_pixel = vinfo.bits_per_pixel / 8;

    // Only 32bpp XRGB8888 and 16bpp RGB565 have drawing primitives (see the
    // pixel-format dispatch above). Anything else would silently write the wrong
    // pixel size into the allocation, so try to force 32bpp on the fd we already
    // hold and give up loudly if the panel will not take it — a heap-corrupting
    // app is worse than one that refuses to start with a reason on stdout.
    if (fb->bytes_per_pixel != 4 && fb->bytes_per_pixel != 2) {
        fprintf(stderr, "Framebuffer: %d bpp is not a format the drawing primitives"
                        " support — forcing 32 bpp\n", vinfo.bits_per_pixel);
        vinfo.bits_per_pixel = 32;
        if (ioctl(fb->fd, FBIOPUT_VSCREENINFO, &vinfo) == -1 ||
            ioctl(fb->fd, FBIOGET_VSCREENINFO, &vinfo) == -1 ||
            (vinfo.bits_per_pixel != 32 && vinfo.bits_per_pixel != 16)) {
            fprintf(stderr, "Framebuffer: still %d bpp — refusing to draw, every"
                            " primitive would write the wrong pixel size\n",
                    vinfo.bits_per_pixel);
            close(fb->fd);
            return -1;
        }
        // Re-read the stride: it changes with the pixel size.
        if (ioctl(fb->fd, FBIOGET_FSCREENINFO, &finfo) == -1) {
            perror("Error re-reading fixed information");
            close(fb->fd);
            return -1;
        }
        fb->bytes_per_pixel = vinfo.bits_per_pixel / 8;
    }

    fb->line_length = finfo.line_length;
    fb->screen_size = fb->line_length * fb->phys_height;  // Physical size for mmap
    fb->back_buffer_size = (size_t)fb->width * fb->height * fb->bytes_per_pixel;

    fb->buffer = (uint32_t *)mmap(0, fb->screen_size, PROT_READ | PROT_WRITE, MAP_SHARED, fb->fd, 0);
    if (fb->buffer == MAP_FAILED) {
        perror("Error mapping framebuffer device to memory");
        close(fb->fd);
        return -1;
    }

    // Allocate back buffer for double buffering
    fb->back_buffer = (uint32_t *)malloc(fb->back_buffer_size);
    if (fb->back_buffer == NULL) {
        perror("Error allocating back buffer");
        munmap(fb->buffer, fb->screen_size);
        close(fb->fd);
        return -1;
    }
    fb->double_buffering = true;

    fb->draw_offset_x = 0;
    fb->draw_offset_y = 0;

    // Black the whole panel once: clears the bezel bands and any leftovers.
    fb_black_panel(fb);

    LIB_LOG(fb_note, "Framebuffer initialized: %dx%d logical at (%d,%d) on a %dx%d panel%s, %d bpp",
           fb->width, fb->height, fb->view_x, fb->view_y, panel_w, panel_h,
           fb->portrait_mode ? " [portrait]" : "", vinfo.bits_per_pixel);
    fb_announced = true;
    fb_note = LOG_LEVEL_INFO;   // a later explicit fb_load_bezel() still reports
    return 0;
}

void fb_close(Framebuffer *fb) {
    if (fb->back_buffer != NULL) {
        free(fb->back_buffer);
    }
    if (fb->buffer != MAP_FAILED) {
        munmap(fb->buffer, fb->screen_size);
    }
    if (fb->fd != -1) {
        close(fb->fd);
    }
}

static void fb_present_full(Framebuffer *fb) {
    if (!fb->double_buffering || fb->back_buffer == NULL)
        return;

    if (fb->portrait_mode) {
        // 90 CCW rotation from the logical surface into the panel, offset by the
        // viewport: logical (lx, ly) -> virtual (lx+view_x, ly+view_y)
        //                            -> physical (vy, phys_height-1-vx)
        // So physical row py = phys_height-1-view_x-lx holds logical COLUMN lx,
        // laid out contiguously from physical x = view_y.
        //
        // The loop is ordered for the write side: /dev/fb0 is an uncached
        // write-combined mapping, so every store goes to the panel in ascending
        // address runs, and the strided walk is done on the malloc'd (cached)
        // back buffer instead. Writing down a physical column — 4-byte stores
        // at a line_length stride — is the inferred cause of Frogger costing
        // ~22 ms more CPU per frame in portrait than in landscape (measured on
        // RW09 from /proc/<pid>/stat). The logical rows are taken in bands of
        // FB_ROT_BAND so the band's source cache lines (one per logical row)
        // stay resident in L1 while the column index walks across them.
        // Pointer steps only: no multiply or divide in the inner loop.
        enum { FB_ROT_BAND = 64 };
        const uint32_t bpp = fb->bytes_per_pixel;
        const bool is16 = FB_IS_16BPP(fb);
        const uint32_t lw = fb->width;
        const uint32_t lh = fb->height;
        const size_t ll = fb->line_length;
        // Physical row of the LAST logical column; rows ascend as lx descends.
        const uint32_t py_first = fb->phys_height - 1 - (fb->view_x + lw - 1);
        uint8_t *dst_base = (uint8_t *)fb->buffer + (size_t)py_first * ll
                          + (size_t)fb->view_y * bpp;

        for (uint32_t ly0 = 0; ly0 < lh; ly0 += FB_ROT_BAND) {
            const uint32_t n = (lh - ly0 < FB_ROT_BAND) ? lh - ly0 : FB_ROT_BAND;
            uint8_t *drow = dst_base + (size_t)ly0 * bpp;
            if (is16) {
                const uint16_t *scol = (const uint16_t *)fb->back_buffer
                                     + (size_t)ly0 * lw + (lw - 1);
                for (uint32_t c = 0; c < lw; c++, scol--, drow += ll) {
                    uint16_t *d = (uint16_t *)drow;
                    const uint16_t *s = scol;
                    for (uint32_t i = 0; i < n; i++, s += lw) d[i] = *s;
                }
            } else {
                const uint32_t *scol = (const uint32_t *)fb->back_buffer
                                     + (size_t)ly0 * lw + (lw - 1);
                for (uint32_t c = 0; c < lw; c++, scol--, drow += ll) {
                    uint32_t *d = (uint32_t *)drow;
                    const uint32_t *s = scol;
                    for (uint32_t i = 0; i < n; i++, s += lw) d[i] = *s;
                }
            }
        }
        return;
    }

    if (fb->view_x == 0 && fb->view_y == 0 &&
        fb->width == fb->phys_width && fb->height == fb->phys_height &&
        fb->line_length == fb->width * fb->bytes_per_pixel) {
        // No bezel and no line padding: one straight copy
        memcpy(fb->buffer, fb->back_buffer, fb->back_buffer_size);
        return;
    }

    // Letterboxed: copy row by row into the visible rectangle. Byte-based so it
    // is correct at any bpp (ScummVM runs this path at 16bpp).
    const uint32_t bpp = fb->bytes_per_pixel;
    const uint32_t row_bytes = fb->width * bpp;
    const uint8_t *src = (const uint8_t *)fb->back_buffer;
    uint8_t *dst = (uint8_t *)fb->buffer
                 + (size_t)fb->view_y * fb->line_length
                 + (size_t)fb->view_x * bpp;

    for (uint32_t y = 0; y < fb->height; y++) {
        memcpy(dst, src, row_bytes);
        src += row_bytes;
        dst += fb->line_length;
    }
}

static void fb_present_rect(Framebuffer *fb, int x, int y, int w, int h) {
    if (!fb->double_buffering || fb->back_buffer == NULL)
        return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > (int)fb->width)  w = (int)fb->width - x;
    if (y + h > (int)fb->height) h = (int)fb->height - y;
    if (w <= 0 || h <= 0)
        return;

    const uint32_t bpp = fb->bytes_per_pixel;
    const uint32_t lw = fb->width;
    const size_t ll = fb->line_length;

    if (fb->portrait_mode) {
        // fb_swap()'s mapping: logical column lx lands on physical row
        // phys_height-1-view_x-lx, logical row ly at physical x view_y+ly.
        // Taken column by column so each store run ascends in address.
        const bool is16 = FB_IS_16BPP(fb);
        for (int c = 0; c < w; c++) {
            const uint32_t lx = (uint32_t)(x + c);
            uint8_t *drow = (uint8_t *)fb->buffer
                          + (size_t)(fb->phys_height - 1 - fb->view_x - lx) * ll
                          + (size_t)(fb->view_y + (uint32_t)y) * bpp;
            if (is16) {
                const uint16_t *s = (const uint16_t *)fb->back_buffer
                                  + (size_t)y * lw + lx;
                uint16_t *d = (uint16_t *)drow;
                for (int i = 0; i < h; i++, s += lw) d[i] = *s;
            } else {
                const uint32_t *s = (const uint32_t *)fb->back_buffer
                                  + (size_t)y * lw + lx;
                uint32_t *d = (uint32_t *)drow;
                for (int i = 0; i < h; i++, s += lw) d[i] = *s;
            }
        }
        return;
    }

    const size_t run = (size_t)w * bpp;
    const uint8_t *src = (const uint8_t *)fb->back_buffer
                       + ((size_t)y * lw + (size_t)x) * bpp;
    uint8_t *dst = (uint8_t *)fb->buffer
                 + (size_t)(fb->view_y + (uint32_t)y) * ll
                 + (size_t)(fb->view_x + (uint32_t)x) * bpp;
    for (int r = 0; r < h; r++) {
        memcpy(dst, src, run);
        src += (size_t)lw * bpp;
        dst += ll;
    }
}

// Over-temperature square (common/overtemp.h): arcamond sets MON_FLAG_OVERTEMP
// in its ring header, and every present paints a red square into the FRONT
// buffer at the visible area's top-right corner — never into the back buffer,
// so no app's content is touched and clearing is a copy back from it. The
// ring is read at most once per OVERTEMP_CHECK_MS, per process; between reads
// a present costs one clock_gettime and, while set, FB_OT_SIZE^2 stores.
#define FB_OT_SIZE 16

static struct { long long last_ms; int on; } fb_ot = { -1, 0 };

static int fb_overtemp_read(void) {
    int fd = open(MON_RING_PATH, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    char buf[64];
    struct stat st;
    ssize_t n = fstat(fd, &st) == 0 ? read(fd, buf, sizeof(buf) - 1) : -1;
    close(fd);
    if (n <= 0) return 0;
    buf[n] = '\0';
    return overtemp_fresh((long long)st.st_mtime, (long long)time(NULL))
        && overtemp_ring_flag(buf) == 1;
}

static void fb_overtemp_paint(Framebuffer *fb) {
    const bool is16 = FB_IS_16BPP(fb);
    const uint32_t red = 0xFF0000;
    const int s = FB_OT_SIZE;
    if ((int)fb->width < s || (int)fb->height < s) return;
    const uint32_t x0 = fb->width - (uint32_t)s;
    for (int r = 0; r < s; r++) {
        for (int c = 0; c < s; c++) {
            const uint32_t lx = x0 + (uint32_t)c, ly = (uint32_t)r;
            uint8_t *row;
            uint32_t px;
            if (fb->portrait_mode) {   // fb_swap()'s rotation, one pixel
                row = (uint8_t *)fb->buffer
                    + (size_t)(fb->phys_height - 1 - fb->view_x - lx) * fb->line_length;
                px = fb->view_y + ly;
            } else {
                row = (uint8_t *)fb->buffer
                    + (size_t)(fb->view_y + ly) * fb->line_length;
                px = fb->view_x + lx;
            }
            fb_store(row, px, red, is16);
        }
    }
}

void fb_overtemp_poll(Framebuffer *fb) {
    if (!fb->double_buffering || fb->back_buffer == NULL || fb->buffer == NULL)
        return;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    long long now_ms = (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
    int was = fb_ot.on;
    if (overtemp_due(now_ms, &fb_ot.last_ms))
        fb_ot.on = fb_overtemp_read();
    if (fb_ot.on)
        fb_overtemp_paint(fb);
    else if (was)
        fb_present_rect(fb, (int)fb->width - FB_OT_SIZE, 0, FB_OT_SIZE, FB_OT_SIZE);
}

void fb_swap(Framebuffer *fb) {
    if (!fb->double_buffering || fb->back_buffer == NULL)
        return;
    fb_present_full(fb);
    fb_overtemp_poll(fb);
    hw_blank_poll();
}

void fb_swap_rect(Framebuffer *fb, int x, int y, int w, int h) {
    fb_present_rect(fb, x, y, w, h);
    fb_overtemp_poll(fb);
    hw_blank_poll();
}

void fb_clear(Framebuffer *fb, uint32_t color) {
    /* Clear the back buffer if double buffering is enabled */
    void *target = fb_target(fb);
    size_t total = (size_t)fb->width * fb->height;
    if (color == 0) {
        /* Fast path: memset for black (all-zero). Sized in bytes so it stays
         * inside the allocation at any bpp. */
        memset(target, 0, fb->double_buffering ? fb->back_buffer_size
                                               : fb->screen_size);
    } else if (FB_IS_16BPP(fb)) {
        /* One packed word per pixel — a uint32_t fill would run off the end of
         * a 16bpp allocation, which is the 2x overflow the dispatch above
         * exists to prevent. */
        uint16_t c = fb_pack565(color);
        uint16_t *p = (uint16_t *)target;
        for (size_t i = 0; i < total; i++) p[i] = c;
    } else {
        uint32_t *p = (uint32_t *)target;
        for (size_t i = 0; i < total; i++) p[i] = color;
    }
}

void fb_set_draw_offset(Framebuffer *fb, int dx, int dy) {
    fb->draw_offset_x = dx;
    fb->draw_offset_y = dy;
}

void fb_clear_draw_offset(Framebuffer *fb) {
    fb->draw_offset_x = 0;
    fb->draw_offset_y = 0;
}

void fb_draw_pixel(Framebuffer *fb, int x, int y, uint32_t color) {
    if (x >= 0 && x < (int)fb->width && y >= 0 && y < (int)fb->height) {
        // Draw to back buffer if double buffering is enabled
        fb_store(fb_target(fb), (size_t)y * fb->width + x, color, FB_IS_16BPP(fb));
    }
}

void fb_draw_rect(Framebuffer *fb, int x, int y, int w, int h, uint32_t color) {
    x += fb->draw_offset_x;
    y += fb->draw_offset_y;
    for (int i = 0; i < w; i++) {
        fb_draw_pixel(fb, x + i, y, color);
        fb_draw_pixel(fb, x + i, y + h - 1, color);
    }
    for (int i = 0; i < h; i++) {
        fb_draw_pixel(fb, x, y + i, color);
        fb_draw_pixel(fb, x + w - 1, y + i, color);
    }
}

void fb_fill_rect(Framebuffer *fb, int x, int y, int w, int h, uint32_t color) {
    x += fb->draw_offset_x;
    y += fb->draw_offset_y;
    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++) {
            fb_draw_pixel(fb, x + i, y + j, color);
        }
    }
}

void fb_draw_circle(Framebuffer *fb, int cx, int cy, int radius, uint32_t color) {
    cx += fb->draw_offset_x;
    cy += fb->draw_offset_y;
    int x = radius;
    int y = 0;
    int err = 0;
    
    while (x >= y) {
        fb_draw_pixel(fb, cx + x, cy + y, color);
        fb_draw_pixel(fb, cx + y, cy + x, color);
        fb_draw_pixel(fb, cx - y, cy + x, color);
        fb_draw_pixel(fb, cx - x, cy + y, color);
        fb_draw_pixel(fb, cx - x, cy - y, color);
        fb_draw_pixel(fb, cx - y, cy - x, color);
        fb_draw_pixel(fb, cx + y, cy - x, color);
        fb_draw_pixel(fb, cx + x, cy - y, color);
        
        if (err <= 0) {
            y += 1;
            err += 2*y + 1;
        }
        if (err > 0) {
            x -= 1;
            err -= 2*x + 1;
        }
    }
}

void fb_fill_circle(Framebuffer *fb, int cx, int cy, int radius, uint32_t color) {
    cx += fb->draw_offset_x;
    cy += fb->draw_offset_y;
    for (int y = -radius; y <= radius; y++) {
        for (int x = -radius; x <= radius; x++) {
            if (x*x + y*y <= radius*radius) {
                fb_draw_pixel(fb, cx + x, cy + y, color);
            }
        }
    }
}

// Draw filled rounded rectangle
void fb_fill_rounded_rect(Framebuffer *fb, int x, int y, int w, int h, int r, uint32_t color) {
    if (r < 0) r = 0;
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    // Central rectangle
    fb_fill_rect(fb, x + r, y, w - 2 * r, h, color);
    // Left and right rectangles
    fb_fill_rect(fb, x, y + r, r, h - 2 * r, color);
    fb_fill_rect(fb, x + w - r, y + r, r, h - 2 * r, color);
    // Four corner circles
    fb_fill_circle(fb, x + r,     y + r,     r, color);
    fb_fill_circle(fb, x + w - r - 1, y + r,     r, color);
    fb_fill_circle(fb, x + r,     y + h - r - 1, r, color);
    fb_fill_circle(fb, x + w - r - 1, y + h - r - 1, r, color);
}

// Draw rounded rectangle outline
void fb_draw_rounded_rect(Framebuffer *fb, int x, int y, int w, int h, int r, uint32_t color) {
    x += fb->draw_offset_x;
    y += fb->draw_offset_y;
    if (r < 0) r = 0;
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    // Top and bottom edges
    for (int i = r; i < w - r; i++) {
        fb_draw_pixel(fb, x + i, y, color);
        fb_draw_pixel(fb, x + i, y + h - 1, color);
    }
    // Left and right edges
    for (int i = r; i < h - r; i++) {
        fb_draw_pixel(fb, x, y + i, color);
        fb_draw_pixel(fb, x + w - 1, y + i, color);
    }
    // Four corner arcs (Bresenham circle)
    int cx, cy, err;
    cx = r; cy = 0; err = 0;
    while (cx >= cy) {
        fb_draw_pixel(fb, x + r - cx,     y + r - cy,     color); // TL
        fb_draw_pixel(fb, x + r - cy,     y + r - cx,     color);
        fb_draw_pixel(fb, x + w-1-r + cx, y + r - cy,     color); // TR
        fb_draw_pixel(fb, x + w-1-r + cy, y + r - cx,     color);
        fb_draw_pixel(fb, x + r - cx,     y + h-1-r + cy, color); // BL
        fb_draw_pixel(fb, x + r - cy,     y + h-1-r + cx, color);
        fb_draw_pixel(fb, x + w-1-r + cx, y + h-1-r + cy, color); // BR
        fb_draw_pixel(fb, x + w-1-r + cy, y + h-1-r + cx, color);
        if (err <= 0) { cy++; err += 2*cy + 1; }
        if (err > 0)  { cx--; err -= 2*cx + 1; }
    }
}

void fb_draw_text(Framebuffer *fb, int x, int y, const char *text, uint32_t color, int scale) {
    x += fb->draw_offset_x;
    y += fb->draw_offset_y;
    int offset_x = 0;
    
    /* UTF-8: one glyph per code point.  Controls (including '\n') and DEL
     * advance a blank cell; every other unmapped code point, and every
     * malformed byte, draws the fallback box (font.h). */
    while (*text) {
        uint32_t cp = utf8_next(&text);

        if (cp < 0x20 || cp == 0x7F) {
            offset_x += FONT_ADVANCE * scale;
            continue;
        }

        const uint16_t *glyph = font_glyph(cp);
        for (int col = 0; col < FONT_COLS; col++) {
            uint16_t column = glyph[col];
            if (!column) continue;
            for (int row = 0; row < FONT_ROWS; row++) {
                if (column & (1u << row)) {
                    for (int sy = 0; sy < scale; sy++) {
                        for (int sx = 0; sx < scale; sx++) {
                            fb_draw_pixel(fb, x + offset_x + col * scale + sx,
                                        y + row * scale + sy, color);
                        }
                    }
                }
            }
        }

        offset_x += FONT_ADVANCE * scale;
    }
}

/* -- Line drawing (Bresenham) ------------------------------------------- */
void fb_draw_line(Framebuffer *fb, int x0, int y0, int x1, int y1, uint32_t color) {
    x0 += fb->draw_offset_x; y0 += fb->draw_offset_y;
    x1 += fb->draw_offset_x; y1 += fb->draw_offset_y;
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        fb_draw_pixel(fb, x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

/* -- Thick line --------------------------------------------------------- */
void fb_draw_thick_line(Framebuffer *fb, int x0, int y0, int x1, int y1,
                        int thickness, uint32_t color) {
    int half = thickness / 2;
    for (int d = -half; d <= half; d++) {
        int dx = abs(x1 - x0), dy = abs(y1 - y0);
        if (dx >= dy) {
            fb_draw_line(fb, x0, y0 + d, x1, y1 + d, color);
        } else {
            fb_draw_line(fb, x0 + d, y0, x1 + d, y1, color);
        }
    }
}

/* -- Alpha-blended pixel ------------------------------------------------ */
void fb_draw_pixel_alpha(Framebuffer *fb, int x, int y, uint32_t color, uint8_t alpha) {
    x += fb->draw_offset_x;
    y += fb->draw_offset_y;
    if (x < 0 || x >= (int)fb->width || y < 0 || y >= (int)fb->height) return;
    void *target = fb_target(fb);
    bool is16 = FB_IS_16BPP(fb);
    size_t idx = (size_t)y * fb->width + x;
    /* Read-modify-write, so the destination has to be UNPACKED first: at 16bpp
     * the stored word is RGB565 and blending it as if it were RGB888 produces a
     * colour unrelated to what is on screen. */
    uint32_t dst = fb_load(target, idx, is16);
    uint32_t sr = (color >> 16) & 0xFF, sg = (color >> 8) & 0xFF, sb = color & 0xFF;
    uint32_t dr = (dst   >> 16) & 0xFF, dg = (dst   >> 8) & 0xFF, db = dst   & 0xFF;
    uint32_t a = alpha, ia = 255 - alpha;
    uint32_t r = (sr * a + dr * ia) / 255;
    uint32_t g = (sg * a + dg * ia) / 255;
    uint32_t b = (sb * a + db * ia) / 255;
    fb_store(target, idx, (r << 16) | (g << 8) | b, is16);
}

/* One channel-triple of fb_draw_pixel_alpha, with the source terms already
 * multiplied by alpha (sa_* = s * a) and ia = 255 - a.  Same expression and
 * same /255 as the per-pixel path, so the output is identical. */
static inline uint32_t fb_blend_px(uint32_t dst, uint32_t sar, uint32_t sag,
                                   uint32_t sab, uint32_t ia) {
    uint32_t dr = (dst >> 16) & 0xFF, dg = (dst >> 8) & 0xFF, db = dst & 0xFF;
    uint32_t r = (sar + dr * ia) / 255;
    uint32_t g = (sag + dg * ia) / 255;
    uint32_t b = (sab + db * ia) / 255;
    return (r << 16) | (g << 8) | b;
}

/* Clip once, then blend row by row.  Per pixel this does the same work as
 * fb_draw_pixel_alpha (unpack, blend, repack) with no call, bounds check or
 * target lookup in the loop. */
void fb_fill_rect_alpha(Framebuffer *fb, int x, int y, int w, int h,
                        uint32_t color, uint8_t alpha) {
    if (w <= 0 || h <= 0) return;
    x += fb->draw_offset_x;
    y += fb->draw_offset_y;
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w, y1 = y + h;
    if (x1 > (int)fb->width)  x1 = (int)fb->width;
    if (y1 > (int)fb->height) y1 = (int)fb->height;
    if (x0 >= x1 || y0 >= y1) return;

    void *target = fb_target(fb);
    const uint32_t a = alpha, ia = 255 - alpha;
    const uint32_t sar = ((color >> 16) & 0xFF) * a;
    const uint32_t sag = ((color >>  8) & 0xFF) * a;
    const uint32_t sab = (color & 0xFF) * a;
    const size_t stride = fb->width;

    if (FB_IS_16BPP(fb)) {
        for (int py = y0; py < y1; py++) {
            uint16_t *row = (uint16_t *)target + (size_t)py * stride;
            for (int px = x0; px < x1; px++)
                row[px] = fb_pack565(fb_blend_px(fb_unpack565(row[px]), sar, sag, sab, ia));
        }
    } else {
        for (int py = y0; py < y1; py++) {
            uint32_t *row = (uint32_t *)target + (size_t)py * stride;
            for (int px = x0; px < x1; px++)
                row[px] = fb_blend_px(row[px] & 0x00FFFFFF, sar, sag, sab, ia);
        }
    }
}

/* -- Vertical gradient filled rect -------------------------------------- */
void fb_fill_rect_gradient(Framebuffer *fb, int x, int y, int w, int h,
                           uint32_t top_color, uint32_t bottom_color) {
    x += fb->draw_offset_x;
    y += fb->draw_offset_y;
    /* The channel deltas MUST be signed. A descending gradient (bottom channel
     * darker than top) makes bottom < top, and an unsigned subtraction wraps to
     * ~4.29e9 instead of going negative. The division that follows destroys the
     * modular-arithmetic equivalence that would otherwise have rescued it, so
     * every row got a garbage colour rather than a ramp. */
    int tr = (int)((top_color >> 16) & 0xFF);
    int tg = (int)((top_color >>  8) & 0xFF);
    int tb = (int)( top_color        & 0xFF);
    int dr = (int)((bottom_color >> 16) & 0xFF) - tr;
    int dg = (int)((bottom_color >>  8) & 0xFF) - tg;
    int db = (int)( bottom_color        & 0xFF) - tb;
    int span = (h > 1) ? h - 1 : 1;
    for (int j = 0; j < h; j++) {
        /* j <= span, so |d*j/span| <= |d| and each channel stays between its
         * two endpoints — both already masked to 0..255. No clamp needed. */
        uint32_t r = (uint32_t)(tr + dr * j / span);
        uint32_t g = (uint32_t)(tg + dg * j / span);
        uint32_t b = (uint32_t)(tb + db * j / span);
        uint32_t c = (r << 16) | (g << 8) | b;
        for (int i = 0; i < w; i++)
            fb_draw_pixel(fb, x + i, y + j, c);
    }
}

void fb_fade_out(Framebuffer *fb) {
    // Fade out backlight smoothly, then clear screen
    #include "../common/hardware.h"
    
    for (int i = 100; i >= 0; i -= 5) {
        hw_set_backlight(i);
        usleep(30000);  // 30ms per step = 600ms total
    }
    
    // Clear screen to black
    fb_clear(fb, COLOR_BLACK);
    fb_swap(fb);
    
    // Restore backlight for next app
    hw_set_backlight(100);
}

void fb_fade_in(Framebuffer *fb) {
    // Fade in backlight smoothly from black
    #include "../common/hardware.h"
    
    // Start with backlight off
    hw_set_backlight(0);
    
    // Fade in
    for (int i = 0; i <= 100; i += 5) {
        hw_set_backlight(i);
        usleep(30000);  // 30ms per step = 600ms total
    }
}

/* ======================================================================
 * Sprite Blitting Functions
 *
 * The SOURCE is always uint32_t RGB888 — that is what ppm.c produces and what
 * every sprite table in the games is declared as — while the DESTINATION is the
 * surface's own format. So the colour-key comparison happens in source space
 * (two distinct RGB888 colours can collapse onto one RGB565 word, and a key
 * compared after packing would make an opaque pixel transparent), and only the
 * store converts. `is16` is hoisted out of the loops.
 * ====================================================================== */

void fb_blit_sprite(Framebuffer *fb, const uint32_t *src_pixels, int src_w,
                    int sx, int sy, int dx, int dy, int w, int h,
                    uint32_t color_key) {
    int off_x = fb->draw_offset_x;
    int off_y = fb->draw_offset_y;
    int fb_w  = (int)fb->width;
    int fb_h  = (int)fb->height;
    void *target = fb_target(fb);
    const bool is16 = FB_IS_16BPP(fb);

    for (int row = 0; row < h; row++) {
        int dest_y = dy + off_y + row;
        if (dest_y < 0) continue;
        if (dest_y >= fb_h) break;

        int src_row_offset = (sy + row) * src_w + sx;

        for (int col = 0; col < w; col++) {
            int dest_x = dx + off_x + col;
            if (dest_x < 0) continue;
            if (dest_x >= fb_w) break;

            uint32_t pixel = src_pixels[src_row_offset + col];
            if (pixel == color_key) continue;

            fb_store(target, (size_t)dest_y * fb_w + dest_x, pixel, is16);
        }
    }
}

void fb_blit_sprite_flipped(Framebuffer *fb, const uint32_t *src_pixels, int src_w,
                            int sx, int sy, int dx, int dy, int w, int h,
                            uint32_t color_key) {
    int off_x = fb->draw_offset_x;
    int off_y = fb->draw_offset_y;
    int fb_w  = (int)fb->width;
    int fb_h  = (int)fb->height;
    void *target = fb_target(fb);
    const bool is16 = FB_IS_16BPP(fb);

    for (int row = 0; row < h; row++) {
        int dest_y = dy + off_y + row;
        if (dest_y < 0) continue;
        if (dest_y >= fb_h) break;

        int src_row_offset = (sy + row) * src_w;

        for (int col = 0; col < w; col++) {
            int dest_x = dx + off_x + col;
            if (dest_x < 0) continue;
            if (dest_x >= fb_w) break;

            /* Read source pixels right-to-left (horizontal flip) */
            uint32_t pixel = src_pixels[src_row_offset + (sx + w - 1 - col)];
            if (pixel == color_key) continue;

            fb_store(target, (size_t)dest_y * fb_w + dest_x, pixel, is16);
        }
    }
}

void fb_blit_sprite_scaled(Framebuffer *fb, const uint32_t *src_pixels, int src_w,
                           int sx, int sy, int sw, int sh,
                           int dx, int dy, int dw, int dh,
                           uint32_t color_key) {
    if (dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) return;

    int off_x = fb->draw_offset_x;
    int off_y = fb->draw_offset_y;
    int fb_w  = (int)fb->width;
    int fb_h  = (int)fb->height;
    void *target = fb_target(fb);
    const bool is16 = FB_IS_16BPP(fb);

    for (int j = 0; j < dh; j++) {
        int dest_y = dy + off_y + j;
        if (dest_y < 0) continue;
        if (dest_y >= fb_h) break;

        /* Nearest-neighbor: map dest row to source row */
        int src_y = sy + (j * sh) / dh;

        for (int i = 0; i < dw; i++) {
            int dest_x = dx + off_x + i;
            if (dest_x < 0) continue;
            if (dest_x >= fb_w) break;

            /* Nearest-neighbor: map dest col to source col */
            int src_x = sx + (i * sw) / dw;

            uint32_t pixel = src_pixels[src_y * src_w + src_x];
            if (pixel == color_key) continue;

            fb_store(target, (size_t)dest_y * fb_w + dest_x, pixel, is16);
        }
    }
}
