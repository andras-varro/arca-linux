/*
 * tests/audio_stream_test.c — the fd music source: header walk on a socket,
 * pre-roll, underrun, EOF and the format refusal.
 *
 * A socketpair stands in for the HTTP body.  The header is the shape the music
 * server sends (RIFF and data sizes 0xFFFFFFFF, an ODD-sized LIST chunk between
 * `fmt ` and `data`, so the pad byte is exercised too), and the PCM is a byte
 * pattern, so "comes out unchanged" is an exact compare.  Nothing here opens a
 * sound device: the fill is called directly, as the mix bus does.
 *
 * Build and run (host gcc, from native_apps/):
 *   gcc -Wall -Wextra -Wno-unused-parameter -I. \
 *       -o build/audio_stream_test tests/audio_stream_test.c \
 *       common/audio_wav.c common/audio_gen.c -lm && ./build/audio_stream_test
 */

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "common/audio_wav.h"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); \
    printf(__VA_ARGS__); printf("\n"); } } while (0)

#define FRAMES 4096

static int hdr_tag = 1;                                /* wFormatTag the next header carries */
#define RAMPB 882                                       /* 10 ms edge: 441 frames * 2 bytes   */

static void put32(unsigned char *p, unsigned long v)
{
    p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24);
}

/* Header: RIFF/WAVE, fmt, an odd LIST chunk (171 bytes + 1 pad), data. */
static size_t make_header(unsigned char *b, int rate, int ch, unsigned long data_size)
{
    size_t n = 0;
    memcpy(b + n, "RIFF", 4); put32(b + n + 4, 0xFFFFFFFFUL); memcpy(b + n + 8, "WAVE", 4); n += 12;
    memcpy(b + n, "fmt ", 4); put32(b + n + 4, 16); n += 8;
    b[n] = (unsigned char)hdr_tag; b[n + 1] = (unsigned char)(hdr_tag >> 8); b[n + 2] = (unsigned char)ch; b[n + 3] = 0;
    put32(b + n + 4, (unsigned long)rate);
    put32(b + n + 8, (unsigned long)rate * 2UL * (unsigned long)ch);
    b[n + 12] = (unsigned char)(2 * ch); b[n + 13] = 0; b[n + 14] = 16; b[n + 15] = 0; n += 16;
    memcpy(b + n, "LIST", 4); put32(b + n + 4, 171); n += 8;
    memset(b + n, 'x', 172); n += 172;                 /* 171 + pad */
    memcpy(b + n, "data", 4); put32(b + n + 4, data_size); n += 8;
    return n;
}


static unsigned long pcm_i;                            /* next pattern byte to WRITE */
static void send_pcm(int fd, size_t n)
{
    unsigned char *b = malloc(n);
    for (size_t i = 0; i < n; i++) b[i] = (unsigned char)((pcm_i + i) * 7 + 3);
    size_t off = 0;
    while (off < n) {
        ssize_t w = write(fd, b + off, n - off);
        if (w <= 0) break;
        off += (size_t)w;
    }
    CHECK(off == n, "wrote %zu of %zu", off, n);
    pcm_i += off;
    free(b);
}

static int pair(int sv[2])
{
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) { perror("socketpair"); exit(2); }
    fcntl(sv[0], F_SETFL, fcntl(sv[0], F_GETFL) | O_NONBLOCK);
    pcm_i = 0;
    return 0;
}

/* Is dst[0..bytes) the pattern from `from`? */
static int matches(const int16_t *dst, size_t bytes, unsigned long from)
{
    const unsigned char *p = (const unsigned char *)dst;
    for (size_t i = 0; i < bytes; i++)
        if (p[i] != (unsigned char)((from + i) * 7 + 3)) return 0;
    return 1;
}

/* Bytes [from,to) of dst are the pattern from `base`+from. */
static int matches_part(const int16_t *dst, size_t from, size_t to, unsigned long base)
{
    const unsigned char *p = (const unsigned char *)dst;
    for (size_t i = from; i < to; i++)
        if (p[i] != (unsigned char)((base + i) * 7 + 3)) return 0;
    return 1;
}

static int all_zero(const int16_t *d, long frames)
{
    for (long i = 0; i < frames; i++) if (d[i]) return 0;
    return 1;
}

int main(void)
{
    static int16_t out[FRAMES];
    unsigned char hdr[512];
    int sv[2];
    AudioWavStream s;
    memset(&s, 0, sizeof(s));

    /* ── A: pre-roll, then the PCM comes out unchanged ───────────────────── */
    pair(sv);
    size_t hl = make_header(hdr, 44100, 1, 0xFFFFFFFFUL);
    CHECK(write(sv[1], hdr, hl) == (ssize_t)hl, "header write");
    send_pcm(sv[1], 1000);                              /* far below one second */
    CHECK(audio_wav_stream_open(&s, sv[0], 44100, 1), "open refused a good header");
    CHECK(audio_wav_stream_buffered(&s) == 1000, "LIST skip lost sync: buffered %ld, want 1000",
          audio_wav_stream_buffered(&s));
    memset(out, 0x55, sizeof(out));
    long n = audio_wav_stream_fill(&s, out, FRAMES);
    CHECK(n == FRAMES && all_zero(out, FRAMES), "before pre-roll must be %d frames of silence, got %ld", FRAMES, n);
    CHECK(audio_wav_stream_buffered(&s) == 1000, "silence consumed buffered bytes");

    send_pcm(sv[1], 100001);                            /* ODD total: the drain leaves an odd leftover byte */
    unsigned long rd = 0;
    n = audio_wav_stream_fill(&s, out, FRAMES);
    CHECK(n == FRAMES && matches(out, FRAMES * 2, rd), "first block is not the stream's first PCM bytes");
    rd += FRAMES * 2;
    for (int i = 0; i < 5; i++) {
        n = audio_wav_stream_fill(&s, out, FRAMES);
        CHECK(n == FRAMES && matches(out, FRAMES * 2, rd), "block %d differs", i);
        rd += FRAMES * 2;
    }
    CHECK(s.underruns == 0, "underrun counted while data was available");

    /* ── B: a starved stream yields silence, and resumes only after pre-roll ── */
    while (audio_wav_stream_buffered(&s) >= FRAMES * 2) {
        n = audio_wav_stream_fill(&s, out, FRAMES);
        CHECK(n == FRAMES && matches(out, FRAMES * 2, rd), "drain block differs");
        rd += FRAMES * 2;
    }
    long left = audio_wav_stream_buffered(&s);          /* < one block */
    n = audio_wav_stream_fill(&s, out, FRAMES);
    CHECK(n == FRAMES, "underrun block must stay full length (a short one ends the voice), got %ld", n);
    CHECK(matches_part(out, 0, (size_t)(left & ~1L) > RAMPB ? (size_t)(left & ~1L) - RAMPB : 0, rd),
          "the tail before the gap differs");
    CHECK(out[(left >> 1) - 1] == 0, "the underrun cuts to zero instead of ramping: last sample %d", out[(left >> 1) - 1]);
    CHECK(all_zero(out + (left >> 1), FRAMES - (left >> 1)), "gap after the tail is not silence");
    rd += (unsigned long)(left & ~1L);
    CHECK(s.underruns == 1, "underruns=%ld want 1", s.underruns);
    n = audio_wav_stream_fill(&s, out, FRAMES);
    CHECK(n == FRAMES && all_zero(out, FRAMES), "starved fill is not silence");
    CHECK(s.underruns == 1, "a starved wait counted again: %ld", s.underruns);
    send_pcm(sv[1], 20000);                              /* < pre-roll: still silent */
    n = audio_wav_stream_fill(&s, out, FRAMES);
    CHECK(n == FRAMES && all_zero(out, FRAMES), "resumed before the pre-roll level");
    send_pcm(sv[1], 80000);
    n = audio_wav_stream_fill(&s, out, FRAMES);
    /* The leftover ODD byte comes out FIRST (it is the low half of a sample), so
     * the resumed block continues at `rd`, which already stops short of it. */
    CHECK(left & 1, "fixture: the drain must leave an odd leftover byte (left=%ld)", left);
    CHECK(n == FRAMES && matches_part(out, RAMPB, FRAMES * 2, rd), "did not resume where the stream left off");
    CHECK(out[0] == 0, "the resume does not start from silence: first sample %d", out[0]);
    rd += FRAMES * 2;

    /* ── C: EOF after draining ends the track, nothing lost ──────────────── */
    shutdown(sv[1], SHUT_WR);
    long total_out = 0;
    for (int guard = 0; guard < 100; guard++) {
        n = audio_wav_stream_fill(&s, out, FRAMES);
        if (n > 0) {
            size_t bl = (size_t)n * 2;
            int last = audio_wav_stream_buffered(&s) < 2;      /* this block drained the ring: its end ramps */
            CHECK(matches_part(out, 0, last && bl > RAMPB ? bl - RAMPB : bl, rd), "tail block differs");
            if (last) CHECK(out[n - 1] == 0, "the end of the track is not ramped to zero: %d", out[n - 1]);
            rd += (unsigned long)n * 2;
            total_out += n;
        }
        if (n < FRAMES) break;
    }
    CHECK(rd == pcm_i - (pcm_i & 1) || rd == pcm_i, "PCM out %lu of %lu sent", rd, pcm_i);
    CHECK(audio_wav_stream_fill(&s, out, FRAMES) == 0, "fill after the end must return 0");
    CHECK(total_out > 0, "nothing came out after EOF");
    audio_wav_stream_close(&s);
    close(sv[1]);

    /* ── D: a header split across reads ──────────────────────────────────── */
    pair(sv);
    CHECK(audio_wav_stream_open(&s, sv[0], 44100, 1), "open with nothing on the wire must succeed (pending)");
    for (size_t i = 0; i < hl; i++) {
        CHECK(write(sv[1], hdr + i, 1) == 1, "byte write");
        audio_wav_stream_fill(&s, out, FRAMES);
    }
    send_pcm(sv[1], 90000);
    audio_wav_stream_fill(&s, out, FRAMES);
    n = audio_wav_stream_fill(&s, out, FRAMES);
    CHECK(n == FRAMES && matches(out, FRAMES * 2, FRAMES * 2), "split-header stream desynchronised");
    audio_wav_stream_close(&s);
    close(sv[1]);

    /* ── E: a header that fixes the length stops at it ───────────────────── */
    pair(sv);
    hl = make_header(hdr, 44100, 1, 6000);
    CHECK(write(sv[1], hdr, hl) == (ssize_t)hl, "header write");
    send_pcm(sv[1], 9000);                               /* 3000 bytes of trailing junk */
    CHECK(audio_wav_stream_open(&s, sv[0], 44100, 1), "open");
    n = audio_wav_stream_fill(&s, out, FRAMES);          /* eof is already known: plays out */
    CHECK(n == 3000 && matches_part(out, 0, 6000 - RAMPB, 0) && out[2999] == 0, "declared length not honoured: %ld frames", n);
    audio_wav_stream_close(&s);
    close(sv[1]);

    /* ── F: 48 kHz stereo is refused, loudly and without playing ─────────── */
    pair(sv);
    hl = make_header(hdr, 48000, 2, 0xFFFFFFFFUL);
    CHECK(write(sv[1], hdr, hl) == (ssize_t)hl, "header write");
    send_pcm(sv[1], 100000);
    CHECK(!audio_wav_stream_open(&s, sv[0], 44100, 1), "48 kHz stereo was accepted");
    CHECK(audio_wav_stream_failed(&s), "refusal not recorded");
    CHECK(audio_wav_stream_fill(&s, out, FRAMES) == 0, "a refused stream produced audio");
    close(sv[0]);
    close(sv[1]);
    audio_wav_stream_free(&s);

    /* ── H: the peer hangs up in the middle of the header ───────────────── */
    pair(sv);
    hl = make_header(hdr, 44100, 1, 0xFFFFFFFFUL);
    CHECK(write(sv[1], hdr, 20) == 20, "partial header write");
    shutdown(sv[1], SHUT_WR);
    CHECK(!audio_wav_stream_open(&s, sv[0], 44100, 1) && audio_wav_stream_failed(&s),
          "a header cut off at EOF must FAIL, not end silently");
    CHECK(audio_wav_stream_fill(&s, out, FRAMES) == 0, "a cut-off stream produced audio");
    close(sv[0]);
    close(sv[1]);
    audio_wav_stream_free(&s);
    /* (control) the same bytes with the peer still connected are merely pending */
    pair(sv);
    CHECK(write(sv[1], hdr, 20) == 20, "partial header write");
    CHECK(audio_wav_stream_open(&s, sv[0], 44100, 1) && !audio_wav_stream_failed(&s),
          "control: a header still arriving must not be a failure");
    audio_wav_stream_close(&s);
    close(sv[1]);

    /* ── I: a non-PCM format tag is refused (stream and file); 0xFFFE is not ─ */
    pair(sv);
    hdr_tag = 3;                                         /* IEEE float */
    hl = make_header(hdr, 44100, 1, 0xFFFFFFFFUL);
    CHECK(write(sv[1], hdr, hl) == (ssize_t)hl, "header write");
    send_pcm(sv[1], 100000);
    CHECK(!audio_wav_stream_open(&s, sv[0], 44100, 1) && audio_wav_stream_failed(&s),
          "a float-tagged stream was accepted");
    close(sv[0]);
    close(sv[1]);
    audio_wav_stream_free(&s);
    {
        const char *path = "build/audio_stream_test_tag3.wav";
        FILE *f = fopen(path, "wb");
        CHECK(f != NULL, "cannot write the file fixture");
        if (f) {
            fwrite(hdr, 1, hl, f);
            for (int i = 0; i < 2000; i++) fputc(i, f);
            fclose(f);
            AudioWav w;
            CHECK(!audio_wav_open(&w, path, false), "a float-tagged FILE was accepted");
            hdr_tag = 1;
            hl = make_header(hdr, 44100, 1, 2000);
            f = fopen(path, "wb");
            fwrite(hdr, 1, hl, f);
            for (int i = 0; i < 2000; i++) fputc(i, f);
            fclose(f);
            CHECK(audio_wav_open(&w, path, false), "control: the same file tagged PCM was refused");
            audio_wav_close(&w);
            remove(path);
        }
    }
    hdr_tag = 0xFFFE;
    pair(sv);
    hl = make_header(hdr, 44100, 1, 0xFFFFFFFFUL);
    CHECK(write(sv[1], hdr, hl) == (ssize_t)hl, "header write");
    CHECK(audio_wav_stream_open(&s, sv[0], 44100, 1), "control: an extensible tag was refused");
    audio_wav_stream_close(&s);
    close(sv[1]);
    hdr_tag = 1;
    audio_wav_stream_free(&s);

    /* ── G: the pure decision ───────────────────────────────────────────── */
    CHECK(audio_stream_decide(0, false, false, 100) == AWS_SILENCE, "decide: start, empty");
    CHECK(audio_stream_decide(99, false, false, 100) == AWS_SILENCE, "decide: below pre-roll");
    CHECK(audio_stream_decide(100, false, false, 100) == AWS_PLAY, "decide: pre-roll reached");
    CHECK(audio_stream_decide(2, false, true, 100) == AWS_PLAY, "decide: playing keeps playing");
    CHECK(audio_stream_decide(0, false, true, 100) == AWS_REBUFFER, "decide: drained while playing");
    CHECK(audio_stream_decide(10, true, false, 100) == AWS_PLAY, "decide: EOF plays the remainder");
    CHECK(audio_stream_decide(0, true, true, 100) == AWS_END, "decide: EOF and empty");

    printf("audio_stream_test: %d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
