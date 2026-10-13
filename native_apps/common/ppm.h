/**
 * PPM Image Loader for Arca
 *
 * Loads P6 (binary) PPM files and converts to ARGB pixel arrays
 * suitable for direct framebuffer blitting.  Includes nearest-neighbor
 * scaling for icon resizing.
 */

#ifndef PPM_H
#define PPM_H

#include <stdint.h>

/**
 * Load a P6 binary PPM file into an ARGB pixel array.
 * Returns NULL on failure.  Caller must free() the returned buffer.
 * Sets *out_width and *out_height on success.
 */
uint32_t *ppm_load(const char *filename, int *out_width, int *out_height);

/**
 * Scale an ARGB pixel array to new dimensions (nearest-neighbor).
 * Returns NULL on failure.  Caller must free() the returned buffer.
 */
uint32_t *ppm_scale(const uint32_t *src, int src_w, int src_h,
                    int dst_w, int dst_h);

#endif
