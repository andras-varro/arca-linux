#ifndef VNC_RENDERER_H
#define VNC_RENDERER_H

/*
 * VNC Renderer - Phase 2 Optimized + Bilinear Interpolation
 *
 * Performance optimizations adopted from ScummVM Arca backend:
 *   O2:  Precomputed X-coordinate LUT (eliminates per-pixel division)
 *   O3:  Border-only clearing (not full screen every frame)
 *   O8:  16bpp RGB565 framebuffer (halves memory bandwidth)
 *   O11: Row deduplication via cached temp row (skips ~57% of rows)
 *
 * Bilinear interpolation (new):
 *   - 2×2 source pixel sampling with fixed-point weighted averaging
 *   - Precomputed X LUT stores (x0, x1, frac_x) per dest column
 *   - Dramatically improves text readability at 1920×1080 → 795×447
 *   - ~10-15% CPU overhead (acceptable with 5% baseline)
 *
 * Key changes from v1.0:
 *   - Direct uint16_t buffer writes (bypasses fb_draw_pixel entirely)
 *   - Partial region updates (only re-render dirty VNC rectangles)
 *   - No full-frame render on every VNC callback
 *   - Frame rate capped at TARGET_FPS with gettimeofday
 */

#include <stdint.h>
#include <stdbool.h>
#include <sys/time.h>
#include "../native_apps/common/framebuffer.h"
#include "config.h"

/* RGB565 helpers */
#define RGB565(r, g, b) \
    (((uint16_t)((r) >> 3) << 11) | ((uint16_t)((g) >> 2) << 5) | ((uint16_t)((b) >> 3)))
#define RGB565_BLACK  0x0000
#define RGB565_WHITE  0xFFFF
#define RGB565_RED    0xF800
#define RGB565_GREEN  0x07E0
#define RGB565_BLUE   0x001F
#define RGB565_YELLOW 0xFFE0
#define RGB565_GREY   0x8410

/* Precomputed bilinear X-coordinate entry */
typedef struct {
    int x0;         /* left source pixel index */
    int x1;         /* right source pixel index (clamped) */
    uint8_t frac;   /* fractional part 0-255 (0 = fully x0, 255 = fully x1) */
} BilinearXEntry;

typedef struct VNCRenderer {
    Framebuffer *fb;

    /* Remote display dimensions */
    int remote_width;
    int remote_height;

    /* Scaling parameters (integer arithmetic, no floats in hot path) */
    ScalingMode scaling_mode;
    int offset_x;
    int offset_y;
    int scaled_width;
    int scaled_height;

    /* Precomputed bilinear X-coordinate lookup table (O2 + bilinear).
     * Sized for the widest possible panel; only fb->width entries are used. */
    BilinearXEntry src_x_lut[PANEL_MAX_WIDTH];

    /* Row deduplication temp buffer - stays L1-cache hot (O11) */
    uint16_t temp_row[PANEL_MAX_WIDTH];

    /* State tracking */
    bool needs_present;
    bool borders_cleared;

    /* Performance counters */
    uint32_t frame_count;
    uint32_t update_count;
    struct timeval last_fps_time;
    float current_fps;

    /* Frame rate cap */
    struct timeval last_present_time;
} VNCRenderer;

/* ── The content rectangle ─────────────────────────────────────────────
 * The remote desktop is letterboxed into the TOUCH-SAFE rectangle, not the
 * whole visible surface: we cannot audit a third-party desktop for which of
 * its pixels have to be pressable, so the only safe assumption is all of
 * them.  A remote taskbar on the bottom row must be clickable.
 *
 * `content_area = visible` in the config opts out and gives the picture the
 * whole surface back, at the cost of a band at each end that a finger cannot
 * reach.  The opt-out moves ONLY the picture — this component's own touch UI
 * (settings screen, reconnect row, exit gesture) always uses SCREEN_SAFE_*
 * and ignores it, because stranding our own SAVE button is not a trade the
 * user should be able to make by accident.
 *
 * SCREEN_SAFE_* is zero-inset until a panel's edge reach has been swept, so
 * on an uncalibrated device both settings produce identical output.  It is
 * only correct after BOTH fb_init() and touch_init(). */
void vnc_content_set_full(bool full);

/* The rectangle the picture is letterboxed into, in logical coordinates. */
void vnc_content_rect(const Framebuffer *fb, int *x, int *y, int *w, int *h);

/* Initialize renderer */
int vnc_renderer_init(VNCRenderer *renderer, Framebuffer *fb);

/* Set remote display size and calculate scaling parameters.
 * Also clears letterbox borders once. */
void vnc_renderer_set_remote_size(VNCRenderer *renderer, int width, int height);

/* Render a dirty region from the VNC framebuffer into the local back buffer.
 * Only converts and scales the pixels in (rx, ry, rw, rh).
 * remote_fb: the complete VNC client framebuffer (client->frameBuffer) */
void vnc_renderer_update_region(VNCRenderer *renderer, const uint8_t *remote_fb,
                                int rx, int ry, int rw, int rh, int bytes_per_pixel);

/* Present the back buffer to the screen (fb_swap) with frame rate cap.
 * Returns true if a frame was actually swapped. */
bool vnc_renderer_present(VNCRenderer *renderer);

/* Map screen (touch) coordinates → remote VNC coordinates.
 * Returns false if the point falls in the letterbox border. */
bool vnc_renderer_screen_to_remote(VNCRenderer *renderer, int screen_x, int screen_y,
                                   int *remote_x, int *remote_y);

/* 16bpp utility: clear entire back buffer to black */
void vnc_renderer_clear_screen(Framebuffer *fb);

/* 16bpp utility: draw text using built-in 5×7 bitmap font */
void vnc_renderer_draw_text(Framebuffer *fb, int x, int y, const char *text,
                            uint16_t color, int scale);

/* 16bpp utility: draw a filled rectangle */
void vnc_renderer_fill_rect(Framebuffer *fb, int x, int y, int w, int h,
                            uint16_t color);

/* 16bpp utility: measure text width in pixels for a given scale */
int vnc_renderer_text_width(const char *text, int scale);

/* Cleanup renderer (restores 32bpp on exit) */
void vnc_renderer_cleanup(VNCRenderer *renderer);

#endif /* VNC_RENDERER_H */
