/*
 * http_test.c — host regression for common/http.c.
 *
 * Three layers: the pure request builder, the pure head parser (split at every
 * byte offset), and the polled socket client against a forked loopback server
 * that serves canned replies.  The client is driven the way an app drives it, a
 * loop of http_poll() plus a short usleep(), and every call is timed: a poll that
 * takes more than a few ms is a render-loop stall and fails here.
 *
 *   gcc -Wall -Wextra -Wno-unused-parameter -I common -fsanitize=address,undefined \
 *       -o build/http_test tests/http_test.c common/http.c -lssl -lcrypto -lm && ./build/http_test
 *
 * SIGPIPE is deliberately NOT ignored in this process: the hang-up case sends a
 * large body to a peer that has gone, and the client's MSG_NOSIGNAL is the only
 * thing between that and a dead test.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <dirent.h>
#include <fcntl.h>
#include <netdb.h>
#include <time.h>
#include <unistd.h>

#include "http.h"

static int checks, failed;
#define CHECK(c) do { checks++; if (!(c)) { failed++; \
    printf("  FAIL line %d: %s\n", __LINE__, #c); } } while (0)

static long long now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

/* ── layer 1: the builder ──────────────────────────────────────────────── */

static void test_builder(void)
{
    char o[512];
    int n = http_build_request(o, sizeof(o), "GET", "10.0.0.5", 8096, false, "/Users/Me",
                               "Authorization: MediaBrowser Token=\"T\"\r\n", 0);
    CHECK(n > 0 && (size_t)n == strlen(o));
    CHECK(strcmp(o, "GET /Users/Me HTTP/1.0\r\nHost: 10.0.0.5:8096\r\n"
                    "Authorization: MediaBrowser Token=\"T\"\r\n"
                    "Connection: close\r\n\r\n") == 0);

    n = http_build_request(o, sizeof(o), "GET", "h", 80, false, "/", NULL, 0);
    CHECK(n > 0 && strcmp(o, "GET / HTTP/1.0\r\nHost: h\r\nConnection: close\r\n\r\n") == 0);

    /* a missing final CRLF is added; a body adds Content-Length */
    n = http_build_request(o, sizeof(o), "POST", "h", 81, false, "/a", "X-A: 1", 7);
    CHECK(n > 0 && strcmp(o, "POST /a HTTP/1.0\r\nHost: h:81\r\nX-A: 1\r\n"
                             "Content-Length: 7\r\nConnection: close\r\n\r\n") == 0);
    /* POST with no body still states its length */
    n = http_build_request(o, sizeof(o), "POST", "h", 80, false, "/a", "", 0);
    CHECK(n > 0 && strstr(o, "Content-Length: 0\r\n") != NULL);
    /* GET without a body does not */
    n = http_build_request(o, sizeof(o), "GET", "h", 80, false, "/a", "", 0);
    CHECK(n > 0 && strstr(o, "Content-Length") == NULL);

    /* framing guards */
    CHECK(http_build_request(o, sizeof(o), "GET", "h", 80, false, "/a b", NULL, 0) < 0);
    CHECK(http_build_request(o, sizeof(o), "GET", "h", 80, false, "/a\r\nX: y", NULL, 0) < 0);
    CHECK(http_build_request(o, sizeof(o), "GET", "h\r\n", 80, false, "/", NULL, 0) < 0);
    CHECK(http_build_request(o, sizeof(o), "GE T", "h", 80, false, "/", NULL, 0) < 0);
    CHECK(http_build_request(o, sizeof(o), "GET", "h", 80, false, "a", NULL, 0) < 0);
    CHECK(http_build_request(o, sizeof(o), "GET", "h", 0, false, "/", NULL, 0) < 0);
    CHECK(http_build_request(o, sizeof(o), "GET", "h", 80, false, "/", "A: b\r\n\r\nGET /evil", 0) < 0);
    CHECK(http_build_request(o, sizeof(o), "GET", "h", 80, false, "/", "\r\nA: b", 0) < 0);
    CHECK(http_build_request(o, sizeof(o), "GET", "h", 80, false, NULL, NULL, 0) < 0);

    /* exact fit succeeds, one byte short fails */
    n = http_build_request(o, sizeof(o), "GET", "h", 80, false, "/", NULL, 0);
    CHECK(http_build_request(o, (size_t)n + 1, "GET", "h", 80, false, "/", NULL, 0) == n);
    CHECK(http_build_request(o, (size_t)n, "GET", "h", 80, false, "/", NULL, 0) < 0);
}

/* ── layer 2: the parser ───────────────────────────────────────────────── */

static const char HEAD1[] =
    "HTTP/1.0 200 OK\r\nContent-Type: audio/wav\r\ncontent-LENGTH:  1234 \r\n"
    "Server: x\r\nNoColonLine\r\n\r\n";

static void push_split(const char *text, size_t k, HttpHeadResult *res, HttpHead *h,
                       size_t *used_total)
{
    size_t n = strlen(text), u1 = 0, u2 = 0;
    http_head_init(h);
    HttpHeadResult r = http_head_push(h, text, k, &u1);
    if (r == HTTP_HEAD_MORE) r = http_head_push(h, text + k, n - k, &u2);
    *res = r;
    *used_total = u1 + u2;
}

static void test_parser(void)
{
    HttpHead h;
    HttpHeadResult r;
    size_t used;
    size_t n = strlen(HEAD1);

    /* every split point: same verdict, same fields, body bytes are not consumed */
    char with_body[sizeof(HEAD1) + 8];
    snprintf(with_body, sizeof(with_body), "%sBODYBODY", HEAD1);
    for (size_t k = 0; k <= n + 8; k++) {
        push_split(with_body, k, &r, &h, &used);
        CHECK(r == HTTP_HEAD_DONE);
        CHECK(used == n);
        CHECK(h.status == 200 && h.content_length == 1234 && !h.chunked);
    }
    /* a head that stops one byte early is not done at any split */
    for (size_t k = 0; k < n - 1; k++) {
        char cut[sizeof(HEAD1)];
        memcpy(cut, HEAD1, n - 1);
        cut[n - 1] = 0;
        push_split(cut, k, &r, &h, &used);
        CHECK(r == HTTP_HEAD_MORE);
    }
    /* one byte at a time */
    http_head_init(&h);
    for (size_t i = 0; i < n; i++) r = http_head_push(&h, HEAD1 + i, 1, &used);
    CHECK(r == HTTP_HEAD_DONE && h.status == 200 && h.content_length == 1234);

    /* bare LF, mixed endings */
    push_split("HTTP/1.1 404 Not Found\nContent-Length: 0\n\n", 7, &r, &h, &used);
    CHECK(r == HTTP_HEAD_DONE && h.status == 404 && h.content_length == 0);
    push_split("HTTP/1.0 200 OK\r\nA: b\nC: d\r\n\n", 3, &r, &h, &used);
    CHECK(r == HTTP_HEAD_DONE);

    /* no Content-Length, chunked in any case */
    push_split("HTTP/1.0 200 OK\r\nTRANSFER-encoding: Chunked\r\n\r\n", 20, &r, &h, &used);
    CHECK(r == HTTP_HEAD_DONE && h.chunked && h.content_length == -1);
    push_split("HTTP/1.0 200 OK\r\nTransfer-Encoding: identity\r\n\r\n", 20, &r, &h, &used);
    CHECK(r == HTTP_HEAD_DONE && !h.chunked);
    push_split("HTTP/1.0 200\r\n\r\n", 5, &r, &h, &used);          /* no reason phrase */
    CHECK(r == HTTP_HEAD_DONE && h.status == 200);

    /* malformed */
    push_split("HTTP/2 200 OK\r\n\r\n", 4, &r, &h, &used);          CHECK(r == HTTP_HEAD_BAD);
    push_split("HTTP/1.1 20 OK\r\n\r\n", 4, &r, &h, &used);         CHECK(r == HTTP_HEAD_BAD);
    push_split("HTTP/1.1 2000 OK\r\n\r\n", 4, &r, &h, &used);       CHECK(r == HTTP_HEAD_BAD);
    push_split("HTTP/1.1 099 OK\r\n\r\n", 4, &r, &h, &used);        CHECK(r == HTTP_HEAD_BAD);
    push_split("GET / HTTP/1.0\r\n\r\n", 4, &r, &h, &used);         CHECK(r == HTTP_HEAD_BAD);
    push_split("\r\nHTTP/1.0 200 OK\r\n\r\n", 4, &r, &h, &used);    CHECK(r == HTTP_HEAD_BAD);
    push_split("HTTP/1.0 200 OK\r\nContent-Length: abc\r\n\r\n", 9, &r, &h, &used);
    CHECK(r == HTTP_HEAD_BAD);
    push_split("HTTP/1.0 200 OK\r\nContent-Length: 99999999999\r\n\r\n", 9, &r, &h, &used);
    CHECK(r == HTTP_HEAD_BAD);
    push_split("HTTP/1.0 200 OK\r\nContent-Length:\r\n\r\n", 9, &r, &h, &used);
    CHECK(r == HTTP_HEAD_BAD);
    push_split("HTTP/1.0 200 OK\r\nContent-Length: 2147483647\r\n\r\n", 9, &r, &h, &used);
    CHECK(r == HTTP_HEAD_DONE && h.content_length == 2147483647L);
    push_split("HTTP/1.0 200 OK\r\nContent-Length: 2147483648\r\n\r\n", 9, &r, &h, &used);
    CHECK(r == HTTP_HEAD_BAD);

    /* the 8 KiB cap: exactly HTTP_HEAD_MAX bytes ending in the blank line is fine,
     * one more is TOO_BIG, and a head that never ends is TOO_BIG */
    static char big[HTTP_HEAD_MAX + 64];
    for (int extra = 0; extra <= 1; extra++) {
        size_t len = 0;
        len += (size_t)snprintf(big + len, sizeof(big) - len, "HTTP/1.0 200 OK\r\n");
        size_t total = (size_t)HTTP_HEAD_MAX + (size_t)extra;
        /* header lines of at most 100 bytes: one line is capped at the line buffer */
        for (size_t rem = total - len - 2; rem > 0;) {
            size_t ll = rem < 100 ? rem : 100;
            if (rem > ll && rem - ll < 3) ll -= 3;
            memset(big + len, 'a', ll - 2);
            big[len + ll - 2] = '\r'; big[len + ll - 1] = '\n';
            len += ll;
            rem -= ll;
        }
        big[len++] = '\r'; big[len++] = '\n';
        big[len] = 0;
        CHECK(len == (size_t)HTTP_HEAD_MAX + (size_t)extra);
        http_head_init(&h);
        r = http_head_push(&h, big, len, &used);
        CHECK(r == (extra ? HTTP_HEAD_TOO_BIG : HTTP_HEAD_DONE));
        /* same verdict when fed one byte at a time */
        http_head_init(&h);
        for (size_t i = 0; i < len && r != HTTP_HEAD_DONE && r != HTTP_HEAD_TOO_BIG; i++)
            r = http_head_push(&h, big + i, 1, &used);
        CHECK(r == (extra ? HTTP_HEAD_TOO_BIG : HTTP_HEAD_DONE));
    }
    http_head_init(&h);
    memset(big, 'z', sizeof(big));
    for (size_t i = 63; i < sizeof(big); i += 64) big[i] = 0x0a;   /* short lines, no CRLF needed */
    memcpy(big, "HTTP/1.0 200 OK\r\n", 17);
    r = HTTP_HEAD_MORE;
    for (int i = 0; i < 4 && r == HTTP_HEAD_MORE; i++) r = http_head_push(&h, big, 4096, &used);
    CHECK(r == HTTP_HEAD_TOO_BIG);
    CHECK(http_head_push(&h, "x", 1, &used) == HTTP_HEAD_TOO_BIG && used == 0);   /* inert */
}

/* ── layer 3: the loopback server ──────────────────────────────────────── */

enum { S_CL, S_CLOSE, S_DRIP, S_404, S_CHUNKED, S_BIG_CL, S_BIG_CLOSE, S_SILENT,
       S_STREAM, S_STREAM_DRIP, S_ECHO, S_HANGUP, S_SHORT,
       S_TRICKLE, S_OVER, S_OVER_LATE };

static void wr_all(int fd, const void *p, size_t n)
{
    const char *c = p;
    while (n) {
        ssize_t w = write(fd, c, n);
        if (w <= 0) return;
        c += w; n -= (size_t)w;
    }
}

/* A literal's own length, so a hand-counted size cannot read past it. */
#define WR_LIT(fd, s) wr_all((fd), (s), sizeof(s) - 1)

/* read the request head (and, if it says so, its body) into req */
static size_t read_request(int fd, char *req, size_t cap)
{
    size_t len = 0;
    for (;;) {
        ssize_t n = read(fd, req + len, cap - 1 - len);
        if (n <= 0) break;
        len += (size_t)n;
        req[len] = 0;
        char *e = strstr(req, "\r\n\r\n");
        if (e) {
            char *cl = strstr(req, "Content-Length: ");
            size_t want = (size_t)(e + 4 - req) + (cl ? (size_t)atol(cl + 16) : 0);
            if (len >= want) break;
        }
    }
    req[len] = 0;
    return len;
}

static void serve(int ls, int scenario, int param)
{
    int c = accept(ls, NULL, NULL);
    if (c < 0) _exit(1);
    char req[4096];
    char body[256];
    if (scenario == S_HANGUP) { close(c); _exit(0); }
    if (scenario == S_SILENT) { sleep(3); _exit(0); }
    size_t rl = read_request(c, req, sizeof(req));
    switch (scenario) {
    case S_CL:
        WR_LIT(c, "HTTP/1.0 200 OK\r\nContent-Length: 11\r\nContent-Type: x\r\n\r\nhello world");
        break;
    case S_SHORT:        /* promises 50, delivers 5 */
        WR_LIT(c, "HTTP/1.0 200 OK\r\nContent-Length: 50\r\n\r\nhello");
        break;
    case S_CLOSE: {      /* no Content-Length: the body is whatever arrives before the close */
        WR_LIT(c, "HTTP/1.0 200 OK\r\n\r\n");
        static char b[100000];
        for (int i = 0; i < param; i++) b[i] = (char)('a' + i % 26);
        wr_all(c, b, (size_t)param);
        break;
    }
    case S_DRIP: {
        const char *s = "HTTP/1.0 200 OK\r\nContent-Length: 5\r\n\r\nabcde";
        for (size_t i = 0; s[i]; i++) { wr_all(c, s + i, 1); usleep(300); }
        break;
    }
    case S_404:
        WR_LIT(c, "HTTP/1.0 404 Not Found\r\nContent-Length: 4\r\n\r\nnope");
        break;
    case S_CHUNKED:
        WR_LIT(c, "HTTP/1.0 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n");
        break;
    case S_BIG_CL:
        WR_LIT(c, "HTTP/1.0 200 OK\r\nContent-Length: 1000\r\n\r\n");
        for (int i = 0; i < 1000; i++) { char x = 'q'; wr_all(c, &x, 1); }
        break;
    case S_BIG_CLOSE:
        memset(body, 'q', sizeof(body));
        WR_LIT(c, "HTTP/1.0 200 OK\r\n\r\n");
        wr_all(c, body, (size_t)param);
        break;
    case S_STREAM:       /* head and body in ONE write: the body is already in the socket */
    case S_STREAM_DRIP: {
        static char msg[4200];
        size_t hl = (size_t)snprintf(msg, sizeof(msg), "HTTP/1.0 200 OK\r\nContent-Type: audio/wav\r\n\r\n");
        for (int i = 0; i < 4000; i++) msg[hl + (size_t)i] = (char)(i * 7 + 3);
        if (scenario == S_STREAM) {
            wr_all(c, msg, hl + 4000);
        } else {
            for (size_t i = 0; i < hl; i += 5) { wr_all(c, msg + i, hl - i < 5 ? hl - i : 5); usleep(500); }
            usleep(2000);
            wr_all(c, msg + hl, 4000);
        }
        break;
    }
    case S_TRICKLE: {    /* one byte every 200 ms, until the client gives up or we are killed */
        const char *tr = "HTTP/1.0 200 OK\r\nContent-Length: 100\r\n\r\n";
        for (size_t i = 0;; i++) {
            char x = i < strlen(tr) ? tr[i] : 'a';
            wr_all(c, &x, 1);
            usleep(200000);
        }
    }
    case S_OVER:         /* Content-Length 5, nine body bytes in one write */
        WR_LIT(c, "HTTP/1.0 200 OK\r\nContent-Length: 5\r\n\r\n123456789");
        break;
    case S_OVER_LATE:    /* the head and one body byte, the rest after a pause */
        WR_LIT(c, "HTTP/1.0 200 OK\r\nContent-Length: 5\r\n\r\n1");
        usleep(50000);
        WR_LIT(c, "23456789");
        break;
    case S_ECHO: {       /* reply with the request exactly as received */
        char h[128];
        int hn = snprintf(h, sizeof(h), "HTTP/1.0 200 OK\r\nContent-Length: %zu\r\n\r\n", rl);
        wr_all(c, h, (size_t)hn);
        wr_all(c, req, rl);
        break;
    }
    }
    close(c);
    _exit(0);
}

typedef struct { int ls; int port; pid_t pid; } Srv;

static Srv srv_start(int scenario, int param)
{
    Srv s;
    s.ls = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(s.ls, (struct sockaddr *)&a, sizeof(a)) != 0 || listen(s.ls, 4) != 0) {
        perror("bind/listen"); exit(2);
    }
    socklen_t l = sizeof(a);
    getsockname(s.ls, (struct sockaddr *)&a, &l);
    s.port = ntohs(a.sin_port);
    s.pid = fork();
    if (s.pid == 0) serve(s.ls, scenario, param);
    return s;
}

static void srv_stop(Srv *s)
{
    kill(s->pid, SIGKILL);
    waitpid(s->pid, NULL, 0);
    close(s->ls);
}

static long long worst_poll_us;

/* The app's loop: poll, sleep a millisecond.  Gives up after `limit_ms`. */
static HttpPoll drive(HttpReq *r, int limit_ms)
{
    long long t0 = now_us();
    for (;;) {
        long long a = now_us();
        HttpPoll p = http_poll(r);
        long long d = now_us() - a;
        if (d > worst_poll_us) worst_poll_us = d;
        if (p != HTTP_PENDING) return p;
        if ((now_us() - t0) / 1000 > limit_ms) return HTTP_PENDING;
        usleep(1000);
    }
}

#define SECRET "Authorization: MediaBrowser Token=\"SECRET-TOKEN-123\"\r\n"

static void test_buffer_ok(void)
{
    HttpReq r;
    char buf[64];
    Srv s = srv_start(S_CL, 0);
    http_init(&r);
    http_begin(&r, "127.0.0.1", s.port, false, "GET", "/x", SECRET, NULL, 0, HTTP_BUFFER, buf, sizeof(buf));
    CHECK(drive(&r, 3000) == HTTP_DONE);
    CHECK(http_status(&r) == 200 && http_body_len(&r) == 11);
    CHECK(strcmp(http_body(&r), "hello world") == 0);
    CHECK(r.req[0] == 0 && memchr(r.req, 'S', sizeof(r.req)) == NULL);   /* token wiped */
    CHECK(http_take_fd(&r) == -1);
    http_end(&r);
    srv_stop(&s);

    /* body until close, larger than one recv chunk, buffer fits it exactly + NUL */
    static char big[100001];
    s = srv_start(S_CLOSE, 90000);
    http_init(&r);
    http_begin(&r, "127.0.0.1", s.port, false, "GET", "/x", NULL, NULL, 0, HTTP_BUFFER, big, 90001);
    CHECK(drive(&r, 5000) == HTTP_DONE);
    CHECK(http_body_len(&r) == 90000 && big[90000] == 0);
    int ok = 1;
    for (int i = 0; i < 90000; i++) if (big[i] != (char)('a' + i % 26)) ok = 0;
    CHECK(ok);
    http_end(&r);
    srv_stop(&s);

    /* one byte at a time */
    s = srv_start(S_DRIP, 0);
    http_init(&r);
    http_begin(&r, "127.0.0.1", s.port, false, "GET", "/x", NULL, NULL, 0, HTTP_BUFFER, buf, sizeof(buf));
    CHECK(drive(&r, 5000) == HTTP_DONE);
    CHECK(http_body_len(&r) == 5 && strcmp(http_body(&r), "abcde") == 0);
    http_end(&r);
    srv_stop(&s);

    /* a 404 is a reply, not an error: the caller reads the status */
    s = srv_start(S_404, 0);
    http_init(&r);
    http_begin(&r, "127.0.0.1", s.port, false, "GET", "/x", NULL, NULL, 0, HTTP_BUFFER, buf, sizeof(buf));
    CHECK(drive(&r, 3000) == HTTP_DONE);
    CHECK(http_status(&r) == 404 && strcmp(http_body(&r), "nope") == 0);
    http_end(&r);
    srv_stop(&s);
}

static void test_buffer_errors(void)
{
    HttpReq r;
    char buf[64];
    Srv s;

    s = srv_start(S_CHUNKED, 0);
    http_init(&r);
    http_begin(&r, "127.0.0.1", s.port, false, "GET", "/x", SECRET, NULL, 0, HTTP_BUFFER, buf, sizeof(buf));
    CHECK(drive(&r, 3000) == HTTP_ERROR);
    CHECK(strstr(http_error(&r), "chunked") != NULL);
    http_end(&r);
    srv_stop(&s);

    /* Content-Length over the buffer: refused at the head */
    s = srv_start(S_BIG_CL, 0);
    http_init(&r);
    http_begin(&r, "127.0.0.1", s.port, false, "GET", "/x", NULL, NULL, 0, HTTP_BUFFER, buf, sizeof(buf));
    CHECK(drive(&r, 3000) == HTTP_ERROR);
    CHECK(strstr(http_error(&r), "larger") != NULL);
    http_end(&r);
    srv_stop(&s);

    /* no Content-Length: capacity-1 bytes fit, one more does not */
    s = srv_start(S_BIG_CLOSE, 63);
    http_init(&r);
    http_begin(&r, "127.0.0.1", s.port, false, "GET", "/x", NULL, NULL, 0, HTTP_BUFFER, buf, sizeof(buf));
    CHECK(drive(&r, 3000) == HTTP_DONE && http_body_len(&r) == 63);
    http_end(&r);
    srv_stop(&s);
    s = srv_start(S_BIG_CLOSE, 64);
    http_init(&r);
    http_begin(&r, "127.0.0.1", s.port, false, "GET", "/x", NULL, NULL, 0, HTTP_BUFFER, buf, sizeof(buf));
    CHECK(drive(&r, 3000) == HTTP_ERROR);
    http_end(&r);
    srv_stop(&s);
    s = srv_start(S_BIG_CLOSE, 200);
    http_init(&r);
    http_begin(&r, "127.0.0.1", s.port, false, "GET", "/x", NULL, NULL, 0, HTTP_BUFFER, buf, sizeof(buf));
    CHECK(drive(&r, 3000) == HTTP_ERROR);
    http_end(&r);
    srv_stop(&s);

    /* closed before the declared length */
    s = srv_start(S_SHORT, 0);
    http_init(&r);
    http_begin(&r, "127.0.0.1", s.port, false, "GET", "/x", NULL, NULL, 0, HTTP_BUFFER, buf, sizeof(buf));
    CHECK(drive(&r, 3000) == HTTP_ERROR);
    CHECK(strstr(http_error(&r), "mid-body") != NULL);
    http_end(&r);
    srv_stop(&s);

    /* accepts, then says nothing: the inactivity timeout, kept short here */
    s = srv_start(S_SILENT, 0);
    http_init(&r);
    http_begin(&r, "127.0.0.1", s.port, false, "GET", "/x", SECRET, NULL, 0, HTTP_BUFFER, buf, sizeof(buf));
    r.timeout_ms = 250;
    long long t0 = now_us();
    CHECK(drive(&r, 3000) == HTTP_ERROR);
    long long ms = (now_us() - t0) / 1000;
    CHECK(ms >= 200 && ms < 1500);
    CHECK(strstr(http_error(&r), "timeout") != NULL);
    CHECK(strstr(http_error(&r), "SECRET") == NULL);
    CHECK(http_poll(&r) == HTTP_ERROR);          /* stays failed */
    http_end(&r);
    srv_stop(&s);

    /* connection refused: bind to learn a free port, then close it */
    int t = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(t, (struct sockaddr *)&a, sizeof(a));
    socklen_t l = sizeof(a);
    getsockname(t, (struct sockaddr *)&a, &l);
    close(t);
    http_init(&r);
    http_begin(&r, "127.0.0.1", ntohs(a.sin_port), false, "GET", "/x", SECRET, NULL, 0, HTTP_BUFFER, buf, sizeof(buf));
    CHECK(drive(&r, 3000) == HTTP_ERROR);
    CHECK(strstr(http_error(&r), "SECRET") == NULL && http_error(&r)[0] != 0);
    http_end(&r);

    /* bad begin arguments fail the first poll, not the process */
    http_init(&r);
    CHECK(http_begin(&r, "127.0.0.1", 80, false, "GET", "no-slash", NULL, NULL, 0, HTTP_BUFFER, buf, sizeof(buf)) < 0);
    CHECK(http_poll(&r) == HTTP_ERROR);
    http_end(&r);
    http_init(&r);
    CHECK(http_begin(&r, "127.0.0.1", 80, false, "GET", "/", NULL, NULL, 0, HTTP_BUFFER, NULL, 0) < 0);
    CHECK(http_poll(&r) == HTTP_ERROR);
    http_end(&r);
    CHECK(http_poll(&r) == HTTP_ERROR);          /* ended: nothing to poll */
    http_end(&r);
}

static char post_body[300000];

static void test_post_and_hangup(void)
{
    HttpReq r;
    static char buf[8192];
    char port_s[32];
    Srv s = srv_start(S_ECHO, 0);
    http_init(&r);
    http_begin(&r, "127.0.0.1", s.port, false, "POST", "/Users/AuthenticateByName",
               SECRET "Content-Type: application/json\r\n", "{\"a\":1}", 7, HTTP_BUFFER, buf, sizeof(buf));
    CHECK(drive(&r, 3000) == HTTP_DONE);
    snprintf(port_s, sizeof(port_s), "Host: 127.0.0.1:%d\r\n", s.port);
    CHECK(strstr(http_body(&r), "POST /Users/AuthenticateByName HTTP/1.0\r\n") == http_body(&r));
    CHECK(strstr(http_body(&r), port_s) != NULL);
    CHECK(strstr(http_body(&r), "Authorization: MediaBrowser Token=\"SECRET-TOKEN-123\"\r\n") != NULL);
    CHECK(strstr(http_body(&r), "Content-Length: 7\r\nConnection: close\r\n\r\n{\"a\":1}") != NULL);
    http_end(&r);
    srv_stop(&s);

    /* the peer is gone while we send 300 KB: an ERROR, and no SIGPIPE */
    memset(post_body, 'x', sizeof(post_body));
    s = srv_start(S_HANGUP, 0);
    usleep(20000);
    http_init(&r);
    http_begin(&r, "127.0.0.1", s.port, false, "POST", "/x", NULL, post_body, sizeof(post_body),
               HTTP_BUFFER, buf, sizeof(buf));
    CHECK(drive(&r, 3000) == HTTP_ERROR);
    http_end(&r);
    srv_stop(&s);
}

static int read_exact(int fd, unsigned char *out, size_t n, int limit_ms)
{
    size_t got = 0;
    long long t0 = now_us();
    while (got < n && (now_us() - t0) / 1000 < limit_ms) {
        ssize_t k = read(fd, out + got, n - got);
        if (k > 0) got += (size_t)k;
        else if (k == 0) break;
        else if (errno != EAGAIN && errno != EINTR) break;
        else usleep(500);
    }
    return got == n;
}

static void test_stream(void)
{
    for (int drip = 0; drip <= 1; drip++) {
        HttpReq r;
        Srv s = srv_start(drip ? S_STREAM_DRIP : S_STREAM, 0);
        http_init(&r);
        http_begin(&r, "127.0.0.1", s.port, false, "GET", "/Audio/x/stream", SECRET, NULL, 0, HTTP_STREAM, NULL, 0);
        CHECK(drive(&r, 3000) == HTTP_DONE);
        CHECK(http_status(&r) == 200);
        int fd = http_take_fd(&r);
        CHECK(fd >= 0);
        CHECK(http_take_fd(&r) == -1);
        http_end(&r);                    /* must not close the taken fd */
        /* the first body bytes are exactly what the server wrote after the head */
        static unsigned char got[4000];
        CHECK(fd >= 0 && read_exact(fd, got, sizeof(got), 3000));
        int ok = 1;
        for (int i = 0; i < 4000; i++) if (got[i] != (unsigned char)(i * 7 + 3)) ok = 0;
        CHECK(ok);
        if (fd >= 0) close(fd);
        srv_stop(&s);
    }
    /* STREAM on an error status still hands over the fd: the caller checks the code */
    HttpReq r;
    Srv s = srv_start(S_404, 0);
    http_init(&r);
    http_begin(&r, "127.0.0.1", s.port, false, "GET", "/x", NULL, NULL, 0, HTTP_STREAM, NULL, 0);
    CHECK(drive(&r, 3000) == HTTP_DONE && http_status(&r) == 404);
    int fd = http_take_fd(&r);
    unsigned char b[4];
    CHECK(fd >= 0 && read_exact(fd, b, 4, 2000) && memcmp(b, "nope", 4) == 0);
    if (fd >= 0) close(fd);
    http_end(&r);
    srv_stop(&s);
    /* an un-taken STREAM fd is closed by http_end (no leak): the take then fails */
    s = srv_start(S_STREAM, 0);
    http_init(&r);
    http_begin(&r, "127.0.0.1", s.port, false, "GET", "/x", NULL, NULL, 0, HTTP_STREAM, NULL, 0);
    CHECK(drive(&r, 3000) == HTTP_DONE);
    http_end(&r);
    CHECK(http_take_fd(&r) == -1);
    srv_stop(&s);
}

/* ── review fixes: lifetime, deadline, DNS timing, framing, limits ───────── */

/* Test-only resolver: a host name goes here after a 400 ms delay, so a timer that
 * starts before name resolution reads as a 400 ms gap.  Literal IPs never reach it. */
int getaddrinfo(const char *node, const char *svc, const struct addrinfo *hints,
                struct addrinfo **res)
{
    (void)node; (void)svc; (void)hints;
    usleep(400000);
    struct addrinfo *ai = calloc(1, sizeof(*ai) + sizeof(struct sockaddr_in));
    if (!ai) return EAI_MEMORY;
    struct sockaddr_in *sa = (struct sockaddr_in *)(void *)(ai + 1);
    sa->sin_family = AF_INET;
    sa->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ai->ai_family = AF_INET;
    ai->ai_socktype = SOCK_STREAM;
    ai->ai_addrlen = sizeof(*sa);
    ai->ai_addr = (struct sockaddr *)(void *)sa;
    *res = ai;
    return 0;
}

void freeaddrinfo(struct addrinfo *ai) { free(ai); }

static int count_fds(void)
{
    int n = 0;
    DIR *d = opendir("/proc/self/fd");
    if (!d) return -1;
    while (readdir(d)) n++;
    closedir(d);
    return n;
}

static void test_ownership(void)
{
    /* a zeroed struct owns nothing: http_end must not close fd 0 */
    int pp[2];
    CHECK(pipe(pp) == 0);
    int saved0 = dup(0);
    dup2(pp[0], 0);
    close(pp[0]);
    HttpReq z;
    memset(&z, 0, sizeof(z));
    http_end(&z);
    CHECK(fcntl(0, F_GETFD) != -1);
    dup2(saved0, 0);
    close(saved0);
    close(pp[1]);

    /* a second http_begin on a struct that still owns a socket closes the first */
    Srv s = srv_start(S_SILENT, 0);
    HttpReq r;
    char buf[64];
    http_init(&r);
    http_begin(&r, "127.0.0.1", s.port, false, "GET", "/x", NULL, NULL, 0, HTTP_BUFFER, buf, sizeof(buf));
    int one = count_fds();
    http_begin(&r, "127.0.0.1", s.port, false, "GET", "/x", NULL, NULL, 0, HTTP_BUFFER, buf, sizeof(buf));
    CHECK(one > 0 && count_fds() == one);
    http_end(&r);
    CHECK(count_fds() == one - 1);
    srv_stop(&s);
}

static void test_total_deadline(void)
{
    HttpReq r;
    char buf[64];
    Srv s = srv_start(S_TRICKLE, 0);
    http_init(&r);
    http_begin(&r, "127.0.0.1", s.port, false, "GET", "/x", NULL, NULL, 0, HTTP_BUFFER, buf, sizeof(buf));
    r.timeout_ms = 1000;          /* every byte restarts this, so only the total can fire */
    r.total_timeout_ms = 1500;
    long long t0 = now_us();
    HttpPoll p = drive(&r, 4000);
    long long ms = (now_us() - t0) / 1000;
    CHECK(p == HTTP_ERROR);
    CHECK(ms >= 1400 && ms < 2000);
    CHECK(strstr(http_error(&r), "deadline") != NULL);
    http_end(&r);
    srv_stop(&s);
}

static void test_timer_after_dns(void)
{
    HttpReq r;
    char buf[64];
    Srv s = srv_start(S_SILENT, 0);
    http_init(&r);
    http_begin(&r, "fake.test", s.port, false, "GET", "/x", NULL, NULL, 0, HTTP_BUFFER, buf, sizeof(buf));
    CHECK(now_us() / 1000 - r.last_ms < 200);   /* the stub spends 400 ms in DNS */
    http_end(&r);
    srv_stop(&s);
}

static void test_body_capped(void)
{
    HttpReq r;
    char buf[64];
    for (int late = 0; late <= 1; late++) {
        Srv s = srv_start(late ? S_OVER_LATE : S_OVER, 0);
        http_init(&r);
        http_begin(&r, "127.0.0.1", s.port, false, "GET", "/x", NULL, NULL, 0, HTTP_BUFFER, buf, sizeof(buf));
        CHECK(drive(&r, 3000) == HTTP_DONE);
        CHECK(http_body_len(&r) == 5);
        CHECK(strcmp(http_body(&r), "12345") == 0);
        http_end(&r);
        srv_stop(&s);
    }
}

static void test_head_limits(void)
{
    HttpHead h;
    HttpHeadResult r;
    size_t used;
    /* a header line whose content is 255 bytes is fine, 256 is BAD */
    for (int L = 247; L <= 248; L++) {
        char b[512];
        int k = snprintf(b, sizeof(b), "HTTP/1.0 200 OK\r\nX-Long: ");
        memset(b + k, 'a', (size_t)L);
        k += L;
        memcpy(b + k, "\r\n\r\n", 4);
        k += 4;
        http_head_init(&h);
        r = http_head_push(&h, b, (size_t)k, &used);
        CHECK(r == (L == 247 ? HTTP_HEAD_DONE : HTTP_HEAD_BAD));
    }
    push_split("HTTP/1.0 200 OK\r\nContent-Length: 5\r\nContent-Length: 6\r\n\r\n", 9, &r, &h, &used);
    CHECK(r == HTTP_HEAD_BAD);
    push_split("HTTP/1.0 200 OK\r\nContent-Length: 5\r\ncontent-length: 5\r\n\r\n", 9, &r, &h, &used);
    CHECK(r == HTTP_HEAD_DONE && h.content_length == 5);
}

static void test_extras_framing(void)
{
    char o[512];
    CHECK(http_build_request(o, sizeof(o), "GET", "h", 80, false, "/", "X: a\rY: b\r\n", 0) < 0);
    CHECK(http_build_request(o, sizeof(o), "GET", "h", 80, false, "/", "X: a\nY: b\r\n", 0) < 0);
    CHECK(http_build_request(o, sizeof(o), "GET", "h", 80, false, "/", "X: a\rr\rY: b\r\n", 0) < 0);
    CHECK(http_build_request(o, sizeof(o), "GET", "h", 80, false, "/", "X: a\r\n\r\n", 0) < 0);
    CHECK(http_build_request(o, sizeof(o), "GET", "h", 80, false, "/", "\r\nX: a\r\n", 0) < 0);
    CHECK(http_build_request(o, sizeof(o), "GET", "h", 80, false, "/", "\nX: a\r\n", 0) < 0);
    CHECK(http_build_request(o, sizeof(o), "GET", "h", 80, false, "/", "X: a\r\nY: b\r\n", 0) > 0);
}

int main(void)
{
    test_builder();
    test_parser();
    test_buffer_ok();
    test_buffer_errors();
    test_post_and_hangup();
    test_stream();
    test_ownership();
    test_total_deadline();
    test_timer_after_dns();
    test_body_capped();
    test_head_limits();
    test_extras_framing();
    /* A poll is a slice of one frame.  The bound is 5 ms, loose for a loaded host and
     * ASan; a poll that waited on a socket would be hundreds of ms. */
    printf("worst http_poll: %lld us\n", worst_poll_us);
    CHECK(worst_poll_us < 5000);
    printf("http_test: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
