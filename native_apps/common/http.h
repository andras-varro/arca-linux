/*
 * http.h — a non-blocking HTTP/1.0 client, driven by polling from the render loop.
 *
 * One request per `HttpReq`, caller-owned.  `http_begin()` opens the socket and
 * queues the request; `http_poll()` is then called once per frame and does a
 * bounded amount of non-blocking work (a few `recv`/`send` calls, never a wait)
 * until it reports DONE or ERROR.  No thread, no `select` with a timeout, so the
 * 60 Hz loop never stalls on the network.
 *
 * ⚠️ **The one accepted block is name resolution.**  An IPv4 literal goes through
 * `inet_pton()` and cannot block; a hostname falls back to a blocking
 * `getaddrinfo()` inside `http_begin()`, which can hold the frame for as long as
 * the resolver takes.  Pass an address when the caller has one.
 *
 * HTTP/1.0 ONLY, on purpose.  Measured against Jellyfin 12.1: an HTTP/1.0 request
 * gets an unchunked body that ends at connection close, an HTTP/1.1 request gets
 * `Transfer-Encoding: chunked`.  This client has no chunk decoder, so a chunked
 * reply is an ERROR rather than garbage handed to a JSON parser or the audio layer.
 *
 * Two modes:
 *   HTTP_BUFFER  the body is collected into a caller-supplied buffer.  A body that
 *                does not fit is an ERROR, never a truncation (a half-read JSON
 *                document parses as an error far from the cause).  The buffer is
 *                always NUL-terminated, so the largest body is `buf_size - 1`.
 *   HTTP_STREAM  stops after the head.  `http_take_fd()` hands over the socket
 *                positioned at the FIRST BODY BYTE: the head is read with
 *                `MSG_PEEK` and consumed exactly, so no body byte is ever lost
 *                (`audio_music_start_fd()` needs that).  The fd stays non-blocking.
 *
 * ⚠️ **The extra headers carry a token.**  This module never logs them and
 * `http_error()` never quotes one.  The request is built in the struct's own head
 * buffer, which is overwritten with zeros as soon as the request has been sent (or
 * the request ends), so the token does not outlive the send inside this object.
 * The caller's own copy is the caller's to wipe.
 *
 * Timeout: one inactivity timer on CLOCK_MONOTONIC covers connect, request send,
 * head and body — it restarts whenever any byte moves, so a slow but live stream
 * never trips it and a server that accepts and goes silent does, after
 * `timeout_ms` (default 5000).  A second deadline, `total_timeout_ms` (default
 * 60000, 0 = none), runs from the end of `http_begin()` and is not restarted by
 * progress, so a server that trickles a byte at a time still ends.  In HTTP_STREAM
 * it covers only the wait for the head.
 *
 * The request builder and the head parser are pure (no socket) and are exported so
 * `tests/http_test.c` reaches them directly.
 */

#ifndef ARCA_HTTP_H
#define ARCA_HTTP_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

#define HTTP_HEAD_MAX        8192   /**< request head and reply head both fit this  */
#define HTTP_DEFAULT_TIMEOUT_MS 5000
#define HTTP_DEFAULT_TOTAL_MS   60000

/* ── pure: the request builder ─────────────────────────────────────────── */

/**
 * Build the request head into `out` (NUL-terminated).  Returns its length, or -1
 * when it does not fit `cap` or an argument would break the framing: `method`,
 * `host` and `path` must be free of CR, LF and space (a header-injection guard),
 * and `path` must start with '/'.
 *
 * Emits, in order: the request line (`HTTP/1.0`), `Host:` (with `:port` unless the
 * port is the scheme's default, 80 plain or 443 with `tls`), `extra_headers` verbatim (zero or more lines; a missing final CRLF
 * is added; NULL or "" for none), `Content-Length:` when `body_len > 0` or the
 * method is POST/PUT, `Connection: close`, and the blank line.
 */
int http_build_request(char *out, size_t cap, const char *method, const char *host,
                       int port, bool tls, const char *path, const char *extra_headers,
                       size_t body_len);

/* ── pure: the reply-head parser ───────────────────────────────────────── */

typedef struct {
    int    status;          /**< the status code once the status line is parsed, else 0 */
    long   content_length;  /**< -1 when the reply carries none                          */
    bool   chunked;         /**< a Transfer-Encoding naming "chunked" was seen           */
    /* parser state — private */
    size_t total;           /**< head bytes consumed so far                              */
    size_t line_len;
    bool   have_status;
    bool   overlong;        /**< the current line has outrun line[]; only its prefix is kept */
    int    sticky;          /**< the terminal result once reached, so later pushes are inert */
    char   line[256];       /**< the current line, or the prefix of a longer one          */
} HttpHead;

typedef enum {
    HTTP_HEAD_MORE = 0,     /**< need more bytes                                         */
    HTTP_HEAD_DONE = 1,     /**< the blank line was consumed                             */
    HTTP_HEAD_BAD  = -1,    /**< malformed status line or Content-Length, or an overlong line the parser reads */
    HTTP_HEAD_TOO_BIG = -2  /**< more than HTTP_HEAD_MAX bytes without a blank line      */
} HttpHeadResult;

void http_head_init(HttpHead *h);

/**
 * Push bytes; `*used` receives how many of the `n` were part of the head.  They
 * differ only on DONE, where the rest is the start of the body.  Header names are
 * case-insensitive; CRLF and a bare LF both end a line.  After DONE, BAD or
 * TOO_BIG the parser consumes nothing further.
 */
HttpHeadResult http_head_push(HttpHead *h, const char *buf, size_t n, size_t *used);

/* ── the polled request ────────────────────────────────────────────────── */

typedef enum { HTTP_BUFFER = 0, HTTP_STREAM = 1 } HttpMode;
typedef enum { HTTP_PENDING = 0, HTTP_DONE = 1, HTTP_ERROR = -1 } HttpPoll;

typedef struct {
    /* the caller may set this between http_begin() and the first http_poll() */
    int      timeout_ms;
    int      total_timeout_ms;       /**< whole-request deadline; 0 = none                    */

    /* private — read through the accessors below */
    int      fd;
    bool     tls;                   /**< this request runs over TLS                          */
    void    *ssl;                    /**< the SSL object while TLS is live (opaque: no OpenSSL header here) */
    int      phase;
    HttpMode mode;
    HttpHead head;
    size_t   sent, head_len;
    const char *body_out;            /**< caller's request body, must outlive DONE   */
    size_t   body_out_len, body_sent;
    char    *buf;                    /**< HTTP_BUFFER destination                    */
    size_t   buf_size, buf_len;
    long long start_ms, last_ms;
    char     err[96];
    char     req[HTTP_HEAD_MAX];     /**< request head, wiped after the send         */
} HttpReq;

/**
 * Start a request.  `body` may be NULL (body_len 0).  The body is NOT copied: it
 * must stay valid until DONE or ERROR.  In HTTP_BUFFER mode `buf`/`buf_size`
 * receive the reply body (`buf_size >= 1`); HTTP_STREAM ignores them.
 *
 * Returns 0, or -1 with `http_error()` set; a failed begin also makes the first
 * `http_poll()` return HTTP_ERROR, so a caller may ignore the return and poll.
 * Always pair with `http_end()`.  Call `http_init()` once on a new struct before
 * the first `http_begin()`; a begin on a struct that still owns a socket closes it.
 */
int http_begin(HttpReq *r, const char *host, int port, bool tls, const char *method,
               const char *path, const char *extra_headers, const char *body,
               size_t body_len, HttpMode mode, char *buf, size_t buf_size);

/**
 * With `tls` the request is HTTPS: after the connect a non-blocking TLS handshake runs
 * from http_poll() under the same timeouts.  ⚠️ **Verification is always on and cannot
 * be turned off**: the chain must verify against the CA bundle, the certificate must
 * match `host` (an IP literal is matched against IP SANs) and SNI is sent for a name.
 * A refusal is an HTTP_ERROR whose http_error() starts "TLS:" and names the reason
 * (not trusted / host name mismatch / expired / no CA bundle).  The bundle is
 * /etc/ssl/certs/ca-certificates.crt, else /opt/arca/ca-certificates.crt; it is
 * loaded once per process, on the first TLS begin (a blocking parse, like name resolution).
 * HTTPS replies without Content-Length end at connection close; a close without
 * close_notify after the head counts as that end (with a Content-Length a short body
 * is still an error).
 */

/** Test hook: use `path` as the only CA bundle from now on (NULL = the defaults); drops the cached context. */
void http_tls_set_ca_file(const char *path);

/** Make a struct that holds no socket.  Call once before first use (not on one in use). */
void        http_init(HttpReq *r);

/** One bounded slice of work.  Cheap to call every frame; never blocks. */
HttpPoll http_poll(HttpReq *r);

/** Status code of the reply (valid once the head is in), else 0. */
int         http_status(const HttpReq *r);
/** HTTP_BUFFER after DONE: the body (NUL-terminated) and its length. */
const char *http_body(const HttpReq *r);
size_t      http_body_len(const HttpReq *r);
/**
 * HTTP_STREAM after DONE: transfer ownership of the socket (positioned at the
 * first body byte).  Returns -1 in any other state.  `http_end()` will not close it.
 */
int         http_take_fd(HttpReq *r);
/**
 * The reader and closer for an fd that http_take_fd() returned.  Plain request: exactly
 * read(2) and close(2).  TLS request: SSL_read on the connection the fd belongs to (the
 * fd stays the raw non-blocking socket, so poll() on it still means "bytes may be
 * readable"), and the close frees the TLS state first.  Same signatures as the
 * `AudioFdIo` members, so a TLS stream reaches `audio_wav.c` without that module
 * (or ScummVM, which links it) knowing about TLS.  An fd that is not a live TLS
 * stream falls through to read()/close(); the table is keyed by fd AND socket inode, so
 * an fd closed behind its back and reused is never read as TLS.
 */
ssize_t     http_stream_read(int fd, void *buf, size_t n);
void        http_stream_close(int fd);
/** A short reason for the log after ERROR, "" otherwise.  Never contains headers. */
const char *http_error(const HttpReq *r);
/** Close whatever the request still owns and wipe the request head.  Idempotent. */
void        http_end(HttpReq *r);

#endif
