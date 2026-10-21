/*
 * http.c — see http.h.  Polled, non-blocking HTTP/1.0 client; pure request
 * builder and head parser at the top, the socket state machine below.
 */

#include "http.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509_vfy.h>
#include <openssl/x509v3.h>

/* ── pure: request builder ─────────────────────────────────────────────── */

/* No byte a header line could be split on: space, CR, LF and every other control. */
static bool clean_token(const char *s)
{
    if (!s || !*s) return false;
    for (; *s; s++)
        if ((unsigned char)*s <= 0x20 || *s == 0x7f) return false;
    return true;
}

/* The extras must be whole CRLF lines: every CR is followed by LF, every LF is
 * preceded by CR, and no line is empty (that would be the blank line ending the head). */
static bool extras_framing_ok(const char *s)
{
    size_t line_start = 0;
    for (size_t i = 0; s[i]; i++) {
        if (s[i] == '\r') {
            if (s[i + 1] != '\n') return false;
        } else if (s[i] == '\n') {
            if (i == 0 || s[i - 1] != '\r') return false;
            if (i - 1 == line_start) return false;
            line_start = i + 1;
        }
    }
    return true;
}

int http_build_request(char *out, size_t cap, const char *method, const char *host,
                       int port, bool tls, const char *path, const char *extra_headers,
                       size_t body_len)
{
    if (!out || cap == 0 || !clean_token(method) || !clean_token(host)
        || !clean_token(path) || path[0] != '/' || port < 1 || port > 65535)
        return -1;
    if (!extra_headers) extra_headers = "";
    /* A bare CR or LF, or a blank line, inside the extras would end the head early or
     * start a second one, so refuse it rather than send a different request. */
    if (!extras_framing_ok(extra_headers))
        return -1;

    size_t xl = strlen(extra_headers);
    bool need_crlf = xl > 0 && extra_headers[xl - 1] != '\n';
    bool want_len = body_len > 0 || strcmp(method, "POST") == 0 || strcmp(method, "PUT") == 0;

    int n = snprintf(out, cap, "%s %s HTTP/1.0\r\n", method, path);
    if (n < 0 || (size_t)n >= cap) return -1;
    size_t len = (size_t)n;

    if (port == (tls ? 443 : 80)) n = snprintf(out + len, cap - len, "Host: %s\r\n", host);
    else            n = snprintf(out + len, cap - len, "Host: %s:%d\r\n", host, port);
    if (n < 0 || (size_t)n >= cap - len) return -1;
    len += (size_t)n;

    if (len + xl + 2 >= cap) return -1;
    memcpy(out + len, extra_headers, xl);
    len += xl;
    if (need_crlf) { out[len++] = '\r'; out[len++] = '\n'; }

    if (want_len) {
        n = snprintf(out + len, cap - len, "Content-Length: %lu\r\n", (unsigned long)body_len);
        if (n < 0 || (size_t)n >= cap - len) return -1;
        len += (size_t)n;
    }
    n = snprintf(out + len, cap - len, "Connection: close\r\n\r\n");
    if (n < 0 || (size_t)n >= cap - len) return -1;
    return (int)(len + (size_t)n);
}

/* ── pure: reply-head parser ───────────────────────────────────────────── */

void http_head_init(HttpHead *h)
{
    memset(h, 0, sizeof(*h));
    h->content_length = -1;
}

static char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

/* Case-insensitive: does [a, a+n) equal the lower-case literal `lit`? */
static bool ieq(const char *a, size_t n, const char *lit)
{
    if (strlen(lit) != n) return false;
    for (size_t i = 0; i < n; i++)
        if (lower(a[i]) != lit[i]) return false;
    return true;
}

static bool contains_chunked(const char *v, size_t n)
{
    static const char w[] = "chunked";
    for (size_t i = 0; i + 7 <= n; i++) {
        size_t k = 0;
        while (k < 7 && lower(v[i + k]) == w[k]) k++;
        if (k == 7) return true;
    }
    return false;
}

/* One complete line, CR already removed.  0 ok, -1 bad. */
static int head_line(HttpHead *h, const char *l, size_t n)
{
    if (!h->have_status) {
        /* "HTTP/1.x SSS[ reason]" */
        if (n < 12 || strncmp(l, "HTTP/1.", 7) != 0 || l[7] < '0' || l[7] > '9' || l[8] != ' ')
            return -1;
        int st = 0;
        for (int i = 9; i < 12; i++) {
            if (l[i] < '0' || l[i] > '9') return -1;
            st = st * 10 + (l[i] - '0');
        }
        if (n > 12 && l[12] != ' ') return -1;
        if (st < 100 || st > 599) return -1;
        h->status = st;
        h->have_status = true;
        return 0;
    }
    const char *colon = memchr(l, ':', n);
    if (!colon) return 0;                    /* not a header we care about */
    size_t nl = (size_t)(colon - l);
    while (nl > 0 && (l[nl - 1] == ' ' || l[nl - 1] == '\t')) nl--;
    const char *v = colon + 1;
    size_t vn = n - (size_t)(colon + 1 - l);
    while (vn > 0 && (*v == ' ' || *v == '\t')) { v++; vn--; }
    while (vn > 0 && (v[vn - 1] == ' ' || v[vn - 1] == '\t')) vn--;

    if (ieq(l, nl, "content-length")) {
        if (vn == 0) return -1;
        long val = 0;
        for (size_t i = 0; i < vn; i++) {
            if (v[i] < '0' || v[i] > '9') return -1;
            if (val > (0x7fffffffL - (v[i] - '0')) / 10) return -1;   /* 32-bit long */
            val = val * 10 + (v[i] - '0');
        }
        if (h->content_length >= 0 && h->content_length != val) return -1;   /* two lengths disagree */
        h->content_length = val;
    } else if (ieq(l, nl, "transfer-encoding")) {
        if (contains_chunked(v, vn)) h->chunked = true;
    }
    return 0;
}

HttpHeadResult http_head_push(HttpHead *h, const char *buf, size_t n, size_t *used)
{
    size_t i = 0;
    if (h->sticky) { if (used) *used = 0; return (HttpHeadResult)h->sticky; }
    while (i < n) {
        char c = buf[i++];
        h->total++;
        if (c == '\n') {
            size_t ll = h->line_len;
            if (ll > 0 && h->line[ll - 1] == '\r') ll--;
            if (ll == 0 && h->line_len <= 1) {
                if (!h->have_status) { h->sticky = HTTP_HEAD_BAD; break; }
                h->sticky = HTTP_HEAD_DONE;
                break;
            }
            if (head_line(h, h->line, ll) != 0) { h->sticky = HTTP_HEAD_BAD; break; }
            h->line_len = 0;
        } else if (h->line_len >= sizeof(h->line)) {
            h->sticky = HTTP_HEAD_BAD;               /* a line the buffer cannot hold is not parsed from a prefix */
            break;
        } else {
            h->line[h->line_len++] = c;
        }
        if (h->total >= HTTP_HEAD_MAX) { h->sticky = HTTP_HEAD_TOO_BIG; break; }
    }
    if (used) *used = i;
    return (HttpHeadResult)h->sticky;
}

/* ── the socket state machine ──────────────────────────────────────────── */

enum { PH_IDLE = 0, PH_FAILED, PH_CONNECT, PH_TLS, PH_SEND, PH_SENDBODY, PH_HEAD, PH_BODY, PH_DONE };

#define POLL_BUDGET 16        /* syscall-sized steps per http_poll() */
#define BODY_CHUNK  8192

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Not memset: the compiler may drop a memset of a buffer it can see is not read again. */
static void wipe(void *p, size_t n)
{
    volatile unsigned char *q = p;
    while (n--) *q++ = 0;
}

static const char *phase_name(int ph)
{
    switch (ph) {
    case PH_CONNECT:  return "connect";
    case PH_TLS:      return "TLS handshake";
    case PH_SEND:     return "send";
    case PH_SENDBODY: return "send body";
    case PH_HEAD:     return "reply head";
    case PH_BODY:     return "reply body";
    default:          return "request";
    }
}

/* ── TLS: one process-wide context, one SSL per request ────────────────── */

#define TLS_CA_SYSTEM "/etc/ssl/certs/ca-certificates.crt"
#define TLS_CA_DEPLOYED "/opt/arca/ca-certificates.crt"

static SSL_CTX *g_ctx;
static const char *g_ca_override;      /* test hook; NULL = the two default paths */

void http_tls_set_ca_file(const char *path)
{
    if (g_ctx) { SSL_CTX_free(g_ctx); g_ctx = NULL; }
    g_ca_override = path;
}

/* The shared context, built on first use.  NULL means no usable CA bundle. */
static SSL_CTX *tls_ctx(void)
{
    if (g_ctx) return g_ctx;
    SSL_CTX *c = SSL_CTX_new(TLS_client_method());
    if (!c) return NULL;
    SSL_CTX_set_min_proto_version(c, TLS1_2_VERSION);
    SSL_CTX_set_verify(c, SSL_VERIFY_PEER, NULL);          /* never SSL_VERIFY_NONE */
    SSL_CTX_set_mode(c, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
#ifdef SSL_OP_IGNORE_UNEXPECTED_EOF
    SSL_CTX_set_options(c, SSL_OP_IGNORE_UNEXPECTED_EOF);  /* OpenSSL 3: a close without close_notify is EOF */
#endif
    const char *paths[2] = { g_ca_override ? g_ca_override : TLS_CA_SYSTEM,
                             g_ca_override ? NULL : TLS_CA_DEPLOYED };
    bool loaded = false;
    for (int i = 0; i < 2 && !loaded; i++)
        if (paths[i] && access(paths[i], R_OK) == 0
            && SSL_CTX_load_verify_locations(c, paths[i], NULL) == 1)
            loaded = true;
    ERR_clear_error();
    if (!loaded) { SSL_CTX_free(c); return NULL; }
    g_ctx = c;
    return g_ctx;
}

static void tls_free(HttpReq *r)
{
    if (r->ssl) { SSL_free((SSL *)r->ssl); r->ssl = NULL; }
}

/* Start TLS on the connected-or-connecting socket.  0 or -1 with `*why` set. */
static int tls_start(HttpReq *r, const char *host, bool is_ip, const char **why)
{
    SSL_CTX *c = tls_ctx();
    if (!c) { *why = "TLS: no CA bundle found"; return -1; }
    SSL *s = SSL_new(c);
    if (!s) { *why = "TLS: out of memory"; return -1; }
    X509_VERIFY_PARAM *vp = SSL_get0_param(s);
    X509_VERIFY_PARAM_set_hostflags(vp, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
    int ok;
    if (is_ip) {
        ok = X509_VERIFY_PARAM_set1_ip_asc(vp, host);
    } else {
        ok = SSL_set1_host(s, host);                       /* the name the certificate must carry */
        if (ok) ok = (int)SSL_set_tlsext_host_name(s, host);   /* SNI: a tunnel routes on it */
    }
    if (!ok || !SSL_set_fd(s, r->fd)) { SSL_free(s); *why = "TLS: setup failed"; return -1; }
    SSL_set_connect_state(s);
    r->ssl = s;
    return 0;
}

/* Why a handshake failed, in words the operator can act on. */
static const char *tls_why(SSL *s)
{
    long vr = SSL_get_verify_result(s);
    switch (vr) {
    case X509_V_OK:
        return "TLS: handshake failed";
    case X509_V_ERR_HOSTNAME_MISMATCH:
    case X509_V_ERR_IP_ADDRESS_MISMATCH:
        return "TLS: host name mismatch";
    case X509_V_ERR_CERT_HAS_EXPIRED:
    case X509_V_ERR_CERT_NOT_YET_VALID:
        return "TLS: certificate expired or not yet valid (clock?)";
    default:
        return "TLS: certificate not trusted";
    }
}

/* recv()/send() shaped: n > 0 bytes, 0 end of stream, -1 with errno (EAGAIN = not now). */
static ssize_t tls_read(SSL *s, void *buf, size_t n, bool peek)
{
    ERR_clear_error();
    int k = peek ? SSL_peek(s, buf, (int)n) : SSL_read(s, buf, (int)n);
    if (k > 0) return k;
    int e = SSL_get_error(s, k);
    if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) { errno = EAGAIN; return -1; }
    if (e == SSL_ERROR_ZERO_RETURN) return 0;
#ifdef SSL_R_UNEXPECTED_EOF_WHILE_READING
    if (e == SSL_ERROR_SSL && ERR_GET_REASON(ERR_peek_error()) == SSL_R_UNEXPECTED_EOF_WHILE_READING)
        return 0;
#endif
    if (e == SSL_ERROR_SYSCALL && k == 0 && ERR_peek_error() == 0) return 0;   /* 1.1.1: EOF, no close_notify */
    errno = ECONNRESET;
    return -1;
}

static ssize_t io_recv(HttpReq *r, void *buf, size_t n, bool peek)
{
    if (!r->tls) return recv(r->fd, buf, n, peek ? MSG_PEEK : 0);
    return tls_read((SSL *)r->ssl, buf, n, peek);
}

static ssize_t io_send(HttpReq *r, const void *buf, size_t n)
{
    if (!r->tls) return send(r->fd, buf, n, MSG_NOSIGNAL);
    SSL *s = (SSL *)r->ssl;
    ERR_clear_error();
    int k = SSL_write(s, buf, (int)n);
    if (k > 0) return k;
    int e = SSL_get_error(s, k);
    if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) { errno = EAGAIN; return -1; }
    errno = EPIPE;
    return -1;
}

/* The TLS streams handed out by http_take_fd(): the fd stays the raw socket and this
 * table says which fds read through SSL.  Keyed by fd AND socket inode so an fd that
 * somebody close()d directly and the kernel reused is never mistaken for TLS. */
#define TLS_STREAMS 4
static struct { int fd; SSL *ssl; dev_t dev; ino_t ino; } g_streams[TLS_STREAMS];

static bool stream_live(int i)
{
    struct stat st;
    if (!g_streams[i].ssl) return false;
    return fstat(g_streams[i].fd, &st) == 0 && st.st_dev == g_streams[i].dev
           && st.st_ino == g_streams[i].ino;
}

static int stream_find(int fd)
{
    for (int i = 0; i < TLS_STREAMS; i++)
        if (g_streams[i].ssl && g_streams[i].fd == fd && stream_live(i)) return i;
    return -1;
}

static bool stream_register(int fd, SSL *s)
{
    struct stat st;
    if (fstat(fd, &st) != 0) return false;
    int slot = -1;
    for (int i = 0; i < TLS_STREAMS; i++) {
        if (g_streams[i].ssl && !stream_live(i)) {            /* orphaned by a bare close() */
            SSL_free(g_streams[i].ssl);
            g_streams[i].ssl = NULL;
        }
        if (!g_streams[i].ssl && slot < 0) slot = i;
    }
    if (slot < 0) return false;
    g_streams[slot].fd = fd; g_streams[slot].ssl = s;
    g_streams[slot].dev = st.st_dev; g_streams[slot].ino = st.st_ino;
    return true;
}

ssize_t http_stream_read(int fd, void *buf, size_t n)
{
    int i = stream_find(fd);
    if (i < 0) return read(fd, buf, n);
    return tls_read(g_streams[i].ssl, buf, n, false);
}

void http_stream_close(int fd)
{
    int i = stream_find(fd);
    if (i >= 0) { SSL_free(g_streams[i].ssl); g_streams[i].ssl = NULL; }
    close(fd);
}

static HttpPoll fail(HttpReq *r, const char *what, int err)
{
    if (err) snprintf(r->err, sizeof(r->err), "%s: %s", what, strerror(err));
    else     snprintf(r->err, sizeof(r->err), "%s", what);
    tls_free(r);
    if (r->fd >= 0) { close(r->fd); r->fd = -1; }
    wipe(r->req, sizeof(r->req));
    r->phase = PH_FAILED;
    return HTTP_ERROR;
}

void http_init(HttpReq *r)
{
    memset(r, 0, sizeof(*r));
    r->fd = -1;
    r->phase = PH_IDLE;
}

int http_begin(HttpReq *r, const char *host, int port, bool tls, const char *method,
               const char *path, const char *extra_headers, const char *body,
               size_t body_len, HttpMode mode, char *buf, size_t buf_size)
{
    /* A struct that still owns a socket (a begin without an end) must not leak it. */
    if (r->fd >= 0 && r->phase != PH_IDLE) close(r->fd);
    tls_free(r);
    memset(r, 0, sizeof(*r));
    r->fd = -1;
    r->tls = tls;
    r->timeout_ms = HTTP_DEFAULT_TIMEOUT_MS;
    r->total_timeout_ms = HTTP_DEFAULT_TOTAL_MS;
    r->mode = mode;
    r->buf = buf;
    r->buf_size = buf_size;
    http_head_init(&r->head);

    if (mode == HTTP_BUFFER && (!buf || buf_size < 1)) { fail(r, "no body buffer", 0); return -1; }
    if (body_len > 0 && !body)                          { fail(r, "body missing", 0); return -1; }
    if (mode == HTTP_BUFFER) buf[0] = 0;
    r->body_out = body;
    r->body_out_len = body_len;

    int rl = http_build_request(r->req, sizeof(r->req), method, host, port, tls, path,
                                extra_headers, body_len);
    if (rl < 0) { fail(r, "bad or oversized request", 0); return -1; }
    r->head_len = (size_t)rl;

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((unsigned short)port);
    bool is_ip = inet_pton(AF_INET, host, &sa.sin_addr) == 1;
    if (!is_ip) {
        /* The one blocking call in this module — see http.h. */
        struct addrinfo hints, *res = NULL;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        if (getaddrinfo(host, NULL, &hints, &res) != 0 || !res) {
            if (res) freeaddrinfo(res);
            fail(r, "cannot resolve host", 0);
            return -1;
        }
        sa.sin_addr = ((struct sockaddr_in *)(void *)res->ai_addr)->sin_addr;
        freeaddrinfo(res);
    }

    r->fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (r->fd < 0) { fail(r, "socket", errno); return -1; }
    if (tls) {
        const char *why = NULL;
        if (tls_start(r, host, is_ip, &why) != 0) { fail(r, why, 0); return -1; }
    }
    if (connect(r->fd, (struct sockaddr *)&sa, sizeof(sa)) == 0) {
        r->phase = tls ? PH_TLS : PH_SEND;
    } else if (errno == EINPROGRESS || errno == EINTR) {
        r->phase = PH_CONNECT;
    } else {
        fail(r, "connect", errno);
        return -1;
    }
    /* The timers start now, after the one blocking call (name resolution) is behind us. */
    r->start_ms = r->last_ms = now_ms();
    return 0;
}

/* Append to the caller's buffer; the last byte is kept for the NUL. */
static bool body_append(HttpReq *r, const char *p, size_t n)
{
    if (n > r->buf_size - 1 - r->buf_len) return false;
    memcpy(r->buf + r->buf_len, p, n);
    r->buf_len += n;
    return true;
}

static HttpPoll body_finish(HttpReq *r)
{
    r->buf[r->buf_len] = 0;
    tls_free(r);
    close(r->fd);
    r->fd = -1;
    r->phase = PH_DONE;
    return HTTP_DONE;
}

/* The reply head is complete: pick the mode's next phase.  `rest` is the body
 * prefix that arrived with it (BUFFER mode only). */
static HttpPoll head_complete(HttpReq *r, const char *rest, size_t rest_n)
{
    if (r->head.chunked)
        return fail(r, "chunked reply (this client speaks HTTP/1.0)", 0);
    if (r->mode == HTTP_STREAM) { r->phase = PH_DONE; return HTTP_DONE; }
    if (r->head.content_length >= 0 && (size_t)r->head.content_length > r->buf_size - 1)
        return fail(r, "body larger than the buffer", 0);
    /* Bytes past the declared length are not this reply's body. */
    if (r->head.content_length >= 0 && rest_n > (size_t)r->head.content_length)
        rest_n = (size_t)r->head.content_length;
    if (!body_append(r, rest, rest_n))
        return fail(r, "body larger than the buffer", 0);
    r->phase = PH_BODY;
    if (r->head.content_length >= 0 && r->buf_len >= (size_t)r->head.content_length)
        return body_finish(r);
    return HTTP_PENDING;
}

HttpPoll http_poll(HttpReq *r)
{
    if (r->phase == PH_DONE)   return HTTP_DONE;
    if (r->phase == PH_IDLE || r->phase == PH_FAILED) return HTTP_ERROR;

    bool progress = false;
    for (int budget = POLL_BUDGET; budget > 0; budget--) {
        switch (r->phase) {
        case PH_CONNECT: {
            struct pollfd pf = { r->fd, POLLOUT, 0 };
            int pr = poll(&pf, 1, 0);
            if (pr < 0) { if (errno == EINTR) continue; return fail(r, "poll", errno); }
            if (pr == 0) goto idle;
            int soerr = 0;
            socklen_t sl = sizeof(soerr);
            if (getsockopt(r->fd, SOL_SOCKET, SO_ERROR, &soerr, &sl) != 0) soerr = errno;
            if (soerr) return fail(r, "connect", soerr);
            r->phase = r->tls ? PH_TLS : PH_SEND;
            progress = true;
            continue;
        }
        case PH_TLS: {
            SSL *s = (SSL *)r->ssl;
            ERR_clear_error();
            int rc = SSL_connect(s);
            if (rc == 1) { r->phase = PH_SEND; progress = true; continue; }
            int e = SSL_get_error(s, rc);
            if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) goto idle;
            return fail(r, tls_why(s), 0);
        }
        case PH_SEND:
        case PH_SENDBODY: {
            bool head = r->phase == PH_SEND;
            const char *p = head ? r->req + r->sent : r->body_out + r->body_sent;
            size_t left = head ? r->head_len - r->sent : r->body_out_len - r->body_sent;
            ssize_t n = io_send(r, p, left);
            if (n < 0) {
                if (errno == EINTR) continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) goto idle;
                return fail(r, "send", errno);
            }
            if (n == 0) goto idle;
            progress = true;
            if (head) {
                r->sent += (size_t)n;
                if (r->sent == r->head_len) {
                    wipe(r->req, sizeof(r->req));          /* the token is gone */
                    r->phase = r->body_out_len ? PH_SENDBODY : PH_HEAD;
                }
            } else {
                r->body_sent += (size_t)n;
                if (r->body_sent == r->body_out_len) r->phase = PH_HEAD;
            }
            continue;
        }
        case PH_HEAD: {
            char tmp[1024];
            /* STREAM peeks, then consumes exactly the head: whatever follows stays
             * in the socket for the caller. */
            ssize_t n = io_recv(r, tmp, sizeof(tmp), r->mode == HTTP_STREAM);
            if (n < 0) {
                if (errno == EINTR) continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) goto idle;
                return fail(r, "recv", errno);
            }
            if (n == 0) return fail(r, "connection closed before the reply head", 0);
            progress = true;
            size_t used = 0;
            HttpHeadResult hr = http_head_push(&r->head, tmp, (size_t)n, &used);
            if (r->mode == HTTP_STREAM && used > 0) {
                ssize_t c = io_recv(r, tmp, used, false);
                if (c < 0 || (size_t)c != used) return fail(r, "recv (consume head)", c < 0 ? errno : 0);
            }
            if (hr == HTTP_HEAD_BAD)     return fail(r, "malformed reply head", 0);
            if (hr == HTTP_HEAD_TOO_BIG) return fail(r, "reply head over 8 KiB", 0);
            if (hr == HTTP_HEAD_DONE) {
                HttpPoll hp = head_complete(r, tmp + used, (size_t)n - used);
                if (hp != HTTP_PENDING) return hp;
            }
            continue;
        }
        case PH_BODY: {
            size_t space = r->buf_size - 1 - r->buf_len;
            long cl = r->head.content_length;
            if (cl >= 0 && r->buf_len >= (size_t)cl) return body_finish(r);
            if (space == 0) {
                /* Full with no declared end: one more byte is an overflow, EOF is the end. */
                char probe;
                ssize_t n = io_recv(r, &probe, 1, false);
                if (n > 0) return fail(r, "body larger than the buffer", 0);
                if (n == 0) return body_finish(r);
                if (errno == EINTR) continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) goto idle;
                return fail(r, "recv", errno);
            }
            size_t want = space < BODY_CHUNK ? space : BODY_CHUNK;
            if (cl >= 0 && (size_t)cl - r->buf_len < want) want = (size_t)cl - r->buf_len;
            ssize_t n = io_recv(r, r->buf + r->buf_len, want, false);
            if (n < 0) {
                if (errno == EINTR) continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) goto idle;
                return fail(r, "recv", errno);
            }
            if (n == 0) {
                if (cl >= 0 && r->buf_len < (size_t)cl)
                    return fail(r, "connection closed mid-body", 0);
                return body_finish(r);
            }
            progress = true;
            r->buf_len += (size_t)n;
            continue;
        }
        default:
            return HTTP_ERROR;
        }
    }
idle:
    {
        long long t = now_ms();
        /* The inactivity timer below is restarted by every byte, so a server that trickles
         * one forever would never trip it; this one runs from the start of the request. */
        if (r->total_timeout_ms > 0 && t - r->start_ms > r->total_timeout_ms) {
            char msg[64];
            snprintf(msg, sizeof(msg), "deadline passed in %s", phase_name(r->phase));
            return fail(r, msg, 0);
        }
        if (progress) {
            r->last_ms = t;
        } else if (t - r->last_ms > r->timeout_ms) {
            char msg[64];
            snprintf(msg, sizeof(msg), "timeout in %s", phase_name(r->phase));
            return fail(r, msg, 0);
        }
    }
    return HTTP_PENDING;
}

int http_status(const HttpReq *r)       { return r->head.status; }
const char *http_body(const HttpReq *r) { return r->phase == PH_DONE && r->mode == HTTP_BUFFER ? r->buf : ""; }
size_t http_body_len(const HttpReq *r)  { return r->phase == PH_DONE && r->mode == HTTP_BUFFER ? r->buf_len : 0; }
const char *http_error(const HttpReq *r){ return r->err; }

int http_take_fd(HttpReq *r)
{
    if (r->phase != PH_DONE || r->mode != HTTP_STREAM || r->fd < 0) return -1;
    if (r->tls) {
        /* The SSL object moves to the stream table; the caller reads through
         * http_stream_read().  No free slot: refuse, http_end() cleans up. */
        if (!stream_register(r->fd, (SSL *)r->ssl)) return -1;
        r->ssl = NULL;
    }
    int fd = r->fd;
    r->fd = -1;
    return fd;
}

void http_end(HttpReq *r)
{
    tls_free(r);
    if (r->fd >= 0 && r->phase != PH_IDLE) close(r->fd);
    r->fd = -1;
    wipe(r->req, sizeof(r->req));
    if (r->phase != PH_DONE) r->phase = PH_IDLE;
}
