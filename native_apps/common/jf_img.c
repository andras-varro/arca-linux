/*
 * jf_img.c — JPEG -> square XRGB8888 tile, plus the on-disk cache of the server's JPEG bytes.  See jf_img.h.
 *
 * Decoder: the vendored stb_image.h v2.30 (public domain / MIT, Sean Barrett),
 * upstream https://github.com/nothings/stb, file stb_image.h last changed in commit
 * 013ac3beddff3dbffafd5177e7972067cd2b5083 (2024-05-31).  Kept byte-identical to
 * upstream.  Configured for this device: JPEG only (baseline and progressive), memory
 * input only, no SIMD path (no NEON assumption), no HDR / linear float, no
 * thread-local state, no failure strings.
 */
#define LOGGER_LIB_CLIENT
#include "logger.h"
#include "jf_img.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include <utime.h>

#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_NO_SIMD
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_NO_THREAD_LOCALS
#define STBI_NO_FAILURE_STRINGS
#define STB_IMAGE_IMPLEMENTATION
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
#include "stb_image.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

/* ── geometry ──────────────────────────────────────────────────────────── */

bool jf_img_fit(int sw, int sh, int size, int *dw, int *dh, int *ox, int *oy)
{
    if (sw <= 0 || sh <= 0 || size < 1 || size > JF_IMG_MAX_SIZE) return false;
    long w, h;
    /* the longer side takes the whole square; the other follows the aspect, rounded */
    if (sw >= sh) { w = size; h = ((long)sh * size + sw / 2) / sw; }
    else          { h = size; w = ((long)sw * size + sh / 2) / sh; }
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    *dw = (int)w; *dh = (int)h;
    *ox = (size - (int)w) / 2;
    *oy = (size - (int)h) / 2;
    return true;
}

/* Bilinear with 16.16 positions.  Every divide is outside the pixel loops: the
 * two steps are computed once, and the per-column table is built once per call. */
void jf_img_scale_rgb(const uint8_t *rgb, int sw, int sh, int size, uint32_t *out)
{
    int dw, dh, ox, oy;
    for (int i = 0; i < size * size; i++) out[i] = 0xFF000000u;
    if (!jf_img_fit(sw, sh, size, &dw, &dh, &ox, &oy)) return;

    int      x0[JF_IMG_MAX_SIZE], fx[JF_IMG_MAX_SIZE];
    int32_t  stepx = (sw << 16) / dw;
    int32_t  stepy = (sh << 16) / dh;
    int32_t  maxx = (sw - 1) << 16, maxy = (sh - 1) << 16;

    for (int x = 0; x < dw; x++) {
        int32_t p = x * stepx + (stepx >> 1) - 32768;     /* pixel-centre mapping */
        if (p < 0) p = 0;
        if (p > maxx) p = maxx;
        x0[x] = p >> 16;
        fx[x] = (p >> 8) & 255;
    }
    for (int y = 0; y < dh; y++) {
        int32_t p = y * stepy + (stepy >> 1) - 32768;
        if (p < 0) p = 0;
        if (p > maxy) p = maxy;
        int y0 = p >> 16, fy = (p >> 8) & 255;
        int y1 = y0 + 1 < sh ? y0 + 1 : y0;
        const uint8_t *r0 = rgb + (size_t)y0 * sw * 3;
        const uint8_t *r1 = rgb + (size_t)y1 * sw * 3;
        uint32_t *dst = out + (size_t)(oy + y) * size + ox;
        for (int x = 0; x < dw; x++) {
            int xa = x0[x] * 3, xb = (x0[x] + 1 < sw ? x0[x] + 1 : x0[x]) * 3, f = fx[x];
            uint32_t c[3];
            for (int k = 0; k < 3; k++) {
                int top = (r0[xa + k] * (256 - f) + r0[xb + k] * f) >> 8;
                int bot = (r1[xa + k] * (256 - f) + r1[xb + k] * f) >> 8;
                c[k] = (uint32_t)((top * (256 - fy) + bot * fy) >> 8);
            }
            dst[x] = 0xFF000000u | (c[0] << 16) | (c[1] << 8) | c[2];
        }
    }
}

bool jf_img_decode_square(const uint8_t *data, size_t len, int size, uint32_t *out)
{
    if (!data || !out || len == 0 || len > 0x7FFFFFFF || size < 1 || size > JF_IMG_MAX_SIZE)
        return false;
    int w = 0, h = 0, comp = 0;
    if (!stbi_info_from_memory(data, (int)len, &w, &h, &comp)) return false;
    if (w < 1 || h < 1 || w > JF_IMG_MAX_SRC_DIM || h > JF_IMG_MAX_SRC_DIM) return false;
    unsigned char *rgb = stbi_load_from_memory(data, (int)len, &w, &h, &comp, 3);
    if (!rgb) return false;
    jf_img_scale_rgb(rgb, w, h, size, out);
    stbi_image_free(rgb);
    return true;
}

/* ── key ───────────────────────────────────────────────────────────────── */

static bool hex_run(const char *s, size_t min, size_t max)
{
    size_t n = strlen(s);
    if (n < min || n > max) return false;
    for (size_t i = 0; i < n; i++)
        if (!isxdigit((unsigned char)s[i])) return false;
    return true;
}

int jf_img_key(char *out, size_t cap, const char *album_id, const char *tag, int size)
{
    if (!out || !album_id || size < 1 || size > JF_IMG_MAX_SIZE) return -1;
    if (!hex_run(album_id, 32, 32)) return -1;
    if (!tag || !*tag) tag = "0";
    if (!hex_run(tag, 1, JF_IMG_TAG_MAX)) return -1;
    int n = snprintf(out, cap, "%s_%s_%d.jpg", album_id, tag, size);
    if (n < 0 || (size_t)n >= cap) return -1;
    for (int i = 0; i < n; i++) out[i] = (char)tolower((unsigned char)out[i]);
    return n;
}

/* a file name this module could have written: [0-9a-f_]+ ".jpg", no separator */
static bool key_name_ok(const char *k)
{
    size_t n = strlen(k);
    if (n < 5 || n >= JF_IMG_KEY_MAX || strcmp(k + n - 4, ".jpg") != 0) return false;
    for (size_t i = 0; i < n - 4; i++)
        if (!((k[i] >= '0' && k[i] <= '9') || (k[i] >= 'a' && k[i] <= 'f') || k[i] == '_'))
            return false;
    return true;
}

/* ── budget and eviction ───────────────────────────────────────────────── */

long long jf_img_cap_bytes(long long free_bytes)
{
    if (free_bytes < 0) return 0;
    long long q = free_bytes / JF_IMG_CACHE_FREE_DIV;   /* constant power of two: shifts, no divide instruction */
    return q < JF_IMG_CACHE_MAX_BYTES ? q : JF_IMG_CACHE_MAX_BYTES;
}

typedef struct { time_t m; long long s; int i; } EvRec;

static int ev_cmp(const void *a, const void *b)
{
    const EvRec *x = a, *y = b;
    if (x->m != y->m) return x->m < y->m ? -1 : 1;
    return x->i < y->i ? -1 : (x->i > y->i);
}

int jf_img_evict_bytes(const time_t *mtime, const long long *size, int n, long long total, long long cap, int *out)
{
    if (n <= 0 || cap < 0 || total <= cap) return 0;
    EvRec *r = malloc((size_t)n * sizeof *r);
    if (!r) return 0;
    for (int i = 0; i < n; i++) { r[i].m = mtime[i]; r[i].s = size[i]; r[i].i = i; }
    qsort(r, (size_t)n, sizeof *r, ev_cmp);
    int k = 0;
    for (int j = 0; j < n && total > cap; j++) { out[k++] = r[j].i; total -= r[j].s; }
    free(r);
    return k;
}

/* ── cache ─────────────────────────────────────────────────────────────── */

static bool join(char *out, size_t cap, const char *dir, const char *name)
{
    int n = snprintf(out, cap, "%s/%s", dir, name);
    return n > 0 && (size_t)n < cap;
}

static void mkdir_p(const char *dir)
{
    char tmp[256];
    size_t n = strlen(dir);
    if (n == 0 || n >= sizeof tmp) return;
    memcpy(tmp, dir, n + 1);
    for (size_t i = 1; i < n; i++)
        if (tmp[i] == '/') { tmp[i] = 0; mkdir(tmp, 0755); tmp[i] = '/'; }
    mkdir(tmp, 0755);
}

bool jf_img_cache_open(JfImgCache *c, const char *dir)
{
    if (!c) return false;
    memset(c, 0, sizeof *c);
    if (!dir || strlen(dir) >= sizeof c->dir) return false;
    strcpy(c->dir, dir);
    mkdir_p(dir);
    struct statvfs sv;
    if (statvfs(dir, &sv) != 0) return false;
    c->cap = jf_img_cap_bytes((long long)sv.f_bavail * (long long)sv.f_frsize);
    DIR *d = opendir(dir);
    if (!d) return false;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        char path[256];
        struct stat st;
        if (!join(path, sizeof path, dir, e->d_name)) continue;
        if (strncmp(e->d_name, ".tmp_", 5) == 0) { unlink(path); continue; }     /* a crashed write */
        if (!key_name_ok(e->d_name) || stat(path, &st) != 0 || !S_ISREG(st.st_mode)) continue;
        c->total += st.st_size;
        c->files++;
    }
    closedir(d);
    c->open = true;
    return true;
}

bool jf_img_cache_get(JfImgCache *c, const char *key, uint8_t **data, size_t *len)
{
    char path[256];
    struct stat st;
    if (!c || !c->open || !key || !data || !len || !key_name_ok(key) || !join(path, sizeof path, c->dir, key))
        return false;
    if (stat(path, &st) != 0) return false;                                       /* plain miss */
    if (st.st_size <= 0 || st.st_size > JF_IMG_FILE_MAX) { jf_img_cache_drop(c, key); return false; }
    FILE *f = fopen(path, "rb");
    uint8_t *b = f ? malloc((size_t)st.st_size) : NULL;
    if (!b || fread(b, 1, (size_t)st.st_size, f) != (size_t)st.st_size) {
        if (f) fclose(f);
        free(b);
        jf_img_cache_drop(c, key);
        return false;
    }
    fclose(f);
    utime(path, NULL);                                                            /* recently used */
    *data = b; *len = (size_t)st.st_size;
    return true;
}

void jf_img_cache_drop(JfImgCache *c, const char *key)
{
    char path[256];
    struct stat st;
    if (!c || !c->open || !key || !key_name_ok(key) || !join(path, sizeof path, c->dir, key)) return;
    if (stat(path, &st) != 0) return;
    if (unlink(path) == 0) {
        c->total -= st.st_size;
        c->files--;
        LIB_LOG(LOG_LEVEL_INFO, "jf_img: dropped cache file %s", key);
    }
}

/* Read the directory once, delete the oldest (never `keep`) until the sum fits, and set
 * the running totals to what the directory really holds. */
static void trim_to_cap(JfImgCache *c, const char *keep)
{
    DIR *d = opendir(c->dir);
    if (!d) return;
    int n = 0, capn = 256;
    char (*names)[JF_IMG_KEY_MAX] = malloc((size_t)capn * JF_IMG_KEY_MAX);
    time_t *mt = malloc((size_t)capn * sizeof(time_t));
    long long *sz = malloc((size_t)capn * sizeof(long long));
    long long total = 0;
    struct dirent *e;
    while (names && mt && sz && (e = readdir(d)) != NULL) {
        char path[256];
        struct stat st;
        if (!key_name_ok(e->d_name) || !join(path, sizeof path, c->dir, e->d_name) ||
            stat(path, &st) != 0 || !S_ISREG(st.st_mode)) continue;
        if (n == capn) {
            capn *= 2;
            char (*nn)[JF_IMG_KEY_MAX] = realloc(names, (size_t)capn * JF_IMG_KEY_MAX);
            time_t *nm = realloc(mt, (size_t)capn * sizeof(time_t));
            long long *ns = realloc(sz, (size_t)capn * sizeof(long long));
            if (nn) names = nn;
            if (nm) mt = nm;
            if (ns) sz = ns;
            if (!nn || !nm || !ns) { n = -1; break; }
        }
        strcpy(names[n], e->d_name);
        mt[n] = strcmp(e->d_name, keep) == 0 ? (time_t)0x7FFFFFFF : st.st_mtime;   /* picked last */
        sz[n] = st.st_size;
        total += st.st_size;
        n++;
    }
    closedir(d);
    if (names && mt && sz && n >= 0) {
        int *pick = malloc((size_t)(n ? n : 1) * sizeof(int));
        int k = pick ? jf_img_evict_bytes(mt, sz, n, total, c->cap, pick) : 0;
        for (int i = 0; i < k; i++) {
            char path[256];
            if (join(path, sizeof path, c->dir, names[pick[i]]) && unlink(path) == 0) { total -= sz[pick[i]]; n--; }
        }
        free(pick);
        c->total = total;
        c->files = n;
    }
    free(names); free(mt); free(sz);
}

bool jf_img_cache_put(JfImgCache *c, const char *key, const uint8_t *data, size_t len)
{
    char path[256], tmp[256], name[40];
    struct stat st;
    if (!c || !c->open || !key || !data || len == 0 || !key_name_ok(key)) return false;
    if (len > JF_IMG_FILE_MAX || (long long)len > c->cap) return false;
    snprintf(name, sizeof name, ".tmp_%ld", (long)getpid());
    if (!join(path, sizeof path, c->dir, key) || !join(tmp, sizeof tmp, c->dir, name)) return false;
    mkdir_p(c->dir);

    FILE *f = fopen(tmp, "wb");
    if (!f) return false;
    bool ok = fwrite(data, 1, len, f) == len;
    if (fclose(f) != 0) ok = false;
    long long old = stat(path, &st) == 0 ? (long long)st.st_size : -1;
    if (!ok || rename(tmp, path) != 0) { unlink(tmp); return false; }
    if (old >= 0) c->total -= old; else c->files++;
    c->total += (long long)len;
    if (c->total > c->cap) trim_to_cap(c, key);
    return true;
}
