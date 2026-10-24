/*
 * vid_dec.h — the decoder thread of the video player as a library: reads an
 * MPEG-PS byte stream from a file descriptor, decodes it with pl_mpeg, and fills
 * the two rings of vid_ring.h (YUV frame slots, s16 PCM with a pts per block).
 *
 * Two phases, so the caller learns the picture size before it sizes the rings:
 *   vid_dec_open()  creates the buffer and pl_mpeg and waits for the stream headers
 *   vid_dec_info()  w, h, fps, audio rate
 *   vid_dec_run()   the loop; returns at EOF, on stop, or on error
 * The caller runs vid_dec_run() on its own thread (nice +5 there, never RT) and
 * is the rings' consumer.  Nothing here calls audio_*, fb_* or the logger: the
 * counters are atomics and the caller logs them.
 *
 * Stop: store 1 to *stop (release).  The fd is polled in 100 ms slices that look
 * at the flag, so a blocked read ends within that, and the loop looks at it on
 * every iteration, so vid_dec_run() returns well inside 250 ms even when the
 * rings are full and nobody drains them.  The fd is not closed here.
 */
#ifndef ARCA_VID_DEC_H
#define ARCA_VID_DEC_H

#include <stdatomic.h>
#include "vid_ring.h"

typedef struct VidDec VidDec;

typedef struct {
    int    w, h;             /**< picture size                                  */
    double fps;
    int    audio_rate;       /**< Hz; 0 without audio                           */
    int    has_audio;
} VidDecInfo;

typedef struct {
    int    out_channels;     /**< 1 (mono = (L+R)/2) or 2; must match the PCM ring */
    /** Optional float -> s16 stage (level/limiter): `in` is `frames` frames of
     *  out_channels interleaved floats in -1..1.  NULL: plain clamp(x * 32767). */
    void (*pcm_cb)(void *user, const float *in, int frames, int16_t *out);
    void  *user;
    double pcm_target_s;     /**< keep this much PCM queued (0: 1.0 s)          */
} VidDecCfg;

/** Counters, written by the decoder thread, readable from any thread. */
typedef struct {
    _Atomic long frames;     /**< video frames committed                        */
    _Atomic long pcm_frames; /**< PCM frames written                            */
    _Atomic long bytes_in;   /**< bytes read from the fd                        */
    _Atomic int  state;      /**< VID_DEC_* below                               */
} VidDecStats;

enum { VID_DEC_OPEN, VID_DEC_RUNNING, VID_DEC_EOF, VID_DEC_STOPPED, VID_DEC_ERROR };

/** Reads the headers.  0 ok; -1 when no headers arrived (EOF, error or stop) — nothing is left to free then. */
int  vid_dec_open(VidDec **out, int fd, atomic_int *stop);
const VidDecInfo  *vid_dec_info(const VidDec *d);
const VidDecStats *vid_dec_stats(const VidDec *d);
/** The loop.  0 stream ended, 1 stopped, -1 error (bad config / ring mismatch). */
int  vid_dec_run(VidDec *d, VidFrameRing *frames, VidPcmRing *pcm, const VidDecCfg *cfg);
void vid_dec_close(VidDec *d);

#endif
