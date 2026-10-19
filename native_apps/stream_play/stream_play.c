/*
 * stream_play.c — play one HTTP WAV stream through the mix bus.  A temporary
 * check of audio_music_start_fd(); the source API in common/audio_wav.h is the
 * permanent part.
 *
 *   stream_play <host> <port> <path-and-query> [seconds]
 *
 * With `seconds`, plays that long and exits 0 (the device has no `timeout`).
 *
 * Sends a plain HTTP/1.0 GET (no chunked encoding: the body is the WAV until the
 * server closes), checks for status 200, consumes the headers, then hands the
 * non-blocking socket to the audio layer and pumps it at ~60 Hz.  Prints one
 * status line a second.  The query string (a token, if the server wants one)
 * comes from argv only — nothing is stored in the tree.  It draws nothing and
 * reads no touch.
 */

#include <errno.h>
#include <netdb.h>
#include <signal.h>
#include <sys/time.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "common/audio.h"

static volatile sig_atomic_t running = 1;
static void on_signal(int sig) { (void)sig; running = 0; }

/* ~5 s on connect (SNDTIMEO) and on the blocking header read (RCVTIMEO); a dead
 * server would otherwise hold this tool until the kernel gives up. */
static void set_timeouts(int fd)
{
    struct timeval tv;
    tv.tv_sec = 5; tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

static int connect_to(const char *host, const char *port)
{
    struct addrinfo hints, *res, *ai;
    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    int rc = getaddrinfo(host, port, &hints, &res);
    if (rc != 0) {
        fprintf(stderr, "stream_play: cannot resolve %s: %s\n", host, gai_strerror(rc));
        return -1;
    }
    int fd = -1;
    for (ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        set_timeouts(fd);
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) break;
        if (!running) { close(fd); fd = -1; break; }
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) fprintf(stderr, "stream_play: cannot connect to %s:%s\n", host, port);
    return fd;
}

/* Blocking, one byte at a time: the header is a few hundred bytes and reading
 * exactly to the blank line leaves every body byte for the audio layer. */
static int skip_http_headers(int fd)
{
    char line[256];
    size_t len = 0;
    int first = 1, blank_run = 0;
    for (;;) {
        char c;
        ssize_t n = read(fd, &c, 1);
        if (n < 0 && errno == EINTR) { if (!running) return -1; continue; }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            fprintf(stderr, "stream_play: timed out reading the headers\n");
            return -1;
        }
        if (n <= 0) { fprintf(stderr, "stream_play: connection closed in the headers\n"); return -1; }
        if (c == '\r') continue;
        if (c != '\n') {
            blank_run = 0;
            if (len < sizeof(line) - 1) line[len++] = c;
            continue;
        }
        line[len] = 0;
        if (first) {
            first = 0;
            if (strncmp(line, "HTTP/", 5) != 0 || !strchr(line, ' ')
                || strncmp(strchr(line, ' ') + 1, "200", 3) != 0) {
                fprintf(stderr, "stream_play: server said: %s\n", line);
                return -1;
            }
        }
        if (len == 0 && ++blank_run >= 1) return 0;     /* the blank line */
        len = 0;
    }
}

int main(int argc, char *argv[])
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc != 4 && argc != 5) {
        fprintf(stderr, "usage: %s <host> <port> <path-and-query> [seconds]\n", argv[0]);
        return 2;
    }
    /* sigaction WITHOUT SA_RESTART: signal() restarts a blocked connect() or read(),
     * so Ctrl-C would not interrupt them. */
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
    signal(SIGPIPE, SIG_IGN);

    int fd = connect_to(argv[1], argv[2]);
    if (fd < 0) return 1;

    char req[1024];
    int rl = snprintf(req, sizeof(req), "GET %s HTTP/1.0\r\nHost: %s\r\n\r\n", argv[3], argv[1]);
    if (rl <= 0 || rl >= (int)sizeof(req) || write(fd, req, (size_t)rl) != rl) {
        fprintf(stderr, "stream_play: cannot send the request\n");
        close(fd);
        return 1;
    }
    if (skip_http_headers(fd) != 0) { close(fd); return 1; }

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
