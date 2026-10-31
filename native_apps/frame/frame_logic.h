/*
 * frame_logic.h — the picture frame's pure decisions: which files are importable, what a source picture
 * is called in the library (a stable key, so a second import skips it), and the order a slideshow walks.
 * No I/O, no framebuffer; tests/frame_logic_test.c drives it on the host.
 */
#ifndef FRAME_LOGIC_H
#define FRAME_LOGIC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Is this directory entry name a JPEG or PNG (extension, any case)?  Hidden names ("." first), which
 * includes the "._" resource-fork copies a Mac leaves on a stick, are not pictures. */
bool frame_is_image_name(const char *name);

/* The library file name for a source picture: "<sanitised stem>-<hash of basename>-<size hex>.jpg".
 * Same basename and size give the same key on every run; a different size or name gives another.  The
 * basename is the part after the last '/'.  Returns the length, or -1 if `n` is too small. */
int frame_key(const char *src_path, long size, char *out, size_t n);

/* The order a slideshow walks `n` pictures.  Sequential: 0..n-1 round and round.  Shuffle: a fresh random
 * permutation each round (Fisher-Yates), and the first picture of a new round is never the last of the
 * one before while n > 1.  The array is caller-owned (n ints). */
typedef struct {
    int     *idx;
    int      n;
    int      pos;           /* position of the picture on screen now */
    bool     shuffle;
    uint32_t rng;
} FrameOrder;

void frame_order_init(FrameOrder *o, int *storage, int n, bool shuffle, uint32_t seed);
int  frame_order_cur(const FrameOrder *o);          /* picture index on screen now, -1 when n == 0 */
int  frame_order_next(FrameOrder *o);               /* advance (a new shuffled round at the end); the new index */
int  frame_order_prev(FrameOrder *o);               /* step back; before the start it wraps to the end of the round */
int  frame_order_peek_next(const FrameOrder *o);    /* what next() will return, or -1 when it is not known yet
                                                       (the end of a shuffled round) */

/* Fill-the-screen choice (start menu FILL, saved as "fill" in frame.conf). */
enum { FRAME_FILL_OFF = 0, FRAME_FILL_LANDSCAPE, FRAME_FILL_ALL, FRAME_FILL_N };

/* Does the setting fill a pw x ph picture on an sw x sh screen?  LANDSCAPE: only a picture whose orientation
 * matches the screen's (wider-than-tall on a wider-than-tall screen, a square on a square one); ALL: every
 * picture; anything else (OFF, a bad value): none. */
static inline bool frame_fill_wanted(int setting, int pw, int ph, int sw, int sh)
{
    if (setting == FRAME_FILL_ALL) return true;
    if (setting != FRAME_FILL_LANDSCAPE) return false;
    int p = (pw > ph) - (pw < ph), s = (sw > sh) - (sw < sh);
    return p == s;
}

#endif
