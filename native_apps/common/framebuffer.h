#ifndef FRAMEBUFFER_H
#define FRAMEBUFFER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>  // For size_t

typedef struct {
    int fd;
    // Pixel storage. Typed uint32_t for history, but the pixel SIZE is
    // bytes_per_pixel: 4 = XRGB8888, 2 = RGB565. The drawing primitives pack and
    // unpack accordingly — never index these as uint32_t from outside
    // framebuffer.c without checking bytes_per_pixel first.
    uint32_t *buffer;        // Front buffer (mapped to screen), PANEL sized
    uint32_t *back_buffer;   // Back buffer (for double buffering), LOGICAL sized
    uint32_t width;          // Logical (visible) width  — what apps draw into
    uint32_t height;         // Logical (visible) height — what apps draw into
    uint32_t line_length;
    uint32_t bytes_per_pixel;
    size_t screen_size;
    bool double_buffering;   // Enable/disable double buffering
    int draw_offset_x;      // Draw offset X (for screen shake etc.)
    int draw_offset_y;      // Draw offset Y (for screen shake etc.)
    bool portrait_mode;      // Portrait mode active (90° rotation in fb_swap)
    uint32_t phys_width;     // Physical framebuffer width (from hardware)
    uint32_t phys_height;    // Physical framebuffer height (from hardware)
    size_t back_buffer_size; // Back buffer size (logical dims × bpp)
    int view_x;              // Logical surface origin within the panel (bezel left)
    int view_y;              // Logical surface origin within the panel (bezel top)
} Framebuffer;

// ---------------------------------------------------------------------------
// Bezel viewport
//
// The plastic bezel hides a band of pixels at the panel edges. Apps never see
// those pixels: fb_init() shrinks the drawing surface to the visible rectangle
// and fb_swap() places it on the panel at (view_x, view_y), leaving the hidden
// bands black. fb->width / fb->height are therefore the LOGICAL (visible) size,
// and every drawing primitive works in logical coordinates.
//
// touch_input.c consumes the same globals to translate a panel touch back into
// logical space, so drawing and touch always share one coordinate system.
// ---------------------------------------------------------------------------

// Bezel margins used when the config file has no margin line
#define FB_BEZEL_TOP_DEFAULT    15
#define FB_BEZEL_BOTTOM_DEFAULT 15
#define FB_BEZEL_LEFT_DEFAULT    0
#define FB_BEZEL_RIGHT_DEFAULT   0

// Runtime bezel margins in pixels (loaded from /etc/touch_calibration.conf).
// Stored in the app's orientation: fb_init() rotates them in portrait mode.
extern int screen_bezel_top;
extern int screen_bezel_bottom;
extern int screen_bezel_left;
extern int screen_bezel_right;

// The SURFACE dimensions in the app's orientation — the space fb->buffer is
// indexed in, and the space touch_input.c maps raw readings onto. 800x480 on
// fb0, swapped in portrait, and 400x240 on a 400x240 fb1 that a scaling overlay
// upscales to fill the panel. ⚠️ Despite the name they are NOT the panel's own
// resolution on a scaled node: that is read from the display's mode timings
// inside fb_apply_viewport() and deliberately not published, because every
// reader of these two needs the surface. Naming them "panel" has already misled
// one reading of the touch chain into believing it needed a scale divide.
extern int screen_panel_width;
extern int screen_panel_height;

// Convert bezel margins from PANEL pixels into SURFACE pixels, in place. Pure —
// no sysfs, no device — so a host test can reach it; fb_init() calls it with the
// panel size read from the display's mode timings.
//
// It exists because the margins name pixels the plastic bezel physically covers,
// and a scaling DSS overlay draws a smaller surface upscaled to fill the panel:
// subtracting a panel-space margin from that surface spends it at the upscale
// factor. Measured at 400x240 upscaled 2x, T=15 took 30 of the panel's 480 rows.
// A panel dimension of 0, or one equal to the surface, is left 1:1 — so this is
// a no-op at the shipped 800x480 geometry.
//
// It converts DRAWING margins. Touch needs no conversion of its own and must not
// be given one: stage 1 maps raw onto screen_panel_width/height, which are the
// SURFACE dims above, and its knots are dim/4 and 3*dim/4, so the whole curve
// tracks the surface; stage 2 then subtracts a view origin this function has
// already converted. The map is proportional by construction. ⚠️ An earlier note
// here asserted the opposite — that touch on a scaled node is wrong by the scale
// factor and needs dividing — and that is measured false: applying that divide
// costs 12 failures in native_apps/tests/touch_map_test.c group J, which drives
// one full-size calibration over 800x480, 400x240 and 200x120 surfaces.
void fb_scale_bezel_to_surface(int panel_w, int panel_h, int surf_w, int surf_h,
                               int *top, int *bottom, int *left, int *right);

// ---------------------------------------------------------------------------
// UI pixel scaling: design pixels -> surface pixels
//
// A control that must keep its PHYSICAL size — a finger-sized button, a HUD band,
// a dialog — is a fixed fraction of the panel, and a scaling DSS overlay draws a
// SMALLER surface upscaled to fill that panel. A constant left in surface pixels
// therefore GROWS on the panel by the upscale factor: BTN_MENU_WIDTH 70 on a
// 400x240 surface covers 140 of the panel's 800 columns, twice its design size,
// and the shared 220x60 game-over stack covers 88% of the rows instead of 44%.
//
// The conversion is the surface:panel ratio. ⚠️ NOT a ratio against a fixed
// 800x480 reference, which would shrink every control in portrait, where the
// surface is 480x800 at FULL resolution and must not be scaled at all. ⚠️ And NOT
// against the LOGICAL dims, which the bezel has already shrunk — that is the
// fixed-divisor trap that made a drawing tool put its own label at scale 1
// instead of 2, having derived the scale from a height the bezel cut 240 -> 227.
//
// fb_scale_ui_px() is the pure half, so a host test can reach it
// (native_apps/tests/ui_scale_test.c); fb_ui_px_x() / fb_ui_px_y() are the thin
// wrappers over the published geometry. Bit-exact no-ops whenever the surface
// equals the panel, which is every shipped unit in both orientations today, so
// this changes nothing on fb0 and the host test's group 1 asserts exactly that.
//
// ⚠️ It scales GEOMETRY only. Text is an integer glyph multiplier with no rung
// below 1, so a scale-3 label cannot halve; see the note on button_init in
// common.h for what that costs and why it is not papered over here.
int fb_scale_ui_px(int panel_dim, int surf_dim, int px);
int fb_ui_px_x(int px);
int fb_ui_px_y(int px);

// The panel's OWN resolution in the app's orientation, from the display's mode
// timings — 800x480 on every unit today, swapped in portrait. Published solely so
// the UI scale above can be computed, and deliberately a SEPARATE pair from
// screen_panel_width/height, whose name says panel and whose value is the
// surface: changing what those two mean would silently move touch. Falls back to
// the surface when the timings cannot be read, which makes the scale a 1:1 no-op.
extern int screen_true_panel_width;
extern int screen_true_panel_height;

// Logical surface origin within the panel (== bezel left/top after rotation)
extern int screen_view_x;
extern int screen_view_y;

// Logical (visible) screen dimensions — set by fb_init(), default 800x480
extern int screen_base_width;
extern int screen_base_height;

// ---------------------------------------------------------------------------
// Two rectangles, not one: visible and touch-safe
//
// The whole logical screen is DRAWABLE. It is not all PRESSABLE. The digitiser
// saturates before the physical panel edge — measured on RW09, raw pins at 4095
// from panel row ~450 of 480 and at 0 from row ~30 — so a band at each end of Y
// is visible but cannot be addressed by a finger. X reaches both edges. How wide
// the band is depends on the panel and the calibration, so it is MEASURED at
// runtime, never assumed: touch_input.c maps the raw hardware corners through the
// live map and calls fb_set_touch_inset(). An uncalibrated device gets zero.
//
//   SCREEN_VISIBLE_*  the full logical screen. Backgrounds, borders, titles,
//                     status and score rows, game playfields — anything the user
//                     only has to SEE. The band is good screen area; use it.
//   SCREEN_SAFE_*     visible AND touchable. Buttons, toggles, tab bars, touch
//                     grids — anything the user has to PRESS.
//
// Drawing primitives are unchanged: they take logical coordinates and fb->width /
// fb->height remain the full logical size.
//
// SCREEN_SAFE_* is only correct after touch_init() (or a later
// touch_set_screen_size() / touch_set_raw_curve()), so compute layout after both
// fb_init() and touch_init() — which is the documented app lifecycle anyway. The
// macros read the globals at each use, so a layout recomputed after
// fb_set_bezel() picks up the new values.
// ---------------------------------------------------------------------------

// Logical pixels at each edge that are visible but not touchable. Set by
// touch_input.c via fb_set_touch_inset(); zero until then, so an app that never
// initialises touch sees the whole logical screen as safe.
extern int screen_touch_inset_top;
extern int screen_touch_inset_bottom;
extern int screen_touch_inset_left;
extern int screen_touch_inset_right;

// Largest inset that will be believed, in DESIGN pixels — ~10% of the short axis.
// A calibration that puts more than this out of reach is broken, and silently
// shrinking every UI to match it hides the fault where a loud warning finds it.
// fb_set_touch_inset() converts it with fb_ui_px_*(), because it is a fraction of
// the panel: flat, it would be 20% of a 240-row surface instead of 10%.
#define FB_TOUCH_INSET_MAX 48

// Publish the measured inset. Values are clamped to [0, converted
// FB_TOUCH_INSET_MAX] and a clamp is reported on stdout — see the note above.
void fb_set_touch_inset(int top, int bottom, int left, int right);

#define SCREEN_VISIBLE_LEFT   0
#define SCREEN_VISIBLE_TOP    0
#define SCREEN_VISIBLE_RIGHT  (screen_base_width)
#define SCREEN_VISIBLE_BOTTOM (screen_base_height)
#define SCREEN_VISIBLE_WIDTH  (SCREEN_VISIBLE_RIGHT - SCREEN_VISIBLE_LEFT)
#define SCREEN_VISIBLE_HEIGHT (SCREEN_VISIBLE_BOTTOM - SCREEN_VISIBLE_TOP)

#define SCREEN_SAFE_LEFT   (screen_touch_inset_left)
#define SCREEN_SAFE_TOP    (screen_touch_inset_top)
#define SCREEN_SAFE_RIGHT  (screen_base_width  - screen_touch_inset_right)
#define SCREEN_SAFE_BOTTOM (screen_base_height - screen_touch_inset_bottom)
#define SCREEN_SAFE_WIDTH  (SCREEN_SAFE_RIGHT - SCREEN_SAFE_LEFT)
#define SCREEN_SAFE_HEIGHT (SCREEN_SAFE_BOTTOM - SCREEN_SAFE_TOP)

// Load bezel margins from the calibration config file.
// Called automatically by fb_init().
void fb_load_bezel(void);

// Check if portrait mode is enabled (flag file /opt/games/portrait.mode exists)
bool fb_is_portrait_mode(void);

// Initialize framebuffer
int fb_init(Framebuffer *fb, const char *device);

// Close framebuffer
void fb_close(Framebuffer *fb);

// Swap buffers (present back buffer to screen)
void fb_swap(Framebuffer *fb);

// Present only the logical rectangle (x, y, w, h) of the back buffer, clipped
// to the surface — fb_swap()'s placement and rotation, for a screen whose only
// change is one small widget (the Input testers' hold-to-exit bar), so it need
// not pay for a full-frame copy.  Byte-identical to fb_swap() inside the
// rectangle, no store outside it (tests/fb_rotate_test.c) — except the
// over-temperature square below, which both presents end in.
void fb_swap_rect(Framebuffer *fb, int x, int y, int w, int h);

// The over-temperature square (common/overtemp.h): a 16x16 red square at the
// visible area's top-right, painted on the front buffer while arcamond's ring
// carries MON_FLAG_OVERTEMP; the ring is read at most once a second.  fb_swap()
// and fb_swap_rect() call it; a loop that can idle without presenting (the
// launcher, control_panel) calls it once per iteration so the square still
// comes and goes.  No-op without double buffering.
void fb_overtemp_poll(Framebuffer *fb);

// Clear screen with color
void fb_clear(Framebuffer *fb, uint32_t color);

// Set draw offset — applied automatically to all drawing primitives
void fb_set_draw_offset(Framebuffer *fb, int dx, int dy);

// Clear draw offset (reset to 0,0)
void fb_clear_draw_offset(Framebuffer *fb);

// ---------------------------------------------------------------------------
// Drawing primitives
//
// All of them take logical coordinates and a 24-bit RGB888 colour (the RGB() /
// COLOR_* macros) and work at either supported depth — they pack to the
// surface's pixel format internally. An app never needs to know the bpp, and
// must not assume 32: /dev/fb0 keeps whatever the previous process set.
// ---------------------------------------------------------------------------

// Draw pixel
void fb_draw_pixel(Framebuffer *fb, int x, int y, uint32_t color);

// Draw rectangle
void fb_draw_rect(Framebuffer *fb, int x, int y, int w, int h, uint32_t color);

// Draw filled rectangle
void fb_fill_rect(Framebuffer *fb, int x, int y, int w, int h, uint32_t color);

// Draw circle
void fb_draw_circle(Framebuffer *fb, int cx, int cy, int radius, uint32_t color);

// Draw filled circle
void fb_fill_circle(Framebuffer *fb, int cx, int cy, int radius, uint32_t color);

// Draw text (simple bitmap font — lowercase mapped to uppercase automatically)
void fb_draw_text(Framebuffer *fb, int x, int y, const char *text, uint32_t color, int scale);

// Draw filled rounded rectangle (r = corner radius)
void fb_fill_rounded_rect(Framebuffer *fb, int x, int y, int w, int h, int r, uint32_t color);

// Draw rounded rectangle outline (r = corner radius)
void fb_draw_rounded_rect(Framebuffer *fb, int x, int y, int w, int h, int r, uint32_t color);

// Draw a line between two points (Bresenham's algorithm)
void fb_draw_line(Framebuffer *fb, int x0, int y0, int x1, int y1, uint32_t color);

// Draw a filled rectangle with a vertical gradient (top_color → bottom_color)
void fb_fill_rect_gradient(Framebuffer *fb, int x, int y, int w, int h,
                           uint32_t top_color, uint32_t bottom_color);

// Draw a pixel with alpha blending (alpha 0-255, 255 = opaque)
void fb_draw_pixel_alpha(Framebuffer *fb, int x, int y, uint32_t color, uint8_t alpha);

// Draw a filled rectangle with alpha blending
void fb_fill_rect_alpha(Framebuffer *fb, int x, int y, int w, int h,
                        uint32_t color, uint8_t alpha);

// Draw a thick line (thickness in pixels)
void fb_draw_thick_line(Framebuffer *fb, int x0, int y0, int x1, int y1,
                        int thickness, uint32_t color);

// Screen transition effects
void fb_fade_out(Framebuffer *fb);
void fb_fade_in(Framebuffer *fb);

// RGB color helper
#define RGB(r, g, b) (((r) << 16) | ((g) << 8) | (b))

// Set framebuffer bits-per-pixel (16 or 32). Must be called BEFORE fb_init.
//
// EVERY app should pin this rather than inherit it. /dev/fb0 keeps whatever the
// last process set, so a game launched over SSH after ScummVM or vnc_client
// starts at 16bpp. That used to be a 2x heap overflow in the drawing
// primitives; they are bpp-aware now, so what is left is appearance and
// determinism — 16bpp bands every gradient, and "how does this app look"
// should not depend on which app ran before it.
//
// Native UIs and games want 32. ScummVM and the VNC session set 16 on purpose,
// to halve write bandwidth on this memory-bound part, and fb_init() must keep
// accepting that.
int fb_set_bpp(const char *device, int bpp);

// Change the bezel margins on a live framebuffer: resizes the logical surface,
// republishes the globals and re-blacks the panel. Returns 0 on success, -1 if
// the margins leave no usable area (in which case fb is left unchanged).
// Callers that also own a TouchInput must follow up with touch_set_viewport().
int fb_set_bezel(Framebuffer *fb, int top, int bottom, int left, int right);

// Common colors
#define COLOR_BLACK   RGB(0, 0, 0)
#define COLOR_WHITE   RGB(255, 255, 255)
#define COLOR_RED     RGB(255, 0, 0)
#define COLOR_GREEN   RGB(0, 255, 0)
#define COLOR_BLUE    RGB(0, 0, 255)
#define COLOR_YELLOW  RGB(255, 255, 0)
#define COLOR_CYAN    RGB(0, 255, 255)
#define COLOR_MAGENTA RGB(255, 0, 255)
#define COLOR_ORANGE  RGB(255, 165, 0)
#define COLOR_PURPLE  RGB(128, 0, 128)
#define COLOR_GRAY    RGB(128, 128, 128)

// Sprite transparency color key (magenta)
#define SPRITE_TRANSPARENT RGB(255, 0, 255)

// Blit a rectangular region from a source pixel buffer to the framebuffer
// src_pixels: source pixel array (uint32_t ARGB), src_w: total source image width
// sx, sy: source rectangle top-left in source image
// dx, dy: destination position on framebuffer
// w, h: dimensions of the rectangle to copy
// color_key: transparent color (pixels matching this are skipped), use 0xFFFFFFFF to disable
void fb_blit_sprite(Framebuffer *fb, const uint32_t *src_pixels, int src_w,
                    int sx, int sy, int dx, int dy, int w, int h,
                    uint32_t color_key);

// Same as fb_blit_sprite but horizontally flipped
void fb_blit_sprite_flipped(Framebuffer *fb, const uint32_t *src_pixels, int src_w,
                            int sx, int sy, int dx, int dy, int w, int h,
                            uint32_t color_key);

// Blit with scaling (nearest-neighbor) — for zoom effects or different tile sizes
void fb_blit_sprite_scaled(Framebuffer *fb, const uint32_t *src_pixels, int src_w,
                           int sx, int sy, int sw, int sh,
                           int dx, int dy, int dw, int dh,
                           uint32_t color_key);

#endif
