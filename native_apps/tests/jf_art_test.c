/*
 * jf_art_test.c — host regression for common/jf_img.c (decode, geometry, key,
 * byte-budget cache) and jf_shared/jf_art.c (request/poll/get against a fork()ed
 * loopback server).  Every JPEG is generated here with stb_image_write; no album art.
 *
 * Build: gcc -I common -I jellyfin -I jf_shared -I tests tests/jf_art_test.c common/jf_img.c
 *        common/http.c jf_shared/jf_art.c jf_shared/jf_url.c -lssl -lcrypto -o jf_art_test
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <utime.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include "stb_image_write.h"

#include "jf_art.h"
#include "jf_img.h"

#define WR(...) do { if (write(__VA_ARGS__) < 0) { } } while (0)
static int g_fail = 0, g_checks = 0;
#define CHECK(cond) do { g_checks++; if (!(cond)) { g_fail++; \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static const char *ID  = "0123456789abcdef0123456789abcdef";
static const char *TAG = "fedcba9876543210fedcba9876543210";

/* ── JPEG fixtures ─────────────────────────────────────────────────────── */

typedef struct { uint8_t *p; size_t n, cap; } Buf;
static void sink(void *ctx, void *data, int n)
{
    Buf *b = ctx;
    if (b->n + (size_t)n > b->cap) { b->cap = (b->n + (size_t)n) * 2; b->p = realloc(b->p, b->cap); }
    memcpy(b->p + b->n, data, (size_t)n);
    b->n += (size_t)n;
}
/* mode 0: R = x gradient, G = y gradient, B = 128;  mode 1: solid (200,100,50) */
static Buf make_jpeg(int w, int h, int mode)
{
    uint8_t *rgb = malloc((size_t)w * h * 3);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint8_t *p = rgb + ((size_t)y * w + x) * 3;
            if (mode == 0) { p[0] = (uint8_t)(x * 255 / (w > 1 ? w - 1 : 1)); p[1] = (uint8_t)(y * 255 / (h > 1 ? h - 1 : 1)); p[2] = 128; }
            else { p[0] = 200; p[1] = 100; p[2] = 50; }
        }
    Buf b = {0};
    stbi_write_jpg_to_func(sink, &b, w, h, 3, rgb, 90);
    free(rgb);
    return b;
}
static long long mono_ms(void)
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
static int near(uint32_t px, int r, int g, int b, int tol)
{
    int pr = (px >> 16) & 255, pg = (px >> 8) & 255, pb = px & 255;
    return abs(pr - r) <= tol && abs(pg - g) <= tol && abs(pb - b) <= tol;
}

/* ── geometry ──────────────────────────────────────────────────────────── */

static void test_fit(void)
{
    int dw, dh, ox, oy;
    CHECK(jf_img_fit(198, 200, 160, &dw, &dh, &ox, &oy) && dw == 158 && dh == 160 && ox == 1 && oy == 0);
    CHECK(jf_img_fit(200, 150, 160, &dw, &dh, &ox, &oy) && dw == 160 && dh == 120 && ox == 0 && oy == 20);
    CHECK(jf_img_fit(1, 1, 160, &dw, &dh, &ox, &oy) && dw == 160 && dh == 160 && ox == 0 && oy == 0);
    CHECK(jf_img_fit(1, 1000, 160, &dw, &dh, &ox, &oy) && dw == 1 && dh == 160 && ox == 79 && oy == 0);
    CHECK(jf_img_fit(400, 400, 160, &dw, &dh, &ox, &oy) && dw == 160 && dh == 160);
    CHECK(!jf_img_fit(0, 10, 160, &dw, &dh, &ox, &oy));
    CHECK(!jf_img_fit(10, -1, 160, &dw, &dh, &ox, &oy));
    CHECK(!jf_img_fit(10, 10, 0, &dw, &dh, &ox, &oy));
    CHECK(!jf_img_fit(10, 10, JF_IMG_MAX_SIZE + 1, &dw, &dh, &ox, &oy));
}

/* ── decode ────────────────────────────────────────────────────────────── */

static void test_decode(void)
{
    enum { S = 64 };
    uint32_t out[S * S];

    Buf j = make_jpeg(200, 150, 0);                    /* landscape: 64x48 at oy 8 */
    memset(out, 0, sizeof out);
    CHECK(jf_img_decode_square(j.p, j.n, S, out));
    CHECK(out[0] == 0xFF000000u && out[7 * S + 30] == 0xFF000000u);             /* top bar */
    CHECK(out[56 * S + 30] == 0xFF000000u && out[63 * S + 63] == 0xFF000000u);  /* bottom bar */
    CHECK(near(out[8 * S + 0], 0, 0, 128, 12));                                  /* content top-left */
    CHECK(near(out[55 * S + 63], 255, 255, 128, 12));                            /* content bottom-right */
    CHECK(near(out[32 * S + 32], 128, 128, 128, 14));                            /* middle of the gradient */
    free(j.p);

    j = make_jpeg(198, 200, 0);                        /* portrait-ish: a 1 px bar on the right */
    CHECK(jf_img_decode_square(j.p, j.n, S, out));
    CHECK(out[32 * S + 63] == 0xFF000000u && !near(out[32 * S + 62], 0, 0, 0, 20));
    free(j.p);

    j = make_jpeg(40, 40, 1);                          /* upscale of a solid colour stays that colour */
    CHECK(jf_img_decode_square(j.p, j.n, S, out));
    int bad = 0;
    for (int i = 0; i < S * S; i++) if (!near(out[i], 200, 100, 50, 8)) bad++;
    CHECK(bad == 0);
    free(j.p);

    j = make_jpeg(1, 1, 1);                            /* 1x1 fills the square */
    CHECK(jf_img_decode_square(j.p, j.n, S, out) && near(out[0], 200, 100, 50, 10) && near(out[S * S - 1], 200, 100, 50, 10));
    free(j.p);

    /* refusals leave the buffer alone */
    memset(out, 0x5A, sizeof out);
    uint8_t junk[64]; memset(junk, 0xAB, sizeof junk);
    CHECK(!jf_img_decode_square(junk, sizeof junk, S, out) && out[0] == 0x5A5A5A5Au);
    CHECK(!jf_img_decode_square(junk, 0, S, out));
    CHECK(!jf_img_decode_square(NULL, 10, S, out));
    j = make_jpeg(8, 8, 1);
    CHECK(!jf_img_decode_square(j.p, j.n, 0, out));
    CHECK(!jf_img_decode_square(j.p, j.n, JF_IMG_MAX_SIZE + 1, out));
    CHECK(!jf_img_decode_square(j.p, 10, S, out));      /* cut inside the header */
    free(j.p);
    j = make_jpeg(JF_IMG_MAX_SRC_DIM + 8, 8, 1);        /* over the dimension cap */
    CHECK(!jf_img_decode_square(j.p, j.n, S, out) && out[0] == 0x5A5A5A5Au);
    free(j.p);
}

/* ── key ───────────────────────────────────────────────────────────────── */

static void test_key(void)
{
    char k[JF_IMG_KEY_MAX];
    char want[128];
    snprintf(want, sizeof want, "%s_%s_160.jpg", ID, TAG);
    CHECK(jf_img_key(k, sizeof k, ID, TAG, 160) == (int)strlen(want) && strcmp(k, want) == 0);
    snprintf(want, sizeof want, "%s_0_160.jpg", ID);
    CHECK(jf_img_key(k, sizeof k, ID, "", 160) > 0 && strcmp(k, want) == 0);
    CHECK(jf_img_key(k, sizeof k, ID, NULL, 160) > 0 && strcmp(k, want) == 0);
    CHECK(jf_img_key(k, sizeof k, "0123456789ABCDEF0123456789ABCDEF", "AB", 64) > 0 &&
          strcmp(k, "0123456789abcdef0123456789abcdef_ab_64.jpg") == 0);          /* case folded */
    CHECK(jf_img_key(k, sizeof k, "../../etc/passwd", TAG, 160) < 0);
    CHECK(jf_img_key(k, sizeof k, "0123456789abcdef0123456789abcde", TAG, 160) < 0);   /* 31 */
    CHECK(jf_img_key(k, sizeof k, "0123456789abcdef0123456789abcdef0", TAG, 160) < 0); /* 33 */
    CHECK(jf_img_key(k, sizeof k, "0123456789abcdef0123456789abcdeg", TAG, 160) < 0);  /* non-hex */
    CHECK(jf_img_key(k, sizeof k, ID, "ab/cd", 160) < 0);
    CHECK(jf_img_key(k, sizeof k, ID, "ab cd", 160) < 0);
    CHECK(jf_img_key(k, sizeof k, ID, "..", 160) < 0);
    CHECK(jf_img_key(k, sizeof k, ID, "0123456789012345678901234567890123456789", 160) > 0);   /* 40 */
    CHECK(jf_img_key(k, sizeof k, ID, "01234567890123456789012345678901234567890", 160) < 0);  /* 41 */
    CHECK(jf_img_key(k, 20, ID, TAG, 160) < 0);                                      /* no room */
    CHECK(jf_img_key(k, sizeof k, ID, TAG, 0) < 0 && jf_img_key(k, sizeof k, ID, TAG, JF_IMG_MAX_SIZE + 1) < 0);
    CHECK(jf_img_key(k, sizeof k, NULL, TAG, 160) < 0);
}

/* ── budget and eviction ───────────────────────────────────────────────── */

static void test_evict(void)
{
    const long long MB = 1024 * 1024;
    CHECK(jf_img_cap_bytes(100 * MB) == 25 * MB);                       /* a quarter of the free space */
    CHECK(jf_img_cap_bytes(10000 * MB) == 150 * MB);                    /* the ceiling */
    CHECK(jf_img_cap_bytes(600 * MB) == 150 * MB && jf_img_cap_bytes(600 * MB - 4) < 150 * MB);
    CHECK(jf_img_cap_bytes(0) == 0 && jf_img_cap_bytes(-5) == 0);

    time_t t[5] = { 50, 10, 30, 20, 40 };
    long long s[5] = { 100, 100, 100, 100, 100 };
    int out[8];
    int n = jf_img_evict_bytes(t, s, 5, 500, 300, out);
    CHECK(n == 2 && out[0] == 1 && out[1] == 3);                         /* oldest first: t=10, t=20 */
    CHECK(jf_img_evict_bytes(t, s, 5, 500, 500, out) == 0);              /* exactly at the cap: nothing */
    CHECK(jf_img_evict_bytes(t, s, 5, 500, 900, out) == 0);
    CHECK(jf_img_evict_bytes(t, s, 0, 0, 0, out) == 0);
    n = jf_img_evict_bytes(t, s, 5, 500, 0, out);
    CHECK(n == 5 && out[0] == 1 && out[4] == 0);                         /* all, oldest first */
    long long v[5] = { 10, 400, 10, 10, 10 };                            /* sizes count, not file counts */
    n = jf_img_evict_bytes(t, v, 5, 440, 100, out);
    CHECK(n == 1 && out[0] == 1);                                        /* t=10 (400 bytes) alone brings 440 under 100 */
    time_t same[4] = { 7, 7, 7, 7 };
    long long s4[4] = { 5, 5, 5, 5 };
    n = jf_img_evict_bytes(same, s4, 4, 20, 10, out);
    CHECK(n == 2 && out[0] == 0 && out[1] == 1);                         /* ties: lower index first */
    CHECK(jf_img_evict_bytes(t, s, 5, 500, -1, out) == 0);
}

/* ── cache on disk ─────────────────────────────────────────────────────── */

static char g_root[256];

static long file_size(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0 ? (long)st.st_size : -1;
}
static int count_files(const char *dir, const char *prefix)
{
    char cmd[400];
    snprintf(cmd, sizeof cmd, "ls -A \"%s\" 2>/dev/null | grep -c \"^%s\"", dir, prefix);
    FILE *f = popen(cmd, "r");
    int n = -1;
    if (f) { if (fscanf(f, "%d", &n) != 1) n = 0; pclose(f); }
    return n;
}

static void set_mtime(const char *dir, const char *key, time_t t)
{
    char path[400];
    snprintf(path, sizeof path, "%s/%s", dir, key);
    struct utimbuf ub = { t, t };
    utime(path, &ub);
}

static void test_cache(void)
{
    char dir[300], path[400], key[JF_IMG_KEY_MAX];
    snprintf(dir, sizeof dir, "%s/cache/nested", g_root);       /* open must create it */
    uint8_t blob[1000];
    for (int i = 0; i < (int)sizeof blob; i++) blob[i] = (uint8_t)(i * 7 + 3);
    JfImgCache c;
    uint8_t *got = NULL;
    size_t glen = 0;

    CHECK(jf_img_key(key, sizeof key, ID, TAG, 32) > 0);
    CHECK(jf_img_cache_open(&c, dir) && c.open && c.total == 0 && c.files == 0);
    CHECK(c.cap > 0 && c.cap <= JF_IMG_CACHE_MAX_BYTES);
    CHECK(!jf_img_cache_get(&c, key, &got, &glen));                       /* miss */
    CHECK(jf_img_cache_put(&c, key, blob, sizeof blob));
    snprintf(path, sizeof path, "%s/%s", dir, key);
    CHECK(file_size(path) == (long)sizeof blob);
    CHECK(c.total == (long long)sizeof blob && c.files == 1);
    CHECK(count_files(dir, ".tmp") == 0);                                 /* atomic: no temp left */
    CHECK(jf_img_cache_get(&c, key, &got, &glen) && glen == sizeof blob && memcmp(got, blob, glen) == 0);
    free(got);

    /* an overwrite replaces, it does not add */
    CHECK(jf_img_cache_put(&c, key, blob, 400));
    CHECK(c.total == 400 && c.files == 1 && file_size(path) == 400);
    CHECK(jf_img_cache_put(&c, key, blob, sizeof blob));

    /* a hit refreshes the mtime */
    set_mtime(dir, key, 1000);
    CHECK(jf_img_cache_get(&c, key, &got, &glen)); free(got);
    struct stat st;
    CHECK(stat(path, &st) == 0 && st.st_mtime > 1000000);

    /* a drop takes the file and its size off the total */
    jf_img_cache_drop(&c, key);
    CHECK(file_size(path) < 0 && c.total == 0 && c.files == 0);
    jf_img_cache_drop(&c, key);                                           /* absent: no double count */
    CHECK(c.total == 0 && c.files == 0);

    /* an empty or oversized file is dropped as a miss */
    FILE *f = fopen(path, "wb"); fclose(f);
    c.files++;
    CHECK(!jf_img_cache_get(&c, key, &got, &glen) && file_size(path) < 0);
    f = fopen(path, "wb");
    for (int i = 0; i < JF_IMG_FILE_MAX / 100 + 2; i++) { char z[100]; memset(z, 1, sizeof z); fwrite(z, 1, sizeof z, f); }
    fclose(f);
    c.total += file_size(path); c.files++;
    CHECK(!jf_img_cache_get(&c, key, &got, &glen) && file_size(path) < 0 && c.total == 0);

    /* names that are not ours never touch the disk */
    CHECK(!jf_img_cache_put(&c, "../escape.jpg", blob, 10));
    CHECK(!jf_img_cache_put(&c, "UPPER.jpg", blob, 10));
    CHECK(!jf_img_cache_put(&c, "ab.txt", blob, 10));
    CHECK(!jf_img_cache_put(&c, "ab.ppm", blob, 10));
    snprintf(path, sizeof path, "%s/cache/escape.jpg", g_root);
    CHECK(file_size(path) < 0);
    CHECK(!jf_img_cache_get(&c, "../escape.jpg", &got, &glen));
    CHECK(!jf_img_cache_put(&c, key, blob, 0));                           /* empty */

    /* trimming by BYTES: cap 4500 with 1000-byte files, the oldest go */
    time_t now = time(NULL);
    c.cap = 4500;
    char names[6][JF_IMG_KEY_MAX];
    for (int i = 0; i < 4; i++) {
        char id[40]; snprintf(id, sizeof id, "%032x", i + 1);
        CHECK(jf_img_key(names[i], sizeof names[i], id, "aa", 32) > 0);
        CHECK(jf_img_cache_put(&c, names[i], blob, sizeof blob));
        set_mtime(dir, names[i], now + (i == 2 ? 50000 : 100000 + i));   /* all newer than the put below; #2 is the oldest */
    }
    CHECK(c.total == 4000 && c.files == 4);
    snprintf(path, sizeof path, "%s/notes.txt", dir);
    f = fopen(path, "wb"); fputs("keep", f); fclose(f);
    snprintf(path, sizeof path, "%s/.tmp_99999", dir);
    f = fopen(path, "wb"); fputs("crash", f); fclose(f);
    CHECK(jf_img_cache_put(&c, key, blob, sizeof blob));                  /* 5000 > 4500: one must go, and not the new (oldest-looking) file */
    CHECK(c.total == 4000 && c.files == 4);
    snprintf(path, sizeof path, "%s/%s", dir, names[2]);
    CHECK(file_size(path) < 0);                                           /* the oldest went */
    snprintf(path, sizeof path, "%s/%s", dir, names[0]);
    CHECK(file_size(path) > 0);                                           /* only one had to go */
    snprintf(path, sizeof path, "%s/%s", dir, names[3]);
    CHECK(file_size(path) > 0);                                           /* a newest stayed */
    snprintf(path, sizeof path, "%s/%s", dir, key);
    CHECK(file_size(path) > 0);                                           /* the one just written stayed */
    snprintf(path, sizeof path, "%s/notes.txt", dir);
    CHECK(file_size(path) == 4);
    CHECK(count_files(dir, "[0-9a-f_]*\\.jpg") == 4);
    CHECK(!jf_img_cache_put(&c, key, blob, 5000));                        /* larger than the whole budget */
    c.cap = JF_IMG_CACHE_MAX_BYTES;

    /* open sums what is there with one scan, and removes crashed temporaries */
    JfImgCache c2;
    CHECK(jf_img_cache_open(&c2, dir) && c2.files == 4 && c2.total == 4000);
    CHECK(count_files(dir, ".tmp") == 0);
    CHECK(!jf_img_cache_open(&c2, ""));                                   /* no directory name */
}

/* ── glue against a loopback server ────────────────────────────────────── */

enum { SRV_OK = 0, SRV_404, SRV_BIG, SRV_SLOW };

static int srv_listen(int *port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0), one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    if (bind(fd, (struct sockaddr *)&a, sizeof a) != 0 || listen(fd, 4) != 0) return -1;
    socklen_t l = sizeof a;
    getsockname(fd, (struct sockaddr *)&a, &l);
    *port = ntohs(a.sin_port);
    return fd;
}

static void reap(pid_t pid) { kill(pid, SIGKILL); waitpid(pid, NULL, 0); }

static pid_t srv_fork(int lfd, int mode, const Buf *jpeg, int size)
{
    pid_t pid = fork();
    if (pid != 0) return pid;
    int c = accept(lfd, NULL, NULL);
    char req[4096]; size_t n = 0;
    while (n < sizeof req - 1) {
        ssize_t r = read(c, req + n, sizeof req - 1 - n);
        if (r <= 0) break;
        n += (size_t)r; req[n] = 0;
        if (strstr(req, "\r\n\r\n")) break;
    }
    char want[256];
    snprintf(want, sizeof want, "GET /Items/%s/Images/Primary?maxWidth=%d&maxHeight=%d&quality=90&tag=%s HTTP/1.0\r\n",
             ID, size, size, TAG);
    int good = strncmp(req, want, strlen(want)) == 0 && strstr(req, "Authorization: MediaBrowser") &&
               strstr(req, "Token=\"TESTTOKEN\"");
    char head[200];
    if (!good || mode == SRV_404) {
        const char *r = good ? "HTTP/1.0 404 Not Found\r\n\r\n" : "HTTP/1.0 400 Bad\r\nContent-Length: 0\r\n\r\n";
        WR(c, r, strlen(r));
        if (good) WR(c, jpeg->p, jpeg->n);      /* a 404 that still carries a decodable image */
    } else if (mode == SRV_BIG) {
        size_t big = JF_ART_MAX_BYTES + 4096;
        snprintf(head, sizeof head, "HTTP/1.0 200 OK\r\nContent-Length: %zu\r\n\r\n", big);
        WR(c, head, strlen(head));
        char z[4096]; memset(z, 0xFF, sizeof z);
        for (size_t o = 0; o < big; o += sizeof z) if (write(c, z, sizeof z) <= 0) break;
    } else {
        if (mode == SRV_SLOW) usleep(300 * 1000);
        snprintf(head, sizeof head, "HTTP/1.0 200 OK\r\nContent-Type: image/jpeg\r\nContent-Length: %zu\r\n\r\n", jpeg->n);
        WR(c, head, strlen(head));
        WR(c, jpeg->p, jpeg->n);
    }
    close(c);
    _exit(0);
}

/* poll until `get` yields a tile or the request settles; returns max single-poll ms */
static long long run_until_settled(const char *id, const char *tag, int *ready, long long *max_poll)
{
    long long t0 = mono_ms(), worst = 0;
    *ready = 0;
    while (mono_ms() - t0 < 6000) {
        long long a = mono_ms();
        jf_art_poll(a);
        long long d = mono_ms() - a;
        if (d > worst) worst = d;
        if (jf_art_get(id, tag)) { *ready = 1; break; }
        if (!jf_art_busy()) break;
        usleep(1000);
    }
    *max_poll = worst;
    return mono_ms() - t0;
}

static void test_glue(void)
{
    enum { S = 64 };
    char dir[300];
    snprintf(dir, sizeof dir, "%s/glue", g_root);
    Buf jpeg = make_jpeg(60, 64, 0);
    int port, ready;
    long long worst;
    signal(SIGPIPE, SIG_IGN);

    char path[300];
    CHECK(jf_art_path(path, sizeof path, ID, TAG, 160) > 0 &&
          strcmp(path, "/Items/0123456789abcdef0123456789abcdef/Images/Primary?maxWidth=160&maxHeight=160&quality=90&tag=fedcba9876543210fedcba9876543210") == 0);
    CHECK(jf_art_path(path, sizeof path, ID, "", 160) > 0 && !strstr(path, "tag="));
    CHECK(jf_art_path(path, sizeof path, "../x", TAG, 160) < 0);
    CHECK(jf_art_path(path, sizeof path, ID, "a&b=c", 160) < 0);
    CHECK(jf_art_path(path, 30, ID, TAG, 160) < 0);

    CHECK(jf_art_init(dir, S));
    CHECK(jf_art_get(ID, TAG) == NULL);
    jf_art_request("not-an-id", TAG);                         /* a bad id never makes work */
    CHECK(!jf_art_busy());

    /* 1. cold: network, decode, cache file appears */
    int lfd = srv_listen(&port);
    CHECK(lfd >= 0);
    pid_t pid = srv_fork(lfd, SRV_OK, &jpeg, S);
    jf_art_set_server("127.0.0.1", port, "Ludfin", "arca", "dev01", "TESTTOKEN");
    jf_art_request(ID, TAG);
    CHECK(jf_art_busy());
    run_until_settled(ID, TAG, &ready, &worst);
    reap(pid);
    const LvIcon *ic = jf_art_get(ID, TAG);
    CHECK(ready && ic && ic->w == S && ic->h == S && ic->pixels);
    CHECK(ic && near(ic->pixels[32 * S + 5], 20, 128, 128, 40));
    CHECK(!jf_art_busy());
    char key[JF_IMG_KEY_MAX], f[400];
    jf_img_key(key, sizeof key, ID, TAG, S);
    snprintf(f, sizeof f, "%s/%s", dir, key);
    CHECK(file_size(f) == (long)jpeg.n);                      /* the cache holds the server bytes, not a decode */
    close(lfd);

    /* 2. warm: a fresh module state, the server gone: served from the disk cache in one poll */
    jf_art_shutdown();
    CHECK(jf_art_init(dir, S));
    jf_art_set_server("127.0.0.1", 1, "Ludfin", "arca", "dev01", "TESTTOKEN");     /* nothing listens */
    jf_art_request(ID, TAG);
    jf_art_poll(mono_ms());
    CHECK(jf_art_get(ID, TAG) != NULL && !jf_art_busy());

    /* 3. corrupt cache file: dropped and refetched */
    jf_art_shutdown();
    CHECK(truncate(f, 100) == 0);
    CHECK(jf_art_init(dir, S));
    lfd = srv_listen(&port);
    pid = srv_fork(lfd, SRV_OK, &jpeg, S);
    jf_art_set_server("127.0.0.1", port, "Ludfin", "arca", "dev01", "TESTTOKEN");
    jf_art_request(ID, TAG);
    run_until_settled(ID, TAG, &ready, &worst);
    reap(pid); close(lfd);
    CHECK(ready && file_size(f) > 1000);

    /* 3b. bytes that do not decode are dropped even when the refetch then fails */
    jf_art_shutdown();
    CHECK(truncate(f, 100) == 0);
    CHECK(jf_art_init(dir, S));
    lfd = srv_listen(&port);
    pid = srv_fork(lfd, SRV_404, &jpeg, S);
    jf_art_set_server("127.0.0.1", port, "Ludfin", "arca", "dev01", "TESTTOKEN");
    jf_art_request(ID, TAG);
    run_until_settled(ID, TAG, &ready, &worst);
    reap(pid); close(lfd);
    CHECK(!ready && file_size(f) < 0);

    /* 4. a 404 settles to FAILED, not retried at once */
    jf_art_shutdown();
    unlink(f);
    CHECK(jf_art_init(dir, S));
    lfd = srv_listen(&port);
    pid = srv_fork(lfd, SRV_404, &jpeg, S);
    jf_art_set_server("127.0.0.1", port, "Ludfin", "arca", "dev01", "TESTTOKEN");
    jf_art_request(ID, TAG);
    run_until_settled(ID, TAG, &ready, &worst);
    reap(pid); close(lfd);
    CHECK(!ready && !jf_art_busy() && jf_art_get(ID, TAG) == NULL && file_size(f) < 0);
    jf_art_request(ID, TAG);
    CHECK(!jf_art_busy());                                    /* inside the retry window */

    /* 5. a reply over the size cap is refused */
    jf_art_shutdown(); unlink(f);
    CHECK(jf_art_init(dir, S));
    lfd = srv_listen(&port);
    pid = srv_fork(lfd, SRV_BIG, &jpeg, S);
    jf_art_set_server("127.0.0.1", port, "Ludfin", "arca", "dev01", "TESTTOKEN");
    jf_art_request(ID, TAG);
    run_until_settled(ID, TAG, &ready, &worst);
    reap(pid); close(lfd);
    CHECK(!ready && !jf_art_busy() && file_size(f) < 0);

    /* 6. a slow server: every poll returns promptly while waiting */
    jf_art_shutdown(); unlink(f);
    CHECK(jf_art_init(dir, S));
    lfd = srv_listen(&port);
    pid = srv_fork(lfd, SRV_SLOW, &jpeg, S);
    jf_art_set_server("127.0.0.1", port, "Ludfin", "arca", "dev01", "TESTTOKEN");
    jf_art_request(ID, TAG);
    long long took = run_until_settled(ID, TAG, &ready, &worst);
    reap(pid); close(lfd);
    CHECK(ready && took >= 250);                              /* it really waited ... */
    CHECK(worst < 40);                                        /* ... and no poll did */

    /* 7. no server configured: FAILED, no crash */
    jf_art_shutdown();
    unlink(f);
    CHECK(jf_art_init(dir, S));
    jf_art_request(ID, TAG);
    run_until_settled(ID, TAG, &ready, &worst);
    CHECK(!ready && !jf_art_busy());
    jf_art_shutdown();
    free(jpeg.p);
}

/* Two instances side by side: own box, own slot count, own state; the default instance is unaffected. */
static void test_instances(void)
{
    char dir[300];
    snprintf(dir, sizeof dir, "%s/two", g_root);
    const char *id1 = "0123456789abcdef0123456789abcdef", *id2 = "fedcba9876543210fedcba9876543210";
    CHECK(jf_art_create(dir, 0, 4) == NULL);                  /* a bad box */
    CHECK(jf_art_create(NULL, 64, 4) == NULL);
    JfArt *thumb = jf_art_create(dir, 64, 3);
    JfArt *poster = jf_art_create(dir, 200, 1);
    CHECK(thumb && poster);
    if (!thumb || !poster) { jf_art_destroy(thumb); jf_art_destroy(poster); return; }
    CHECK(!jf_art_busy_ex(thumb) && !jf_art_busy_ex(poster) && !jf_art_busy());
    jf_art_request_ex(thumb, id1, "ab");
    CHECK(jf_art_busy_ex(thumb) && !jf_art_busy_ex(poster) && !jf_art_busy());     /* a request lands in its own instance only */
    jf_art_request_ex(poster, id1, "ab");
    jf_art_request_ex(poster, id2, "ab");                      /* one slot, already wanted: the second waits for the caller's next ask */
    CHECK(jf_art_busy_ex(poster));
    CHECK(jf_art_get_ex(thumb, id1, "ab") == NULL && jf_art_get_ex(poster, id1, "ab") == NULL);   /* nothing ready yet */
    for (long long t = 1000; t < 1000 + 4 * JF_ART_TOTAL_MS; t += 500) {    /* no server set: every want fails, none hangs */
        jf_art_poll_ex(thumb, t); jf_art_poll_ex(poster, t);
    }
    CHECK(!jf_art_busy_ex(thumb) && !jf_art_busy_ex(poster));
    CHECK(jf_art_generation_ex(thumb) == 0 && jf_art_generation_ex(poster) == 0 && jf_art_generation() == jf_art_generation());
    jf_art_destroy(thumb);
    CHECK(!jf_art_busy_ex(poster));                            /* destroying one leaves the other usable */
    jf_art_request_ex(poster, id2, "ab");
    CHECK(jf_art_busy_ex(poster));
    jf_art_destroy(poster);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    snprintf(g_root, sizeof g_root, "/tmp/jf_art_test_XXXXXX");
    if (!mkdtemp(g_root)) { perror("mkdtemp"); return 2; }

    test_fit(); test_decode(); test_key(); test_evict(); test_cache(); test_glue(); test_instances();

    char cmd[300];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", g_root);
    if (system(cmd) != 0) printf("note: cleanup of %s failed\n", g_root);
    printf("jf_art_test: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
