/*
 * stream_play.c — play one HTTP WAV stream through the mix bus.  A temporary
 * check of audio_music_start_fd(); the source API in common/audio_wav.h is the
 * permanent part.
 *
 *   stream_play <host> <port> <path-and-query> [seconds]
 *
 * With `seconds`, plays that long and exits 0 (the device has no `timeout`).
 *
 * The request goes through common/http.c in HTTP_STREAM mode: a plain HTTP/1.0 GET
 * (so no chunked encoding: the body is the WAV until the server closes), driven by
 * polling.  http.c reads the reply head exactly, so the socket it hands over sits
 * at the first body byte, and that non-blocking socket goes straight to the audio
 * layer, which is pumped at ~60 Hz.  Prints one status line a second.
 *
 * A token can reach the server two ways.  The environment variable JF_TOKEN, if
 * set and non-empty, is sent as an Authorization header, so it never appears in
 * argv or in the process list.  Otherwise a token the server accepts in the query
 * string comes from argv, as before.  Neither is printed or stored in the tree.
 * It draws nothing and reads no touch.
 */

#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "common/audio.h"
#include "common/http.h"

static volatile sig_atomic_t running = 1;
static void on_signal(int sig) { (void)sig; running = 0; }

/* Not memset: the compiler may drop a memset of a buffer it can see is not read again. */
static void wipe(void *p, size_t n)
{
    volatile unsigned char *q = p;
    while (n--) *q++ = 0;
}

int main(int argc, char *argv[])
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc != 4 && argc != 5) {
        fprintf(stderr, "usage: %s <host> <port> <path-and-query> [seconds]\n", argv[0]);
        return 2;
    }
    /* sigaction WITHOUT SA_RESTART: signal() restarts a blocked system call, so
     * Ctrl-C would not interrupt it. */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    int play_secs = argc == 5 ? atoi(argv[4]) : 0;
    if (argc == 5 && play_secs <= 0) {
        fprintf(stderr, "stream_play: seconds must be a positive number\n");
        return 2;
    }
    int port = atoi(argv[2]);
    if (port < 1 || port > 65535) {
        fprintf(stderr, "stream_play: port must be 1..65535\n");
        return 2;
    }
    signal(SIGPIPE, SIG_IGN);

    char hdr[512];
    const char *headers = NULL;
    const char *token = getenv("JF_TOKEN");
    if (token && *token) {
        int hl = snprintf(hdr, sizeof(hdr),
                          "Authorization: MediaBrowser Client=\"Arca\", Device=\"Arca\", "
                          "DeviceId=\"arca-stream-play\", Version=\"1\", Token=\"%s\"\r\n",
                          token);
        if (hl <= 0 || hl >= (int)sizeof(hdr)) {
            fprintf(stderr, "stream_play: JF_TOKEN is too long\n");
            return 2;
        }
        headers = hdr;
    }

    HttpReq req;
    http_init(&req);
    http_begin(&req, argv[1], port, "GET", argv[3], headers, NULL, 0, HTTP_STREAM, NULL, 0);
    HttpPoll hp;
    wipe(hdr, sizeof(hdr));                    /* the request keeps its own copy; the token is not needed here */
    while ((hp = http_poll(&req)) == HTTP_PENDING) {
        if (!running) {
            http_end(&req);
            fprintf(stderr, "stream_play: interrupted\n");
            return 1;
        }
        usleep(10000);
    }
    if (hp == HTTP_ERROR) {
        fprintf(stderr, "stream_play: %s\n", http_error(&req));
        http_end(&req);
        return 1;
    }
    int status = http_status(&req);
    int fd = http_take_fd(&req);
    http_end(&req);
    if (status != 200 || fd < 0) {
        fprintf(stderr, "stream_play: server said: %d\n", status);
        if (fd >= 0) close(fd);
        return 1;
    }

    Audio audio;
    if (audio_init(&audio) != 0) {
        fprintf(stderr, "stream_play: no audio output\n");
        close(fd);
        return 1;
    }
    if (!audio_music_start_fd(&audio, fd)) {
        fprintf(stderr, "stream_play: the audio layer refused the stream\n");
        close(fd);
        audio_close(&audio);
        return 1;
    }

    time_t last = 0, began = time(NULL);
    bool timed_out = false;
    while (running && !timed_out && audio_music_active(&audio)) {
        audio_pump(&audio);
        time_t now = time(NULL);
        if (play_secs > 0 && now - began >= play_secs) timed_out = true;
        if (now != last) {
            last = now;
            printf("buffered=%ld bytes underruns=%ld\n",
                   audio_music_stream_buffered(&audio),
                   audio_music_stream_underruns(&audio));
        }
        usleep(16667);
    }
    printf("stream_play: %s\n", timed_out ? "time up" : running ? "track ended" : "interrupted");

    audio_music_stop(&audio);
    for (int i = 0; i < 20; i++) { audio_pump(&audio); usleep(16667); }  /* let the release fade out */
    audio_close(&audio);
    return 0;
}
