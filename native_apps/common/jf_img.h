/*
 * jf_img.h — decode a JPEG (from memory) into a fixed-size square XRGB8888 tile,
 * and keep the server's JPEG bytes in a byte-bounded on-disk cache.
 *
 * Pure of sockets and screens.  The decoder is the vendored stb_image.h (JPEG only,
 * no stdio, no SIMD; see jf_img.c).  The scale loop is fixed-point and divide-free
 * per pixel (a Cortex-A8 has no hardware divide).
 *
 * Cache file name: "<32 hex album id>_<hex tag>_<size>.jpg".  The tag is the
 * server's image tag, so a changed cover is a different file; an empty tag maps
 * to "0".  Anything else in an id or tag is refused, because both reach a path.
 */
#ifndef ARCA_JF_IMG_H
#define ARCA_JF_IMG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#define JF_IMG_MAX_SIZE     512      /**< largest square a caller may ask for        */
#define JF_IMG_MAX_SRC_DIM  1600     /**< a source wider/taller than this is refused */
#define JF_IMG_TAG_MAX      40
#define JF_IMG_KEY_MAX      (32 + 1 + JF_IMG_TAG_MAX + 1 + 3 + 4 + 1)

/* ── pure geometry ─────────────────────────────────────────────────────── */

/**
 * Fit an sw x sh source inside a size x size square, aspect kept, scaled UP as
 * well as down.  Writes the scaled extent (dw, dh >= 1) and the offset that
 * centres it.  False for a non-positive source or a size outside 1..JF_IMG_MAX_SIZE.
 */
bool jf_img_fit(int sw, int sh, int size, int *dw, int *dh, int *ox, int *oy);

/**
 * Decode JPEG `data` into `out` (size*size uint32_t, 0xFFrrggbb), letterboxed in
 * black.  False on a bad/unsupported/oversized image or bad arguments; `out` is
 * then untouched.  Allocates (and frees) a transient copy of the source pixels.
 */
bool jf_img_decode_square(const uint8_t *data, size_t len, int size, uint32_t *out);

/** Bilinear-fit raw RGB (3 bytes/pixel) into the square; the scaler under the decoder, exported for tests. */
void jf_img_scale_rgb(const uint8_t *rgb, int sw, int sh, int size, uint32_t *out);

/* ── cache key ─────────────────────────────────────────────────────────── */

/** Build the file name.  Length, or -1 for a bad id/tag/size or a buffer too small. */
int jf_img_key(char *out, size_t cap, const char *album_id, const char *tag, int size);

/* ── cache budget and eviction (pure) ──────────────────────────────────── */

#define JF_IMG_CACHE_MAX_BYTES  (150LL * 1024 * 1024)   /**< absolute ceiling of the cache            */
#define JF_IMG_CACHE_FREE_DIV   4                       /**< ... and a quarter of the free space      */
#define JF_IMG_FILE_MAX         (512 * 1024)            /**< a cached file larger than this is corrupt */

/** The byte budget: min(JF_IMG_CACHE_MAX_BYTES, free_bytes / 4); 0 for a negative input. */
long long jf_img_cap_bytes(long long free_bytes);

/**
 * Which of `n` files to delete so the sum of sizes drops to at most `cap`: oldest by
 * `mtime` first, ties broken by the lower index.  `total` is the current sum.  Writes
 * the indices to `out` (room for n) and returns how many; 0 when total <= cap.
 */
int jf_img_evict_bytes(const time_t *mtime, const long long *size, int n, long long total, long long cap, int *out);

/* ── cache on disk: the server's JPEG bytes ────────────────────────────── */

typedef struct {
    char      dir[200];
    long long cap;       /**< byte budget, fixed at open                       */
    long long total;     /**< running sum of the cache files' sizes            */
    int       files;     /**< running count                                    */
    bool      open;
} JfImgCache;

/**
 * Open the cache: create `dir`, read the budget from statvfs of its filesystem
 * (jf_img_cap_bytes of the free space, once), remove leftover temporaries, and sum the
 * existing files with ONE readdir.  Every later call keeps the sums by arithmetic.
 */
bool jf_img_cache_open(JfImgCache *c, const char *dir);

/**
 * Fetch a cached file's bytes into a malloc'd buffer (caller frees).  A hit refreshes the
 * file's mtime (least-recently-used).  A missing file is a plain miss; an empty or
 * oversized one is dropped and reported as a miss.  The bytes are NOT checked to be an
 * image: a caller whose decode fails calls jf_img_cache_drop.
 */
bool jf_img_cache_get(JfImgCache *c, const char *key, uint8_t **data, size_t *len);

/** Delete one cached file and take its size off the running total. */
void jf_img_cache_drop(JfImgCache *c, const char *key);

/**
 * Store bytes atomically (temporary file, then rename).  When the total then exceeds the
 * budget, delete the oldest files (never this one) until it fits; this is the only
 * time the directory is read again.  False if the write failed or `len` alone exceeds
 * the budget (nothing is left under the final name).
 */
bool jf_img_cache_put(JfImgCache *c, const char *key, const uint8_t *data, size_t len);

#endif
