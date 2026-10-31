/*
 * frame_core.h — picture-frame library core: import a photo into the optimised library, load one back.
 *
 * The slideshow plays from a LIBRARY of pictures stored at their COVER size (scaled so they cover the panel, aspect
 * kept, never enlarged; frame_load_mode fits or fills them at display time), EXIF orientation applied and re-encoded as a baseline JPEG.  Importing is a
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

/* Decode src (JPEG or PNG), apply its orientation, box-filter it down to the COVER size of the max_w x max_h screen (frame_cover_size)
* (never enlarged), encode as JPEG at `quality` (0 -> FRAME_QUALITY) and write dst atomically
 * (dst.tmp, fsync, rename).  info may be NULL.  Returns FRAME_OK or a FRAME_E_* code; on failure no
 * dst and no tmp is left behind. */
int frame_import(const char *src, const char *dst, int max_w, int max_h, int quality, frame_info_t *info);

/* Put src into the library at dst.  A complete JPEG that is already its own cover size with EXIF orientation 1
 * (or none) is written byte for byte (atomically, like frame_import) and info says out == src; anything else
 * (PNG, oversize, rotated, truncated) goes through frame_import, so the library keeps one format and a bad
 * file fails exactly as it does there. */
int frame_store(const char *src, const char *dst, int max_w, int max_h, int quality, frame_info_t *info);

/* Decode a library picture into an XRGB8888 buffer: the whole dst_w x dst_h area is cleared to black,
 * then the picture is centred (clipped, centred, if it is larger than the buffer).  stride is in
 * PIXELS.  If out is non-NULL it receives the picture's own size (out_w/out_h).  Returns FRAME_OK or
 * FRAME_E_*. */
int frame_load(const char *path, uint32_t *dst, int dst_w, int dst_h, int stride, frame_info_t *out);

/* Cover size for a (oriented) ow x oh picture on an sw x sh screen: scaled so the copy covers the screen
 * (the relatively shorter edge reaches it: 4:3 -> 800x600 on 800x480), never enlarged, neither side above
 * FRAME_MAX_W / FRAME_MAX_H.  frame_import stores this size. */
void frame_cover_size(int ow, int oh, int sw, int sh, int *dw, int *dh);

/* Fill geometry for a pw x ph stored picture on an sw x sh screen: it is shown scaled to fw x fh (unscaled
 * when it already covers, else enlarged just enough) and cropped at (cx, cy), centred. */
void frame_fill_geom(int pw, int ph, int sw, int sh, int *fw, int *fh, int *cx, int *cy);

/* Like frame_load, for a library picture.  fill_setting is FRAME_FILL_* (frame_logic.h): when it applies to this
 * picture the screen is filled by a centre crop; otherwise the picture is fitted (letterbox): a stored copy
 * larger than the screen is area-averaged down, a smaller one is left at its size.  stride in PIXELS. */
int frame_load_mode(const char *path, uint32_t *dst, int dst_w, int dst_h, int stride, int fill_setting, frame_info_t *out);

#endif
