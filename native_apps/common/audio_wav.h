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
