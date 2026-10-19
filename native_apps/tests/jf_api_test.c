/*
 * jf_api_test.c — the Jellyfin request queue against a forked loopback server.
 *
 * The fake server answers each accepted connection from a script (one action per
 * connection, the last repeating) and reports every request head it receives
 * through a pipe, so the test can see what went on the wire and how often.
 *
 * Build (also a CTEST_ROWS row in tests/run-all.sh):
 *   gcc -Wall -Wextra -Wno-unused-parameter -I common -I jellyfin -fsanitize=undefined \
 *       -o /tmp/jf_api_test tests/jf_api_test.c jellyfin/jf_api.c jellyfin/jf_model.c \
 *       jellyfin/jf_url.c common/http.c common/json.c common/logger.c
 */

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "jf_api.h"

static int checks, failed;
#define CHECK(c) do { checks++; if (!(c)) { failed++; \
    printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

enum { A_ITEMS, A_AUTH, A_STATUS, A_SILENT, A_STREAM };
typedef struct { int kind; int status; } Act;

#define ID1 "0123456789abcdef0123456789abcdef"
#define ID2 "fedcba9876543210fedcba9876543210"
#define PASSWORD "PW-SECRET-9"
#define TOKEN    "tok123"

static const char ITEMS_BODY[] =
    "{\"Items\":[{\"Name\":\"Alpha\",\"Id\":\"" ID1 "\",\"Type\":\"MusicArtist\"},"
    "{\"Name\":\"Beta\",\"Id\":\"" ID2 "\",\"Type\":\"MusicArtist\"}],"
    "\"TotalRecordCount\":2,\"StartIndex\":0}";
static const char AUTH_BODY[] =
    "{\"User\":{\"Name\":\"u\",\"Id\":\"" ID1 "\"},\"AccessToken\":\"" TOKEN "\"}";

static void reply(int c, int status, const char *body, size_t n)
{
    char head[160];
    int hl = snprintf(head, sizeof(head), "HTTP/1.0 %d X\r\nContent-Length: %zu\r\n\r\n", status, n);
    if (write(c, head, (size_t)hl) < 0) return;
    if (n && write(c, body, n) < 0) return;
}

static void serve(int ls, int hpipe, const Act *script, int nscript)
{
    int conn = 0;
    signal(SIGPIPE, SIG_IGN);
    for (;;) {
        int c = accept(ls, NULL, NULL);
        if (c < 0) continue;
        char buf[4096];
        size_t len = 0;
        char *end = NULL;
        while (len < sizeof(buf) - 1) {
            ssize_t n = read(c, buf + len, sizeof(buf) - 1 - len);
            if (n <= 0) break;
            len += (size_t)n;
            buf[len] = 0;
            if ((end = strstr(buf, "\r\n\r\n")) != NULL) break;
        }
        if (!end) { close(c); continue; }
        /* drain a request body so the close does not reset the reply, and report it after the head */
        char body[8192];
        size_t have = len - (size_t)(end + 4 - buf);
        const char *cl = strstr(buf, "Content-Length: ");
        size_t want = cl ? (size_t)atoi(cl + 16) : 0;
        if (want > sizeof(body) - 1) want = sizeof(body) - 1;
        if (have > want) have = want;
        memcpy(body, end + 4, have);
        while (have < want) {
            ssize_t n = read(c, body + have, want - have);
            if (n <= 0) break;
            have += (size_t)n;
        }
        (void)!write(hpipe, buf, (size_t)(end + 4 - buf));
        if (have) (void)!write(hpipe, body, have);
        Act a = script[conn < nscript ? conn : nscript - 1];
        conn++;
        switch (a.kind) {
        case A_ITEMS:  reply(c, 200, ITEMS_BODY, strlen(ITEMS_BODY)); break;
        case A_AUTH:   reply(c, 200, AUTH_BODY, strlen(AUTH_BODY)); break;
        case A_STATUS: reply(c, a.status, "nope", 4); break;
        case A_STREAM: reply(c, a.status ? a.status : 200, "0123456789", 10); break;
        case A_SILENT: continue;                 /* keep the connection open, say nothing */
        }
        close(c);
    }
}

typedef struct { int ls, port, hp[2]; pid_t pid; char heads[16384]; size_t hlen; } Srv;

static Srv *srv_start(const Act *script, int n)
{
    static Srv s;
    memset(&s, 0, sizeof(s));
    s.ls = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(s.ls, (struct sockaddr *)&a, sizeof(a)) != 0 || listen(s.ls, 8) != 0 || pipe(s.hp) != 0) {
        perror("bind/listen/pipe"); exit(2);
    }
    socklen_t l = sizeof(a);
    getsockname(s.ls, (struct sockaddr *)&a, &l);
    s.port = ntohs(a.sin_port);
    s.pid = fork();
    if (s.pid == 0) serve(s.ls, s.hp[1], script, n);
    fcntl(s.hp[0], F_SETFL, O_NONBLOCK);
    return &s;
}

static void srv_stop(Srv *s)
{
    kill(s->pid, SIGKILL);
    waitpid(s->pid, NULL, 0);
    close(s->ls); close(s->hp[0]); close(s->hp[1]);
}

/* Everything the server has seen so far (NUL separated is not needed: heads end in a blank line). */
static const char *heads(Srv *s)
{
    ssize_t n;
    while (s->hlen < sizeof(s->heads) - 1 &&
           (n = read(s->hp[0], s->heads + s->hlen, sizeof(s->heads) - 1 - s->hlen)) > 0)
        s->hlen += (size_t)n;
    s->heads[s->hlen] = 0;
    return s->heads;
}

static int count_str(const char *hay, const char *needle)
{
    int c = 0;
    for (const char *p = hay; (p = strstr(p, needle)) != NULL; p += strlen(needle)) c++;
    return c;
}

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* The app's loop: poll, sleep a millisecond, until idle or `limit_ms`.  Returns the results seen. */
static int pump(JfApi *a, JfResult *res, int max, int limit_ms)
{
    long long t0 = now_ms();
    int n = 0;
    while (now_ms() - t0 < limit_ms && jf_api_busy(a)) {
        JfResult r;
        if (jf_api_poll(a, &r) && n < max) res[n++] = r;
        usleep(1000);
    }
    return n;
}

static void setup(JfApi *a, Srv *s)
{
    CHECK(jf_api_init(a) == 0);
    jf_api_set_server(a, "127.0.0.1", s->port);
    jf_api_set_auth(a, "dev", "did", NULL);
    a->retry_ms = 50;
}

static JfItem store[8];
static JfList list;
static void fresh_list(void) { jf_list_init(&list, store, 8); }

static void test_list_and_header(void)
{
    Act sc[] = {{A_ITEMS, 0}};
    Srv *s = srv_start(sc, 1);
    JfApi a; JfResult r[4];
    setup(&a, s);
    fresh_list();
    CHECK(jf_api_enqueue_list(&a, 7, "/Items?x=1", &list) == 0);
    CHECK(pump(&a, r, 4, 3000) == 1);
    CHECK(r[0].kind == JF_KIND_LIST && r[0].tag == 7 && r[0].err == JF_ERR_NONE && r[0].status == 200);
    CHECK(r[0].parse == JF_PARSE_OK && r[0].list == &list);
    CHECK(list.n == 2 && strcmp(store[0].name, "Alpha") == 0 && strcmp(store[1].name, "Beta") == 0);
    const char *h = heads(s);
    CHECK(strstr(h, "GET /Items?x=1 HTTP/1.0") != NULL);
    CHECK(strstr(h, "Authorization: MediaBrowser") != NULL);
    CHECK(strstr(h, "Token=") == NULL);                         /* not signed in: no token */

    /* signed in: the token goes out on a GET, never on the sign-in POST */
    jf_api_set_auth(&a, "dev", "did", "TOK-ABC");
    fresh_list();
    CHECK(jf_api_enqueue_list(&a, 8, "/Items?x=2", &list) == 0);
    CHECK(pump(&a, r, 4, 3000) == 1 && r[0].err == JF_ERR_NONE);
    h = heads(s);
    CHECK(strstr(strstr(h, "x=2"), "Token=\"TOK-ABC\"") != NULL);
    CHECK(jf_api_enqueue_auth(&a, 9, "u", PASSWORD) == 0);
    pump(&a, r, 4, 3000);
    h = heads(s);
    const char *post = strstr(h, "POST /Users/AuthenticateByName");
    CHECK(post != NULL);
    CHECK(post && strstr(post, "TOK-ABC") == NULL);
    /* the real server answers 415 to a sign-in without it */
    CHECK(post && strstr(post, "\r\nContent-Type: application/json\r\n") != NULL);
    jf_api_free(&a);
    srv_stop(s);
}

static void test_errors(void)
{
    JfApi a; JfResult r[4];
    Act s401[] = {{A_STATUS, 401}};
    Srv *s = srv_start(s401, 1);
    setup(&a, s);
    fresh_list();
    jf_api_enqueue_list(&a, 1, "/x", &list);
    CHECK(pump(&a, r, 4, 3000) == 1 && r[0].err == JF_ERR_UNAUTH && r[0].status == 401 && list.n == 0);
    CHECK(count_str(heads(s), "GET /x") == 1);                  /* 401 is not retried */
    jf_api_free(&a); srv_stop(s);

    Act s500[] = {{A_STATUS, 500}, {A_ITEMS, 0}};
    s = srv_start(s500, 2);
    setup(&a, s);
    fresh_list();
    jf_api_enqueue_list(&a, 2, "/x", &list);
    CHECK(pump(&a, r, 4, 3000) == 1 && r[0].err == JF_ERR_NONE && list.n == 2);
    CHECK(count_str(heads(s), "GET /x") == 2);                  /* one retry, then success */
    jf_api_free(&a); srv_stop(s);

    /* the retry is single: 500 forever ends SERVER after exactly two requests */
    Act s500b[] = {{A_STATUS, 500}};
    s = srv_start(s500b, 1);
    setup(&a, s);
    fresh_list();
    jf_api_enqueue_list(&a, 3, "/x", &list);
    CHECK(pump(&a, r, 4, 3000) == 1 && r[0].err == JF_ERR_SERVER && r[0].status == 500);
    CHECK(count_str(heads(s), "GET /x") == 2);
    jf_api_free(&a); srv_stop(s);

    /* an AUTH is never retried, even on a 500 */
    s = srv_start(s500b, 1);
    setup(&a, s);
    jf_api_enqueue_auth(&a, 4, "u", PASSWORD);
    CHECK(pump(&a, r, 4, 3000) == 1 && r[0].kind == JF_KIND_AUTH && r[0].err == JF_ERR_SERVER);
    CHECK(count_str(heads(s), "POST ") == 1);
    jf_api_free(&a); srv_stop(s);

    /* a server that accepts and goes silent: the idle timeout, then one retry, UNREACHABLE */
    Act sil[] = {{A_SILENT, 0}};
    s = srv_start(sil, 1);
    setup(&a, s);
    a.idle_ms = 250;
    fresh_list();
    jf_api_enqueue_list(&a, 5, "/x", &list);
    long long t0 = now_ms();
    CHECK(pump(&a, r, 4, 4000) == 1 && r[0].err == JF_ERR_UNREACHABLE && r[0].status == 0 && r[0].tag == 5);
    CHECK(now_ms() - t0 < 3000);
    CHECK(count_str(heads(s), "GET /x") == 2);
    jf_api_free(&a); srv_stop(s);
}

static void test_supersede_and_priority(void)
{
    JfApi a; JfResult r[8];
    Act sc[] = {{A_ITEMS, 0}};
    Srv *s = srv_start(sc, 1);
    setup(&a, s);

    /* three queued LISTs: only the newest is sent and delivered */
    fresh_list();
    jf_api_enqueue_list(&a, 1, "/a", &list);
    jf_api_enqueue_list(&a, 2, "/b", &list);
    jf_api_enqueue_list(&a, 3, "/c", &list);
    int n = pump(&a, r, 8, 3000);
    CHECK(n == 1 && r[0].tag == 3);
    CHECK(strstr(heads(s), "GET /a") == NULL && strstr(heads(s), "GET /c") != NULL);

    jf_api_free(&a); srv_stop(s);

    /* an in-flight LIST is ended by a newer one.  The server never answers
     * the first connection, so tag 10 cannot finish before it is superseded
     * however loaded the host is; a reply that did would be a legal result */
    Act sl[] = {{A_SILENT, 0}, {A_ITEMS, 0}};
    s = srv_start(sl, 2);
    setup(&a, s);
    fresh_list();
    jf_api_enqueue_list(&a, 10, "/d", &list);
    JfResult tmp;
    for (int i = 0; i < 200 && !strstr(heads(s), "GET /d"); i++) {
        CHECK(!jf_api_poll(&a, &tmp));                          /* starts tag 10 */
        usleep(5000);
    }
    CHECK(strstr(heads(s), "GET /d") != NULL);
    jf_api_enqueue_list(&a, 11, "/e", &list);
    n = pump(&a, r, 8, 3000);
    CHECK(n == 1 && r[0].tag == 11 && list.n == 2);
    jf_api_free(&a); srv_stop(s);

    /* AUTH > STREAM > LIST whatever the order they were queued in */
    Act pr[] = {{A_AUTH, 0}, {A_STREAM, 0}, {A_ITEMS, 0}};
    s = srv_start(pr, 3);
    setup(&a, s);
    fresh_list();
    CHECK(jf_api_enqueue_list(&a, 1, "/list", &list) == 0);
    CHECK(jf_api_enqueue_stream(&a, 2, "/stream") == 0);
    CHECK(jf_api_enqueue_auth(&a, 3, "u", PASSWORD) == 0);
    n = pump(&a, r, 8, 3000);
    CHECK(n == 3);
    CHECK(r[0].kind == JF_KIND_AUTH && r[1].kind == JF_KIND_STREAM && r[2].kind == JF_KIND_LIST);
    CHECK(r[0].tag == 3 && r[1].tag == 2 && r[2].tag == 1);
    CHECK(r[0].err == JF_ERR_NONE && strcmp(r[0].token, TOKEN) == 0 && strcmp(r[0].user_id, ID1) == 0);
    const char *h = heads(s);
    const char *p1 = strstr(h, "POST "), *p2 = strstr(h, "GET /stream"), *p3 = strstr(h, "GET /list");
    CHECK(p1 && p2 && p3 && p1 < p2 && p2 < p3);
    if (r[1].fd >= 0) close(r[1].fd);

    /* the password and the token are not left behind in the object */
    CHECK(memmem(&a, sizeof(a), PASSWORD, strlen(PASSWORD)) == NULL);
    CHECK(memmem(&a, sizeof(a), TOKEN, strlen(TOKEN)) == NULL);
    CHECK(memmem(a.body, JF_API_BODY_BYTES, TOKEN, strlen(TOKEN)) == NULL);
    CHECK(memmem(a.body, JF_API_BODY_BYTES, PASSWORD, strlen(PASSWORD)) == NULL);
    jf_api_free(&a); srv_stop(s);

    /* an AUTH as the LAST request: its reply buffer (the token) and body (the password) are wiped */
    Act au[] = {{A_AUTH, 0}};
    s = srv_start(au, 1);
    setup(&a, s);
    jf_api_enqueue_auth(&a, 1, "u", PASSWORD);
    CHECK(pump(&a, r, 8, 3000) == 1 && strcmp(r[0].token, TOKEN) == 0);
    CHECK(memmem(a.body, JF_API_BODY_BYTES, TOKEN, strlen(TOKEN)) == NULL);
    CHECK(memmem(&a, sizeof(a), PASSWORD, strlen(PASSWORD)) == NULL);
    jf_api_free(&a); srv_stop(s);

    /* cancel and the ring bound */
    s = srv_start(sc, 1);
    setup(&a, s);
    fresh_list();
    jf_api_enqueue_list(&a, 1, "/a", &list);
    jf_api_cancel_kind(&a, JF_KIND_LIST);
    CHECK(!jf_api_busy(&a));
    for (int i = 0; i < JF_API_SLOTS; i++) CHECK(jf_api_enqueue_auth(&a, (unsigned)i, "u", "p") == 0);
    CHECK(jf_api_enqueue_auth(&a, 99, "u", "p") == -1);
    jf_api_cancel_kind(&a, JF_KIND_AUTH);
    CHECK(!jf_api_busy(&a));
    jf_api_free(&a); srv_stop(s);
}

static void test_stream(void)
{
    JfApi a; JfResult r[4];
    Act sc[] = {{A_STREAM, 0}};
    Srv *s = srv_start(sc, 1);
    setup(&a, s);
    jf_api_enqueue_stream(&a, 5, "/Audio/x/stream");
    CHECK(pump(&a, r, 4, 3000) == 1);
    CHECK(r[0].kind == JF_KIND_STREAM && r[0].tag == 5 && r[0].err == JF_ERR_NONE && r[0].fd >= 0);
    char got[16];
    size_t n = 0;
    long long t0 = now_ms();
    while (r[0].fd >= 0 && n < 10 && now_ms() - t0 < 2000) {
        ssize_t k = read(r[0].fd, got + n, 10 - n);
        if (k > 0) n += (size_t)k; else usleep(1000);
    }
    CHECK(n == 10 && memcmp(got, "0123456789", 10) == 0);      /* first body byte first */
    if (r[0].fd >= 0) close(r[0].fd);
    jf_api_free(&a); srv_stop(s);

    Act nf[] = {{A_STREAM, 404}};
    s = srv_start(nf, 1);
    setup(&a, s);
    jf_api_enqueue_stream(&a, 6, "/Audio/y/stream");
    CHECK(pump(&a, r, 4, 3000) == 1 && r[0].err == JF_ERR_NOTFOUND && r[0].fd == -1);
    jf_api_free(&a); srv_stop(s);
}

static void test_save(void)
{
    JfApi a;
    JfResult r[8];
    const char *ids2[] = { ID2, ID1 };

    /* a 200 whose reply body is not JSON is still a success: the body of a SAVE is ignored */
    Act ok[] = {{A_STATUS, 200}};
    Srv *s = srv_start(ok, 1);
    setup(&a, s);
    jf_api_set_auth(&a, "dev", "did", "TOK-SAVE");
    CHECK(jf_api_enqueue_save(&a, 21, "My \"mix\" \134 \303\251", ID1, ids2, 2) == 0);
    CHECK(jf_api_busy(&a));
    CHECK(pump(&a, r, 4, 3000) == 1);
    CHECK(r[0].kind == JF_KIND_SAVE && r[0].tag == 21 && r[0].err == JF_ERR_NONE && r[0].status == 200);
    CHECK(r[0].parse == JF_PARSE_OK && r[0].list == NULL && r[0].fd == -1);
    const char *h = heads(s);
    CHECK(strstr(h, "POST /Playlists HTTP/1.0") != NULL);
    CHECK(strstr(h, "\r\nContent-Type: application/json\r\n") != NULL);
    CHECK(strstr(h, "Token=\"TOK-SAVE\"") != NULL);             /* unlike the sign-in, the token goes */
    CHECK(strstr(h, "\r\n\r\n{\"Name\":\"My \134\"mix\134\" \134\134 \303\251\",\"Ids\":[\"" ID2 "\",\"" ID1
                    "\"],\"UserId\":\"" ID1 "\",\"MediaType\":\"Audio\"}") != NULL);
    jf_api_free(&a);
    srv_stop(s);

    /* the largest save: 100 ids, in order, the whole body arrives */
    char idbuf[JF_SAVE_IDS_MAX][JF_ID_LEN + 1];
    const char *ids[JF_SAVE_IDS_MAX];
    for (int i = 0; i < JF_SAVE_IDS_MAX; i++) {
        snprintf(idbuf[i], sizeof(idbuf[i]), "%032x", i + 1);
        ids[i] = idbuf[i];
    }
    s = srv_start(ok, 1);
    setup(&a, s);
    CHECK(jf_api_enqueue_save(&a, 1, "Big", ID1, ids, JF_SAVE_IDS_MAX) == 0);
    CHECK(pump(&a, r, 4, 3000) == 1 && r[0].err == JF_ERR_NONE);
    h = heads(s);
    const char *p1 = strstr(h, idbuf[0]), *p100 = strstr(h, idbuf[99]);
    CHECK(p1 && p100 && p1 < p100);
    CHECK(strstr(h, "\"],\"UserId\":\"" ID1 "\",\"MediaType\":\"Audio\"}") != NULL);
    CHECK(jf_api_enqueue_save(&a, 2, "Big", ID1, ids, JF_SAVE_IDS_MAX + 1) == -1);
    CHECK(jf_api_enqueue_save(&a, 2, "Big", ID1, ids, 0) == -1);
    CHECK(jf_api_enqueue_save(&a, 2, "Big", "short", ids, 1) == -1);
    CHECK(!jf_api_busy(&a));                                    /* a refused enqueue queues nothing */
    jf_api_free(&a);
    srv_stop(s);

    /* never retried: a 500 and a silent server each cost exactly one POST (a LIST costs two) */
    Act s500[] = {{A_STATUS, 500}};
    s = srv_start(s500, 1);
    setup(&a, s);
    jf_api_enqueue_save(&a, 3, "x", ID1, ids2, 1);
    CHECK(pump(&a, r, 4, 3000) == 1 && r[0].kind == JF_KIND_SAVE && r[0].err == JF_ERR_SERVER && r[0].status == 500);
    CHECK(count_str(heads(s), "POST /Playlists") == 1);
    jf_api_free(&a); srv_stop(s);

    Act sil[] = {{A_SILENT, 0}};
    s = srv_start(sil, 1);
    setup(&a, s);
    a.idle_ms = 250;
    jf_api_enqueue_save(&a, 4, "x", ID1, ids2, 1);
    CHECK(pump(&a, r, 4, 4000) == 1 && r[0].kind == JF_KIND_SAVE && r[0].err == JF_ERR_UNREACHABLE && r[0].tag == 4);
    CHECK(count_str(heads(s), "POST /Playlists") == 1);
    jf_api_free(&a); srv_stop(s);

    /* no supersession: two saves both go out, oldest first; a 401 is reported as such */
    Act two[] = {{A_STATUS, 200}, {A_STATUS, 401}};
    s = srv_start(two, 2);
    setup(&a, s);
    CHECK(jf_api_enqueue_save(&a, 5, "first", ID1, ids2, 1) == 0);
    CHECK(jf_api_enqueue_save(&a, 6, "second", ID1, ids2, 1) == 0);
    CHECK(pump(&a, r, 4, 3000) == 2);
    CHECK(r[0].tag == 5 && r[0].err == JF_ERR_NONE && r[1].tag == 6 && r[1].err == JF_ERR_UNAUTH);
    h = heads(s);
    CHECK(strstr(h, "first") != NULL && strstr(h, "second") != NULL && strstr(h, "first") < strstr(h, "second"));
    jf_api_free(&a); srv_stop(s);

    /* AUTH > SAVE > STREAM > LIST whatever the order they were queued in */
    Act pr[] = {{A_AUTH, 0}, {A_STATUS, 200}, {A_STREAM, 0}, {A_ITEMS, 0}};
    s = srv_start(pr, 4);
    setup(&a, s);
    fresh_list();
    CHECK(jf_api_enqueue_list(&a, 1, "/list", &list) == 0);
    CHECK(jf_api_enqueue_stream(&a, 2, "/stream") == 0);
    CHECK(jf_api_enqueue_save(&a, 3, "x", ID1, ids2, 1) == 0);
    CHECK(jf_api_enqueue_auth(&a, 4, "u", PASSWORD) == 0);
    CHECK(pump(&a, r, 8, 3000) == 4);
    CHECK(r[0].kind == JF_KIND_AUTH && r[1].kind == JF_KIND_SAVE && r[2].kind == JF_KIND_STREAM && r[3].kind == JF_KIND_LIST);
    if (r[2].fd >= 0) close(r[2].fd);
    jf_api_free(&a); srv_stop(s);

    /* cancel drops queued saves and nothing else; the ring still bounds them */
    s = srv_start(ok, 1);
    setup(&a, s);
    fresh_list();
    for (int i = 0; i < JF_API_SLOTS; i++) CHECK(jf_api_enqueue_save(&a, (unsigned)i, "x", ID1, ids2, 1) == 0);
    CHECK(jf_api_enqueue_save(&a, 99, "x", ID1, ids2, 1) == -1);
    jf_api_cancel_kind(&a, JF_KIND_LIST);
    CHECK(jf_api_busy(&a));
    jf_api_cancel_kind(&a, JF_KIND_SAVE);
    CHECK(!jf_api_busy(&a));
    CHECK(pump(&a, r, 4, 300) == 0);
    CHECK(heads(s)[0] == 0);                                    /* nothing reached the wire */
    /* ... and the one in flight */
    CHECK(jf_api_enqueue_save(&a, 7, "x", ID1, ids2, 1) == 0);
    CHECK(!jf_api_poll(&a, &r[0]));                             /* begins it */
    CHECK(jf_api_busy(&a));
    jf_api_cancel_kind(&a, JF_KIND_SAVE);
    CHECK(!jf_api_busy(&a));
    jf_api_free(&a); srv_stop(s);
}

static void test_classify(void)
{
    CHECK(jf_api_classify(200, "") == JF_ERR_NONE);
    CHECK(jf_api_classify(401, "") == JF_ERR_UNAUTH);
    CHECK(jf_api_classify(403, "") == JF_ERR_FORBIDDEN);
    CHECK(jf_api_classify(404, "") == JF_ERR_NOTFOUND);
    CHECK(jf_api_classify(500, "") == JF_ERR_SERVER);
    CHECK(jf_api_classify(503, NULL) == JF_ERR_SERVER);
    CHECK(jf_api_classify(204, "") == JF_ERR_BADREPLY);
    CHECK(jf_api_classify(302, "") == JF_ERR_BADREPLY);
    CHECK(jf_api_classify(0, "connect: Connection refused") == JF_ERR_UNREACHABLE);
    CHECK(jf_api_classify(0, "timeout in reply head") == JF_ERR_UNREACHABLE);
    CHECK(jf_api_classify(0, "deadline passed in reply body") == JF_ERR_UNREACHABLE);
    CHECK(jf_api_classify(0, "cannot resolve host") == JF_ERR_UNREACHABLE);
    CHECK(jf_api_classify(0, "connection closed before the reply head") == JF_ERR_UNREACHABLE);
    CHECK(jf_api_classify(0, "connection closed mid-body") == JF_ERR_UNREACHABLE);
    CHECK(jf_api_classify(0, "chunked reply (this client speaks HTTP/1.0)") == JF_ERR_BADREPLY);
    CHECK(jf_api_classify(0, "body larger than the buffer") == JF_ERR_BADREPLY);
    CHECK(jf_api_classify(0, "malformed reply head") == JF_ERR_BADREPLY);
    CHECK(jf_api_classify(0, "reply head over 8 KiB") == JF_ERR_BADREPLY);
    CHECK(jf_api_classify(0, "") == JF_ERR_BADREPLY);
    CHECK(jf_api_classify(0, NULL) == JF_ERR_BADREPLY);
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGPIPE, SIG_IGN);
    test_classify();
    test_list_and_header();
    test_errors();
    test_supersede_and_priority();
    test_stream();
    test_save();
    printf("jf_api_test: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
