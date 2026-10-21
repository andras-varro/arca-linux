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

static struct {
    bool     up;
    JfImgCache cache;
    int      size;
    Slot     slot[JF_ART_MEM_SLOTS];
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
    char     device[64], device_id[64], token[128];
    bool     have_server;
} G = { .cur = -1 };

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

static int find(const char *id, const char *tag)
{
    for (int i = 0; i < JF_ART_MEM_SLOTS; i++)
        if (same(&G.slot[i], id, tag)) return i;
    return -1;
}

static void slot_free(Slot *s)
{
    free(s->px);
    memset(s, 0, sizeof *s);
}

static int victim(void)
{
    int best = -1;
    for (int i = 0; i < JF_ART_MEM_SLOTS; i++) {
        Slot *s = &G.slot[i];
        if (s->state == SL_EMPTY) return i;
        if (s->state == SL_WANT || s->state == SL_LOADING) continue;
        if (best < 0 || s->used < G.slot[best].used) best = i;
    }
    return best;
}

bool jf_art_init(const char *cache_dir, int size)
{
    jf_art_shutdown();
    if (!cache_dir || strlen(cache_dir) >= sizeof G.cache.dir || size < 1 || size > JF_IMG_MAX_SIZE) return false;
    if (!jf_img_cache_open(&G.cache, cache_dir))
        LIB_LOG(LOG_LEVEL_WARN, "jf_art: cache unavailable, tiles are fetched every time");
    G.size = size;
    LIB_LOG(LOG_LEVEL_INFO, "jf_art: cache %lld files, %lld of %lld bytes", (long long)G.cache.files, G.cache.total, G.cache.cap);
    G.cur = -1;
    http_init(&G.req);
    G.up = true;
    return true;
}

void jf_art_set_server(const char *host, int port, const char *device, const char *device_id, const char *token)
{
    if (!host || !device || !device_id || !token) return;
    if (strlen(host) > JF_HOST_MAX || strlen(device) >= sizeof G.device ||
        strlen(device_id) >= sizeof G.device_id || strlen(token) >= sizeof G.token) return;
    strcpy(G.host, host); G.port = port;
    strcpy(G.device, device); strcpy(G.device_id, device_id); strcpy(G.token, token);
    G.have_server = true;
}

void jf_art_request(const char *album_id, const char *tag)
{
    if (!G.up || !album_id) return;
    char key[JF_IMG_KEY_MAX];
    if (jf_img_key(key, sizeof key, album_id, tag, G.size) < 0) return;   /* a bad id never gets a slot */
    if (!tag) tag = "";
    int i = find(album_id, tag);
    if (i >= 0) {
        G.slot[i].used = ++G.clock;
        if (G.slot[i].state == SL_FAILED && G.now - G.slot[i].stamp >= JF_ART_RETRY_MS) G.slot[i].state = SL_WANT;
        return;
    }
    i = victim();
    if (i < 0) return;             /* every slot busy: the caller asks again next frame */
    slot_free(&G.slot[i]);
    Slot *s = &G.slot[i];
    s->state = SL_WANT;
    snprintf(s->id, sizeof s->id, "%s", album_id);
    snprintf(s->tag, sizeof s->tag, "%s", tag);
    s->used = ++G.clock;
}

const LvIcon *jf_art_get(const char *album_id, const char *tag)
{
    if (!G.up || !album_id) return NULL;
    int i = find(album_id, tag);
    if (i < 0 || G.slot[i].state != SL_READY) return NULL;
    G.slot[i].used = ++G.clock;
    return &G.slot[i].icon;
}

unsigned jf_art_generation(void) { return G.gen; }

bool jf_art_busy(void)
{
    for (int i = 0; i < JF_ART_MEM_SLOTS; i++)
        if (G.slot[i].state == SL_WANT || G.slot[i].state == SL_LOADING) return true;
    return false;
}

/* ── the poll ──────────────────────────────────────────────────────────── */

static void make_ready(Slot *s)
{
    s->icon.pixels = s->px; s->icon.w = G.size; s->icon.h = G.size;
    s->state = SL_READY;
    G.gen++;
}

static void fail(Slot *s, long long now)
{
    free(s->px); s->px = NULL;
    s->state = SL_FAILED;
    s->stamp = now;
}

static void end_fetch(void)
{
    http_end(&G.req);
    free(G.buf); G.buf = NULL;
    G.cur = -1;
}

static void start_fetch(int i, long long now)
{
    Slot *s = &G.slot[i];
    char path[256], hdr[512];
    if (!G.have_server || jf_art_path(path, sizeof path, s->id, s->tag, G.size) < 0 ||
        jf_auth_header(hdr, sizeof hdr, G.device, G.device_id, G.token) < 0) {
        fail(s, now);
        return;
    }
    G.buf = malloc(JF_ART_MAX_BYTES);
    if (!G.buf) { fail(s, now); return; }
    http_init(&G.req);
    G.req.timeout_ms = 4000;
    G.req.total_timeout_ms = JF_ART_TOTAL_MS;
    http_begin(&G.req, G.host, G.port, "GET", path, hdr, NULL, 0, HTTP_BUFFER, G.buf, JF_ART_MAX_BYTES);
    for (volatile char *p = hdr; p < hdr + sizeof hdr; p++) *p = 0;      /* the header holds the token */
    s->state = SL_LOADING;
    G.cur = i;
}

static void finish_fetch(long long now)
{
    Slot *s = &G.slot[G.cur];
    bool ok = false;
    s->px = malloc((size_t)G.size * G.size * sizeof(uint32_t));
    if (s->px && http_status(&G.req) == 200 &&
        jf_img_decode_square((const uint8_t *)http_body(&G.req), http_body_len(&G.req), G.size, s->px)) {
        char key[JF_IMG_KEY_MAX];                 /* the bytes decoded, so they are worth keeping */
        if (jf_img_key(key, sizeof key, s->id, s->tag, G.size) > 0 &&
            !jf_img_cache_put(&G.cache, key, (const uint8_t *)http_body(&G.req), http_body_len(&G.req)))
            LIB_LOG(LOG_LEVEL_WARN, "jf_art: cache write failed (tile kept in memory)");
        ok = true;
    } else {
        LIB_LOG(LOG_LEVEL_INFO, "jf_art: no usable art (status %d, %zu bytes)", http_status(&G.req), http_body_len(&G.req));
    }
    end_fetch();
    if (ok) make_ready(s); else fail(s, now);
}

void jf_art_poll(long long now_ms)
{
    if (!G.up) return;
    G.now = now_ms;
    if (G.cur >= 0) {
        HttpPoll r = http_poll(&G.req);
        if (r == HTTP_PENDING) return;
        if (r == HTTP_DONE) { finish_fetch(now_ms); return; }
        LIB_LOG(LOG_LEVEL_INFO, "jf_art: fetch failed: %s", http_error(&G.req));
        Slot *s = &G.slot[G.cur];
        end_fetch();
        fail(s, now_ms);
        return;
    }
    int w = -1;                                          /* newest wanted entry first */
    for (int i = 0; i < JF_ART_MEM_SLOTS; i++)
        if (G.slot[i].state == SL_WANT && (w < 0 || G.slot[i].used > G.slot[w].used)) w = i;
    if (w < 0) return;
    Slot *s = &G.slot[w];
    char key[JF_IMG_KEY_MAX];
    if (jf_img_key(key, sizeof key, s->id, s->tag, G.size) < 0) { fail(s, now_ms); return; }
    s->px = malloc((size_t)G.size * G.size * sizeof(uint32_t));
    uint8_t *jpg = NULL;
    size_t jlen = 0;
    if (s->px && jf_img_cache_get(&G.cache, key, &jpg, &jlen)) {
        long long t0 = now_ms_mono();
        bool dec = jf_img_decode_square(jpg, jlen, G.size, s->px);
        free(jpg);
        if (dec) {
            LIB_LOG(LOG_LEVEL_INFO, "jf_art: cached tile decoded in %lld ms", now_ms_mono() - t0);
            make_ready(s);
            return;
        }
        jf_img_cache_drop(&G.cache, key);                 /* bytes that do not decode are corrupt: refetch */
    }
    free(s->px); s->px = NULL;
    start_fetch(w, now_ms);
}

void jf_art_shutdown(void)
{
    if (G.cur >= 0 || G.buf) end_fetch();
    for (int i = 0; i < JF_ART_MEM_SLOTS; i++) slot_free(&G.slot[i]);
    for (volatile char *p = G.token; p < G.token + sizeof G.token; p++) *p = 0;
    memset(G.device, 0, sizeof G.device); memset(G.device_id, 0, sizeof G.device_id);
    G.have_server = false;
    G.up = false;
    G.cur = -1;
}
