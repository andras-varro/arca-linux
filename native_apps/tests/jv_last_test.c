/*
 * tests/jv_last_test.c — the last-played record file: round trip, absent file, bad id, clamping, clear.
 *
 * Build and run (host gcc, from native_apps/):
 *   gcc -std=c11 -Wall -Wextra -I common -I jf_shared -I jfvideo tests/jv_last_test.c jfvideo/jv_last.c \
 *       jf_shared/jf_url.c common/config.c -o build/jv_last_test && ./build/jv_last_test
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "jv_last.h"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); \
    printf(__VA_ARGS__); printf("\n"); } } while (0)

static char dir[128], path[160];

static void put(const char *text)
{
    FILE *f = fopen(path, "w");
    if (f) { fputs(text, f); fclose(f); }
}

int main(void)
{
    JvLast r, t;
    snprintf(dir, sizeof dir, "/tmp/jv_last_XXXXXX");
    if (!mkdtemp(dir)) { printf("mkdtemp failed\n"); return 2; }
    snprintf(path, sizeof path, "%s/sub/last.conf", dir);      /* the parent directory does not exist yet */

    CHECK(jv_last_load(&r, path) == -1 && !r.id[0] && r.audio_index == -1 && r.sub_index == -1, "absent file: no record, tracks -1");

    memset(&r, 0, sizeof r);
    snprintf(r.id, sizeof r.id, "5eed0000000000000000000000000030");
    r.position_s = 4321; r.audio_index = 2; r.sub_index = -1; r.saved_at = 1790000000LL; r.attempted = true;
    CHECK(jv_last_save(&r, path) == 0, "save creates the directory");
    CHECK(jv_last_load(&t, path) == 0 && !strcmp(t.id, r.id) && t.position_s == 4321 && t.audio_index == 2 && t.sub_index == -1 &&
          t.saved_at == 1790000000LL && t.attempted, "round trip");
    r.attempted = false; r.sub_index = 7; r.audio_index = -1;
    CHECK(jv_last_save(&r, path) == 0 && jv_last_load(&t, path) == 0 && !t.attempted && t.sub_index == 7 && t.audio_index == -1, "second round trip");

    put("id=nothex\nposition_s=5\n");
    CHECK(jv_last_load(&t, path) == -1 && !t.id[0], "a bad id is no record");
    put("position_s=5\n");
    CHECK(jv_last_load(&t, path) == -1, "no id is no record");
    put("id=5eed0000000000000000000000000030\nposition_s=-9\naudio_index=-5\nsub_index=zz\nsaved_at=-3\nattempted=maybe\n");
    CHECK(jv_last_load(&t, path) == 0 && t.position_s == 0 && t.audio_index == -1 && t.sub_index == -1 && t.saved_at == 0 && !t.attempted,
          "malformed values clamp to the defaults (pos %d a %d s %d at %lld att %d)", t.position_s, t.audio_index, t.sub_index, t.saved_at, t.attempted);

    CHECK(jv_last_clear(path) == 0 && access(path, F_OK) != 0, "clear removes the file");
    CHECK(jv_last_clear(path) == 0, "clearing an absent file is fine");
    CHECK(jv_last_load(&t, path) == -1, "cleared: no record");

    {   char cmd[300]; snprintf(cmd, sizeof cmd, "rm -rf %s", dir); if (system(cmd)) {} }
    printf("jv_last_test: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
