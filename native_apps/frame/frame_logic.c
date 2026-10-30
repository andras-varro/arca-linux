/* frame_logic.c — see frame_logic.h. */
#include "frame_logic.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

bool frame_is_image_name(const char *name)
{
    if (!name || name[0] == '.' || name[0] == '\0') return false;
    const char *dot = strrchr(name, '.');
    if (!dot || dot == name) return false;
    char ext[8];
    size_t l = strlen(dot + 1);
    if (l == 0 || l >= sizeof ext) return false;
    for (size_t i = 0; i < l; i++) ext[i] = (char)tolower((unsigned char)dot[1 + i]);
    ext[l] = '\0';
    return !strcmp(ext, "jpg") || !strcmp(ext, "jpeg") || !strcmp(ext, "png");
}

static uint32_t fnv1a(const char *s)
{
    uint32_t h = 2166136261u;
    for (; *s; s++) { h ^= (unsigned char)*s; h *= 16777619u; }
    return h;
}

int frame_key(const char *src_path, long size, char *out, size_t n)
{
    if (!src_path || !out || n == 0) return -1;
    const char *base = strrchr(src_path, '/');
    base = base ? base + 1 : src_path;
    char stem[41];
    size_t k = 0;
    for (const char *p = base; *p && *p != '.' && k < sizeof stem - 1; p++) {
        unsigned char c = (unsigned char)*p;
        stem[k++] = (isalnum(c) || c == '-' || c == '_') ? (char)c : '_';
    }
    stem[k] = '\0';
    int w = snprintf(out, n, "%s-%08x-%lx.jpg", stem, (unsigned)fnv1a(base), (unsigned long)size);
    return (w < 0 || (size_t)w >= n) ? -1 : w;
}

static uint32_t rnd(FrameOrder *o)
{
    uint32_t x = o->rng ? o->rng : 0x9e3779b9u;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    o->rng = x;
    return x;
}

static void identity(FrameOrder *o)
{
    for (int i = 0; i < o->n; i++) o->idx[i] = i;
}

/* A new shuffled round.  `avoid` (-1: none) must not come first. */
static void reshuffle(FrameOrder *o, int avoid)
{
    identity(o);
    for (int i = o->n - 1; i > 0; i--) {
        int j = (int)(rnd(o) % (uint32_t)(i + 1));
        int t = o->idx[i]; o->idx[i] = o->idx[j]; o->idx[j] = t;
    }
    if (o->n > 1 && o->idx[0] == avoid) { int t = o->idx[0]; o->idx[0] = o->idx[1]; o->idx[1] = t; }
}

void frame_order_init(FrameOrder *o, int *storage, int n, bool shuffle, uint32_t seed)
{
    o->idx = storage;
    o->n = n < 0 ? 0 : n;
    o->pos = 0;
    o->shuffle = shuffle;
    o->rng = seed;
    if (shuffle) reshuffle(o, -1); else identity(o);
}

int frame_order_cur(const FrameOrder *o)
{
    return o->n > 0 ? o->idx[o->pos] : -1;
}

int frame_order_next(FrameOrder *o)
{
    if (o->n <= 0) return -1;
    if (o->pos + 1 < o->n) { o->pos++; return o->idx[o->pos]; }
    if (o->shuffle) reshuffle(o, o->idx[o->n - 1]);
    o->pos = 0;
    return o->idx[0];
}

int frame_order_prev(FrameOrder *o)
{
    if (o->n <= 0) return -1;
    o->pos = o->pos > 0 ? o->pos - 1 : o->n - 1;
    return o->idx[o->pos];
}

int frame_order_peek_next(const FrameOrder *o)
{
    if (o->n <= 0) return -1;
    if (o->pos + 1 < o->n) return o->idx[o->pos + 1];
    return o->shuffle ? -1 : o->idx[0];
}
