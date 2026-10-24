/*
 * vid_ring.h — the two single-producer / single-consumer rings between the
 * decoder thread (producer) and the main thread (consumer) of the video player:
 * YUV 4:2:0 frame slots, and interleaved s16 PCM with a pts per block.
 *
 * No lock.  Each ring has one writer thread and one reader thread, fixed for its
 * life.  Every index is a free-running unsigned counter (power-of-two capacity, so
 * the slot is `counter & mask` and no integer divide is ever needed; the counters
 * wrap harmlessly because only differences are used).  The producer owns `head`,
 * the consumer owns `tail`; each reads the other's with acquire and publishes its
 * own with release.  The release store of `head` is what makes the data written
 * before it visible to the consumer; the release store of `tail` is what tells the
 * producer the slot may be overwritten.  See vid_ring.c for why this matters on the
 * ARM target, where a missing barrier is not hidden by x86's ordering.
 *
 * reset() and free() are for when BOTH sides are quiescent (decoder joined).
 */
#ifndef ARCA_VID_RING_H
#define ARCA_VID_RING_H

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

/* ---- frames ---------------------------------------------------------------- */

typedef struct {
    double   pts;            /**< seconds, as the demuxer stamped it         */
    int      w, h;           /**< picture size                               */
    int      ys, cs;         /**< luma / chroma row stride in bytes          */
    uint8_t *y, *cb, *cr;    /**< planes: ys*hp, cs*(hp/2), cs*(hp/2) bytes  */
} VidFrame;

typedef struct {
    VidFrame        *slot;
    uint8_t         *mem;
    unsigned         mask;               /**< slots - 1 */
    int              w, h, hp;           /**< hp: h rounded up to 16 */
    _Atomic unsigned head, tail;
} VidFrameRing;

/** `slots` must be a power of two.  Planes are sized for w x h padded to 16.  0 ok, -1 failure. */
int  vid_fring_init(VidFrameRing *r, unsigned slots, int w, int h);
void vid_fring_free(VidFrameRing *r);
void vid_fring_reset(VidFrameRing *r);

/** Producer: the next free slot to fill, or NULL when full.  Fill it, set pts, then commit. */
VidFrame *vid_fring_prod_slot(VidFrameRing *r);
void      vid_fring_prod_commit(VidFrameRing *r);
/** Consumer: the oldest committed frame, or NULL.  Valid until release(). */
const VidFrame *vid_fring_peek(VidFrameRing *r);
void            vid_fring_release(VidFrameRing *r);
/** Frames committed and not yet released (either side; a snapshot). */
unsigned vid_fring_count(VidFrameRing *r);

/* ---- PCM -------------------------------------------------------------------- */

#define VID_PRING_TAGS 256            /**< pts tags in flight (a write is refused when none is free) */

typedef struct { uint32_t start; double pts; } VidPcmTag;

typedef struct {
    int16_t         *pcm;
    unsigned         mask;               /**< capacity in frames - 1 */
    int              nch;
    double           inv_rate;           /**< seconds per frame      */
    VidPcmTag        tag[VID_PRING_TAGS];
    _Atomic unsigned head, tail;         /**< frames written / read                 */
    _Atomic unsigned thead, ttail;       /**< tags written / consumer's current tag */
} VidPcmRing;

/** `frames` must be a power of two; nch 1 or 2; rate in Hz.  0 ok, -1 failure. */
int  vid_pring_init(VidPcmRing *r, unsigned frames, int nch, int rate);
void vid_pring_free(VidPcmRing *r);
void vid_pring_reset(VidPcmRing *r);

/** Producer: append `frames` frames whose first sample has time `pts`.  All or nothing: returns frames, or 0 when there is no room. */
unsigned vid_pring_write(VidPcmRing *r, const int16_t *src, unsigned frames, double pts);
/** Consumer: take up to `max` frames; *pts_first (if non-NULL) is the time of the first one returned.  Returns frames taken. */
unsigned vid_pring_read(VidPcmRing *r, int16_t *dst, unsigned max, double *pts_first);
/** Frames waiting (a snapshot). */
unsigned vid_pring_fill(VidPcmRing *r);
unsigned vid_pring_space(VidPcmRing *r);

#endif
