/*
 * audio_wav.h — the one RIFF/WAVE reader, streaming and mono.
 *
 * ⚠️ **This exists because a 44-byte header is a WRITER's assumption and a
 * reader that shares it is silently wrong on our own files.** Measured on `.188`
 * 2026-08-20: `/opt/sound/officerunner1-mono.wav` has its `data` chunk ID at byte 164
 * and its PCM at 172, because ffmpeg wrote a LIST/INFO chunk carrying an encoder
 * version string; the vendor's `asl_success.wav` has them at 36 and 44. So a
 * loader hardcoded to 44 plays 128 bytes of the text "made with suno; created=…"
 * as audio, and — this is the trap — it is *correct* on the three vendor effects,
 * which would therefore validate it. Walk the chunks.
 *
 * It reads MONO because the mix bus is mono and `audio_interleave()` is the
 * single conversion point (../CLAUDE.md → Audio). A multi-channel file is
 * averaged, not left-channel-picked: `(L+R)/2` is what makes a stored mono file
 * audibly identical to its stereo original on this speaker, which sums L and R
 * (../../SYSTEM_ANALYSIS.md#34-audio).
 *
 * It STREAMS. Nothing here loads a whole file: `audio_wav_read()` is the pull
 * that a mix-bus sample voice's `AudioVoiceFill` sits on top of, so a 44 s bed
 * costs one `FILE *` and the caller's buffer rather than 3.9 MB of RAM.
 *
 * No mixer, no device, no framebuffer — plain stdio, so it host-tests with no
 * shim at all.
 *
 * **A bed loops with no crossfade, and that is measured rather than assumed.**
 * `officerunner1-mono.wav` ran through three full wraps at a level louder than the
 * one that ships, and no join was audible — so wrapping straight back to the first
 * frame is enough and no fade is needed. ⚠️ That is a property of THOSE FILES, not
 * of this reader: a bed whose first and last frames differ will seam, and the
 * receipt to read it against is `AudioWav.loops`.
 */

#ifndef ARCA_AUDIO_WAV_H
#define ARCA_AUDIO_WAV_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

typedef struct {
    FILE *f;
    int   rate;         /**< the FILE's rate.  Not resampled — see below.     */
    int   channels;     /**< in the file; reads are averaged down to 1        */
    long  data_pos;     /**< byte offset of the first PCM byte (172, not 44)  */
    long  data_bytes;   /**< clamped to what the file really holds            */
    long  frames;       /**< data_bytes / (2 * channels)                      */
    long  pos;           /**< frames consumed since open or wrap               */
    bool  loop;         /**< wrap to data_pos at EOF instead of running dry   */
    long  loops;        /**< times it has wrapped — a countable receipt       */
} AudioWav;

/* ── The header walk, shared by the file reader and the stream reader ────────
 *
 * ⚠️ **ONE parser, and it is a PUSH parser** because a socket delivers a header
 * in whatever pieces the network chose and a file does not need seeking to be
 * walked.  Feed it bytes in order; it consumes only up to the first PCM byte,
 * so the caller's remainder is audio.  `audio_wav_open()` and the stream below
 * both sit on it — a second walk would be a second place to forget the odd-size
 * pad byte. */
typedef enum { AWH_RIFF = 0, AWH_CHUNK, AWH_FMT, AWH_SKIP, AWH_DONE, AWH_BAD } AudioWavHdrState;

typedef struct {
    AudioWavHdrState state;
    unsigned char    acc[16];       /**< the fixed-size field being collected  */
    unsigned int     have, need;    /**< bytes of `acc` held / wanted          */
    unsigned long    chunk_sz;      /**< declared size of the chunk in hand    */
    unsigned long long skip_left;   /**< bytes of an unknown chunk still to go */
    unsigned long    consumed;      /**< every byte taken so far == data offset at DONE */
    bool             have_fmt;
    int              tag, channels, bits, rate;   /**< tag = wFormatTag (1 PCM, 0xFFFE extensible) */
    unsigned long    data_size;     /**< the declared size, raw; 0xFFFFFFFF = streaming */
} AudioWavHdr;

#define AUDIO_WAV_SIZE_UNKNOWN 0xFFFFFFFFUL

/** wFormatTag values we accept: plain PCM, or WAVE_FORMAT_EXTENSIBLE (16-bit mono). */
bool audio_wav_tag_ok(int tag);

void   audio_wav_hdr_init(AudioWavHdr *h);
/** Consume bytes from `p`; returns how many it took (fewer than `n` only once
 *  state is DONE or BAD).  Never blocks, never reads past the data chunk header. */
size_t audio_wav_hdr_feed(AudioWavHdr *h, const unsigned char *p, size_t n);
/** Account for `n` bytes of a skipped chunk the CALLER stepped over by seeking. */
void   audio_wav_hdr_skipped(AudioWavHdr *h, unsigned long long n);

/* ── The stream source: a WAV arriving on a non-blocking fd ───────────────────
 *
 * Plays a RIFF/WAVE that is still being delivered (an HTTP body on a TCP
 * socket) through the same mix-bus sample voice as a file.  Header sizes of
 * 0xFFFFFFFF mean "until EOF".  The format must BE the bus format (mono, 16-bit,
 * `expected_rate`); there is no resampler and no downmix here, so anything else
 * is refused and the stream ends.  Because the header arrives over the wire,
 * a refusal that is not yet visible at open time is reported later through
 * `audio_wav_stream_failed()` and a log line, and the track simply ends.
 *
 * ⚠️ **Buffering rules** (implemented in `audio_wav_stream_fill()`):
 *   - NEVER BLOCK.  The render loop calls the fill once per read-ahead buffer;
 *     each call takes what the socket has (EAGAIN = nothing yet) into a ring
 *     and returns at once.  No thread: an audio thread is forbidden here.
 *   - PRE-ROLL.  Output starts only once AUDIO_STREAM_PREROLL_BYTES (~1 s) are
 *     buffered; before that the voice plays silence.  Starting on the first
 *     packet would underrun at once on a jittery link, one gap per refill.
 *   - UNDERRUN.  If the ring cannot cover a request, the rest of the buffer is
 *     silence (never a short return: a short return ENDS a mix-bus voice) and
 *     the stream returns to pre-roll, so it resumes only after ~1 s is buffered
 *     again.  Resuming on a single packet would stutter at the network's rhythm.
 *   - EOF.  Only when the peer has closed AND the ring is drained does the fill
 *     return short, which ends the voice like the end of a file.
 */
#define AUDIO_STREAM_RING_BYTES     (1UL << 19)   /**< 512 KiB (~5.9 s); a power of two so indexing needs no divide */
#define AUDIO_STREAM_PREROLL_BYTES  88200L        /**< 1 s of the 44100 Hz 16-bit mono bus */

typedef enum { AWS_PLAY, AWS_SILENCE, AWS_REBUFFER, AWS_END } AudioStreamAct;

/** Pure: what the next fill should do, from the buffered byte count alone.
 *  `playing` = output has started and has not underrun since. */
AudioStreamAct audio_stream_decide(long buffered, bool eof, bool playing, long preroll);

typedef struct {
    int            fd;            /**< -1 when closed; owned once handed over */
    unsigned char *ring;          /**< allocated once, kept across opens      */
    unsigned long  head, count;   /**< read index, bytes buffered             */
    AudioWavHdr    hdr;
    bool           hdr_done, failed, eof, playing;
    bool           resume_ramp;   /**< fade the next block in (it follows an underrun)      */
    long           ramp_len, ramp_step;   /**< ~10 ms edge in frames; Q15 gain per frame, no divide per sample */
    int            expected_rate;
    unsigned long  data_left;     /**< bytes the header still promises; ignored when unknown */
    bool           data_known;
    long           underruns;     /**< transitions from playing to rebuffering */
    long           frames_out;     /**< PCM frames handed to the mixer          */
} AudioWavStream;

/** Take `fd` (set O_NONBLOCK here), one initial non-blocking read. Returns false if
 *  the header has already arrived and is not the bus format. Closes nothing on false. */
bool   audio_wav_stream_open(AudioWavStream *s, int fd, int expected_rate);
/** `AudioVoiceFill` over an `AudioWavStream *`.  See the rules above. */
long   audio_wav_stream_fill(void *ctx, int16_t *dst, long frames);
/** Bytes buffered; the header failed; closes the fd (ring kept). */
long   audio_wav_stream_buffered(const AudioWavStream *s);
bool   audio_wav_stream_failed(const AudioWavStream *s);
void   audio_wav_stream_close(AudioWavStream *s);
/** close + free the ring. */
void   audio_wav_stream_free(AudioWavStream *s);

/**
 * Open `path` and walk its chunks. Returns true on success.
 *
 * Accepts 16-bit PCM only, which is what `hw:0,0` grants and what every file we
 * have is. ⚠️ **It does NOT resample.** `rate` is reported so the caller can
 * refuse a mismatch loudly; a bed played at the wrong rate is a pitch bug that
 * sounds like a bad recording, so guessing is worse than failing.
 *
 * `data_bytes` is clamped to the bytes actually present, so a header that
 * over-claims (a truncated file, or a stream written with a placeholder size)
 * ends the voice at real EOF rather than reading past it.
 */
bool audio_wav_open(AudioWav *w, const char *path, bool loop);

/**
 * Read up to `frames` mono frames into `dst`. Returns frames produced.
 *
 * A short return means the source ran dry, which is how a non-looping voice
 * ends — the mix bus treats a short fill as a normal exit, matching
 * `AudioOutFill`'s contract. With `loop` set it wraps at EOF and only ever
 * returns short on a read error.
 */
long audio_wav_read(AudioWav *w, int16_t *dst, long frames);

/** Rewind to the first PCM frame without reopening. */
void audio_wav_rewind(AudioWav *w);

/** Close the file. Safe on an already-closed or zeroed struct. */
void audio_wav_close(AudioWav *w);

/**
 * `AudioVoiceFill` adapter: pass `audio_wav_fill` and an `AudioWav *` as `ctx`
 * to `audio_mix_add_sample()`. Kept here rather than in `audio_gen.c` so the
 * mixer keeps its no-fd property, and rather than in `audio.c` so anything with
 * a bus can play a file without going through the `Audio` device half.
 */
long audio_wav_fill(void *ctx, int16_t *dst, long frames);

#endif /* ARCA_AUDIO_WAV_H */
