/*
 * vid_ring.c — see vid_ring.h.
 *
 * Memory ordering (the target is a Cortex-A8: weakly ordered, so a plain store
 * can become visible to the other thread after a later one; x86 would hide that).
 *
 *   producer:  write the slot / samples / tag          (plain stores)
 *              store head+1 with RELEASE               (publishes all of the above)
 *   consumer:  load head with ACQUIRE                  (pairs with that release)
 *              read the slot / samples / tag           (now guaranteed up to date)
 *              store tail+1 with RELEASE               (publishes "I am done reading")
 *   producer:  load tail with ACQUIRE before it writes into a slot the consumer
 *              just left, so its writes cannot be reordered before the consumer's reads.
 *
 * A thread reads its OWN counter relaxed: nobody else writes it.  The tag counters
 * follow the same pattern and are published before the sample counter, so seeing
 * the new head (acquire) also shows the tag that goes with it.
 */
#include "vid_ring.h"

#include <stdlib.h>
#include <string.h>

#define LD_ACQ(p)    atomic_load_explicit((p), memory_order_acquire)
#define LD_RLX(p)    atomic_load_explicit((p), memory_order_relaxed)
#define ST_REL(p, v) atomic_store_explicit((p), (v), memory_order_release)
#define ST_RLX(p, v) atomic_store_explicit((p), (v), memory_order_relaxed)

static int pow2(unsigned n) { return n && !(n & (n - 1)); }

/* ---- frames ---------------------------------------------------------------- */

int vid_fring_init(VidFrameRing *r, unsigned slots, int w, int h)
{
    memset(r, 0, sizeof *r);
    if (!pow2(slots) || w <= 0 || h <= 0) return -1;
    int wp = (w + 15) & ~15, hp = (h + 15) & ~15;
    size_t ysz = (size_t)wp * hp, csz = (size_t)(wp / 2) * (hp / 2), per = ysz + 2 * csz;
    r->slot = calloc(slots, sizeof *r->slot);
    r->mem  = malloc(per * slots);
    if (!r->slot || !r->mem) { vid_fring_free(r); return -1; }
    for (unsigned i = 0; i < slots; i++) {
        VidFrame *f = &r->slot[i];
        f->y = r->mem + per * i;
        f->cb = f->y + ysz;
        f->cr = f->cb + csz;
        f->ys = wp; f->cs = wp / 2; f->w = w; f->h = h;
    }
    r->mask = slots - 1; r->w = w; r->h = h; r->hp = hp;
    return 0;
}

void vid_fring_free(VidFrameRing *r)
{
    free(r->slot); free(r->mem);
    memset(r, 0, sizeof *r);
}

void vid_fring_reset(VidFrameRing *r)
{
    ST_RLX(&r->head, 0); ST_RLX(&r->tail, 0);
}

VidFrame *vid_fring_prod_slot(VidFrameRing *r)
{
    unsigned h = LD_RLX(&r->head), t = LD_ACQ(&r->tail);
    if (h - t > r->mask) return NULL;             /* full */
    return &r->slot[h & r->mask];
}

void vid_fring_prod_commit(VidFrameRing *r)
{
    ST_REL(&r->head, LD_RLX(&r->head) + 1);
}

const VidFrame *vid_fring_peek(VidFrameRing *r)
{
    unsigned t = LD_RLX(&r->tail), h = LD_ACQ(&r->head);
    if (h == t) return NULL;
    return &r->slot[t & r->mask];
}

void vid_fring_release(VidFrameRing *r)
{
    ST_REL(&r->tail, LD_RLX(&r->tail) + 1);
}

unsigned vid_fring_count(VidFrameRing *r)
{
    unsigned t = LD_ACQ(&r->tail);
    return LD_ACQ(&r->head) - t;
}

/* ---- PCM -------------------------------------------------------------------- */

int vid_pring_init(VidPcmRing *r, unsigned frames, int nch, int rate)
{
    memset(r, 0, sizeof *r);
    if (!pow2(frames) || nch < 1 || nch > 2 || rate <= 0) return -1;
    r->pcm = malloc((size_t)frames * nch * sizeof *r->pcm);
    if (!r->pcm) return -1;
    r->mask = frames - 1; r->nch = nch;
    r->inv_rate = 1.0 / (double)rate;
    return 0;
}

void vid_pring_free(VidPcmRing *r)
{
    free(r->pcm);
    memset(r, 0, sizeof *r);
}

void vid_pring_reset(VidPcmRing *r)
{
    ST_RLX(&r->head, 0); ST_RLX(&r->tail, 0);
    ST_RLX(&r->thead, 0); ST_RLX(&r->ttail, 0);
}

unsigned vid_pring_space(VidPcmRing *r)
{
    unsigned t = LD_ACQ(&r->tail);
    unsigned h = LD_ACQ(&r->head);
    return r->mask + 1 - (h - t);
}

unsigned vid_pring_fill(VidPcmRing *r)
{
    unsigned t = LD_ACQ(&r->tail);
    return LD_ACQ(&r->head) - t;
}

unsigned vid_pring_write(VidPcmRing *r, const int16_t *src, unsigned frames, double pts)
{
    unsigned h = LD_RLX(&r->head), t = LD_ACQ(&r->tail);
    unsigned th = LD_RLX(&r->thead), tt = LD_ACQ(&r->ttail);
    if (!frames || frames > r->mask + 1 - (h - t)) return 0;
    if (th - tt >= VID_PRING_TAGS) return 0;
    unsigned cap = r->mask + 1, pos = h & r->mask, first = cap - pos;
    if (first > frames) first = frames;
    memcpy(r->pcm + (size_t)pos * r->nch, src, (size_t)first * r->nch * sizeof *src);
    if (first < frames)
        memcpy(r->pcm, src + (size_t)first * r->nch, (size_t)(frames - first) * r->nch * sizeof *src);
    r->tag[th & (VID_PRING_TAGS - 1)] = (VidPcmTag){ h, pts };
    ST_REL(&r->thead, th + 1);                    /* tag first, then the samples that own it */
    ST_REL(&r->head, h + frames);
    return frames;
}

unsigned vid_pring_read(VidPcmRing *r, int16_t *dst, unsigned max, double *pts_first)
{
    unsigned t = LD_RLX(&r->tail), h = LD_ACQ(&r->head);
    unsigned n = h - t;
    if (n > max) n = max;
    if (!n) return 0;
    /* the tag whose block holds frame t: step past tags that start at or before t */
    unsigned th = LD_ACQ(&r->thead), tt = LD_RLX(&r->ttail), tt0 = tt;
    while (tt + 1 != th && (int32_t)(r->tag[(tt + 1) & (VID_PRING_TAGS - 1)].start - t) <= 0) tt++;
    if (tt != tt0) ST_REL(&r->ttail, tt);
    if (pts_first) {
        const VidPcmTag *g = &r->tag[tt & (VID_PRING_TAGS - 1)];
        *pts_first = g->pts + (double)(int32_t)(t - g->start) * r->inv_rate;
    }
    unsigned cap = r->mask + 1, pos = t & r->mask, first = cap - pos;
    if (first > n) first = n;
    memcpy(dst, r->pcm + (size_t)pos * r->nch, (size_t)first * r->nch * sizeof *dst);
    if (first < n)
        memcpy(dst + (size_t)first * r->nch, r->pcm, (size_t)(n - first) * r->nch * sizeof *dst);
    ST_REL(&r->tail, t + n);
    return n;
}
