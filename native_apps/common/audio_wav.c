/*
 * audio_wav.c — streaming mono RIFF/WAVE reader.  Rationale in audio_wav.h.
 */

#include "audio_wav.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* One frame of the file, in bytes.  16-bit only, checked at open. */
#define WAV_BYTES_PER_SAMPLE 2

/* Frames pulled from stdio per read() batch.  Bounded so a caller asking for a
 * huge block cannot put an unbounded array on the stack. */
#define WAV_BATCH_FRAMES 1024

static unsigned long le32(const unsigned char *p)
{
    return (unsigned long)p[0] | ((unsigned long)p[1] << 8)
         | ((unsigned long)p[2] << 16) | ((unsigned long)p[3] << 24);
}

bool audio_wav_tag_ok(int tag) { return tag == 1 || tag == 0xFFFE; }

static unsigned int le16(const unsigned char *p)
{
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8);
}

/* ── The header walk (push parser; see audio_wav.h) ─────────────────────────── */

void audio_wav_hdr_init(AudioWavHdr *h)
{
    memset(h, 0, sizeof(*h));
    h->state = AWH_RIFF;
    h->need  = 12;
}

void audio_wav_hdr_skipped(AudioWavHdr *h, unsigned long long n)
{
    if (h->state != AWH_SKIP || n > h->skip_left) return;
    h->skip_left -= n;
    h->consumed  += (unsigned long)n;
    if (h->skip_left == 0) { h->state = AWH_CHUNK; h->have = 0; h->need = 8; }
}

size_t audio_wav_hdr_feed(AudioWavHdr *h, const unsigned char *p, size_t n)
{
    size_t used = 0;

    /* ⚠️ `fmt ` and `data` can arrive in any order and with anything in between —
     * LIST/INFO on our music, nothing on the vendor effects — so every unknown
     * chunk is SKIPPED by its declared size, with the RIFF odd-size pad byte.
     * Dropping that pad desynchronises the walk on any file with an odd-length
     * chunk and the next "chunk ID" is then two bytes of somebody else's payload. */
    while (used < n && h->state != AWH_DONE && h->state != AWH_BAD) {
        if (h->state == AWH_SKIP) {
            unsigned long long take = h->skip_left;
            if (take > n - used) take = n - used;
            used += (size_t)take;
            audio_wav_hdr_skipped(h, take);
            continue;
        }

        size_t take = h->need - h->have;
        if (take > n - used) take = n - used;
        memcpy(h->acc + h->have, p + used, take);
        h->have     += (unsigned int)take;
        h->consumed += (unsigned long)take;
        used        += take;
        if (h->have < h->need) break;       /* field incomplete: wait for more */

        h->have = 0;
        if (h->state == AWH_RIFF) {
            if (memcmp(h->acc, "RIFF", 4) || memcmp(h->acc + 8, "WAVE", 4)) {
                h->state = AWH_BAD;
            } else {
                h->state = AWH_CHUNK;
                h->need  = 8;
            }
        } else if (h->state == AWH_CHUNK) {
            unsigned long sz = le32(h->acc + 4);
            if (!memcmp(h->acc, "fmt ", 4)) {
                if (sz < 16) { h->state = AWH_BAD; continue; }
                h->chunk_sz = sz;
                h->state    = AWH_FMT;
                h->need     = 16;
            } else if (!memcmp(h->acc, "data", 4)) {
                h->data_size = sz;
                h->state     = h->have_fmt ? AWH_DONE : AWH_BAD;
            } else {
                h->skip_left = (unsigned long long)sz + (sz & 1);
                h->state     = AWH_SKIP;
                if (h->skip_left == 0) { h->state = AWH_CHUNK; h->need = 8; }
            }
        } else {                            /* AWH_FMT */
            h->tag      = (int)le16(h->acc);
            h->channels = (int)le16(h->acc + 2);
            h->rate     = (int)le32(h->acc + 4);
            h->bits     = (int)le16(h->acc + 14);
            h->have_fmt = true;
            h->skip_left = (unsigned long long)(h->chunk_sz - 16) + (h->chunk_sz & 1);
            h->state     = AWH_SKIP;
            if (h->skip_left == 0) { h->state = AWH_CHUNK; h->need = 8; }
        }
    }
    return used;
}

/* ── The stream source ───────────────────────────────────────────────────────── */

#define RING_MASK (AUDIO_STREAM_RING_BYTES - 1UL)

AudioStreamAct audio_stream_decide(long buffered, bool eof, bool playing, long preroll)
{
    /* ⚠️ See audio_wav.h for the rules and the reasons.  EOF first: once the peer
     * has closed, waiting for a pre-roll that can never arrive would hang a short
     * track forever, so whatever is left plays out and then the track ends. */
    if (eof)     return buffered >= 2 ? AWS_PLAY : AWS_END;
    if (playing) return buffered >= 2 ? AWS_PLAY : AWS_REBUFFER;
    return buffered >= preroll ? AWS_PLAY : AWS_SILENCE;
}

static void stream_push(AudioWavStream *s, const unsigned char *p, unsigned long n)
{
    if (s->data_known && n > s->data_left) n = s->data_left;
    unsigned long room = AUDIO_STREAM_RING_BYTES - s->count;
    if (n > room) n = room;                 /* header remainder is < 512; cannot happen */
    unsigned long tail = (s->head + s->count) & RING_MASK;
    unsigned long run  = AUDIO_STREAM_RING_BYTES - tail;
    if (run > n) run = n;
    memcpy(s->ring + tail, p, run);
    memcpy(s->ring, p + run, n - run);
    s->count += n;
    if (s->data_known) {
        s->data_left -= n;
        if (s->data_left == 0) s->eof = true;
    }
}

static void stream_header_bytes(AudioWavStream *s, const unsigned char *p, size_t n)
{
    size_t used = audio_wav_hdr_feed(&s->hdr, p, n);
    if (s->hdr.state == AWH_BAD) {
        fprintf(stderr, "audio_wav: stream is not a usable RIFF/WAVE\n");
        s->failed = s->eof = true;
    } else if (s->hdr.state == AWH_DONE) {
        if (!audio_wav_tag_ok(s->hdr.tag)) {
            fprintf(stderr, "audio_wav: stream refused — format tag 0x%04X is not PCM\n",
                    s->hdr.tag);
            s->failed = s->eof = true;
            return;
        }
        if (s->hdr.channels != s->channels || s->hdr.bits != 16 || s->hdr.rate != s->expected_rate) {
            fprintf(stderr, "audio_wav: stream refused — %d Hz %d ch %d bit, the bus is "
                            "%d Hz %d ch 16 bit and there is no resampler\n",
                    s->hdr.rate, s->hdr.channels, s->hdr.bits, s->expected_rate, s->channels);
            s->failed = s->eof = true;
            return;
        }
        s->hdr_done   = true;
        s->data_known = !(s->hdr.data_size == AUDIO_WAV_SIZE_UNKNOWN
                          || s->hdr.data_size > 0x7FFFFFFFUL);
        s->data_left  = s->hdr.data_size;
        if (s->data_known && s->data_left == 0) s->eof = true;
        if (n > used) stream_push(s, p + used, (unsigned long)(n - used));
    }
}

/* The peer closed (or the read failed for good).  ⚠️ Before the header is
 * complete that is a FAILURE, not an empty track: ending quietly would make a
 * server that hung up mid-header look like a stream that played. */
static void stream_ended(AudioWavStream *s)
{
    s->eof = true;
    if (!s->hdr_done && !s->failed) {
        fprintf(stderr, "audio_wav: stream ended before its WAV header was complete\n");
        s->failed = true;
    }
}

/* Take what the socket has; never wait.  Bounded iterations so one call cannot
 * spend a frame copying a whole ring. */
static void stream_pull(AudioWavStream *s)
{
    for (int iter = 0; iter < 16 && !s->eof && s->fd >= 0; iter++) {
        ssize_t n;
        if (!s->hdr_done) {
            unsigned char tmp[512];
            n = s->io ? s->io->read(s->fd, tmp, sizeof(tmp)) : read(s->fd, tmp, sizeof(tmp));
            if (n > 0) { stream_header_bytes(s, tmp, (size_t)n); continue; }
        } else {
            unsigned long room = AUDIO_STREAM_RING_BYTES - s->count;
            if (room == 0) return;          /* full: TCP backpressure does the rest */
            unsigned long tail = (s->head + s->count) & RING_MASK;
            unsigned long run  = AUDIO_STREAM_RING_BYTES - tail;
            if (run > room) run = room;
            if (s->data_known && run > s->data_left) run = s->data_left;
            n = s->io ? s->io->read(s->fd, s->ring + tail, run) : read(s->fd, s->ring + tail, run);
            if (n > 0) {
                s->count += (unsigned long)n;
                if (s->data_known) {
                    s->data_left -= (unsigned long)n;
                    if (s->data_left == 0) s->eof = true;
                }
                continue;
            }
        }
        if (n == 0) { stream_ended(s); break; }
        if (errno == EINTR) continue;
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            fprintf(stderr, "audio_wav: stream read failed: %s\n", strerror(errno));
            stream_ended(s);
        }
        break;
    }
}

bool audio_wav_stream_open(AudioWavStream *s, int fd, int expected_rate, int channels)
{
    return audio_wav_stream_open_io(s, fd, expected_rate, channels, NULL);
}

bool audio_wav_stream_open_io(AudioWavStream *s, int fd, int expected_rate, int channels,
                              const AudioFdIo *io)
{
    if (!s || fd < 0 || (channels != 1 && channels != 2)) return false;
    unsigned char *ring = s->ring;          /* allocated once, kept across opens */
    memset(s, 0, sizeof(*s));
    s->fd = -1;
    if (!ring) {
        ring = (unsigned char *)malloc(AUDIO_STREAM_RING_BYTES);
        if (!ring) {
            fprintf(stderr, "audio_wav: out of memory for the %lu byte stream ring\n",
                    (unsigned long)AUDIO_STREAM_RING_BYTES);
            return false;
        }
    }
    s->ring          = ring;
    s->expected_rate = expected_rate;
    s->channels      = channels;
    s->frame_shift   = channels;          /* 2 bytes per sample: 1 ch -> 2 B (shift 1), 2 ch -> 4 B (shift 2) */
    /* ~10 ms edge for the ends and the gaps.  The one divide is here, once; the
     * per-sample loop multiplies (Cortex-A8 has no hardware divide). */
    s->ramp_len  = expected_rate > 200 ? expected_rate / 100 : 2;
    s->ramp_step = 32768L / s->ramp_len;
    audio_wav_hdr_init(&s->hdr);

    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0 || fcntl(fd, F_SETFL, fl | O_NONBLOCK) != 0) {
        fprintf(stderr, "audio_wav: cannot make the stream fd non-blocking\n");
        return false;
    }
    s->fd = fd;
    s->io = io;
    stream_pull(s);
    if (s->failed) { s->fd = -1; return false; }    /* the caller keeps its fd */
    return true;
}

/* Fade `n` frames of `d` to or from zero over at most `ramp` of them.  Gain is
 * Q15 and steps by a precomputed `step`, so there is no divide per sample.  The
 * mix bus will not do this itself: a stream declares an enormous length, so its
 * envelope release never runs, and a cut to zero is a click. */
static void stream_ramp(int16_t *d, long n, long ramp, long step, bool fade_in, int channels)
{
    long r = n < ramp ? n : ramp;
    int16_t *p = fade_in ? d : d + (n - r) * channels;
    for (long k = 0; k < r; k++) {
        long gain = (fade_in ? k : (r - 1 - k)) * step;
        for (int c = 0; c < channels; c++) {          /* one gain per FRAME, every channel */
            p[k * channels + c] = (int16_t)(((int32_t)p[k * channels + c] * (int32_t)gain) >> 15);
        }
    }
}

long audio_wav_stream_fill(void *ctx, int16_t *dst, long frames)
{
    AudioWavStream *s = (AudioWavStream *)ctx;
    if (!s || !s->ring || !dst || frames <= 0) return 0;

    stream_pull(s);
    if (s->failed) return 0;

    int fs = s->frame_shift;
    long have = (long)(s->count >> fs);                 /* whole frames buffered */
    AudioStreamAct act = audio_stream_decide(have << 1, s->eof, s->playing,
                                             AUDIO_STREAM_PREROLL_BYTES);
    if (act == AWS_END) return 0;
    if (act == AWS_SILENCE || act == AWS_REBUFFER) {
        if (act == AWS_REBUFFER) { s->playing = false; s->underruns++; s->resume_ramp = true; }
        memset(dst, 0, (size_t)frames * (size_t)s->channels * sizeof(int16_t));
        return frames;
    }
    s->playing = true;

    /* Frames, not samples or bytes: `>> fs`, not `/ n` (no hardware divide on this core). */
    long avail = have;
    long n     = frames < avail ? frames : avail;
    unsigned long bytes = (unsigned long)n << fs;
    unsigned long run   = AUDIO_STREAM_RING_BYTES - s->head;
    if (run > bytes) run = bytes;
    memcpy(dst, s->ring + s->head, run);
    memcpy((unsigned char *)dst + run, s->ring, bytes - run);
    s->head   = (s->head + bytes) & RING_MASK;
    s->count -= bytes;
    s->frames_out += n;

    /* ⚠️ Edges.  Coming back from an underrun the first frames ramp UP; and
     * whenever this block leaves the ring empty (EOF drained, a real underrun, or
     * an exact drain) its last ~10 ms ramp DOWN, because what follows is silence
     * or the end of the voice and a hard cut is a click. */
    if (s->resume_ramp) { stream_ramp(dst, n, s->ramp_len, s->ramp_step, true, s->channels); s->resume_ramp = false; }
    if ((long)(s->count >> fs) < 1) {
        stream_ramp(dst, n, s->ramp_len, s->ramp_step, false, s->channels);
        s->resume_ramp = true;              /* the next block starts from silence */
    }

    if (n < frames) {
        if (s->eof) return n;               /* short = the track ends */
        /* Ran dry mid-buffer: pad with silence (a short return would END the
         * voice) and go back to pre-roll. */
        s->playing = false;
        s->resume_ramp = true;
        s->underruns++;
        memset(dst + n * s->channels, 0, (size_t)(frames - n) * (size_t)s->channels * sizeof(int16_t));
    }
    return frames;
}

long audio_wav_stream_buffered(const AudioWavStream *s) { return s ? (long)s->count : 0; }
bool audio_wav_stream_failed(const AudioWavStream *s)   { return s && s->failed; }

void audio_wav_stream_close(AudioWavStream *s)
{
    if (!s) return;
    if (s->fd >= 0) { if (s->io) s->io->close(s->fd); else close(s->fd); }
    s->fd = -1;
    s->io = NULL;
    s->count = s->head = 0;
    s->eof = true;
}

void audio_wav_stream_free(AudioWavStream *s)
{
    if (!s) return;
    audio_wav_stream_close(s);
    free(s->ring);
    s->ring = NULL;
}

bool audio_wav_open(AudioWav *w, const char *path, bool loop)
{
    if (!w || !path) return false;
    memset(w, 0, sizeof(*w));

    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "audio_wav: cannot open %s\n", path);
        return false;
    }

    /* The walk is audio_wav_hdr_feed()'s, shared with the stream source.  The
     * first 512 bytes cover every header we have; a big unknown chunk is stepped
     * over by seeking instead of reading it through. */
    AudioWavHdr h;
    audio_wav_hdr_init(&h);
    unsigned char chunk[512];
    while (h.state != AWH_DONE && h.state != AWH_BAD) {
        if (h.state == AWH_SKIP && h.skip_left >= sizeof(chunk)) {
            if (h.skip_left > 0x7FFFFFFFULL
                || fseek(f, (long)h.skip_left, SEEK_CUR) != 0) break;
            audio_wav_hdr_skipped(&h, h.skip_left);
            continue;
        }
        size_t got = fread(chunk, 1, sizeof(chunk), f);
        if (got == 0) break;
        audio_wav_hdr_feed(&h, chunk, got);
    }
    if (h.state == AWH_BAD && !h.have_fmt && h.consumed <= 12) {
        fprintf(stderr, "audio_wav: %s is not a RIFF/WAVE file\n", path);
        fclose(f);
        return false;
    }

    int  channels = h.channels, bits = h.bits, rate = h.rate;
    long data_pos = (h.state == AWH_DONE) ? (long)h.consumed : 0;
    /* A streaming encoder's placeholder (0xFFFFFFFF), or anything a 32-bit long
     * cannot hold, means "until EOF": the file-size clamp below supplies the real
     * length. */
    long data_bytes = (h.data_size == AUDIO_WAV_SIZE_UNKNOWN || h.data_size > 0x7FFFFFFFUL)
                      ? 0x7FFFFFFFL : (long)h.data_size;

    if (!audio_wav_tag_ok(h.tag)) {
        fprintf(stderr, "audio_wav: %s: format tag 0x%04X is not PCM\n", path, h.tag);
        fclose(f);
        return false;
    }
    if (channels < 1 || bits != 16 || data_pos <= 0 || data_bytes <= 0) {
        fprintf(stderr, "audio_wav: %s: need 16-bit PCM with a data chunk "
                        "(got %d ch, %d bits, %ld data bytes at %ld)\n",
                path, channels, bits, data_bytes, data_pos);
        fclose(f);
        return false;
    }

    /* Believe the FILE over the header.  A size that over-claims — truncated
     * file, or a placeholder written by a streaming encoder — would otherwise
     * make `frames` promise audio that is not there, and the mix bus would keep
     * a voice alive rendering short fills to the end of a length nobody has. */
    if (fseek(f, 0, SEEK_END) == 0) {
        long end = ftell(f);
        if (end > data_pos && (end - data_pos) < data_bytes) {
            if (h.data_size != AUDIO_WAV_SIZE_UNKNOWN)
                fprintf(stderr, "audio_wav: %s: data chunk claims %ld bytes, file "
                                "holds %ld — using the file\n",
                        path, data_bytes, end - data_pos);
            data_bytes = end - data_pos;
        }
    }

    long frame_bytes = (long)WAV_BYTES_PER_SAMPLE * channels;
    long frames      = data_bytes / frame_bytes;
    if (frames <= 0) {
        fprintf(stderr, "audio_wav: %s holds no whole frames\n", path);
        fclose(f);
        return false;
    }

    w->f          = f;
    w->rate       = rate;
    w->channels   = channels;
    w->data_pos   = data_pos;
    w->data_bytes = frames * frame_bytes;   /* whole frames only */
    w->frames     = frames;
    w->pos        = 0;
    w->loop       = loop;
    w->loops      = 0;

    if (fseek(f, data_pos, SEEK_SET) != 0) {
        fprintf(stderr, "audio_wav: %s: cannot seek to data at %ld\n", path, data_pos);
        audio_wav_close(w);
        return false;
    }
    return true;
}

void audio_wav_rewind(AudioWav *w)
{
    if (!w || !w->f) return;
    if (fseek(w->f, w->data_pos, SEEK_SET) == 0) w->pos = 0;
}

void audio_wav_close(AudioWav *w)
{
    if (!w) return;
    if (w->f) fclose(w->f);
    w->f = NULL;
}

long audio_wav_read(AudioWav *w, int16_t *dst, long frames)
{
    if (!w || !w->f || !dst || frames <= 0) return 0;

    long done = 0;
    while (done < frames) {
        if (w->pos >= w->frames) {
            if (!w->loop) break;
            audio_wav_rewind(w);
            w->loops++;
            if (w->pos >= w->frames) break;     /* rewind failed — do not spin */
        }

        long want = frames - done;
        long left = w->frames - w->pos;
        if (want > left)             want = left;
        if (want > WAV_BATCH_FRAMES) want = WAV_BATCH_FRAMES;

        if (w->channels == 1) {
            /* The common case on this device, and it needs no scratch: the file
             * is already what the bus wants. */
            size_t got = fread(dst + done, WAV_BYTES_PER_SAMPLE, (size_t)want, w->f);
            if (got == 0) break;
            done   += (long)got;
            w->pos += (long)got;
            continue;
        }

        /* Multi-channel: downmix a frame at a time, straight into `dst`.  No
         * scratch buffer, because there is nothing to stage — one file frame
         * becomes exactly one bus frame. */
        long batch = want;
        long i;
        for (i = 0; i < batch; i++) {
            int16_t frame[2];
            if (fread(frame, WAV_BYTES_PER_SAMPLE, 2, w->f) != 2) break;
            if (w->channels > 2
                && fseek(w->f, (long)(w->channels - 2) * WAV_BYTES_PER_SAMPLE,
                         SEEK_CUR) != 0) break;
            /* (L+R)/2, not the left channel.  The speaker sums L and R, so an
             * average is what makes a downmix audibly match the original;
             * picking one channel silently drops whatever was panned away.
             * `>> 1` rather than `/ 2` — Cortex-A8 has no hardware divide, and
             * an arithmetic shift right is the floor division we want on both
             * signs. */
            dst[done + i] = (int16_t)(((int32_t)frame[0] + (int32_t)frame[1]) >> 1);
        }
        if (i <= 0) break;
        done   += i;
        w->pos += i;
    }

    return done;
}

long audio_wav_fill(void *ctx, int16_t *dst, long frames)
{
    return audio_wav_read((AudioWav *)ctx, dst, frames);
}
