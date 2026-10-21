/*
 * http_tls_test.c — host regression for the HTTPS half of common/http.c.
 *
 * A throwaway CA, three leaves under it, a second (untrusted) CA with a leaf, and
 * an IP-SAN leaf are generated with the `openssl` CLI into a mkdtemp directory
 * outside the repo.  Each case forks a one-connection TLS (or plain) server on
 * loopback; the client is pointed at the throwaway CA with http_tls_set_ca_file()
 * and driven with http_poll() under a deadline.  Host names (tls-ok.test,
 * tls-other.test) resolve to 127.0.0.1 through a getaddrinfo stub.
 *
 * Cases: Content-Length body; no Content-Length with and without close_notify;
 * untrusted CA; host name mismatch (DNS and IP); a short body is still an error;
 * HTTP_STREAM over TLS identical to the plain stream (also > 64 KB); plain HTTP
 * unchanged; IP-literal host with an IP SAN; a missing CA bundle; an expired leaf, a
 * self-signed leaf and a TLS 1.1-only server (the client floor is 1.2), all refused.
 *
 *   gcc -Wall -Wextra -Wno-unused-parameter -I common -fsanitize=address,undefined \
 *       -o build/http_tls_test tests/http_tls_test.c common/http.c -lssl -lcrypto \
 *       && timeout 120 ./build/http_tls_test
 *
 * Needs the `openssl` command line tool.  Compiles against OpenSSL 1.1.1 and 3.x.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <openssl/err.h>
#include <openssl/ssl.h>

#include "http.h"

static int g_checks, g_failed;
#define CHECK(c) do { g_checks++; if (!(c)) { g_failed++; \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

/* ── stub resolver: every name is the loopback ─────────────────────────── */

int getaddrinfo(const char *node, const char *svc, const struct addrinfo *hints,
                struct addrinfo **res)
{
    (void)node; (void)svc; (void)hints;
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

/* ── throwaway PKI ─────────────────────────────────────────────────────── */

static char g_dir[256];

static void sh(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#include <stdarg.h>
static void sh(const char *fmt, ...)
{
    char cmd[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(cmd, sizeof(cmd), fmt, ap);
    va_end(ap);
    if (system(cmd) != 0) { printf("FAIL setup command: %s\n", cmd); g_failed++; }
}

static void write_ext(const char *name, const char *san)
{
    char p[300];
    snprintf(p, sizeof(p), "%s/%s.ext", g_dir, name);
    FILE *f = fopen(p, "w");
    if (!f) return;
    fprintf(f, "basicConstraints=CA:FALSE\nkeyUsage=digitalSignature\n"
               "extendedKeyUsage=serverAuth\nsubjectAltName=%s\n", san);
    fclose(f);
}

static void make_ca(const char *name)
{
    /* own config: the system one adds basicConstraints itself and a second copy makes an invalid cert */
    char p[300];
    snprintf(p, sizeof(p), "%s/ca.cnf", g_dir);
    FILE *f = fopen(p, "w");
    if (f) {
        fprintf(f, "[req]\ndistinguished_name=dn\nx509_extensions=v3\nprompt=no\n[dn]\nCN=x\n"
                   "[v3]\nbasicConstraints=critical,CA:TRUE\nkeyUsage=keyCertSign\n"
                   "subjectKeyIdentifier=hash\n");
        fclose(f);
    }
    sh("cd '%s' && openssl req -x509 -newkey rsa:2048 -nodes -config ca.cnf "
       "-subj '/CN=%s' -days 2 -keyout %s.key -out %s.pem >/dev/null 2>&1",
       g_dir, name, name, name);
}

static void make_leaf(const char *name, const char *ca, const char *san)
{
    write_ext(name, san);
    sh("cd '%s' && openssl req -new -newkey rsa:2048 -nodes "
       "-subj '/CN=leaf-%s' -keyout %s.key -out %s.csr >/dev/null 2>&1 && "
       "openssl x509 -req -in %s.csr -CA %s.pem -CAkey %s.key -CAcreateserial -days 2 "
       "-extfile %s.ext -out %s.pem >/dev/null 2>&1",
       g_dir, name, name, name, name, ca, ca, name, name);
}

/* `openssl x509 -req` cannot back-date, so this one goes through `openssl ca` with explicit dates */
static void make_expired_leaf(const char *name, const char *ca, const char *san)
{
    char p[300];
    write_ext(name, san);
    snprintf(p, sizeof(p), "%s/%s.cnf", g_dir, name);
    FILE *f = fopen(p, "w");
    if (!f) return;
    fprintf(f, "[ca]\ndefault_ca=c\n[c]\ndatabase=%s/%s.idx\nnew_certs_dir=%s\nserial=%s/%s.srl\n"
               "default_md=sha256\npolicy=pol\nunique_subject=no\n[pol]\ncommonName=supplied\n",
            g_dir, name, g_dir, g_dir, name);
    fclose(f);
    snprintf(p, sizeof(p), "%s/%s.idx", g_dir, name);
    f = fopen(p, "w");
    if (f) fclose(f);
    snprintf(p, sizeof(p), "%s/%s.srl", g_dir, name);
    f = fopen(p, "w");
    if (f) { fputs("0A\n", f); fclose(f); }
    sh("cd '%s' && openssl req -new -newkey rsa:2048 -nodes "
       "-subj '/CN=leaf-%s' -keyout %s.key -out %s.csr >/dev/null 2>&1 && "
       "openssl ca -batch -config %s.cnf -cert %s.pem -keyfile %s.key -notext "
       "-startdate 20200101000000Z -enddate 20200102000000Z -extfile %s.ext "
       "-in %s.csr -out %s.pem >/dev/null 2>&1",
       g_dir, name, name, name, name, ca, ca, name, name, name);
}

static void make_self_signed(const char *name, const char *san)
{
    sh("cd '%s' && openssl req -x509 -newkey rsa:2048 -nodes -subj '/CN=leaf-%s' -days 2 "
       "-addext 'subjectAltName=%s' -keyout %s.key -out %s.pem >/dev/null 2>&1",
       g_dir, name, san, name, name);
}

static const char *P(const char *name, const char *ext)
{
    static char b[8][300];
    static int i;
    char *o = b[i++ & 7];
    snprintf(o, 300, "%s/%s.%s", g_dir, name, ext);
    return o;
}

/* ── one-connection server ─────────────────────────────────────────────── */

typedef struct {
    const char *leaf;     /* leaf name in g_dir, NULL = plain HTTP                      */
    long        cl;       /* Content-Length to announce, -1 = omit the header           */
    size_t      send_len; /* body bytes actually written                                */
    int         notify;   /* TLS: SSL_shutdown (close_notify) before close              */
    int         max_ver;  /* TLS: highest protocol the server offers, 0 = its default   */
} Mode;

typedef struct { int lfd; int port; pid_t pid; } Srv;

static unsigned char g_payload[200000];

static void fill_payload(void)
{
    unsigned x = 12345;
    for (size_t i = 0; i < sizeof(g_payload); i++) {
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        g_payload[i] = (unsigned char)(x >> 11);
    }
}

static void put(SSL *ssl, int fd, const void *b, size_t n)
{
    const char *p = b;
    while (n > 0) {
        int k = ssl ? SSL_write(ssl, p, (int)(n > 4096 ? 4096 : n))
                    : (int)write(fd, p, n > 4096 ? 4096 : n);
        if (k <= 0) return;
        p += k; n -= (size_t)k;
    }
}

static void serve_child(int lfd, const Mode *m)
{
    alarm(20);
    signal(SIGPIPE, SIG_IGN);
    int fd = accept(lfd, NULL, NULL);
    if (fd < 0) _exit(1);
    SSL *ssl = NULL;
    if (m->leaf) {
        SSL_CTX *c = SSL_CTX_new(TLS_server_method());
        if (!c || SSL_CTX_use_certificate_chain_file(c, P(m->leaf, "pem")) != 1
            || SSL_CTX_use_PrivateKey_file(c, P(m->leaf, "key"), SSL_FILETYPE_PEM) != 1)
            _exit(2);
        if (m->max_ver) {
            /* an old protocol is below the default security level of a current OpenSSL, so the
             * server has to be told it may speak it; the refusal under test is the client's */
            SSL_CTX_set_security_level(c, 0);
            SSL_CTX_set_cipher_list(c, "ALL:@SECLEVEL=0");
            SSL_CTX_set_min_proto_version(c, TLS1_VERSION);
            SSL_CTX_set_max_proto_version(c, m->max_ver);
        }
        ssl = SSL_new(c);
        SSL_set_fd(ssl, fd);
        if (SSL_accept(ssl) != 1) _exit(0);        /* the client refused us: expected in some cases */
    }
    char req[2048];
    size_t have = 0;
    while (have < sizeof(req) - 1) {
        int k = ssl ? SSL_read(ssl, req + have, (int)(sizeof(req) - 1 - have))
                    : (int)read(fd, req + have, sizeof(req) - 1 - have);
        if (k <= 0) _exit(0);
        have += (size_t)k;
        req[have] = 0;
        if (strstr(req, "\r\n\r\n")) break;
    }
    char head[256];
    int hl = snprintf(head, sizeof(head), "HTTP/1.0 200 OK\r\nContent-Type: application/octet-stream\r\n");
    if (m->cl >= 0) hl += snprintf(head + hl, sizeof(head) - (size_t)hl, "Content-Length: %ld\r\n", m->cl);
    hl += snprintf(head + hl, sizeof(head) - (size_t)hl, "Connection: close\r\n\r\n");
    put(ssl, fd, head, (size_t)hl);
    put(ssl, fd, g_payload, m->send_len);
    if (ssl && m->notify) SSL_shutdown(ssl);
    close(fd);
    _exit(0);
}

static Srv srv_start(const Mode *m)
{
    Srv s = { -1, 0, -1 };
    s.lfd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int one = 1;
    setsockopt(s.lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if (bind(s.lfd, (struct sockaddr *)&a, sizeof(a)) != 0 || listen(s.lfd, 1) != 0) {
        printf("FAIL bind/listen\n"); g_failed++; return s;
    }
    socklen_t l = sizeof(a);
    getsockname(s.lfd, (struct sockaddr *)&a, &l);
    s.port = ntohs(a.sin_port);
    fflush(stdout);
    s.pid = fork();
    if (s.pid == 0) serve_child(s.lfd, m);
    return s;
}

static void srv_stop(Srv *s)
{
    if (s->pid > 0) { kill(s->pid, SIGTERM); waitpid(s->pid, NULL, 0); }
    if (s->lfd >= 0) close(s->lfd);
}

/* ── client driver ─────────────────────────────────────────────────────── */

static long long now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static HttpPoll drive(HttpReq *r, int ms)
{
    long long end = now_ms() + ms;
    HttpPoll p;
    while ((p = http_poll(r)) == HTTP_PENDING) {
        if (now_ms() > end) return HTTP_PENDING;
        usleep(1000);
    }
    return p;
}

/* GET over `host`:`port`; returns the poll result, body in buf. */
static HttpPoll get(HttpReq *r, const char *host, int port, bool tls, char *buf, size_t cap)
{
    http_init(r);
    http_begin(r, host, port, tls, "GET", "/x", NULL, NULL, 0, HTTP_BUFFER, buf, cap);
    return drive(r, 8000);
}

/* HTTP_STREAM: read the whole body through http_stream_read.  Returns bytes, or -1. */
static long stream_all(const char *host, int port, bool tls, unsigned char *out, size_t cap)
{
    HttpReq r;
    http_init(&r);
    http_begin(&r, host, port, tls, "GET", "/x", NULL, NULL, 0, HTTP_STREAM, NULL, 0);
    if (drive(&r, 8000) != HTTP_DONE) { http_end(&r); return -1; }
    int fd = http_take_fd(&r);
    http_end(&r);
    if (fd < 0) return -1;
    size_t got = 0;
    long long end = now_ms() + 8000;
    long rc = 0;
    for (;;) {
        ssize_t k = http_stream_read(fd, out + got, cap - got);
        if (k > 0) { got += (size_t)k; if (got == cap) { rc = -2; break; } continue; }
        if (k == 0) { rc = (long)got; break; }
        if (errno != EAGAIN && errno != EINTR) { rc = -1; break; }
        if (now_ms() > end) { rc = -3; break; }
        struct pollfd pf = { fd, POLLIN, 0 };
        poll(&pf, 1, 200);
    }
    http_stream_close(fd);
    CHECK(fcntl(fd, F_GETFD) == -1 && errno == EBADF);   /* the close really closed it */
    return rc;
}

/* ── cases ─────────────────────────────────────────────────────────────── */

static void use_ca(const char *ca) { http_tls_set_ca_file(P(ca, "pem")); }

static void test_content_length(void)
{
    static char buf[70000];
    Mode m = { "ok", 40000, 40000, 1, 0 };
    Srv s = srv_start(&m);
    HttpReq r;
    CHECK(get(&r, "tls-ok.test", s.port, true, buf, sizeof(buf)) == HTTP_DONE);
    CHECK(http_status(&r) == 200);
    CHECK(http_body_len(&r) == 40000 && memcmp(http_body(&r), g_payload, 40000) == 0);
    http_end(&r);
    srv_stop(&s);
}

static void test_no_content_length(int notify)
{
    static char buf[70000];
    Mode m = { "ok", -1, 30000, notify, 0 };
    Srv s = srv_start(&m);
    HttpReq r;
    HttpPoll p = get(&r, "tls-ok.test", s.port, true, buf, sizeof(buf));
    CHECK(p == HTTP_DONE);
    if (p != HTTP_DONE) printf("  notify=%d error: %s\n", notify, http_error(&r));
    CHECK(http_body_len(&r) == 30000 && memcmp(http_body(&r), g_payload, 30000) == 0);
    http_end(&r);
    srv_stop(&s);
}

static void test_untrusted(void)
{
    char buf[1024];
    Mode m = { "bad", 10, 10, 1, 0 };
    Srv s = srv_start(&m);
    HttpReq r;
    CHECK(get(&r, "tls-ok.test", s.port, true, buf, sizeof(buf)) == HTTP_ERROR);
    CHECK(strstr(http_error(&r), "TLS:") == http_error(&r));
    CHECK(strstr(http_error(&r), "not trusted") != NULL);
    http_end(&r);
    srv_stop(&s);
}

static void test_name_mismatch(void)
{
    char buf[1024];
    Mode m = { "ok", 10, 10, 1, 0 };
    Srv s = srv_start(&m);
    HttpReq r;
    /* trusted chain, but the certificate carries tls-ok.test, not tls-other.test */
    CHECK(get(&r, "tls-other.test", s.port, true, buf, sizeof(buf)) == HTTP_ERROR);
    CHECK(strstr(http_error(&r), "host name mismatch") != NULL);
    http_end(&r);
    srv_stop(&s);

    /* a DNS-only certificate does not satisfy an IP-literal host */
    s = srv_start(&m);
    CHECK(get(&r, "127.0.0.1", s.port, true, buf, sizeof(buf)) == HTTP_ERROR);
    CHECK(strstr(http_error(&r), "host name mismatch") != NULL);
    http_end(&r);
    srv_stop(&s);

    /* and the right name still works on the same server shape (the control for the above) */
    s = srv_start(&m);
    CHECK(get(&r, "tls-ok.test", s.port, true, buf, sizeof(buf)) == HTTP_DONE);
    http_end(&r);
    srv_stop(&s);
}

static void test_short_body(void)
{
    char buf[1024];
    for (int notify = 0; notify <= 1; notify++) {
        Mode m = { "ok", 100, 10, notify, 0 };
        Srv s = srv_start(&m);
        HttpReq r;
        CHECK(get(&r, "tls-ok.test", s.port, true, buf, sizeof(buf)) == HTTP_ERROR);
        CHECK(strstr(http_error(&r), "mid-body") != NULL);
        http_end(&r);
        srv_stop(&s);
    }
}

/* a leaf whose validity ended before today, from a trusted CA: refused, and said to be a date problem */
static void test_expired(void)
{
    char buf[1024];
    Mode m = { "old", 10, 10, 1, 0 };
    Srv s = srv_start(&m);
    HttpReq r;
    CHECK(get(&r, "tls-ok.test", s.port, true, buf, sizeof(buf)) == HTTP_ERROR);
    CHECK(strstr(http_error(&r), "expired") != NULL);
    http_end(&r);
    srv_stop(&s);
}

/* a self-signed leaf that is not in the bundle is not trusted, even with the right name */
static void test_self_signed(void)
{
    char buf[1024];
    Mode m = { "self", 10, 10, 1, 0 };
    Srv s = srv_start(&m);
    HttpReq r;
    CHECK(get(&r, "tls-ok.test", s.port, true, buf, sizeof(buf)) == HTTP_ERROR);
    CHECK(strstr(http_error(&r), "not trusted") != NULL);
    http_end(&r);
    srv_stop(&s);
}

/* the client's floor is TLS 1.2: a trusted, correctly named server that stops at 1.1 is refused */
static void test_old_protocol(void)
{
    char buf[1024];
    Mode m = { "ok", 10, 10, 1, TLS1_1_VERSION };
    Srv s = srv_start(&m);
    HttpReq r;
    CHECK(get(&r, "tls-ok.test", s.port, true, buf, sizeof(buf)) == HTTP_ERROR);
    CHECK(strstr(http_error(&r), "TLS:") == http_error(&r));
    http_end(&r);
    srv_stop(&s);
}

static unsigned char g_a[200000], g_b[200000];

static void test_stream(void)
{
    size_t sizes[3] = { 200, 70000, 150000 };
    for (int i = 0; i < 3; i++) {
        Mode mt = { "ok", (long)sizes[i], sizes[i], 1, 0 };
        Mode mp = { NULL, (long)sizes[i], sizes[i], 0, 0 };
        Srv st = srv_start(&mt);
        long nt = stream_all("tls-ok.test", st.port, true, g_a, sizeof(g_a));
        srv_stop(&st);
        Srv sp = srv_start(&mp);
        long np = stream_all("tls-ok.test", sp.port, false, g_b, sizeof(g_b));
        srv_stop(&sp);
        CHECK(np == (long)sizes[i]);
        CHECK(nt == np);
        CHECK(nt > 0 && memcmp(g_a, g_b, (size_t)nt) == 0);
        CHECK(nt > 0 && memcmp(g_a, g_payload, (size_t)nt) == 0);
    }
    /* a stream without close_notify ends at 0 too */
    Mode mn = { "ok", -1, 5000, 0, 0 };
    Srv sn = srv_start(&mn);
    CHECK(stream_all("tls-ok.test", sn.port, true, g_a, sizeof(g_a)) == 5000);
    srv_stop(&sn);
}

static void test_plain(void)
{
    char buf[1024];
    Mode m = { NULL, 300, 300, 0, 0 };
    Srv s = srv_start(&m);
    HttpReq r;
    CHECK(get(&r, "127.0.0.1", s.port, false, buf, sizeof(buf)) == HTTP_DONE);
    CHECK(http_body_len(&r) == 300 && memcmp(http_body(&r), g_payload, 300) == 0);
    http_end(&r);
    srv_stop(&s);
}

static void test_ip_and_no_bundle(void)
{
    char buf[1024];
    Mode m = { "ip", 300, 300, 1, 0 };
    Srv s = srv_start(&m);
    HttpReq r;
    CHECK(get(&r, "127.0.0.1", s.port, true, buf, sizeof(buf)) == HTTP_DONE);
    CHECK(http_body_len(&r) == 300 && memcmp(http_body(&r), g_payload, 300) == 0);
    http_end(&r);
    srv_stop(&s);

    /* the IP certificate does not satisfy a DNS name */
    s = srv_start(&m);
    CHECK(get(&r, "tls-ok.test", s.port, true, buf, sizeof(buf)) == HTTP_ERROR);
    CHECK(strstr(http_error(&r), "host name mismatch") != NULL);
    http_end(&r);
    srv_stop(&s);

    http_tls_set_ca_file("/nonexistent-dir/ca.pem");
    http_init(&r);
    CHECK(http_begin(&r, "tls-ok.test", 1, true, "GET", "/x", NULL, NULL, 0, HTTP_BUFFER, buf, sizeof(buf)) < 0);
    CHECK(strstr(http_error(&r), "no CA bundle") != NULL);
    CHECK(drive(&r, 1000) == HTTP_ERROR);
    http_end(&r);
    use_ca("caA");                                   /* and the hook recovers */
    s = srv_start(&m);
    CHECK(get(&r, "127.0.0.1", s.port, true, buf, sizeof(buf)) == HTTP_DONE);
    http_end(&r);
    srv_stop(&s);
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    const char *base = getenv("TMPDIR");
    snprintf(g_dir, sizeof(g_dir), "%s/http_tls_test.XXXXXX", base && *base ? base : "/tmp");
    if (!mkdtemp(g_dir)) { perror("mkdtemp"); return 2; }
    if (system("openssl version >/dev/null 2>&1") != 0) { printf("http_tls_test: no openssl CLI\n"); return 2; }
    fill_payload();

    make_ca("caA");
    make_ca("caB");
    make_leaf("ok", "caA", "DNS:tls-ok.test");
    make_leaf("ip", "caA", "IP:127.0.0.1");
    make_leaf("bad", "caB", "DNS:tls-ok.test");
    make_expired_leaf("old", "caA", "DNS:tls-ok.test");
    make_self_signed("self", "DNS:tls-ok.test");
    use_ca("caA");

    test_content_length();
    test_no_content_length(1);
    test_no_content_length(0);
    test_untrusted();
    test_name_mismatch();
    test_short_body();
    test_expired();
    test_self_signed();
    test_old_protocol();
    test_stream();
    test_plain();
    test_ip_and_no_bundle();

    if (strncmp(g_dir, "/tmp/", 5) == 0 || (base && *base && strncmp(g_dir, base, strlen(base)) == 0))
        sh("rm -rf '%s'", g_dir);
    printf("http_tls_test: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
