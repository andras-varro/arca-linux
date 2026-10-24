/*
 * vid_dec.c — see vid_dec.h.
 *
 * Loop order (design, section 8): PCM first while the audio ring holds less than
 * the target, because audio is the clock and an empty ring is audible; then a
 * video frame while a slot is free; otherwise sleep 5 ms.  Decoding audio drives
 * the demuxer, so video packets queue inside pl_mpeg's own video buffer meanwhile
 * (it grows on demand); that is the demux skew the 512 KB main buffer is for.
 */
#define _GNU_SOURCE
#include "vid_dec.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define PLM_BUFFER_DEFAULT_SIZE (512 * 1024)
#define PL_MPEG_IMPLEMENTATION
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#include "pl_mpeg.h"
#pragma GCC diagnostic pop

#define READ_MAX   16384          /* bytes per read() into the plm_buffer          */
#define POLL_MS    100            /* slice of the blocking wait that checks stop   */
#define IDLE_NS    5000000L       /* nothing to do: sleep 5 ms                     */
#define AUDIO_BLK  PLM_AUDIO_SAMPLES_PER_FRAME

struct VidDec {
    plm_t       *plm;
    int          fd;
    atomic_int  *stop;
    VidDecInfo   info;
    VidDecStats  st;
    int          at_end;          /* the load callback saw EOF / error / stop      */
};

static int stopped(const VidDec *d) { return atomic_load_explicit(d->stop, memory_order_acquire); }

/* plm_buffer load callback: block (in POLL_MS slices) until some bytes arrive,
 * the stream ends, or stop is raised; the last two end the buffer. */
static void load_cb(plm_buffer_t *b, void *u)
{
    VidDec *d = u;
    uint8_t tmp[READ_MAX];
    if (d->at_end) { plm_buffer_signal_end(b); return; }
    for (;;) {
        if (stopped(d)) break;
        struct pollfd p = { .fd = d->fd, .events = POLLIN };
        int r = poll(&p, 1, POLL_MS);
        if (r < 0) { if (errno == EINTR) continue; break; }
        if (r == 0) continue;
        ssize_t n = read(d->fd, tmp, sizeof tmp);
        if (n < 0) { if (errno == EINTR || errno == EAGAIN) continue; break; }
        if (n == 0) break;
        plm_buffer_write(b, tmp, (size_t)n);
        atomic_fetch_add_explicit(&d->st.bytes_in, (long)n, memory_order_relaxed);
        return;
    }
    d->at_end = 1;
    plm_buffer_signal_end(b);
}

int vid_dec_open(VidDec **out, int fd, atomic_int *stop)
{
    *out = NULL;
    VidDec *d = calloc(1, sizeof *d);
    if (!d) return -1;
    d->fd = fd; d->stop = stop;
    plm_buffer_t *buf = plm_buffer_create_with_capacity(PLM_BUFFER_DEFAULT_SIZE);
    if (!buf) { free(d); return -1; }
    plm_buffer_set_load_callback(buf, load_cb, d);
    d->plm = plm_create_with_buffer(buf, 1);              /* owns buf from here */
    if (!d->plm || !plm_has_headers(d->plm) || plm_get_width(d->plm) <= 0) {
        if (d->plm) plm_destroy(d->plm); else plm_buffer_destroy(buf);
        free(d);
        return -1;
    }
    plm_set_loop(d->plm, 0);
    d->info.w = plm_get_width(d->plm);
    d->info.h = plm_get_height(d->plm);
    d->info.fps = plm_get_framerate(d->plm);
    d->info.has_audio = plm_get_num_audio_streams(d->plm) > 0 && plm_get_samplerate(d->plm) > 0;
    d->info.audio_rate = d->info.has_audio ? plm_get_samplerate(d->plm) : 0;
    plm_set_audio_enabled(d->plm, d->info.has_audio);
    *out = d;
    return 0;
}

const VidDecInfo  *vid_dec_info(const VidDec *d)  { return &d->info; }
const VidDecStats *vid_dec_stats(const VidDec *d) { return &d->st; }

void vid_dec_close(VidDec *d)
{
    if (!d) return;
    if (d->plm) plm_destroy(d->plm);
    free(d);
}

static void copy_plane(uint8_t *dst, int dstride, int drows, const plm_plane_t *s)
{
    int rows = s->height < (unsigned)drows ? (int)s->height : drows;
    if ((int)s->width == dstride) { memcpy(dst, s->data, (size_t)dstride * rows); return; }
    int w = (int)s->width < dstride ? (int)s->width : dstride;
    for (int y = 0; y < rows; y++) memcpy(dst + (size_t)y * dstride, s->data + (size_t)y * s->width, (size_t)w);
}

static void nap(void)
{
    struct timespec ts = { 0, IDLE_NS };
    nanosleep(&ts, NULL);
}

static int16_t clamp16(float x)
{
    float v = x * 32767.0f;
    return v >= 32767.0f ? 32767 : v <= -32768.0f ? -32768 : (int16_t)v;
}

int vid_dec_run(VidDec *d, VidFrameRing *frames, VidPcmRing *pcm, const VidDecCfg *cfg)
{
    int nch = cfg->out_channels;
    int audio = d->info.has_audio && pcm;
    if ((nch != 1 && nch != 2) || (audio && pcm->nch != nch)
        || !frames || frames->w != d->info.w || frames->h != d->info.h) {
        atomic_store(&d->st.state, VID_DEC_ERROR);
        return -1;
    }
    double target_s = cfg->pcm_target_s > 0 ? cfg->pcm_target_s : 1.0;
    double target = target_s * d->info.audio_rate;
    float   fbuf[AUDIO_BLK * 2];
    int16_t sbuf[AUDIO_BLK * 2];
    int audio_end = !audio, video_end = 0, rc = 0;

    atomic_store(&d->st.state, VID_DEC_RUNNING);
    for (;;) {
        if (stopped(d)) { rc = 1; break; }
        if (audio_end && video_end) break;
        int worked = 0;

        if (!audio_end && (double)vid_pring_fill(pcm) < target && vid_pring_space(pcm) >= AUDIO_BLK) {
            plm_samples_t *s = plm_decode_audio(d->plm);
            if (stopped(d)) { rc = 1; break; }
            if (!s) audio_end = 1;
            else {
                int n = (int)s->count;
                if (n > AUDIO_BLK) n = AUDIO_BLK;
                if (nch == 1) for (int i = 0; i < n; i++) fbuf[i] = (s->interleaved[2 * i] + s->interleaved[2 * i + 1]) * 0.5f;
                else memcpy(fbuf, s->interleaved, (size_t)n * 2 * sizeof(float));
                if (cfg->pcm_cb) cfg->pcm_cb(cfg->user, fbuf, n, sbuf);
                else for (int i = 0; i < n * nch; i++) sbuf[i] = clamp16(fbuf[i]);
                if (vid_pring_write(pcm, sbuf, (unsigned)n, s->time))
                    atomic_fetch_add_explicit(&d->st.pcm_frames, n, memory_order_relaxed);
                worked = 1;
            }
        } else if (!video_end) {
            VidFrame *slot = vid_fring_prod_slot(frames);
            if (slot) {
                plm_frame_t *f = plm_decode_video(d->plm);
                if (stopped(d)) { rc = 1; break; }
                if (!f) video_end = 1;
                else {
                    copy_plane(slot->y,  slot->ys, frames->hp,     &f->y);
                    copy_plane(slot->cb, slot->cs, frames->hp / 2, &f->cb);
                    copy_plane(slot->cr, slot->cs, frames->hp / 2, &f->cr);
                    slot->pts = f->time;
                    vid_fring_prod_commit(frames);
                    atomic_fetch_add_explicit(&d->st.frames, 1, memory_order_relaxed);
                    worked = 1;
                }
            }
        }
        if (!worked && !(audio_end && video_end)) {
            /* Audio at EOF while the rings are full, or video EOF with PCM waiting: wait for the consumer. */
            if (video_end && audio_end) break;
            nap();
        }
    }
    atomic_store(&d->st.state, rc == 1 ? VID_DEC_STOPPED : VID_DEC_EOF);
    return rc;
}
