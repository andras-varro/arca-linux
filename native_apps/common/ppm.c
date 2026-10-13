/**
 * PPM Image Loader for Arca
 *
 * Supports P6 (binary RGB) format only.  Maxval must be 255.
 * Images larger than 4096×4096 are rejected as a safety limit.
 */

#include "ppm.h"
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

/* Skip whitespace and #-comments in PPM header */
static void skip_ws_comments(FILE *f) {
    int c;
    while ((c = fgetc(f)) != EOF) {
        if (c == '#') {
            while ((c = fgetc(f)) != EOF && c != '\n')
                ;
        } else if (!isspace(c)) {
            ungetc(c, f);
            return;
        }
    }
}

uint32_t *ppm_load(const char *filename, int *out_width, int *out_height) {
    FILE *f = fopen(filename, "rb");
    if (!f) return NULL;

    /* Magic number */
    char magic[3];
    if (fread(magic, 1, 2, f) != 2 || magic[0] != 'P' || magic[1] != '6') {
        fclose(f);
        return NULL;
    }

    int w, h, maxval;
    skip_ws_comments(f);
    if (fscanf(f, "%d", &w) != 1) { fclose(f); return NULL; }
    skip_ws_comments(f);
    if (fscanf(f, "%d", &h) != 1) { fclose(f); return NULL; }
    skip_ws_comments(f);
    if (fscanf(f, "%d", &maxval) != 1) { fclose(f); return NULL; }

    /* Single whitespace byte after maxval separates header from data */
    fgetc(f);

    if (w <= 0 || h <= 0 || w > 4096 || h > 4096 || maxval != 255) {
        fclose(f);
        return NULL;
    }

    uint32_t *pixels = malloc((size_t)w * h * sizeof(uint32_t));
    if (!pixels) { fclose(f); return NULL; }

    unsigned char rgb[3];
    for (int i = 0; i < w * h; i++) {
        if (fread(rgb, 1, 3, f) != 3) {
            free(pixels);
            fclose(f);
            return NULL;
        }
        pixels[i] = 0xFF000000u | ((uint32_t)rgb[0] << 16)
                                 | ((uint32_t)rgb[1] << 8)
                                 |  (uint32_t)rgb[2];
    }

    fclose(f);
    *out_width  = w;
    *out_height = h;
    return pixels;
}

uint32_t *ppm_scale(const uint32_t *src, int src_w, int src_h,
                    int dst_w, int dst_h)
{
    /* A non-positive source dimension makes the clamps below compute a
     * NEGATIVE sy/sx, which reads before the buffer.  Refuse it here.
     *
     * The DESTINATION carries ppm_load's own 4096-per-axis limit, and that is a
     * write-side guard rather than tidiness: size_t is 32 bits on armhf, so
     * (size_t)dst_w * dst_h * 4 WRAPS — 65536x65536 lands on exactly 0 — and the
     * malloc then succeeds tiny while the loops below write the full extent.
     * That is a heap overflow WRITE, strictly worse than the read above, and it
     * cannot be reproduced on the 64-bit dev host where the product still fits.
     * No shipped caller can reach it (the launcher scales to icon constants), so
     * this closes it by construction rather than by luck. */
    if (!src || src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0) return NULL;
    if (src_w > 4096 || src_h > 4096 || dst_w > 4096 || dst_h > 4096) return NULL;

    uint32_t *dst = malloc((size_t)dst_w * dst_h * sizeof(uint32_t));
    if (!dst) return NULL;

    for (int y = 0; y < dst_h; y++) {
        int sy = y * src_h / dst_h;
        if (sy >= src_h) sy = src_h - 1;
        for (int x = 0; x < dst_w; x++) {
            int sx = x * src_w / dst_w;
            if (sx >= src_w) sx = src_w - 1;
            dst[y * dst_w + x] = src[sy * src_w + sx];
        }
    }
    return dst;
}
