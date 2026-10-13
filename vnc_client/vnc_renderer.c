/*
 * VNC Renderer - Phase 2 Optimized + Bilinear Interpolation
 *
 * Renders VNC framebuffer updates to the Arca's visible display area.
 * All hot-path rendering uses direct uint16_t writes to the back buffer
 * in RGB565 format — no fb_draw_pixel() calls, no per-pixel bounds checks.
 *
 * SCREEN SIZE IS RUNTIME.  fb_init() sizes the drawing surface to the rectangle
 * the bezel leaves visible, so the back buffer is fb->width x fb->height (e.g.
 * 800x450 at the shipped margins), not the panel size.  Every stride, bound and
 * letterbox calculation below reads fb->width / fb->height; PANEL_MAX_* is only
 * the capacity of the fixed LUT arrays.
 *
 * THE PICTURE GOES IN THE TOUCH-SAFE RECTANGLE, not the whole surface — see
 * vnc_content_rect() in the header.  fb->width / fb->height remain the stride
 * and the bounds of the back buffer; they are no longer the letterbox target.
 *
 * Key optimizations (see header for details):
 *   - 16bpp RGB565 output (halved memory bandwidth)
 *   - Precomputed X-coord bilinear LUT (no per-pixel division)
 *   - Row deduplication via L1-cached temp row
 *   - Partial region updates (only dirty VNC rectangles)
 *   - Border-only clearing (once, not every frame)
 *   - Frame-rate-capped present (30 fps)
 *   - Bilinear interpolation for smooth downscaling
 */

#include "vnc_renderer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

/* ── Full printable ASCII 5×7 bitmap font (ASCII 32–126, 95 glyphs) ── */
static const uint8_t font_5x7[][5] = {
    {0x00, 0x00, 0x00, 0x00, 0x00}, /* Space */
    {0x00, 0x00, 0x5F, 0x00, 0x00}, /* ! */
    {0x00, 0x07, 0x00, 0x07, 0x00}, /* " */
    {0x14, 0x7F, 0x14, 0x7F, 0x14}, /* # */
    {0x24, 0x2A, 0x7F, 0x2A, 0x12}, /* $ */
    {0x23, 0x13, 0x08, 0x64, 0x62}, /* % */
    {0x36, 0x49, 0x55, 0x22, 0x50}, /* & */
    {0x00, 0x05, 0x03, 0x00, 0x00}, /* ' */
    {0x00, 0x1C, 0x22, 0x41, 0x00}, /* ( */
    {0x00, 0x41, 0x22, 0x1C, 0x00}, /* ) */
    {0x14, 0x08, 0x3E, 0x08, 0x14}, /* * */
    {0x08, 0x08, 0x3E, 0x08, 0x08}, /* + */
    {0x00, 0x50, 0x30, 0x00, 0x00}, /* , */
    {0x08, 0x08, 0x08, 0x08, 0x08}, /* - */
    {0x00, 0x60, 0x60, 0x00, 0x00}, /* . */
    {0x20, 0x10, 0x08, 0x04, 0x02}, /* / */
    {0x3E, 0x51, 0x49, 0x45, 0x3E}, /* 0 */
    {0x00, 0x42, 0x7F, 0x40, 0x00}, /* 1 */
    {0x42, 0x61, 0x51, 0x49, 0x46}, /* 2 */
    {0x21, 0x41, 0x45, 0x4B, 0x31}, /* 3 */
    {0x18, 0x14, 0x12, 0x7F, 0x10}, /* 4 */
    {0x27, 0x45, 0x45, 0x45, 0x39}, /* 5 */
    {0x3C, 0x4A, 0x49, 0x49, 0x30}, /* 6 */
    {0x01, 0x71, 0x09, 0x05, 0x03}, /* 7 */
    {0x36, 0x49, 0x49, 0x49, 0x36}, /* 8 */
    {0x06, 0x49, 0x49, 0x29, 0x1E}, /* 9 */
    {0x00, 0x36, 0x36, 0x00, 0x00}, /* : */
    {0x00, 0x56, 0x36, 0x00, 0x00}, /* ; */
    {0x08, 0x14, 0x22, 0x41, 0x00}, /* < */
    {0x14, 0x14, 0x14, 0x14, 0x14}, /* = */
    {0x00, 0x41, 0x22, 0x14, 0x08}, /* > */
    {0x02, 0x01, 0x51, 0x09, 0x06}, /* ? */
    {0x32, 0x49, 0x79, 0x41, 0x3E}, /* @ */
    {0x7E, 0x11, 0x11, 0x11, 0x7E}, /* A */
    {0x7F, 0x49, 0x49, 0x49, 0x36}, /* B */
    {0x3E, 0x41, 0x41, 0x41, 0x22}, /* C */
    {0x7F, 0x41, 0x41, 0x22, 0x1C}, /* D */
    {0x7F, 0x49, 0x49, 0x49, 0x41}, /* E */
    {0x7F, 0x09, 0x09, 0x09, 0x01}, /* F */
    {0x3E, 0x41, 0x49, 0x49, 0x7A}, /* G */
    {0x7F, 0x08, 0x08, 0x08, 0x7F}, /* H */
    {0x00, 0x41, 0x7F, 0x41, 0x00}, /* I */
    {0x20, 0x40, 0x41, 0x3F, 0x01}, /* J */
    {0x7F, 0x08, 0x14, 0x22, 0x41}, /* K */
    {0x7F, 0x40, 0x40, 0x40, 0x40}, /* L */
    {0x7F, 0x02, 0x0C, 0x02, 0x7F}, /* M */
    {0x7F, 0x04, 0x08, 0x10, 0x7F}, /* N */
    {0x3E, 0x41, 0x41, 0x41, 0x3E}, /* O */
    {0x7F, 0x09, 0x09, 0x09, 0x06}, /* P */
    {0x3E, 0x41, 0x51, 0x21, 0x5E}, /* Q */
    {0x7F, 0x09, 0x19, 0x29, 0x46}, /* R */
    {0x46, 0x49, 0x49, 0x49, 0x31}, /* S */
    {0x01, 0x01, 0x7F, 0x01, 0x01}, /* T */
    {0x3F, 0x40, 0x40, 0x40, 0x3F}, /* U */
    {0x1F, 0x20, 0x40, 0x20, 0x1F}, /* V */
    {0x3F, 0x40, 0x38, 0x40, 0x3F}, /* W */
    {0x63, 0x14, 0x08, 0x14, 0x63}, /* X */
    {0x07, 0x08, 0x70, 0x08, 0x07}, /* Y */
    {0x61, 0x51, 0x49, 0x45, 0x43}, /* Z */
    /* ASCII 91–96: [ \ ] ^ _ ` */
    {0x00, 0x7F, 0x41, 0x41, 0x00}, /* [ */
    {0x02, 0x04, 0x08, 0x10, 0x20}, /* backslash */
    {0x00, 0x41, 0x41, 0x7F, 0x00}, /* ] */
    {0x04, 0x02, 0x01, 0x02, 0x04}, /* ^ */
    {0x40, 0x40, 0x40, 0x40, 0x40}, /* _ */
    {0x00, 0x01, 0x02, 0x04, 0x00}, /* ` */
    /* ASCII 97–122: lowercase a–z */
    {0x20, 0x54, 0x54, 0x54, 0x78}, /* a */
    {0x7F, 0x48, 0x44, 0x44, 0x38}, /* b */
    {0x38, 0x44, 0x44, 0x44, 0x20}, /* c */
    {0x38, 0x44, 0x44, 0x48, 0x7F}, /* d */
    {0x38, 0x54, 0x54, 0x54, 0x18}, /* e */
    {0x08, 0x7E, 0x09, 0x01, 0x02}, /* f */
    {0x0C, 0x52, 0x52, 0x52, 0x3E}, /* g */
    {0x7F, 0x08, 0x04, 0x04, 0x78}, /* h */
    {0x00, 0x44, 0x7D, 0x40, 0x00}, /* i */
    {0x20, 0x40, 0x44, 0x3D, 0x00}, /* j */
    {0x7F, 0x10, 0x28, 0x44, 0x00}, /* k */
    {0x00, 0x41, 0x7F, 0x40, 0x00}, /* l */
    {0x7C, 0x04, 0x18, 0x04, 0x78}, /* m */
    {0x7C, 0x08, 0x04, 0x04, 0x78}, /* n */
    {0x38, 0x44, 0x44, 0x44, 0x38}, /* o */
    {0x7C, 0x14, 0x14, 0x14, 0x08}, /* p */
    {0x08, 0x14, 0x14, 0x18, 0x7C}, /* q */
    {0x7C, 0x08, 0x04, 0x04, 0x08}, /* r */
    {0x48, 0x54, 0x54, 0x54, 0x20}, /* s */
    {0x04, 0x3F, 0x44, 0x40, 0x20}, /* t */
    {0x3C, 0x40, 0x40, 0x20, 0x7C}, /* u */
    {0x1C, 0x20, 0x40, 0x20, 0x1C}, /* v */
    {0x3C, 0x40, 0x30, 0x40, 0x3C}, /* w */
    {0x44, 0x28, 0x10, 0x28, 0x44}, /* x */
    {0x0C, 0x50, 0x50, 0x50, 0x3C}, /* y */
    {0x44, 0x64, 0x54, 0x4C, 0x44}, /* z */
    /* ASCII 123–126: { | } ~ */
    {0x00, 0x08, 0x36, 0x41, 0x00}, /* { */
    {0x00, 0x00, 0x7F, 0x00, 0x00}, /* | */
    {0x00, 0x41, 0x36, 0x08, 0x00}, /* } */
    {0x10, 0x08, 0x08, 0x10, 0x08}, /* ~ */
};
#define FONT_ENTRIES (sizeof(font_5x7) / sizeof(font_5x7[0]))

/* ── Utilities ─────────────────────────────────────────────────────── */

void vnc_renderer_clear_screen(Framebuffer *fb) {
    if (!fb || !fb->back_buffer) return;
    /* back_buffer_size, NOT screen_size: screen_size is the physical mmap size
     * (line_length * panel height) and overruns the logical back buffer. */
    memset(fb->back_buffer, 0, fb->back_buffer_size);
}

void vnc_renderer_draw_text(Framebuffer *fb, int x, int y, const char *text,
                            uint16_t color, int scale) {
    if (!fb || !fb->back_buffer || !text) return;

    uint16_t *buf = (uint16_t *)fb->back_buffer;
    const int sw = (int)fb->width;
    const int sh = (int)fb->height;
    int pen_x = 0;

    while (*text) {
        char c = *text++;
        int idx = c - ' ';

        if (idx < 0 || idx >= 95) {
            pen_x += 6 * scale;
            continue;
        }

        {
            for (int col = 0; col < 5; col++) {
                uint8_t column = font_5x7[idx][col];
                for (int row = 0; row < 7; row++) {
                    if (column & (1 << row)) {
                        for (int sy = 0; sy < scale; sy++) {
                            for (int sx = 0; sx < scale; sx++) {
                                int px = x + pen_x + col * scale + sx;
                                int py = y + row * scale + sy;
                                if (px >= 0 && px < sw &&
                                    py >= 0 && py < sh)
                                    buf[py * sw + px] = color;
                            }
                        }
                    }
                }
            }
        }
        pen_x += 6 * scale;
    }
}

void vnc_renderer_fill_rect(Framebuffer *fb, int x, int y, int w, int h,
                            uint16_t color) {
    if (!fb || !fb->back_buffer) return;
    uint16_t *buf = (uint16_t *)fb->back_buffer;
    const int sw = (int)fb->width;
    const int sh = (int)fb->height;
    for (int row = y; row < y + h && row < sh; row++) {
        if (row < 0) continue;
        for (int col = x; col < x + w && col < sw; col++) {
            if (col < 0) continue;
            buf[row * sw + col] = color;
        }
    }
}

int vnc_renderer_text_width(const char *text, int scale) {
    if (!text) return 0;
    return (int)strlen(text) * 6 * scale;
}

/* ── The content rectangle ─────────────────────────────────────────── */

/* Set from the config once at startup; see the header for why the picture is
 * confined and why our own touch UI deliberately ignores this flag. */
static bool s_content_full = false;

void vnc_content_set_full(bool full) {
    s_content_full = full;
}

void vnc_content_rect(const Framebuffer *fb, int *x, int *y, int *w, int *h) {
    if (!fb) return;
    if (s_content_full) {
        *x = 0;
        *y = 0;
        *w = (int)fb->width;
        *h = (int)fb->height;
    } else {
        *x = SCREEN_SAFE_LEFT;
        *y = SCREEN_SAFE_TOP;
        *w = SCREEN_SAFE_WIDTH;
        *h = SCREEN_SAFE_HEIGHT;
    }
}

/* ── Renderer lifecycle ────────────────────────────────────────────── */

int vnc_renderer_init(VNCRenderer *renderer, Framebuffer *fb) {
    if (!renderer || !fb) return -1;

    memset(renderer, 0, sizeof(VNCRenderer));
    renderer->fb = fb;
    renderer->scaling_mode = DEFAULT_SCALING_MODE;
    renderer->needs_present = false;
    renderer->borders_cleared = false;
    gettimeofday(&renderer->last_fps_time, NULL);
    gettimeofday(&renderer->last_present_time, NULL);

    DEBUG_PRINT("Renderer initialized (16bpp direct-write mode)");
    return 0;
}

void vnc_renderer_set_remote_size(VNCRenderer *renderer, int width, int height) {
    if (!renderer || !renderer->fb) return;

    /* The server announces this geometry; a zero or negative dimension would
     * be a division by zero below. */
    if (width <= 0 || height <= 0) {
        DEBUG_PRINT("Rejecting invalid remote size %dx%d", width, height);
        return;
    }

    /* The letterbox target is the CONTENT rectangle — the touch-safe area by
     * default — not the whole logical surface.  Every pixel of a third-party
     * desktop has to be reachable, and the digitizer saturates before the panel
     * edge.  sw/sh remain the surface, because they are still the stride and the
     * bound for the border clearing below. */
    const int sw = (int)renderer->fb->width;
    const int sh = (int)renderer->fb->height;
    int cx, cy, cw, ch;
    vnc_content_rect(renderer->fb, &cx, &cy, &cw, &ch);

    renderer->remote_width  = width;
    renderer->remote_height = height;

    /* Compute letterbox scaling with fixed-point ×256 to avoid floats */
    int scale_x = (cw * 256) / width;
    int scale_y = (ch * 256) / height;
    int scale   = (scale_x < scale_y) ? scale_x : scale_y;

    renderer->scaled_width  = (width  * scale) / 256;
    renderer->scaled_height = (height * scale) / 256;

    /* Clamp to the content rect, and to the LUT capacity so src_x_lut cannot
     * overflow even if a future panel is wider than PANEL_MAX_WIDTH. */
    if (renderer->scaled_width  > cw) renderer->scaled_width  = cw;
    if (renderer->scaled_width  > PANEL_MAX_WIDTH)
        renderer->scaled_width = PANEL_MAX_WIDTH;
    if (renderer->scaled_height > ch) renderer->scaled_height = ch;

    renderer->offset_x = cx + (cw - renderer->scaled_width)  / 2;
    renderer->offset_y = cy + (ch - renderer->scaled_height) / 2;

    /* ── O2: Precompute bilinear X-coordinate lookup table ──────────── */
    for (int dx = 0; dx < renderer->scaled_width; dx++) {
        /*
         * Map dest X → fractional source X using fixed-point ×256.
         * For downscaling 1920→795, each dest pixel spans ~2.4 source pixels.
         * We compute the exact fractional position for bilinear blending.
         */
        int src_x_256 = (dx * width * 256) / renderer->scaled_width;
        int x0 = src_x_256 >> 8;  /* integer part */
        int frac = src_x_256 & 0xFF;  /* fractional part 0-255 */

        if (x0 >= width - 1) {
            x0 = width - 1;
            frac = 0;
        }

        renderer->src_x_lut[dx].x0   = x0;
        renderer->src_x_lut[dx].x1   = (x0 < width - 1) ? x0 + 1 : x0;
        renderer->src_x_lut[dx].frac = (uint8_t)frac;
    }

    /* ── O3: Clear only the letterbox border strips (once) ───────── */
    uint16_t *buf = (uint16_t *)renderer->fb->back_buffer;

    /* Top border */
    if (renderer->offset_y > 0)
        memset(buf, 0, renderer->offset_y * sw * 2);

    /* Bottom border */
    int bot = renderer->offset_y + renderer->scaled_height;
    if (bot < sh)
        memset(buf + bot * sw, 0, (sh - bot) * sw * 2);

    /* Left/right borders */
    int left_bytes  = renderer->offset_x * 2;
    int right_start = renderer->offset_x + renderer->scaled_width;
    int right_bytes = (sw - right_start) * 2;

    for (int y = renderer->offset_y; y < bot && y < sh; y++) {
        if (left_bytes > 0)
            memset(buf + y * sw, 0, left_bytes);
        if (right_bytes > 0)
            memset(buf + y * sw + right_start, 0, right_bytes);
    }

    renderer->borders_cleared = true;

    DEBUG_PRINT("Remote: %dx%d -> Scaled: %dx%d, Offset: (%d,%d) "
                "in content rect %dx%d at (%d,%d) of %dx%d surface",
                width, height, renderer->scaled_width, renderer->scaled_height,
                renderer->offset_x, renderer->offset_y, cw, ch, cx, cy, sw, sh);
}

/* ── Core rendering: partial region update with bilinear interpolation ── */

/*
 * Bilinear interpolation helper for a single 8-bit channel.
 * Uses fixed-point 8-bit fractions (0-255).
 *   p00──fx──p01
 *    |        |
 *   fy       fy
 *    |        |
 *   p10──fx──p11
 *
 * result = (1-fx)(1-fy)*p00 + fx*(1-fy)*p01 + (1-fx)*fy*p10 + fx*fy*p11
 * All weights are 0-256, so the max product is 256*256*255 = 16,711,680 < 2^24
 */
static inline uint8_t bilerp8(uint8_t p00, uint8_t p01, uint8_t p10, uint8_t p11,
                               unsigned fx, unsigned fy) {
    unsigned ifx = 256 - fx;
    unsigned ify = 256 - fy;
    return (uint8_t)((ifx * ify * p00 + fx * ify * p01 +
                       ifx * fy * p10 + fx * fy * p11 + 32768) >> 16);
}

void vnc_renderer_update_region(VNCRenderer *renderer, const uint8_t *remote_fb,
                                int rx, int ry, int rw, int rh,
                                int bytes_per_pixel) {
    if (!renderer || !renderer->fb || !remote_fb) return;
    if (!renderer->borders_cleared) return;     /* not yet configured */

    const int arca_total = renderer->remote_width;
    const int rh_total = renderer->remote_height;

    /* Map remote dirty rect → screen coordinates (integer math) */
    int sx1 = renderer->offset_x +
              (rx * renderer->scaled_width) / arca_total;
    int sy1 = renderer->offset_y +
              (ry * renderer->scaled_height) / rh_total;
    int sx2 = renderer->offset_x +
              ((rx + rw) * renderer->scaled_width + arca_total - 1) / arca_total;
    int sy2 = renderer->offset_y +
              ((ry + rh) * renderer->scaled_height + rh_total - 1) / rh_total;

    /* Clamp to the active display area */
    const int left   = renderer->offset_x;
    const int top    = renderer->offset_y;
    const int right  = renderer->offset_x + renderer->scaled_width;
    const int bottom = renderer->offset_y + renderer->scaled_height;

    if (sx1 < left)   sx1 = left;
    if (sy1 < top)    sy1 = top;
    if (sx2 > right)  sx2 = right;
    if (sy2 > bottom) sy2 = bottom;
    if (sx1 >= sx2 || sy1 >= sy2) return;

    uint16_t *buf = (uint16_t *)renderer->fb->back_buffer;
    const int fb_stride  = (int)renderer->fb->width;
    const int row_start  = sx1;
    const int row_pixels = sx2 - sx1;

    /*
     * Track the previous (src_y0, src_y1, frac_y) triple.
     * When consecutive dest rows map to the same source row pair with the
     * same Y fraction, we can memcpy the previously computed temp_row
     * instead of recomputing the bilinear blend (O11 row dedup).
     */
    int prev_src_y0 = -1;
    int prev_src_y1 = -1;
    unsigned prev_fy = 0;
    (void)prev_fy; /* suppress warning — used in dedup comparison */

    for (int dy = sy1; dy < sy2; dy++) {
        /* Compute fractional source Y using fixed-point ×256 */
        int src_y_256 = ((dy - top) * rh_total * 256) / renderer->scaled_height;
        int src_y0 = src_y_256 >> 8;
        unsigned fy = src_y_256 & 0xFF;

        if (src_y0 >= rh_total - 1) {
            src_y0 = rh_total - 1;
            fy = 0;
        }
        int src_y1 = (src_y0 < rh_total - 1) ? src_y0 + 1 : src_y0;

        if (src_y0 != prev_src_y0 || src_y1 != prev_src_y1 || fy != prev_fy) {
            /* ── New source row pair: bilinear blend into temp_row ──── */
            prev_src_y0 = src_y0;
            prev_src_y1 = src_y1;
            prev_fy = fy;

            if (bytes_per_pixel == 4) {
                /*
                 * 32bpp BGR → bilinear → RGB565.
                 * x11vnc sends pixels as 0x00BBGGRR on little-endian.
                 *
                 * For each dest pixel, read a 2×2 neighbourhood from
                 * (src_y0, src_y1) × (x0, x1) and blend all 3 channels
                 * before converting to RGB565.
                 */
                const uint32_t *row0 =
                    (const uint32_t *)(remote_fb + (size_t)src_y0 * arca_total * 4);
                const uint32_t *row1 =
                    (const uint32_t *)(remote_fb + (size_t)src_y1 * arca_total * 4);
                uint16_t *tmp = renderer->temp_row + row_start;

                for (int dx = sx1; dx < sx2; dx++) {
                    const BilinearXEntry *bx = &renderer->src_x_lut[dx - left];
                    unsigned fx = bx->frac;

                    uint32_t p00 = row0[bx->x0];
                    uint32_t p01 = row0[bx->x1];
                    uint32_t p10 = row1[bx->x0];
                    uint32_t p11 = row1[bx->x1];

                    /* BGR layout: byte 0=R, byte 1=G, byte 2=B */
                    uint8_t r = bilerp8(p00 & 0xFF, p01 & 0xFF,
                                        p10 & 0xFF, p11 & 0xFF, fx, fy);
                    uint8_t g = bilerp8((p00 >> 8) & 0xFF, (p01 >> 8) & 0xFF,
                                        (p10 >> 8) & 0xFF, (p11 >> 8) & 0xFF, fx, fy);
                    uint8_t b = bilerp8((p00 >> 16) & 0xFF, (p01 >> 16) & 0xFF,
                                        (p10 >> 16) & 0xFF, (p11 >> 16) & 0xFF, fx, fy);

                    *tmp++ = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
                }
            } else if (bytes_per_pixel == 3) {
                /* 24bpp BGR → bilinear → RGB565 */
                const uint8_t *base0 =
                    remote_fb + (size_t)src_y0 * arca_total * 3;
                const uint8_t *base1 =
                    remote_fb + (size_t)src_y1 * arca_total * 3;
                uint16_t *tmp = renderer->temp_row + row_start;

                for (int dx = sx1; dx < sx2; dx++) {
                    const BilinearXEntry *bx = &renderer->src_x_lut[dx - left];
                    unsigned fx = bx->frac;

                    const uint8_t *q00 = base0 + bx->x0 * 3;
                    const uint8_t *q01 = base0 + bx->x1 * 3;
                    const uint8_t *q10 = base1 + bx->x0 * 3;
                    const uint8_t *q11 = base1 + bx->x1 * 3;

                    uint8_t r = bilerp8(q00[0], q01[0], q10[0], q11[0], fx, fy);
                    uint8_t g = bilerp8(q00[1], q01[1], q10[1], q11[1], fx, fy);
                    uint8_t b = bilerp8(q00[2], q01[2], q10[2], q11[2], fx, fy);

                    *tmp++ = ((b >> 3) << 11) | ((g >> 2) << 5) | (r >> 3);
                }
            }
        }
        /* ── Copy temp row → back buffer (single contiguous write) ── */
        memcpy(buf + dy * fb_stride + row_start,
               renderer->temp_row + row_start,
               row_pixels * 2);
    }

    renderer->update_count++;
    renderer->needs_present = true;
}

/* ── Frame presentation with rate cap ──────────────────────────────── */

bool vnc_renderer_present(VNCRenderer *renderer) {
    if (!renderer || !renderer->fb || !renderer->needs_present)
        return false;

    /* Cap frame rate (default ~30 fps) */
    struct timeval now;
    gettimeofday(&now, NULL);
    long elapsed_us = (now.tv_sec  - renderer->last_present_time.tv_sec)  * 1000000L
                    + (now.tv_usec - renderer->last_present_time.tv_usec);

    if (elapsed_us > 0 && elapsed_us < FRAME_INTERVAL_US)
        return false;

    /* Swap back buffer → display */
    fb_swap(renderer->fb);
    renderer->last_present_time = now;
    renderer->needs_present = false;
    renderer->frame_count++;

    /* Log FPS to stderr every 5 seconds */
    long fps_us = (now.tv_sec  - renderer->last_fps_time.tv_sec)  * 1000000L
                + (now.tv_usec - renderer->last_fps_time.tv_usec);
    if (fps_us >= 5000000L) {
        renderer->current_fps =
            (float)renderer->frame_count * 1000000.0f / (float)fps_us;
        fprintf(stderr, "[VNC] FPS: %.1f | VNC updates: %u\n",
                renderer->current_fps, renderer->update_count);
        renderer->frame_count  = 0;
        renderer->update_count = 0;
        renderer->last_fps_time = now;
    }

    return true;
}

/* ── Coordinate mapping (screen → remote) ──────────────────────────── */

bool vnc_renderer_screen_to_remote(VNCRenderer *renderer,
                                   int screen_x, int screen_y,
                                   int *remote_x, int *remote_y) {
    if (!renderer || !renderer->fb || !remote_x || !remote_y) return false;

    int rel_x = screen_x - renderer->offset_x;
    int rel_y = screen_y - renderer->offset_y;

    if (rel_x < 0 || rel_x >= renderer->scaled_width ||
        rel_y < 0 || rel_y >= renderer->scaled_height)
        return false;   /* touch landed in letterbox border */

    if (renderer->scaling_mode == SCALING_STRETCH) {
        /* Stretch fills the CONTENT rect, so the mapping must be relative to it
         * — mapping against the whole surface would disagree with where
         * set_remote_size() actually put the picture. */
        int cx, cy, cw, ch;
        vnc_content_rect(renderer->fb, &cx, &cy, &cw, &ch);
        *remote_x = ((screen_x - cx) * renderer->remote_width)  / (cw > 0 ? cw : 1);
        *remote_y = ((screen_y - cy) * renderer->remote_height) / (ch > 0 ? ch : 1);
    } else {
        *remote_x = (rel_x * renderer->remote_width)  / renderer->scaled_width;
        *remote_y = (rel_y * renderer->remote_height) / renderer->scaled_height;
    }

    /* Clamp */
    if (*remote_x < 0) *remote_x = 0;
    if (*remote_y < 0) *remote_y = 0;
    if (*remote_x >= renderer->remote_width)  *remote_x = renderer->remote_width  - 1;
    if (*remote_y >= renderer->remote_height) *remote_y = renderer->remote_height - 1;

    return true;
}

/* ── Cleanup ───────────────────────────────────────────────────────── */

void vnc_renderer_cleanup(VNCRenderer *renderer) {
    if (!renderer) return;

    DEBUG_PRINT("Renderer cleanup");

#if USE_16BPP
    /* Restore 32bpp for native_apps / app_launcher on exit */
    fb_set_bpp(FB_DEVICE, 32);
    DEBUG_PRINT("Restored 32bpp framebuffer");
#endif
}
