/*
 * tests/audio_stream_stereo_test.c — the fd music source with TWO channels.
 *
 * The stream ring holds bytes, the mixer counts frames, and a stereo frame is
 * four bytes, so every place that turned bytes into frames (or the other way)
 * is checked here with an ASYMMETRIC byte pattern: a swapped, dropped or
 * half-padded channel changes the compare, a symmetric signal would not.
 *
 *   A  stereo header accepted; PCM comes out unchanged across a ring wrap,
 *      fed in odd-sized chunks (a pull boundary lands mid-frame);
 *   B  pre-roll is counted in frames (1 s = 176400 bytes of stereo);
 *   C  the silence pad after an underrun is frames*2 samples, not frames;
 *   D  the EOF tail drops a partial frame;
 *   E  the edge ramp scales L and R by the same gain, per frame;
 *   F  refusals: 3 channels, a channel count other than the one asked, rate.
 *
 * Build and run (host gcc, from native_apps/):
 *   gcc -Wall -Wextra -Wno-unused-parameter -I. -o build/audio_stream_stereo_test \
 *       tests/audio_stream_stereo_test.c common/audio_wav.c common/audio_gen.c -lm
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

static void put32(unsigned char *p, unsigned long v)
{
    p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24);
}

static size_t make_header(unsigned char *b, int rate, int ch, unsigned long data_size)
{
    size_t n = 0;
    memcpy(b + n, "RIFF", 4); put32(b + n + 4, 0xFFFFFFFFUL); memcpy(b + n + 8, "WAVE", 4); n += 12;
    memcpy(b + n, "fmt ", 4); put32(b + n + 4, 16); n += 8;
    b[n] = 1; b[n + 1] = 0; b[n + 2] = (unsigned char)ch; b[n + 3] = 0;
    put32(b + n + 4, (unsigned long)rate);
    put32(b + n + 8, (unsigned long)rate * 2UL * (unsigned long)ch);
    b[n + 12] = (unsigned char)(2 * ch); b[n + 13] = 0; b[n + 14] = 16; b[n + 15] = 0; n += 16;
    memcpy(b + n, "data", 4); put32(b + n + 4, data_size); n += 8;
    return n;
}

static unsigned long pcm_i;
static unsigned char pat(unsigned long i) { return (unsigned char)(i * 7 + 3); }

static void write_all(int fd, const void *b, size_t n)
{
    size_t off = 0;
    while (off < n) {
        ssize_t w = write(fd, (const char *)b + off, n - off);
        if (w <= 0) break;
        off += (size_t)w;
    }
    CHECK(off == n, "wrote %zu of %zu", off, n);
}

static void send_pcm(int fd, size_t n)
{
    unsigned char *b = malloc(n);
    for (size_t i = 0; i < n; i++) b[i] = pat(pcm_i + i);
    write_all(fd, b, n);
    pcm_i += n;
    free(b);
}

static void send_const(int fd, long frames, int16_t l, int16_t r)
{
    int16_t *b = malloc((size_t)frames * 4);
    for (long i = 0; i < frames; i++) { b[2 * i] = l; b[2 * i + 1] = r; }
    write_all(fd, b, (size_t)frames * 4);
    free(b);
}

static void pair(int sv[2])
{
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) { perror("socketpair"); exit(2); }
    fcntl(sv[0], F_SETFL, fcntl(sv[0], F_GETFL) | O_NONBLOCK);
    fcntl(sv[1], F_SETFL, fcntl(sv[1], F_GETFL) | O_NONBLOCK);   /* a refusing stream must fail a CHECK, not hang the writer */
    pcm_i = 0;
}

static int matches(const int16_t *dst, size_t bytes, unsigned long from)
{
    const unsigned char *p = (const unsigned char *)dst;
    for (size_t i = 0; i < bytes; i++)
        if (p[i] != pat(from + i)) return 0;
    return 1;
}

static int16_t out[8192];
static int16_t big[2 * (44100 + 3000)];

/* Open on a fresh pair carrying a header of (rate, ch, data_size); returns open()'s verdict. */
static int open_with(AudioWavStream *s, int sv[2], int rate, int ch, unsigned long size, int want_ch)
{
    unsigned char hdr[64];
    pair(sv);
    size_t hl = make_header(hdr, rate, ch, size);
    write_all(sv[1], hdr, hl);
    return audio_wav_stream_open(s, sv[0], 44100, want_ch);
}

int main(void)
{
    int sv[2];
    AudioWavStream s;
    memset(&s, 0, sizeof(s));
    long n;

    /* ── A: stereo accepted, bytes unchanged across a ring wrap, odd chunks ── */
    pair(sv);
    {
        unsigned char hdr[64];
        size_t hl = make_header(hdr, 44100, 2, 0xFFFFFFFFUL);
        write_all(sv[1], hdr, hl);
    }
    send_pcm(sv[1], 176401);                              /* 1 s of stereo + one stray byte */
    CHECK(audio_wav_stream_open(&s, sv[0], 44100, 2), "stereo header refused");
    unsigned long rd = 0;
    int ok = 1;
    for (int i = 0; i < 700; i++) {                       /* ~2.8 MB through a 512 KiB ring */
        n = audio_wav_stream_fill(&s, out, 1000);
        if (n != 1000 || !matches(out, 4000, rd)) { ok = 0; printf("  block %d: n=%ld\n", i, n); break; }
        rd += 4000;
        send_pcm(sv[1], 4003);                            /* odd: boundaries wander mid-frame */
    }
    CHECK(ok, "stereo PCM did not come out unchanged across the wrap");
    CHECK(s.underruns == 0, "underruns %ld", s.underruns);
    audio_wav_stream_close(&s);
    close(sv[1]);

    /* ── B: pre-roll is one second of FRAMES ────────────────────────────── */
    CHECK(open_with(&s, sv, 44100, 2, 0xFFFFFFFFUL, 2), "open");
    send_pcm(sv[1], 88200 + 4);                           /* half a second of stereo */
    memset(out, 0x55, sizeof(out));
    n = audio_wav_stream_fill(&s, out, 512);
    int silent = 1;
    for (int i = 0; i < 1024; i++) if (out[i]) silent = 0;
    CHECK(n == 512 && silent, "half a second of stereo started playing (n=%ld)", n);
    send_pcm(sv[1], 88200);                               /* now 176404 bytes >= 176400 */
    n = audio_wav_stream_fill(&s, out, 512);
    CHECK(n == 512 && matches(out, 2048, 0), "one full second did not start playback");
    audio_wav_stream_close(&s);
    close(sv[1]);

    /* ── C: silence pad covers frames*2 samples ─────────────────────────── */
    CHECK(open_with(&s, sv, 44100, 2, 0xFFFFFFFFUL, 2), "open");
    send_pcm(sv[1], 176400 + 4 * 1000);
    long want = 44100 + 3000;                             /* wants 2000 more frames than exist */
    memset(big, 0x55, sizeof(big));
    n = audio_wav_stream_fill(&s, big, want);
    CHECK(n == want, "an underrun must return the full request, got %ld", n);
    CHECK(matches(big, 4 * (44100 + 1000 - 441), 0), "data before the ramp differs");
    int tail_zero = 1;
    for (long i = 2 * (44100 + 1000); i < 2 * want; i++) if (big[i]) { tail_zero = 0; break; }
    CHECK(tail_zero, "the pad did not zero all %ld samples of the missing frames", 2 * 2000L);
    CHECK(s.underruns == 1, "underruns %ld, want 1", s.underruns);
    audio_wav_stream_close(&s);
    close(sv[1]);

    /* ── D: EOF with a partial frame ────────────────────────────────────── */
    CHECK(open_with(&s, sv, 44100, 2, 4 * 2000 + 3, 2), "open");
    send_pcm(sv[1], 4 * 2000 + 3 + 400);                  /* + trailing junk */
    n = audio_wav_stream_fill(&s, out, 4096);
    CHECK(n == 2000, "EOF with a partial frame: %ld frames, want 2000", n);
    CHECK(matches(out, 4 * (2000 - 441), 0), "EOF block differs");
    audio_wav_stream_close(&s);
    close(sv[1]);

    /* ── E: ramps scale both channels by one gain, per frame ────────────── */
    CHECK(open_with(&s, sv, 44100, 2, 0xFFFFFFFFUL, 2), "open");
    send_const(sv[1], 44100 + 1000, 20000, -10000);
    n = audio_wav_stream_fill(&s, big, 44100 + 1000);     /* drains the ring exactly */
    CHECK(n == 44100 + 1000, "n=%ld", n);
    long last = n - 1;
    CHECK(big[2 * last] == 0 && big[2 * last + 1] == 0, "last frame not silent: %d %d",
          big[2 * last], big[2 * last + 1]);
    long mid = n - 200;
    int l = big[2 * mid], r = big[2 * mid + 1];
    CHECK(l > 0 && r < 0 && l < 20000 && r > -10000, "mid ramp frame not attenuated: %d %d", l, r);
    CHECK(abs(l + 2 * r) <= 2, "channels got different gains: L=%d R=%d", l, r);
    CHECK(big[2 * (n - 442)] == 20000 && big[2 * (n - 442) + 1] == -10000, "ramp starts too early");
    n = audio_wav_stream_fill(&s, out, 100);              /* ring empty, playing -> rebuffer */
    CHECK(n == 100, "rebuffer n=%ld", n);
    send_const(sv[1], 44100 + 800, 20000, -10000);
    n = audio_wav_stream_fill(&s, big, 800);
    CHECK(n == 800, "resume n=%ld", n);
    CHECK(big[0] == 0 && big[1] == 0, "resume does not start from silence: %d %d", big[0], big[1]);
    l = big[2 * 200]; r = big[2 * 200 + 1];
    CHECK(l > 0 && r < 0 && abs(l + 2 * r) <= 2, "resume ramp gains differ: L=%d R=%d", l, r);
    audio_wav_stream_close(&s);
    close(sv[1]);

    /* ── F: refusals ────────────────────────────────────────────────────── */
    CHECK(!open_with(&s, sv, 44100, 3, 0xFFFFFFFFUL, 2) && audio_wav_stream_failed(&s), "3 channels accepted");
    close(sv[0]); close(sv[1]);
    CHECK(!open_with(&s, sv, 44100, 1, 0xFFFFFFFFUL, 2), "mono stream accepted by a stereo open");
    close(sv[0]); close(sv[1]);
    CHECK(!open_with(&s, sv, 44100, 2, 0xFFFFFFFFUL, 1), "stereo stream accepted by a mono open");
    close(sv[0]); close(sv[1]);
    CHECK(!open_with(&s, sv, 48000, 2, 0xFFFFFFFFUL, 2), "48 kHz accepted");
    close(sv[0]); close(sv[1]);

    audio_wav_stream_free(&s);
    printf("audio_stream_stereo_test: %d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
