/*
 * tests/audio_music_stereo_test.c — a streamed STEREO bed through the production
 * fill (audio_cont_fill_mix), on a stereo device and on a mono one.
 *
 *   A  GOLDEN: mono sources (a file bed, a beep, a mono fd stream) render to the
 *      same bytes as before stereo existed, on a 2-channel and on a 1-channel
 *      grant.  The constants below were measured on the pre-change source
 *      (`GOLDEN` lines this program prints), so a drift in the mono path fails.
 *   B  stereo bed, stereo device: L and R stay apart (asymmetric: L=8000, R=-4000).
 *   C  stereo bed, mono device: the output is the fold (L+R)>>1 of what B gave.
 *   D  a mono bed and a stereo-asked open do not mix: a mono stream is refused
 *      when stereo was asked and the reverse.
 *
 * The device is faked at the link seam used by audio_stream_fd_test.c.
 *
 * Build and run (host gcc, from native_apps/):
 *   gcc -Wall -Wextra -Wno-unused-parameter -I. -Wl,--wrap=audio_out_open_resolved \
 *       -o build/audio_music_stereo_test tests/audio_music_stereo_test.c \
 *       common/audio.c common/audio_gen.c common/audio_out.c common/audio_wav.c \
 *       common/config.c -lm && ./build/audio_music_stereo_test
 */

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "common/audio.h"
#include "common/audio_out.h"

/* Golden hashes of the mono-source renders, taken from the source BEFORE stereo
 * (printed as GOLDEN lines). */
#define GOLDEN_FILE_2CH   0x81296879u
#define GOLDEN_FILE_1CH   0x9E1C996Au
#define GOLDEN_STREAM_2CH 0x3B0EA329u
#define GOLDEN_STREAM_1CH 0xDC287784u

/* START_FD: the pre-change source takes no channel count (scratch copy only). */
#ifndef START_FD
#define START_FD(a, fd, ch) audio_music_start_fd((a), (fd), (ch))
#endif

static int failures, checks;
static void check(bool ok, const char *what)
{
    checks++;
    if (ok) printf("    ok   %s\n", what);
    else  { failures++; printf("    FAIL: %s\n", what); }
}

static int g_grant;                                    /* channels the fake grants; 0 = as asked */

static int fake_open(void *ctx, int rate_req, int ch_req, int *rate, int *bits, int *ch)
{
    (void)ctx; (void)rate_req;
    *rate = 44100; *bits = 16; *ch = g_grant ? g_grant : ch_req;
    return 0;
}
static int fake_space(void *ctx, int frame_bytes, AudioOutSpace *sp)
{
    (void)ctx; (void)frame_bytes;
    sp->period_frames = 1024; sp->ring_frames = 8192; sp->in_flight = 0; sp->space = 8192;
    return 0;
}
static ssize_t fake_write(void *ctx, const void *buf, size_t n, bool *again)
{
    (void)ctx; (void)buf;
    *again = false;
    return (ssize_t)n;
}
static void fake_wait(void *ctx, int usec) { (void)ctx; (void)usec; }
static void fake_close(void *ctx)          { (void)ctx; }
static const AudioOutDev FAKE_DEV = { fake_open, fake_space, fake_write, fake_wait, fake_close };

int __wrap_audio_out_open_resolved(AudioOut *out, int rate_req, int channels_req,
                                   const char **path_out);
int __wrap_audio_out_open_resolved(AudioOut *out, int rate_req, int channels_req,
                                   const char **path_out)
{
    if (audio_out_open(out, &FAKE_DEV, NULL, rate_req, channels_req) != 0) return -1;
    if (path_out) *path_out = "fake-dsp";
    return 0;
}

static void put32(FILE *f, uint32_t v) { for (int i = 0; i < 4; i++) fputc((int)((v >> (8 * i)) & 0xff), f); }
static void put16(FILE *f, uint16_t v) { fputc(v & 0xff, f); fputc((v >> 8) & 0xff, f); }
static bool write_wav(const char *path, long frames)
{
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    uint32_t data = (uint32_t)(frames * 2);
    fwrite("RIFF", 1, 4, f); put32(f, 36 + data); fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f); put32(f, 16); put16(f, 1); put16(f, 1);
    put32(f, 44100); put32(f, 88200); put16(f, 2); put16(f, 16);
    fwrite("data", 1, 4, f); put32(f, data);
    for (long i = 0; i < frames; i++) put16(f, (uint16_t)(((i / 50) & 1) ? 3000 : -3000));
    return fclose(f) == 0;
}

/* A streaming WAV header (sizes 0xFFFFFFFF) onto `fd`. */
static void send_header(int fd, int ch)
{
    unsigned char h[44] = {0};
    memcpy(h, "RIFF", 4); memset(h + 4, 0xff, 4); memcpy(h + 8, "WAVEfmt ", 8);
    h[16] = 16; h[20] = 1; h[22] = (unsigned char)ch; h[24] = 0x44; h[25] = 0xAC;
    h[28] = (unsigned char)(0x88 * ch); h[29] = (unsigned char)(0x58 * ch);
    h[32] = (unsigned char)(2 * ch); h[34] = 16; memcpy(h + 36, "data", 4); memset(h + 40, 0xff, 4);
    if (write(fd, h, 44) != 44) perror("header");
}

/* Mono stream payload: a varying pattern.  `frames` frames. */
static void send_mono(int fd, long frames)
{
    int16_t *b = malloc((size_t)frames * 2);
    for (long i = 0; i < frames; i++) b[i] = (int16_t)(((i * 37) % 4001) - 2000);
    if (write(fd, b, (size_t)frames * 2) < 0) perror("pcm");
    free(b);
}

/* Stereo payload: constant L=8000, R=-4000 for `frames` frames. */
static void send_stereo(int fd, long frames)
{
    int16_t *b = malloc((size_t)frames * 4);
    for (long i = 0; i < frames; i++) { b[2 * i] = 8000; b[2 * i + 1] = -4000; }
    if (write(fd, b, (size_t)frames * 4) < 0) perror("pcm");
    free(b);
}

static uint32_t fnv(uint32_t h, const int16_t *p, long n)
{
    const unsigned char *b = (const unsigned char *)p;
    for (long i = 0; i < n * 2; i++) { h ^= b[i]; h *= 16777619u; }
    return h;
}

static void open_audio(Audio *a, int grant)
{
    g_grant = grant;
    memset(a, 0, sizeof *a);
    if (audio_init_unchecked(a) != 0 || !a->available) { printf("    FAIL: init\n"); exit(2); }
}

#define CHUNK 2048
#define CHUNKS 40
static int16_t one[CHUNK * 2];

/* Render CHUNKS chunks through the production fill at `channels`, hashing; the
 * samples of chunk `keep` (if non-NULL) are copied out. */
static uint32_t render(Audio *a, int channels, int16_t *keep, int keep_chunk)
{
    uint32_t h = 2166136261u;
    for (int c = 0; c < CHUNKS; c++) {
        memset(one, 0, sizeof one);
        long n = audio_cont_fill_mix(a, one, CHUNK, channels);
        if (n <= 0) n = 0;
        h = fnv(h, one, (long)CHUNK * channels);
        if (keep && c == keep_chunk) memcpy(keep, one, sizeof(int16_t) * (size_t)CHUNK * (size_t)channels);
    }
    return h;
}

static void golden(const char *name, uint32_t got, uint32_t want)
{
    printf("    GOLDEN %s 0x%08Xu\n", name, got);
    check(got == want, name);
}

int main(void)
{
    printf("audio_music_stereo_test — stereo bed through audio_cont_fill_mix\n\n");
    char dir[] = "/tmp/audio_mst_XXXXXX";
    if (!mkdtemp(dir)) { printf("    FAIL: mkdtemp\n"); return 1; }
    char wav[256];
    snprintf(wav, sizeof wav, "%s/bed.wav", dir);
    if (!write_wav(wav, 44100L * 3)) { printf("    FAIL: fixture\n"); return 1; }
    Audio a;
    int sv[2];

    printf("  A  mono sources render as before\n");
    for (int g = 2; g >= 1; g--) {
        open_audio(&a, g);
        check(audio_music_start(&a, wav, false), "A: file bed starts");
        audio_beep(&a);
        uint32_t h = render(&a, g, NULL, 0);
        golden(g == 2 ? "file bed + beep, 2ch" : "file bed + beep, 1ch", h,
               g == 2 ? GOLDEN_FILE_2CH : GOLDEN_FILE_1CH);
        audio_close(&a);

        socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
        send_header(sv[1], 1);
        send_mono(sv[1], 60000);
        open_audio(&a, g);
        check(START_FD(&a, sv[0], 1), "A: mono stream starts");
        h = render(&a, g, NULL, 0);
        golden(g == 2 ? "mono stream, 2ch" : "mono stream, 1ch", h,
               g == 2 ? GOLDEN_STREAM_2CH : GOLDEN_STREAM_1CH);
        audio_close(&a);
        close(sv[1]);
    }

#ifndef START_FD_OLD
    static int16_t st[CHUNK * 2], mo[CHUNK * 2];

    printf("  B  stereo bed, stereo device\n");
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    send_header(sv[1], 2);
    send_stereo(sv[1], 45000);
    open_audio(&a, 2);
    check(audio_music_start_fd(&a, sv[0], 2), "B: stereo stream starts on a stereo device");
    render(&a, 2, st, 18);
    long ls = 0, rs = 0; int lneg = 0, rpos = 0, cnt = 0;
    for (int i = 600; i < CHUNK - 600; i++) {
        int l = st[2 * i], r = st[2 * i + 1];
        ls += l; rs += r; cnt++;
        if (l < 0) lneg++;
        if (r > 0) rpos++;
    }
    printf("    mean L %ld  mean R %ld (over %d frames)\n", ls / cnt, rs / cnt, cnt);
    check(ls > 0 && rs < 0 && lneg == 0 && rpos == 0, "B: L positive, R negative");
    int ratio_ok = 1;
    for (int i = 600; i < CHUNK - 600; i++)
        if (abs(st[2 * i] + 2 * st[2 * i + 1]) > 2) { ratio_ok = 0; break; }
    check(ratio_ok, "B: |L| is twice |R| frame by frame (no cross-talk, no swap)");
    audio_close(&a);
    close(sv[1]);

    printf("  C  stereo bed, mono device\n");
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    send_header(sv[1], 2);
    send_stereo(sv[1], 45000);
    open_audio(&a, 1);
    check(a.channels == 1, "C: the fake granted one channel");
    check(audio_music_start_fd(&a, sv[0], 2), "C: stereo stream starts on a mono device");
    render(&a, 1, mo, 18);
    int fold_ok = 1, nz = 0;
    for (int i = 600; i < CHUNK - 600; i++) {
        int want = (st[2 * i] + st[2 * i + 1]) >> 1;
        if (mo[i]) nz++;
        if (abs(mo[i] - want) > 2) { fold_ok = 0; printf("    i=%d mono %d want %d\n", i, mo[i], want); break; }
    }
    check(nz > 0, "C: the mono output is not silent");
    check(fold_ok, "C: mono output is (L+R)>>1 of the stereo output");
    audio_close(&a);
    close(sv[1]);

    printf("  D  channel count must match\n");
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    send_header(sv[1], 1);
    send_mono(sv[1], 1000);
    open_audio(&a, 2);
    check(!audio_music_start_fd(&a, sv[0], 2), "D: mono stream refused when stereo asked");
    audio_close(&a);
    close(sv[0]); close(sv[1]);
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    send_header(sv[1], 2);
    send_stereo(sv[1], 1000);
    open_audio(&a, 2);
    check(!audio_music_start_fd(&a, sv[0], 1), "D: stereo stream refused when mono asked");
    check(!audio_music_start_fd(&a, sv[0], 3), "D: three channels never asked");
    audio_close(&a);
    close(sv[0]); close(sv[1]);
#endif

    printf("\naudio_music_stereo_test: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
