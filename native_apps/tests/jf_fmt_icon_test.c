/* Host regression for the track rows' format icons (jellyfin/jf_fmt_icon.c):
 *
 *   A  a known container loads its file once and returns a tile of the file's size;
 *   B  a name with no file is NULL, and the miss is remembered: a file that appears
 *      afterwards is not read (tried once);
 *   C  a name outside [a-z0-9] or longer than JF_FMT_NAME_MAX is NULL without a
 *      filesystem call: files a traversal / unsanitised name would reach exist, and none is returned;
 *   D  the cache is bounded: names past JF_FMT_CACHE_MAX are NULL, the earlier ones stay served;
 *   E  jf_fmt_icon_set_dir() empties the cache (a miss is retried under the new dir).
 *
 * Build (from native_apps/):
 *   gcc -Wall -Wextra -fsanitize=undefined,address -I common -I jellyfin -I jf_shared -o build/jf_fmt_icon_test tests/jf_fmt_icon_test.c jellyfin/jf_fmt_icon.c common/ppm.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "jf_fmt_icon.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static void put(const char *path, int w, int h)
{
    FILE *f = fopen(path, "wb");
    if (!f) { printf("cannot write %s\n", path); exit(2); }
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int i = 0; i < w * h * 3; i++) fputc(i, f);
    fclose(f);
}

int main(void)
{
    char root[] = "/tmp/jf_fmticon_XXXXXX";
    if (!mkdtemp(root)) { printf("no temp dir\n"); return 2; }
    char dir[96], p[160], sub[160];
    snprintf(dir, sizeof dir, "%s/d", root);
    mkdir(dir, 0755);
    snprintf(sub, sizeof sub, "%s/ludfin-fmt-a", dir);
    mkdir(sub, 0755);
    jf_fmt_icon_set_dir(dir);

    snprintf(p, sizeof p, "%s/ludfin-fmt-flac.ppm", dir); put(p, 32, 32);
    const LvIcon *ic = jf_fmt_icon_get("flac");
    CHECK(ic && ic->pixels && ic->w == 32 && ic->h == 32, "A: flac gave no 32x32 tile");
    CHECK(jf_fmt_icon_get("flac") == ic, "A: a second get did not return the cached tile");

    CHECK(jf_fmt_icon_get("mp3") == NULL, "B: a name with no file is not NULL");
    snprintf(p, sizeof p, "%s/ludfin-fmt-mp3.ppm", dir); put(p, 32, 32);
    CHECK(jf_fmt_icon_get("mp3") == NULL, "B: the miss was not remembered (the file made afterwards was read)");

    /* C: each of these names would resolve to a file if it reached the path unreduced */
    snprintf(p, sizeof p, "%s/ludfin-fmt-FLAC.ppm", dir); put(p, 8, 8);
    snprintf(p, sizeof p, "%s/x.ppm", root); put(p, 8, 8);
    snprintf(p, sizeof p, "%s/ludfin-fmt-a-b.ppm", dir); put(p, 8, 8);
    snprintf(p, sizeof p, "%s/ludfin-fmt-abcdefghijkl.ppm", dir); put(p, 8, 8);
    CHECK(jf_fmt_icon_get("FLAC") == NULL, "C: an upper-case name was read");
    CHECK(jf_fmt_icon_get("a/../../x") == NULL, "C: a traversal name was read");
    CHECK(jf_fmt_icon_get("a-b") == NULL, "C: a name with '-' was read");
    CHECK(jf_fmt_icon_get("abcdefghijkl") == NULL, "C: a %d-character name was read", 12);
    CHECK(jf_fmt_icon_get("") == NULL && jf_fmt_icon_get(NULL) == NULL, "C: an empty / NULL name gave a tile");

    /* D: fill the cache with the remaining slots (flac, mp3 hold two), then one more */
    for (int i = 0; i < JF_FMT_CACHE_MAX - 2; i++) {
        char n[16];
        snprintf(n, sizeof n, "t%d", i);
        snprintf(p, sizeof p, "%s/ludfin-fmt-%s.ppm", dir, n); put(p, 4, 4);
        CHECK(jf_fmt_icon_get(n) != NULL, "D: %s was not cached", n);
    }
    snprintf(p, sizeof p, "%s/ludfin-fmt-late.ppm", dir); put(p, 4, 4);
    CHECK(jf_fmt_icon_get("late") == NULL, "D: a name past the cache bound was served");
    CHECK(jf_fmt_icon_get("flac") == ic, "D: an early entry stopped being served");

    /* E */
    jf_fmt_icon_set_dir(dir);
    CHECK(jf_fmt_icon_get("mp3") != NULL, "E: set_dir did not empty the cache (mp3 still a remembered miss)");

    jf_fmt_icon_set_dir(NULL);
    snprintf(p, sizeof p, "rm -rf %s", root);
    if (system(p) != 0) printf("cleanup of %s failed\n", root);
    if (fails) { printf("jf_fmt_icon_test: %d FAILED\n", fails); return 1; }
    printf("jf_fmt_icon_test: all passed\n");
    return 0;
}
