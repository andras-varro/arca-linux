/* jf_fmt_icon.c — see jf_fmt_icon.h. */
#include "jf_fmt_icon.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ppm.h"

typedef struct {
    char     name[JF_FMT_NAME_MAX + 1];
    uint32_t *pixels;   /* NULL: a remembered miss */
    LvIcon   icon;
} FmtEntry;

static FmtEntry s_cache[JF_FMT_CACHE_MAX];
static int      s_n;
static char     s_dir[200] = JF_FMT_ICON_DIR;

void jf_fmt_icon_set_dir(const char *dir)
{
    for (int i = 0; i < s_n; i++) free(s_cache[i].pixels);
    memset(s_cache, 0, sizeof s_cache);
    s_n = 0;
    snprintf(s_dir, sizeof s_dir, "%s", dir ? dir : JF_FMT_ICON_DIR);
}

static int name_ok(const char *s)
{
    size_t n = s ? strlen(s) : 0;
    if (n == 0 || n > JF_FMT_NAME_MAX) return 0;
    for (size_t i = 0; i < n; i++)
        if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= '0' && s[i] <= '9'))) return 0;
    return 1;
}

const LvIcon *jf_fmt_icon_get(const char *container)
{
    if (!name_ok(container)) return NULL;
    for (int i = 0; i < s_n; i++)
        if (strcmp(s_cache[i].name, container) == 0) return s_cache[i].pixels ? &s_cache[i].icon : NULL;
    if (s_n >= JF_FMT_CACHE_MAX) return NULL;

    FmtEntry *e = &s_cache[s_n++];
    snprintf(e->name, sizeof e->name, "%s", container);
    char path[320];
    snprintf(path, sizeof path, "%s/ludfin-fmt-%s.ppm", s_dir, container);
    int w = 0, h = 0;
    e->pixels = ppm_load(path, &w, &h);
    if (!e->pixels || w <= 0 || h <= 0) {
        free(e->pixels);
        e->pixels = NULL;
        return NULL;
    }
    e->icon.pixels = e->pixels;
    e->icon.w = w;
    e->icon.h = h;
    return &e->icon;
}
