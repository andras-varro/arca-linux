/*
 * jf_art.c — see jf_art.h.
 */
#define LOGGER_LIB_CLIENT
#include "logger.h"
#include "jf_art.h"
#include "jf_img.h"
#include "jf_url.h"
#include "http.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

enum { SL_EMPTY = 0, SL_WANT, SL_LOADING, SL_READY, SL_FAILED };

typedef struct {
    int       state;
    char      id[JF_ID_LEN + 1];
    char      tag[JF_IMG_TAG_MAX + 1];
    uint32_t *px;
    LvIcon    icon;
    unsigned  used;          /* recency counter */
    long long stamp;         /* FAILED: when */
} Slot;

struct JfArt {
    bool     up;
    JfImgCache cache;
    int      size;
    Slot    *slot;           /* nslots tiles, heap */
    int      nslots;
    unsigned clock;
    unsigned gen;            /* tiles made ready so far */
    long long now;           /* last jf_art_poll time, for the retry rule */
    /* the one request */
    HttpReq  req;
    int      cur;            /* slot being fetched, -1 none */
    char    *buf;
    /* identity (the token is a secret: never logged) */
    char     host[JF_HOST_MAX + 1];
    int      port;
    bool     tls;
    char     client[64], device[64], device_id[64], token[128];
    bool     have_server;
};

/* The default instance behind the jf_art_* calls without an instance argument (Ludfin's). */
static JfArt G = { .cur = -1 };

static long long now_ms_mono(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ── pure ──────────────────────────────────────────────────────────────── */

int jf_art_path(char *out, size_t cap, const char *album_id, const char *tag, int size)
{
    if (!out || !album_id || size < 1 || size > JF_IMG_MAX_SIZE) return -1;
    char key[JF_IMG_KEY_MAX];                       /* reuse the key's id/tag validation */
    if (jf_img_key(key, sizeof key, album_id, tag, size) < 0) return -1;
    int n;
    if (tag && *tag)
        n = snprintf(out, cap, "/Items/%s/Images/Primary?maxWidth=%d&maxHeight=%d&quality=90&tag=%s",
                     album_id, size, size, tag);
    else
        n = snprintf(out, cap, "/Items/%s/Images/Primary?maxWidth=%d&maxHeight=%d&quality=90",
                     album_id, size, size);
    return (n < 0 || (size_t)n >= cap) ? -1 : n;
}

/* ── slots ─────────────────────────────────────────────────────────────── */

static bool same(const Slot *s, const char *id, const char *tag)
{
    return s->state != SL_EMPTY && strcasecmp(s->id, id) == 0 && strcmp(s->tag, tag ? tag : "") == 0;
}

static int find(JfArt *g, const char *id, const char *tag)
{
    for (int i = 0; i < g->nslots; i++)
        if (same(&g->slot[i], id, tag)) return i;
    return -1;
}

static void slot_free(Slot *s)
{
    free(s->px);
    memset(s, 0, sizeof *s);
}

static int victim(JfArt *g)
{
    int best = -1;
    for (int i = 0; i < g->nslots; i++) {
        Slot *s = &g->slot[i];
        if (s->state == SL_EMPTY) return i;
        if (s->state == SL_WANT || s->state == SL_LOADING) continue;
        if (best < 0 || s->used < g->slot[best].used) best = i;
    }
    return best;
}

bool jf_art_init_ex(JfArt *g, const char *cache_dir, int size, int mem_slots)
{
    jf_art_shutdown_ex(g);
    if (!cache_dir || strlen(cache_dir) >= sizeof g->cache.dir || size < 1 || size > JF_IMG_MAX_SIZE) return false;
    if (!jf_img_cache_open(&g->cache, cache_dir))
        LIB_LOG(LOG_LEVEL_WARN, "jf_art: cache unavailable, tiles are fetched every time");
    g->slot = calloc((size_t)(mem_slots < 1 ? 1 : mem_slots), sizeof *g->slot);
    if (!g->slot) return false;
    g->nslots = mem_slots < 1 ? 1 : mem_slots;
    g->size = size;
    LIB_LOG(LOG_LEVEL_INFO, "jf_art: cache %lld files, %lld of %lld bytes", (long long)g->cache.files, g->cache.total, g->cache.cap);
    g->cur = -1;
    http_init(&g->req);
    g->up = true;
    return true;
}

void jf_art_set_tls_ex(JfArt *g, bool tls)
{
    g->tls = tls;
}

void jf_art_set_server_ex(JfArt *g, const char *host, int port, const char *client, const char *device, const char *device_id, const char *token)
{
    if (!host || !client || !device || !device_id || !token) return;
    if (strlen(host) > JF_HOST_MAX || strlen(client) >= sizeof g->client || strlen(device) >= sizeof g->device ||
        strlen(device_id) >= sizeof g->device_id || strlen(token) >= sizeof g->token) return;
    strcpy(g->host, host); g->port = port;
    strcpy(g->client, client); strcpy(g->device, device); strcpy(g->device_id, device_id); strcpy(g->token, token);
    g->have_server = true;
}

void jf_art_request_ex(JfArt *g, const char *album_id, const char *tag)
{
    if (!g->up || !album_id) return;
    char key[JF_IMG_KEY_MAX];
    if (jf_img_key(key, sizeof key, album_id, tag, g->size) < 0) return;   /* a bad id never gets a slot */
    if (!tag) tag = "";
    int i = find(g, album_id, tag);
    if (i >= 0) {
        g->slot[i].used = ++g->clock;
        if (g->slot[i].state == SL_FAILED && g->now - g->slot[i].stamp >= JF_ART_RETRY_MS) g->slot[i].state = SL_WANT;
        return;
    }
    i = victim(g);
    if (i < 0) return;             /* every slot busy: the caller asks again next frame */
    slot_free(&g->slot[i]);
    Slot *s = &g->slot[i];
    s->state = SL_WANT;
    snprintf(s->id, sizeof s->id, "%s", album_id);
    snprintf(s->tag, sizeof s->tag, "%s", tag);
    s->used = ++g->clock;
}

const LvIcon *jf_art_get_ex(JfArt *g, const char *album_id, const char *tag)
{
    if (!g->up || !album_id) return NULL;
    int i = find(g, album_id, tag);
    if (i < 0 || g->slot[i].state != SL_READY) return NULL;
    g->slot[i].used = ++g->clock;
    return &g->slot[i].icon;
}

unsigned jf_art_generation_ex(const JfArt *g) { return g->gen; }

bool jf_art_busy_ex(const JfArt *g)
{
    for (int i = 0; i < g->nslots; i++)
        if (g->slot[i].state == SL_WANT || g->slot[i].state == SL_LOADING) return true;
    return false;
}

/* ── the poll ──────────────────────────────────────────────────────────── */

static void make_ready(JfArt *g, Slot *s)
{
    s->icon.pixels = s->px; s->icon.w = g->size; s->icon.h = g->size;
    s->state = SL_READY;
    g->gen++;
}

static void fail(Slot *s, long long now)
{
    free(s->px); s->px = NULL;
    s->state = SL_FAILED;
    s->stamp = now;
}

static void end_fetch(JfArt *g)
{
    http_end(&g->req);
    free(g->buf); g->buf = NULL;
    g->cur = -1;
}

static void start_fetch(JfArt *g, int i, long long now)
{
    Slot *s = &g->slot[i];
    char path[256], hdr[512];
    if (!g->have_server || jf_art_path(path, sizeof path, s->id, s->tag, g->size) < 0 ||
        jf_auth_header(hdr, sizeof hdr, g->client, g->device, g->device_id, g->token) < 0) {
        fail(s, now);
        return;
    }
    g->buf = malloc(JF_ART_MAX_BYTES);
    if (!g->buf) { fail(s, now); return; }
    http_init(&g->req);
    g->req.timeout_ms = 4000;
    g->req.total_timeout_ms = JF_ART_TOTAL_MS;
    http_begin(&g->req, g->host, g->port, g->tls, "GET", path, hdr, NULL, 0, HTTP_BUFFER, g->buf, JF_ART_MAX_BYTES);
    for (volatile char *p = hdr; p < hdr + sizeof hdr; p++) *p = 0;      /* the header holds the token */
    s->state = SL_LOADING;
    g->cur = i;
}

static void finish_fetch(JfArt *g, long long now)
{
    Slot *s = &g->slot[g->cur];
    bool ok = false;
    s->px = malloc((size_t)g->size * g->size * sizeof(uint32_t));
    if (s->px && http_status(&g->req) == 200 &&
        jf_img_decode_square((const uint8_t *)http_body(&g->req), http_body_len(&g->req), g->size, s->px)) {
        char key[JF_IMG_KEY_MAX];                 /* the bytes decoded, so they are worth keeping */
        if (jf_img_key(key, sizeof key, s->id, s->tag, g->size) > 0 &&
            !jf_img_cache_put(&g->cache, key, (const uint8_t *)http_body(&g->req), http_body_len(&g->req)))
            LIB_LOG(LOG_LEVEL_WARN, "jf_art: cache write failed (tile kept in memory)");
        ok = true;
    } else {
        LIB_LOG(LOG_LEVEL_INFO, "jf_art: no usable art (status %d, %zu bytes)", http_status(&g->req), http_body_len(&g->req));
    }
    end_fetch(g);
    if (ok) make_ready(g, s); else fail(s, now);
}

void jf_art_poll_ex(JfArt *g, long long now_ms)
{
    if (!g->up) return;
    g->now = now_ms;
    if (g->cur >= 0) {
        HttpPoll r = http_poll(&g->req);
        if (r == HTTP_PENDING) return;
        if (r == HTTP_DONE) { finish_fetch(g, now_ms); return; }
        LIB_LOG(LOG_LEVEL_INFO, "jf_art: fetch failed: %s", http_error(&g->req));
        Slot *s = &g->slot[g->cur];
        end_fetch(g);
        fail(s, now_ms);
        return;
    }
    int w = -1;                                          /* newest wanted entry first */
    for (int i = 0; i < g->nslots; i++)
        if (g->slot[i].state == SL_WANT && (w < 0 || g->slot[i].used > g->slot[w].used)) w = i;
    if (w < 0) return;
    Slot *s = &g->slot[w];
    char key[JF_IMG_KEY_MAX];
    if (jf_img_key(key, sizeof key, s->id, s->tag, g->size) < 0) { fail(s, now_ms); return; }
    s->px = malloc((size_t)g->size * g->size * sizeof(uint32_t));
    uint8_t *jpg = NULL;
    size_t jlen = 0;
    if (s->px && jf_img_cache_get(&g->cache, key, &jpg, &jlen)) {
        long long t0 = now_ms_mono();
        bool dec = jf_img_decode_square(jpg, jlen, g->size, s->px);
        free(jpg);
        if (dec) {
            LIB_LOG(LOG_LEVEL_INFO, "jf_art: cached tile decoded in %lld ms", now_ms_mono() - t0);
            make_ready(g, s);
            return;
        }
        jf_img_cache_drop(&g->cache, key);                 /* bytes that do not decode are corrupt: refetch */
    }
    free(s->px); s->px = NULL;
    start_fetch(g, w, now_ms);
}

void jf_art_shutdown_ex(JfArt *g)
{
    if (g->cur >= 0 || g->buf) end_fetch(g);
    for (int i = 0; i < g->nslots; i++) slot_free(&g->slot[i]);
    free(g->slot); g->slot = NULL; g->nslots = 0;
    for (volatile char *p = g->token; p < g->token + sizeof g->token; p++) *p = 0;
    memset(g->client, 0, sizeof g->client); memset(g->device, 0, sizeof g->device); memset(g->device_id, 0, sizeof g->device_id);
    g->have_server = false;
    g->up = false;
    g->cur = -1;
}

/* ── instances ─────────────────────────────────────────────────────────── */

JfArt *jf_art_create(const char *cache_dir, int box, int mem_slots)
{
    JfArt *g = calloc(1, sizeof *g);
    if (!g) return NULL;
    g->cur = -1;
    if (!jf_art_init_ex(g, cache_dir, box, mem_slots)) { free(g); return NULL; }
    return g;
}

void jf_art_destroy(JfArt *g)
{
    if (!g) return;
    jf_art_shutdown_ex(g);
    free(g);
}

/* ── the default instance: the original single-instance API ───────────── */

bool jf_art_init(const char *cache_dir, int size) { return jf_art_init_ex(&G, cache_dir, size, JF_ART_MEM_SLOTS); }
void jf_art_set_tls(bool tls) { jf_art_set_tls_ex(&G, tls); }
void jf_art_set_server(const char *host, int port, const char *client, const char *device, const char *device_id, const char *token)
{
    jf_art_set_server_ex(&G, host, port, client, device, device_id, token);
}
void jf_art_request(const char *album_id, const char *tag) { jf_art_request_ex(&G, album_id, tag); }
void jf_art_poll(long long now_ms) { jf_art_poll_ex(&G, now_ms); }
const LvIcon *jf_art_get(const char *album_id, const char *tag) { return jf_art_get_ex(&G, album_id, tag); }
bool jf_art_busy(void) { return jf_art_busy_ex(&G); }
unsigned jf_art_generation(void) { return jf_art_generation_ex(&G); }
void jf_art_shutdown(void) { jf_art_shutdown_ex(&G); }
