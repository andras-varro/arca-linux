/*
 * tests/audio_stream_fd_test.c — the fd music source through the REAL Audio
 * (audio.c), where audio_stream_test.c drives the bare stream.
 *
 *   A  an Audio whose stream voices were never started owns NO descriptor: with
 *      fd 0 (stdin) pointing at a pipe, starting and stopping a FILE bed, then
 *      audio_close(), must leave fd 0 open.  A zeroed `AudioWavStream.fd` is 0,
 *      and closing "the stream" would close stdin.
 *   B  a streamed bed survives an output-device move: still active, same fd.
 *   C  (control) the fd really belongs to the Audio while it plays, and
 *      audio_close() really closes it, so A cannot pass on a stream never open.
 *
 * The device is faked at the same link seam as audio_carry_test.c.
 *
 * Build and run (host gcc, from native_apps/):
 *   gcc -Wall -Wextra -Wno-unused-parameter -I. \
 *       -Wl,--wrap=audio_out_open_resolved \
 *       -o build/audio_stream_fd_test tests/audio_stream_fd_test.c \
 *       common/audio.c common/audio_gen.c common/audio_out.c common/audio_wav.c \
 *       common/config.c -lm && ./build/audio_stream_fd_test
 */

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "common/audio.h"
#include "common/audio_out.h"

static int failures, checks;
static void check(bool ok, const char *what)
{
    checks++;
    if (ok) printf("    ok   %s\n", what);
    else  { failures++; printf("    FAIL: %s\n", what); }
}

static struct { bool gone; int opens, closes; } g_fake;

static int fake_open(void *ctx, int rate_req, int ch_req, int *rate, int *bits, int *ch)
{
    (void)ctx; (void)rate_req;
    *rate = 44100; *bits = 16; *ch = ch_req;
    return 0;
}
static int fake_space(void *ctx, int frame_bytes, AudioOutSpace *sp)
{
    (void)ctx; (void)frame_bytes;
    if (g_fake.gone) { errno = ENODEV; return -1; }
    sp->period_frames = 1024; sp->ring_frames = 8192; sp->in_flight = 0; sp->space = 8192;
    return 0;
}
static ssize_t fake_write(void *ctx, const void *buf, size_t n, bool *again)
{
    (void)ctx; (void)buf;
    *again = false;
    if (g_fake.gone) { errno = ENODEV; return -1; }
    return (ssize_t)n;
}
static void fake_wait(void *ctx, int usec) { (void)ctx; (void)usec; }
static void fake_close(void *ctx)          { (void)ctx; g_fake.closes++; }
static const AudioOutDev FAKE_DEV = { fake_open, fake_space, fake_write, fake_wait, fake_close };

int __wrap_audio_out_open_resolved(AudioOut *out, int rate_req, int channels_req,
                                   const char **path_out);
int __wrap_audio_out_open_resolved(AudioOut *out, int rate_req, int channels_req,
                                   const char **path_out)
{
    g_fake.gone = false;
    g_fake.opens++;
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

static void pump_n(Audio *a, int n) { for (int i = 0; i < n; i++) audio_pump(a); }
static bool fd_open(int fd) { return fcntl(fd, F_GETFD) != -1; }
/* Still the PIPE, not a descriptor that closed and was reused (the file open takes the lowest free one). */
static bool fd0_is_pipe(void) { struct stat st; return fstat(0, &st) == 0 && S_ISFIFO(st.st_mode); }

int main(void)
{
    printf("audio_stream_fd_test — the fd music source through audio.c\n\n");
    char dir[] = "/tmp/audio_sfd_XXXXXX";
    if (!mkdtemp(dir)) { printf("    FAIL: mkdtemp\n"); return 1; }
    char wav[256];
    snprintf(wav, sizeof wav, "%s/bed.wav", dir);
    if (!write_wav(wav, 44100L * 5)) { printf("    FAIL: fixture\n"); return 1; }

    /* ── A: fd 0 is not the Audio's ─────────────────────────────────────── */
    printf("  A  an Audio with no stream started leaves fd 0 alone\n");
    int saved = dup(0);
    int pp[2];
    if (saved < 0 || pipe(pp) != 0) { perror("setup"); return 2; }
    dup2(pp[0], 0);
    close(pp[0]);
    Audio a;
    memset(&g_fake, 0, sizeof g_fake);
    check(audio_init_unchecked(&a) == 0 && a.available, "A: the fake stream opens");
    check(a.music.stream.fd == -1 && a.sfx.stream.fd == -1,
          "A: both stream voices start with fd -1");
    check(audio_music_start(&a, wav, false), "A: a FILE bed starts");
    check(fd_open(0) && fd0_is_pipe(), "A: fd 0 still the pipe after a file bed started");
    pump_n(&a, 5);
    audio_music_stop(&a);
    pump_n(&a, 40);
    check(fd_open(0) && fd0_is_pipe(), "A: fd 0 still the pipe after the bed stopped");
    audio_close(&a);
    check(fd_open(0) && fd0_is_pipe(), "A: fd 0 still the pipe after audio_close()");
    dup2(saved, 0);
    close(saved);
    close(pp[1]);

    /* ── B + C: a streamed bed across a device move ──────────────────────── */
    printf("  B  a streamed bed survives an output-device move\n");
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) { perror("socketpair"); return 2; }
    unsigned char h[44] = {0};
    memcpy(h, "RIFF", 4); memset(h + 4, 0xff, 4); memcpy(h + 8, "WAVEfmt ", 8);
    h[16] = 16; h[20] = 1; h[22] = 1; h[24] = 0x44; h[25] = 0xAC; h[28] = 0x88; h[29] = 0x58;
    h[30] = 1; h[32] = 2; h[34] = 16; memcpy(h + 36, "data", 4); memset(h + 40, 0xff, 4);
    static unsigned char pcm[120000];
    for (size_t i = 0; i < sizeof pcm; i++) pcm[i] = (unsigned char)(i * 7);
    check(write(sv[1], h, 44) == 44, "B: header sent");
    check(write(sv[1], pcm, sizeof pcm) > 0, "B: pcm sent");

    memset(&g_fake, 0, sizeof g_fake);
    check(audio_init_unchecked(&a) == 0 && a.available, "B: the fake stream opens");
    check(audio_music_start_fd(&a, sv[0]), "B: the stream starts");
    pump_n(&a, 10);
    check(audio_music_active(&a), "B: active before the move");
    check(a.music.stream.fd == sv[0], "C: the Audio holds the fd while it plays");
    g_fake.gone = true;
    audio_pump(&a);
    audio_pump(&a);
    check(g_fake.opens == 2 && g_fake.closes == 1, "B: exactly one reopen (control)");
    check(audio_music_active(&a), "B: still active after the move");
    check(a.music.stream.fd == sv[0] && fd_open(sv[0]), "B: the socket was not leaked or closed");
    pump_n(&a, 10);
    check(audio_music_active(&a), "B: still playing on the new bus");
    audio_close(&a);
    check(!fd_open(sv[0]), "C: audio_close() closed the stream fd");
    close(sv[1]);

    printf("\naudio_stream_fd_test: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
