/*
 * frame_core.c — picture-frame library core.  See frame_core.h.
 *
 * Decoder / encoder: the vendored stb_image.h v2.30 and stb_image_write.h v1.16 (public domain / MIT,
 * Sean Barrett, https://github.com/nothings/stb).  Configured like common/jf_img.c (memory input only,
 * no SIMD, no HDR, no thread-local state) but with PNG enabled as well — which is why this file and
 * jf_img.c can never be linked into one binary.
 *
 * Scaling is an exact area average (box filter with fractional edge weights), separable and streamed
 * one oriented source row at a time, so a 4000x3000 photo needs its RGB decode and nothing larger than
 * a few rows besides.  Integer fixed point throughout: the weights are overlap lengths, and the only
 * divisions are two per import (the reciprocals), none per pixel — the Cortex-A8 has no divider.
 */
#define _GNU_SOURCE
#include "frame_core.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_SIMD
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_NO_THREAD_LOCALS
#define STBI_NO_FAILURE_STRINGS
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#endif
#include "stb_image.h"
#include "stb_image_write.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#define FILE_MAX_BYTES (64L * 1024 * 1024)

/* ── file helpers ──────────────────────────────────────────────────────── */

static uint8_t *read_file(const char *path, size_t *len, int *err)
{
    FILE *f = fopen(path, "rb");
    if (!f) { *err = FRAME_E_IO; return NULL; }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); *err = FRAME_E_IO; return NULL; }
    long n = ftell(f);
    if (n <= 0 || n > FILE_MAX_BYTES || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f); *err = (n > FILE_MAX_BYTES || n == 0) ? FRAME_E_FORMAT : FRAME_E_IO; return NULL;
    }
    uint8_t *buf = (uint8_t *)malloc((size_t)n);
    if (!buf) { fclose(f); *err = FRAME_E_NOMEM; return NULL; }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); fclose(f); *err = FRAME_E_IO; return NULL; }
    fclose(f);
    *len = (size_t)n;
    return buf;
}

/* ── EXIF orientation ──────────────────────────────────────────────────── */

static unsigned rd16(const uint8_t *p, int le) { return le ? (unsigned)(p[0] | (p[1] << 8)) : (unsigned)((p[0] << 8) | p[1]); }
static uint32_t rd32(const uint8_t *p, int le)
{
    return le ? ((uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24))
              : (((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3]);
}

/* tiff = the TIFF header (just after "Exif\0\0"), n bytes available */
static int tiff_orientation(const uint8_t *tiff, size_t n)
{
    if (n < 8) return 1;
    int le;
    if (tiff[0] == 'I' && tiff[1] == 'I') le = 1;
    else if (tiff[0] == 'M' && tiff[1] == 'M') le = 0;
    else return 1;
    if (rd16(tiff + 2, le) != 42) return 1;
    uint32_t off = rd32(tiff + 4, le);
    if (off < 8 || off > n || n - off < 2) return 1;
    unsigned cnt = rd16(tiff + off, le);
    size_t p = (size_t)off + 2;
    for (unsigned i = 0; i < cnt; i++, p += 12) {
        if (p > n || n - p < 12) return 1;
        if (rd16(tiff + p, le) != 0x0112) continue;
        unsigned type = rd16(tiff + p + 2, le);
        unsigned v = (type == 3) ? rd16(tiff + p + 8, le) : (type == 1 ? tiff[p + 8] : 0);
        return (v >= 1 && v <= 8) ? (int)v : 1;
    }
    return 1;
}

int frame_exif_orientation(const uint8_t *j, size_t len)
{
    if (!j || len < 4 || j[0] != 0xFF || j[1] != 0xD8) return 1;
    size_t p = 2;
    while (p + 4 <= len) {
        if (j[p] != 0xFF) return 1;
        uint8_t m = j[p + 1];
        if (m == 0xFF) { p++; continue; }                       /* fill byte */
        if (m == 0xD8 || m == 0x01 || (m >= 0xD0 && m <= 0xD7)) { p += 2; continue; }   /* no length */
        if (m == 0xDA || m == 0xD9) return 1;                   /* image data / end: no Exif before it */
        size_t seg = rd16(j + p + 2, 0);
        if (seg < 2 || p + 2 + seg > len) return 1;
        if (m == 0xE1 && seg >= 2 + 6 + 8 && memcmp(j + p + 4, "Exif\0\0", 6) == 0)
            return tiff_orientation(j + p + 10, seg - 2 - 6);
        p += 2 + seg;
    }
    return 1;
}

/* stb_image decodes a truncated JPEG without complaint (the missing scan is filled with grey), so a
 * library or an import would silently keep half a photo.  Walk the markers to the EOI instead: after each
 * SOS the entropy data runs to the next marker that is not a stuffed FF00, a restart or a fill byte. */
static int jpeg_complete(const uint8_t *j, size_t len)
{
    size_t p = 2;
    if (len < 4 || j[0] != 0xFF || j[1] != 0xD8) return 0;
    while (p + 2 <= len) {
        if (j[p] != 0xFF) return 0;
        uint8_t m = j[p + 1];
        if (m == 0xFF) { p++; continue; }
        if (m == 0xD9) return 1;
        if (m == 0xD8 || m == 0x01 || (m >= 0xD0 && m <= 0xD7) || m == 0x00) { p += 2; continue; }
        if (p + 4 > len) return 0;
        size_t seg = rd16(j + p + 2, 0);
        if (seg < 2 || p + 2 + seg > len) return 0;
        p += 2 + seg;
        if (m == 0xDA) {                                    /* entropy-coded data up to the next real marker */
            while (p + 1 < len && !(j[p] == 0xFF && j[p + 1] != 0x00 && !(j[p + 1] >= 0xD0 && j[p + 1] <= 0xD7) && j[p + 1] != 0xFF))
                p++;
            if (p + 1 >= len) return 0;
        }
    }
    return 0;
}

/* ── geometry ──────────────────────────────────────────────────────────── */

/* Source pixel for oriented position (x, y) under EXIF orientation o; w x h is the SOURCE size. */
static inline void orient_map(int o, int w, int h, int x, int y, int *sx, int *sy)
{
    switch (o) {
    case 2:  *sx = w - 1 - x; *sy = y;         break;     /* mirror horizontal */
    case 3:  *sx = w - 1 - x; *sy = h - 1 - y; break;     /* rotate 180 */
    case 4:  *sx = x;         *sy = h - 1 - y; break;     /* mirror vertical */
    case 5:  *sx = y;         *sy = x;         break;     /* transpose */
    case 6:  *sx = y;         *sy = h - 1 - x; break;     /* rotate 90 clockwise */
    case 7:  *sx = w - 1 - y; *sy = h - 1 - x; break;     /* transverse */
    case 8:  *sx = w - 1 - y; *sy = x;         break;     /* rotate 90 counter-clockwise */
    default: *sx = x;         *sy = y;         break;
    }
}

/* Fit ow x oh inside mw x mh, aspect kept, never enlarged. */
static void fit(int ow, int oh, int mw, int mh, int *dw, int *dh)
{
    if (ow <= mw && oh <= mh) { *dw = ow; *dh = oh; return; }
    long w, h;
    if ((long)ow * mh > (long)oh * mw) { w = mw; h = ((long)oh * mw + ow / 2) / ow; }
    else                               { h = mh; w = ((long)ow * mh + oh / 2) / oh; }
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    *dw = (int)w; *dh = (int)h;
}

/* ── box-filter downscale, streamed ────────────────────────────────────── */

/*
 * One axis: sn source pixels onto dn destination pixels (dn <= sn).  Source pixel j covers
 * [j*dn, (j+1)*dn), destination i covers [i*sn, (i+1)*sn) on a common integer line, so every
 * destination's weights sum to exactly sn.  Both axes walk it with the same loop.
 */

/* Horizontal: oriented row (3 bytes/pixel, ow pixels) -> hrow (dw*3 values, scaled by 256). */
static void hscale_row(const uint8_t *row, int ow, int dw, uint32_t inv, uint32_t *hrow)
{
    uint32_t r = 0, g = 0, b = 0;
    int di = 0, rem_dest = ow;
    for (int j = 0; j < ow; j++) {
        int rem_src = dw;
        const uint8_t *px = row + 3 * j;
        while (rem_src > 0) {
            int take = rem_src < rem_dest ? rem_src : rem_dest;
            r += (uint32_t)take * px[0]; g += (uint32_t)take * px[1]; b += (uint32_t)take * px[2];
            rem_src -= take; rem_dest -= take;
            if (rem_dest == 0) {
                uint32_t *h = hrow + 3 * di;
                h[0] = (r * inv + 0x8000u) >> 16; h[1] = (g * inv + 0x8000u) >> 16; h[2] = (b * inv + 0x8000u) >> 16;
                r = g = b = 0; di++; rem_dest = ow;
            }
        }
    }
}

/* Whole picture: decoded RGB src (w x h, orientation o) -> out (dw x dh, RGB).  Returns 0 on OOM. */
static int box_scale(const uint8_t *src, int w, int h, int o, int dw, int dh, uint8_t *out)
{
    int ow = (o >= 5) ? h : w, oh = (o >= 5) ? w : h;       /* oriented size */
    uint8_t  *row  = (uint8_t *)malloc((size_t)ow * 3);
    uint32_t *hrow = (uint32_t *)malloc((size_t)dw * 3 * sizeof(uint32_t));
    uint32_t *acc  = (uint32_t *)calloc((size_t)dw * 3, sizeof(uint32_t));
    if (!row || !hrow || !acc) { free(row); free(hrow); free(acc); return 0; }

    uint32_t inv_x = (1u << 24) / (uint32_t)ow;             /* sum <= ow*255, so sum*inv_x < 2^32 */
    uint32_t inv_y = 0xFFFFFFFFu / (uint32_t)oh;            /* ~2^32 / oh */
    int rem_dest = oh, di = 0;

    for (int y = 0; y < oh; y++) {
        if (o <= 1) {
            memcpy(row, src + (size_t)y * w * 3, (size_t)ow * 3);
        } else {
            for (int x = 0; x < ow; x++) {
                int sx, sy;
                orient_map(o, w, h, x, y, &sx, &sy);
                const uint8_t *p = src + ((size_t)sy * w + sx) * 3;
                row[3 * x] = p[0]; row[3 * x + 1] = p[1]; row[3 * x + 2] = p[2];
            }
        }
        hscale_row(row, ow, dw, inv_x, hrow);

        int rem_src = dh;
        while (rem_src > 0) {
            int take = rem_src < rem_dest ? rem_src : rem_dest;
            for (int k = 0; k < dw * 3; k++) acc[k] += (uint32_t)take * hrow[k];
            rem_src -= take; rem_dest -= take;
            if (rem_dest == 0) {
                uint8_t *d = out + (size_t)di * dw * 3;
                for (int k = 0; k < dw * 3; k++) {
                    uint32_t v = (uint32_t)(((uint64_t)acc[k] * inv_y) >> 32);     /* value * 256 */
                    v = (v + 128u) >> 8;
                    d[k] = (uint8_t)(v > 255u ? 255u : v);
                    acc[k] = 0;
                }
                di++; rem_dest = oh;
            }
        }
    }
    free(row); free(hrow); free(acc);
    return 1;
}

/* ── import ────────────────────────────────────────────────────────────── */

typedef struct { uint8_t *buf; size_t len, cap; int fail; } sink_t;

static void sink_write(void *ctx, void *data, int n)
{
    sink_t *s = (sink_t *)ctx;
    if (s->fail || n <= 0) return;
    if (s->len + (size_t)n > s->cap) {
        size_t cap = s->cap ? s->cap : 65536;
        while (cap < s->len + (size_t)n) cap *= 2;
        uint8_t *nb = (uint8_t *)realloc(s->buf, cap);
        if (!nb) { s->fail = 1; return; }
        s->buf = nb; s->cap = cap;
    }
    memcpy(s->buf + s->len, data, (size_t)n);
    s->len += (size_t)n;
}

static int write_atomic(const char *dst, const uint8_t *data, size_t len)
{
    char tmp[4096];
    if (snprintf(tmp, sizeof tmp, "%s.tmp", dst) >= (int)sizeof tmp) return FRAME_E_ARG;
    FILE *f = fopen(tmp, "wb");
    if (!f) return FRAME_E_IO;
    int ok = fwrite(data, 1, len, f) == len && fflush(f) == 0 && fsync(fileno(f)) == 0;
    if (fclose(f) != 0) ok = 0;
    if (!ok || rename(tmp, dst) != 0) { unlink(tmp); return FRAME_E_IO; }
    return FRAME_OK;
}

int frame_import(const char *src, const char *dst, int max_w, int max_h, int quality, frame_info_t *info)
{
    if (!src || !dst || max_w < 1 || max_h < 1) return FRAME_E_ARG;
    if (max_w > FRAME_MAX_W) max_w = FRAME_MAX_W;
    if (max_h > FRAME_MAX_H) max_h = FRAME_MAX_H;
    if (quality < 1 || quality > 100) quality = FRAME_QUALITY;

    int rc = FRAME_OK;
    size_t flen = 0;
    uint8_t *file = read_file(src, &flen, &rc);
    if (!file) return rc;

    int w = 0, h = 0, n = 0;
    if (!stbi_info_from_memory(file, (int)flen, &w, &h, &n) || w < 1 || h < 1 ||
        w > FRAME_MAX_SRC_DIM || h > FRAME_MAX_SRC_DIM || (long)w * h > FRAME_MAX_SRC_PIX) {
        free(file); return FRAME_E_FORMAT;
    }
    if (file[0] == 0xFF && !jpeg_complete(file, flen)) { free(file); return FRAME_E_FORMAT; }
    int o = frame_exif_orientation(file, flen);        /* a PNG has no 0xFFD8 start: 1 */

    uint8_t *pix = stbi_load_from_memory(file, (int)flen, &w, &h, &n, 3);
    free(file);
    if (!pix) return FRAME_E_FORMAT;                   /* truncated / corrupt (or, rarely, out of memory) */

    int ow = (o >= 5) ? h : w, oh = (o >= 5) ? w : h, dw, dh;
    fit(ow, oh, max_w, max_h, &dw, &dh);

    uint8_t *small = (uint8_t *)malloc((size_t)dw * dh * 3);
    if (!small) { stbi_image_free(pix); return FRAME_E_NOMEM; }
    int ok = box_scale(pix, w, h, o, dw, dh, small);
    stbi_image_free(pix);
    if (!ok) { free(small); return FRAME_E_NOMEM; }

    sink_t s = {0};
    int enc = stbi_write_jpg_to_func(sink_write, &s, dw, dh, 3, small, quality);
    free(small);
    if (!enc || s.fail) { free(s.buf); return FRAME_E_NOMEM; }

    rc = write_atomic(dst, s.buf, s.len);
    if (rc == FRAME_OK && info) {
        info->src_w = w; info->src_h = h; info->orient = o;
        info->out_w = dw; info->out_h = dh; info->out_bytes = (long)s.len;
    }
    free(s.buf);
    return rc;
}

int frame_store(const char *src, const char *dst, int max_w, int max_h, int quality, frame_info_t *info)
{
    if (!src || !dst || max_w < 1 || max_h < 1) return FRAME_E_ARG;
    if (max_w > FRAME_MAX_W) max_w = FRAME_MAX_W;
    if (max_h > FRAME_MAX_H) max_h = FRAME_MAX_H;

    int rc = FRAME_OK;
    size_t flen = 0;
    uint8_t *file = read_file(src, &flen, &rc);
    if (!file) return rc;

    int w = 0, h = 0, n = 0;
    int keep = flen >= 4 && file[0] == 0xFF && file[1] == 0xD8 && jpeg_complete(file, flen) &&
               stbi_info_from_memory(file, (int)flen, &w, &h, &n) && w >= 1 && h >= 1 &&
               w <= max_w && h <= max_h && frame_exif_orientation(file, flen) == 1;
    if (!keep) { free(file); return frame_import(src, dst, max_w, max_h, quality, info); }

    rc = write_atomic(dst, file, flen);
    free(file);
    if (rc == FRAME_OK && info) {
        info->src_w = w; info->src_h = h; info->orient = 1;
        info->out_w = w; info->out_h = h; info->out_bytes = (long)flen;
    }
    return rc;
}

/* ── load ──────────────────────────────────────────────────────────────── */

int frame_load(const char *path, uint32_t *dst, int dst_w, int dst_h, int stride, frame_info_t *out)
{
    if (!path || !dst || dst_w < 1 || dst_h < 1 || stride < dst_w) return FRAME_E_ARG;
    int rc = FRAME_OK;
    size_t flen = 0;
    uint8_t *file = read_file(path, &flen, &rc);
    if (!file) return rc;
    int w = 0, h = 0, n = 0;
    uint8_t *pix = stbi_load_from_memory(file, (int)flen, &w, &h, &n, 3);
    free(file);
    if (!pix) return FRAME_E_FORMAT;

    for (int y = 0; y < dst_h; y++) memset(dst + (size_t)y * stride, 0, (size_t)dst_w * 4);

    int ox = (dst_w - w) / 2, oy = (dst_h - h) / 2;     /* negative: the picture is clipped, still centred */
    int x0 = ox < 0 ? -ox : 0, y0 = oy < 0 ? -oy : 0;
    int cw = w - x0, ch = h - y0;
    if (cw > dst_w) cw = dst_w;
    if (ch > dst_h) ch = dst_h;
    for (int y = 0; y < ch; y++) {
        const uint8_t *s = pix + ((size_t)(y0 + y) * w + x0) * 3;
        uint32_t *d = dst + (size_t)(oy + y0 + y) * stride + (ox + x0);
        for (int x = 0; x < cw; x++, s += 3)
            d[x] = ((uint32_t)s[0] << 16) | ((uint32_t)s[1] << 8) | s[2];
    }
    if (out) { memset(out, 0, sizeof *out); out->out_w = w; out->out_h = h; out->src_w = w; out->src_h = h; out->orient = 1; }
    stbi_image_free(pix);
    return FRAME_OK;
}
