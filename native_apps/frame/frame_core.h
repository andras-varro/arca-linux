/*
 * frame_core.h — picture-frame library core: import a photo into the optimised library, load one back.
 *
 * The slideshow plays from a LIBRARY of pictures stored already fitted to the panel (letterbox, aspect
 * kept, never enlarged), EXIF orientation applied and re-encoded as a baseline JPEG.  Importing is a
 * one-time, slow job (a 12 MP photo decodes in seconds on the 600 MHz core); loading a library picture
 * is cheap.  Originals are not kept.  No UI, no framebuffer: the caller passes the target size (the
 * panel's drawable rectangle, SCREEN_VISIBLE_WIDTH x SCREEN_VISIBLE_HEIGHT, which is a runtime value).
 *
 * Linking: this file carries its own stb_image (JPEG + PNG) and stb_image_write implementations, so it
 * must NOT be linked together with common/jf_img.c (JPEG-only stb_image implementation, same symbols).
 */
#ifndef FRAME_CORE_H
#define FRAME_CORE_H

#include <stddef.h>
#include <stdint.h>

#define FRAME_MAX_W        1024     /* stored pictures never exceed this, whatever the caller asks */
#define FRAME_MAX_H        1024
#define FRAME_MAX_SRC_DIM  16384    /* source side limit; larger is refused before any pixel is decoded */
#define FRAME_MAX_SRC_PIX  (64L * 1024 * 1024)
#define FRAME_QUALITY      90       /* default JPEG quality for the stored picture */

#define FRAME_OK            0
#define FRAME_E_IO         -1       /* open / read / write / rename failed */
#define FRAME_E_FORMAT     -2       /* not a JPEG/PNG, truncated, corrupt, or absurdly large */
#define FRAME_E_NOMEM      -3
#define FRAME_E_ARG        -4

typedef struct {
    int  src_w, src_h;              /* as stored in the source, before orientation */
    int  orient;                    /* EXIF orientation applied, 1..8 (1 for PNG / no tag) */
    int  out_w, out_h;              /* size of the stored picture */
    long out_bytes;                 /* size of the stored file */
} frame_info_t;

/* EXIF orientation (1..8) of a JPEG held in memory; 1 when there is no APP1 Exif / tag / a bad value.
 * Walks the marker segments up to SOS only, both TIFF byte orders, every read bounds-checked. */
int frame_exif_orientation(const uint8_t *jpeg, size_t len);

/* Decode src (JPEG or PNG), apply its orientation, box-filter it down to fit inside max_w x max_h
 * (never enlarged), encode as JPEG at `quality` (0 -> FRAME_QUALITY) and write dst atomically
 * (dst.tmp, fsync, rename).  info may be NULL.  Returns FRAME_OK or a FRAME_E_* code; on failure no
 * dst and no tmp is left behind. */
int frame_import(const char *src, const char *dst, int max_w, int max_h, int quality, frame_info_t *info);

/* Decode a library picture into an XRGB8888 buffer: the whole dst_w x dst_h area is cleared to black,
 * then the picture is centred (clipped, centred, if it is larger than the buffer).  stride is in
 * PIXELS.  If out is non-NULL it receives the picture's own size (out_w/out_h).  Returns FRAME_OK or
 * FRAME_E_*. */
int frame_load(const char *path, uint32_t *dst, int dst_w, int dst_h, int stride, frame_info_t *out);

#endif
