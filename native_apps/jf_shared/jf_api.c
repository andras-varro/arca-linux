/*
 * jf_api.c — see jf_api.h.  One HttpReq, a four-slot ring, polled from the render loop.
 */

#include "jf_api.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

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

static void copy_str(char *dst, size_t cap, const char *s)
{
    size_t n = s ? strlen(s) : 0;
    if (n >= cap) n = cap - 1;
    if (n) memcpy(dst, s, n);
    dst[n] = 0;
}

static const char *kind_name(JfKind k)
{
    return k == JF_KIND_AUTH ? "auth" : k == JF_KIND_LIST ? "list" : k == JF_KIND_STREAM ? "stream" : k == JF_KIND_WRITE ? "write" : k == JF_KIND_USER ? "user"
         : k == JF_KIND_SAVE ? "save" : k == JF_KIND_TAGS ? "tags" : k == JF_KIND_FORMAT ? "format" : "none";
}

/* ── pure: the error table ─────────────────────────────────────────────── */

JfErr jf_api_classify(int status, const char *http_err)
{
    if (status == 0) {
        const char *e = http_err ? http_err : "";
        if (!*e) return JF_ERR_BADREPLY;                       /* no status and no reason */
        /* the reasons http.c gives for a reply it refused; everything else is the network */
        if (strstr(e, "chunked") || strstr(e, "larger than the buffer") ||
            strstr(e, "malformed") || strstr(e, "over 8 KiB") || strstr(e, "bad or oversized"))
            return JF_ERR_BADREPLY;
        return JF_ERR_UNREACHABLE;       /* connect, send, recv, timeout, deadline, closed */
    }
    if (status == 200) return JF_ERR_NONE;
    if (status == 401) return JF_ERR_UNAUTH;
    if (status == 403) return JF_ERR_FORBIDDEN;
    if (status == 404) return JF_ERR_NOTFOUND;
    if (status >= 500 && status <= 599) return JF_ERR_SERVER;
    return JF_ERR_BADREPLY;
}

/* ── lifecycle and setters ─────────────────────────────────────────────── */

int jf_api_init(JfApi *a)
{
    memset(a, 0, sizeof(*a));
    a->idle_ms = 5000;
    a->list_total_ms = 15000;
    a->stream_total_ms = 20000;
    a->auth_total_ms = 10000;
    a->retry_ms = 1500;
    http_init(&a->req);
    a->body = malloc(JF_API_BODY_BYTES);
    a->toks = malloc(sizeof(JsonTok) * JF_TOKENS_PAGE);
    if (!a->body || !a->toks) {
        free(a->body); free(a->toks);
        a->body = NULL; a->toks = NULL;
        return -1;
    }
    return 0;
}

void jf_api_free(JfApi *a)
{
    http_end(&a->req);
    if (a->body) { wipe(a->body, JF_API_BODY_BYTES); free(a->body); }
    free(a->toks);
    wipe(a->token, sizeof(a->token));
    wipe(a->ring, sizeof(a->ring));
    wipe(&a->cur, sizeof(a->cur));
    a->body = NULL; a->toks = NULL;
}

void jf_api_set_server(JfApi *a, const char *host, int port)
{
    copy_str(a->host, sizeof(a->host), host);
    a->port = port;
}

void jf_api_set_tls(JfApi *a, bool tls)
{
    a->tls = tls;
}

void jf_api_set_auth(JfApi *a, const char *client, const char *device, const char *device_id, const char *token)
{
    copy_str(a->client, sizeof(a->client), client);
    copy_str(a->device, sizeof(a->device), device);
    copy_str(a->device_id, sizeof(a->device_id), device_id);
    copy_str(a->token, sizeof(a->token), token);
}

/* ── the queue ─────────────────────────────────────────────────────────── */

static void slot_clear(JfSlot *s)
{
    wipe(s, sizeof(*s));
}

/* Ends the in-flight request without a result. */
static void drop_current(JfApi *a)
{
    http_end(&a->req);
    slot_clear(&a->cur);
    a->waiting = false;
}

void jf_api_cancel_kind(JfApi *a, JfKind kind)
{
    for (int i = 0; i < JF_API_SLOTS; i++)
        if (a->ring[i].kind == kind) slot_clear(&a->ring[i]);
    if (a->cur.kind == kind) drop_current(a);
}

static int enqueue(JfApi *a, JfKind kind, unsigned tag, const char *path, JfList *list,
                   const char *body, size_t body_len, const char *method)
{
    size_t pl = path ? strlen(path) : 0;
    if (pl == 0 || pl >= JF_API_PATH_MAX || body_len > JF_API_POST_BYTES) return -1;
    if (kind == JF_KIND_LIST || kind == JF_KIND_STREAM || kind == JF_KIND_TAGS || kind == JF_KIND_FORMAT) jf_api_cancel_kind(a, kind);   /* supersede */
    if (kind != JF_KIND_TAGS && a->cur.kind == JF_KIND_TAGS) drop_current(a);   /* a tag read never makes anything wait */
    for (int i = 0; i < JF_API_SLOTS; i++) {
        JfSlot *s = &a->ring[i];
        if (s->kind != JF_KIND_NONE) continue;
        s->kind = kind;
        s->tag = tag;
        s->seq = ++a->next_seq;
        s->list = list;
        memcpy(s->path, path, pl + 1);
        copy_str(s->method, sizeof s->method, method);
        if (body_len) memcpy(s->body, body, body_len);
        s->body_len = body_len;
        return 0;
    }
    return -1;
}

int jf_api_enqueue_auth(JfApi *a, unsigned tag, const char *user, const char *password)
{
    char path[64], body[JF_API_BODY_OUT];
    int pl = jf_path_auth(path, sizeof(path));
    int bl = jf_auth_body(body, sizeof(body), user, password);
    int rc = -1;
    if (pl > 0 && bl > 0) rc = enqueue(a, JF_KIND_AUTH, tag, path, NULL, body, (size_t)bl, NULL);
    wipe(body, sizeof(body));
    return rc;
}

int jf_api_enqueue_list(JfApi *a, unsigned tag, const char *path, JfList *list)
{
    if (!list) return -1;
    return enqueue(a, JF_KIND_LIST, tag, path, list, NULL, 0, NULL);
}

int jf_api_enqueue_stream(JfApi *a, unsigned tag, const char *path)
{
    return enqueue(a, JF_KIND_STREAM, tag, path, NULL, NULL, 0, NULL);
}

int jf_api_enqueue_save(JfApi *a, unsigned tag, const char *name, const char *user_id, const char *const *ids, int n)
{
    char path[64], body[JF_API_POST_BYTES];
    int pl = jf_path_playlist_create(path, sizeof(path));
    int bl = jf_save_body(body, sizeof(body), name, user_id, ids, n);
    int rc = -1;
    if (pl > 0 && bl > 0) rc = enqueue(a, JF_KIND_SAVE, tag, path, NULL, body, (size_t)bl, NULL);
    return rc;
}

int jf_api_enqueue_write(JfApi *a, unsigned tag, const char *method, const char *path)
{
    if (!method || (strcmp(method, "POST") != 0 && strcmp(method, "DELETE") != 0)) return -1;
    return enqueue(a, JF_KIND_WRITE, tag, path, NULL, NULL, 0, method);
}

int jf_api_enqueue_user(JfApi *a, unsigned tag, const char *path)
{
    return enqueue(a, JF_KIND_USER, tag, path, NULL, NULL, 0, NULL);
}

int jf_api_enqueue_format(JfApi *a, unsigned tag, const char *path)
{
    return enqueue(a, JF_KIND_FORMAT, tag, path, NULL, NULL, 0, NULL);
}

int jf_api_enqueue_tags(JfApi *a, unsigned tag, const char *path, int range)
{
    if (range < 1 || range > JF_TAGS_HARD || (size_t)range + 1 > JF_API_BODY_BYTES) return -1;
    if (enqueue(a, JF_KIND_TAGS, tag, path, NULL, NULL, 0, NULL) < 0) return -1;
    for (int i = 0; i < JF_API_SLOTS; i++)
        if (a->ring[i].kind == JF_KIND_TAGS && a->ring[i].seq == a->next_seq) a->ring[i].range = range;
    return 0;
}

bool jf_api_busy(const JfApi *a)
{
    if (a->cur.kind != JF_KIND_NONE) return true;
    for (int i = 0; i < JF_API_SLOTS; i++)
        if (a->ring[i].kind != JF_KIND_NONE) return true;
    return false;
}

static int prio(JfKind k)
{
    return k == JF_KIND_AUTH ? 4 : (k == JF_KIND_SAVE || k == JF_KIND_WRITE) ? 3 : k == JF_KIND_STREAM ? 2 : k == JF_KIND_TAGS ? 0 : 1;
}

/* Highest priority, oldest first; -1 when the ring is empty. */
static int pick(const JfApi *a)
{
    int best = -1;
    for (int i = 0; i < JF_API_SLOTS; i++) {
        const JfSlot *s = &a->ring[i];
        if (s->kind == JF_KIND_NONE) continue;
        if (best < 0 || prio(s->kind) > prio(a->ring[best].kind) ||
            (prio(s->kind) == prio(a->ring[best].kind) && s->seq < a->ring[best].seq))
            best = i;
    }
    return best;
}

/* ── one request on the wire ───────────────────────────────────────────── */

static void begin_current(JfApi *a)
{
    char hdr[1024];
    JfSlot *c = &a->cur;
    const char *tok = (c->kind == JF_KIND_AUTH) ? NULL : a->token;
    int total = (c->kind == JF_KIND_AUTH || c->kind == JF_KIND_SAVE || c->kind == JF_KIND_WRITE) ? a->auth_total_ms
              : c->kind == JF_KIND_STREAM ? a->stream_total_ms : a->list_total_ms;
    int hl = jf_auth_header(hdr, sizeof(hdr), a->client, a->device, a->device_id, tok);
    a->began_ms = now_ms();
    a->waiting = false;
    if (hl < 0) {
        /* the identity holds a character the header cannot carry: a begin that is refused */
        http_begin(&a->req, a->host, a->port, a->tls, "GET", "no-slash", NULL, NULL, 0, HTTP_BUFFER, a->body, JF_API_BODY_BYTES);
        wipe(hdr, sizeof(hdr));
        return;
    }
    /* without a JSON content type the sign-in POST is answered 415; the playlist POST needs it too */
    if (c->kind == JF_KIND_AUTH || c->kind == JF_KIND_SAVE)
        hl += snprintf(hdr + hl, sizeof(hdr) - (size_t)hl, "Content-Type: application/json\r\n");
    if (c->kind == JF_KIND_AUTH || c->kind == JF_KIND_SAVE)
        http_begin(&a->req, a->host, a->port, a->tls, "POST", c->path, hdr, c->body, c->body_len,
                   HTTP_BUFFER, a->body, JF_API_BODY_BYTES);
    else if (c->kind == JF_KIND_WRITE)
        http_begin(&a->req, a->host, a->port, a->tls, c->method, c->path, hdr, NULL, 0,
                   HTTP_BUFFER, a->body, JF_API_BODY_BYTES);
    else if (c->kind == JF_KIND_TAGS) {
        /* the whole reply must fit range bytes: a server that ignores Range sends a 200 longer than that, which http.c refuses as
         * "larger than the buffer" as soon as the head says so (or at the first byte past the cap), so at most range+1 bytes are kept */
        char rh[40];
        snprintf(rh, sizeof rh, "Range: bytes=0-%d\r\n", c->range - 1);
        if (hl + (int)strlen(rh) < (int)sizeof(hdr)) strcpy(hdr + hl, rh);
        http_begin(&a->req, a->host, a->port, a->tls, "GET", c->path, hdr, NULL, 0, HTTP_BUFFER, a->body, (size_t)c->range + 1);
    } else if (c->kind == JF_KIND_LIST || c->kind == JF_KIND_USER || c->kind == JF_KIND_FORMAT)
        http_begin(&a->req, a->host, a->port, a->tls, "GET", c->path, hdr, NULL, 0,
                   HTTP_BUFFER, a->body, JF_API_BODY_BYTES);
    else
        http_begin(&a->req, a->host, a->port, a->tls, "GET", c->path, hdr, NULL, 0, HTTP_STREAM, NULL, 0);
    wipe(hdr, sizeof(hdr));
    a->req.timeout_ms = a->idle_ms;
    a->req.total_timeout_ms = total;
}

static void finish(JfApi *a, JfResult *out, int status, const char *herr, int fd)
{
    JfSlot *c = &a->cur;
    memset(out, 0, sizeof(*out));
    out->kind = c->kind;
    out->tag = c->tag;
    out->status = status;
    out->list = c->list;
    out->fd = -1;
    out->parse = JF_PARSE_OK;
    out->err = jf_api_classify(status, herr);
    if (c->kind == JF_KIND_WRITE && status == 204) out->err = JF_ERR_NONE;   /* a DELETE or a playlist add answers 204 No Content */
    if (c->kind == JF_KIND_TAGS) {
        if (status == 206) out->err = JF_ERR_NONE;                 /* partial content is the answer we asked for */
        if (a->tags_overrun) {
            out->tags_rejected = true;
            if (a->log) logger_write(a->log, LOG_LEVEL_WARN, __FILE__, __LINE__, "jf_api: the server answered 200 to a Range request: tag reads are switched off for it");
        }
        if (out->err == JF_ERR_NONE && !jf_tags_parse((const unsigned char *)http_body(&a->req), http_body_len(&a->req), &out->tags)) memset(&out->tags, 0, sizeof out->tags);
    }

    if (out->err == JF_ERR_NONE && c->kind == JF_KIND_LIST) {
        out->parse = jf_parse_items(http_body(&a->req), http_body_len(&a->req), a->toks, JF_TOKENS_PAGE, c->list);
        if (out->parse != JF_PARSE_OK) out->err = JF_ERR_BADREPLY;
    } else if (out->err == JF_ERR_NONE && c->kind == JF_KIND_AUTH) {
        if (jf_parse_auth(http_body(&a->req), http_body_len(&a->req), a->toks, JF_TOKENS_PAGE,
                          out->token, sizeof(out->token), out->user_id, sizeof(out->user_id), &out->policy) != JF_PARSE_OK)
            out->err = JF_ERR_BADREPLY;
    } else if (out->err == JF_ERR_NONE && c->kind == JF_KIND_USER) {
        if (jf_parse_user_policy(http_body(&a->req), http_body_len(&a->req), a->toks, JF_TOKENS_PAGE, &out->policy) != JF_PARSE_OK)
            out->err = JF_ERR_BADREPLY;
    } else if (out->err == JF_ERR_NONE && c->kind == JF_KIND_FORMAT) {
        if (jf_parse_format(http_body(&a->req), http_body_len(&a->req), a->toks, JF_TOKENS_PAGE, &out->format) != JF_PARSE_OK)
            out->err = JF_ERR_BADREPLY;
    } else if (c->kind == JF_KIND_STREAM) {
        if (out->err == JF_ERR_NONE && fd >= 0) out->fd = fd;
        else if (out->err == JF_ERR_NONE) out->err = JF_ERR_BADREPLY;   /* a 200 with no fd to hand over: not a success */
        else if (fd >= 0) close(fd);
    }
    if (c->kind == JF_KIND_AUTH) {
        wipe(a->body, JF_API_BODY_BYTES);     /* the reply holds the token */
        if (out->err != JF_ERR_NONE) { wipe(out->token, sizeof(out->token)); wipe(out->user_id, sizeof(out->user_id)); }
    }

    if (a->log)
        logger_write(a->log, out->err == JF_ERR_NONE ? LOG_LEVEL_INFO : LOG_LEVEL_WARN, __FILE__, __LINE__,
                     "jf_api: %s status=%d err=%d ms=%lld items=%d%s%s", kind_name(c->kind), status, (int)out->err,
                     now_ms() - a->began_ms, (c->kind == JF_KIND_LIST && c->list) ? c->list->n : 0,
                     herr[0] ? " http: " : "", herr);
    http_end(&a->req);
    slot_clear(c);
    a->retried = false;
}

bool jf_api_poll(JfApi *a, JfResult *out)
{
    if (a->cur.kind == JF_KIND_NONE) {
        int i = pick(a);
        if (i < 0) return false;
        a->cur = a->ring[i];
        slot_clear(&a->ring[i]);
        a->retried = false;
        begin_current(a);
    } else if (a->waiting) {
        if (now_ms() < a->retry_at) return false;
        begin_current(a);
    }

    HttpPoll hp = http_poll(&a->req);
    if (hp == HTTP_PENDING) return false;

    int status = hp == HTTP_DONE ? http_status(&a->req) : 0;
    char herr[96];
    copy_str(herr, sizeof(herr), hp == HTTP_ERROR ? http_error(&a->req) : "");
    JfErr e = jf_api_classify(status, herr);

    bool post = a->cur.kind == JF_KIND_AUTH || a->cur.kind == JF_KIND_SAVE || a->cur.kind == JF_KIND_WRITE || a->cur.kind == JF_KIND_TAGS;     /* never retried */
    a->tags_overrun = false;
    if (a->cur.kind == JF_KIND_TAGS && hp == HTTP_ERROR && strstr(herr, "larger than the buffer") && http_status(&a->req) == 200) {
        a->tags_overrun = true;
        status = 200;
    }
    if ((e == JF_ERR_UNREACHABLE || e == JF_ERR_SERVER) && !post && !a->retried) {
        if (hp == HTTP_DONE && a->cur.kind == JF_KIND_STREAM) {
            int f = http_take_fd(&a->req);
            if (f >= 0) http_stream_close(f);
        }
        http_end(&a->req);
        a->retried = true;
        a->waiting = true;
        a->retry_at = now_ms() + a->retry_ms;
        return false;
    }
    int fd = (hp == HTTP_DONE && a->cur.kind == JF_KIND_STREAM) ? http_take_fd(&a->req) : -1;
    finish(a, out, status, herr, fd);
    return true;
}
