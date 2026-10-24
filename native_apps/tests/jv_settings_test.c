/*
 * tests/jv_settings_test.c — defaults, round trip, clamping and malformed values of the video app's settings file.
 *
 * Build and run (host gcc, from native_apps/):
 *   gcc -std=c11 -Wall -Wextra -I common -I jf_shared -I jfvideo tests/jv_settings_test.c jfvideo/jv_settings.c \
 *       jfvideo/jv_tracks.c jf_shared/jf_url.c common/config.c -o build/jv_settings_test && ./build/jv_settings_test
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "jv_settings.h"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); \
    printf(__VA_ARGS__); printf("\n"); } } while (0)

static char dir[128], path[160];

static void put(const char *text)
{
    FILE *f = fopen(path, "w");
    if (f) { fputs(text, f); fclose(f); }
}

static void is_default(const JvSettings *s, const char *what)
{
    CHECK(!strcmp(s->audio_lang, "default") && s->sub_mode == JV_SUB_OFF && !strcmp(s->sub_lang, "hun") &&
          s->sub_scale == 2 && s->av_offset_ms[0] == 0 && s->av_offset_ms[1] == 0 && s->av_offset_ms[2] == 0 &&
          s->agc && !s->library_movies[0] && !s->library_shows[0], "%s: not the defaults", what);
}

int main(void)
{
    JvSettings s, t;
    snprintf(dir, sizeof dir, "/tmp/jv_settings_XXXXXX");
    if (!mkdtemp(dir)) { printf("mkdtemp failed\n"); return 2; }
    snprintf(path, sizeof path, "%s/sub/settings.conf", dir);

    memset(&s, 0x5a, sizeof s);
    CHECK(jv_settings_load(&s, path) == -1, "missing file reports -1");
    is_default(&s, "missing file");

    jv_settings_defaults(&s);
    snprintf(s.audio_lang, sizeof s.audio_lang, "hun,eng");
    s.sub_mode = JV_SUB_FOREIGN;
    snprintf(s.sub_lang, sizeof s.sub_lang, "eng");
    s.sub_scale = 3;
    s.av_offset_ms[JV_SINK_ONBOARD] = 20; s.av_offset_ms[JV_SINK_USB] = -140; s.av_offset_ms[JV_SINK_BT] = 220;
    s.agc = false;
    snprintf(s.library_movies, sizeof s.library_movies, "0123456789abcdef0123456789abcdef");
    CHECK(jv_settings_save(&s, path) == 0, "save creates the directory and writes");
    CHECK(jv_settings_load(&t, path) == 0, "load");
    CHECK(!strcmp(t.audio_lang, "hun,eng") && t.sub_mode == JV_SUB_FOREIGN && !strcmp(t.sub_lang, "eng") &&
          t.sub_scale == 3 && t.av_offset_ms[0] == 20 && t.av_offset_ms[1] == -140 && t.av_offset_ms[2] == 220 &&
          !t.agc && !strcmp(t.library_movies, s.library_movies) && !t.library_shows[0], "round trip");
    for (int m = 0; m < 4; m++) {
        s.sub_mode = (JvSubMode)m;
        jv_settings_save(&s, path); jv_settings_load(&t, path);
        CHECK((int)t.sub_mode == m, "mode %d round trip, got %d", m, (int)t.sub_mode);
    }

    put("audio_lang=hun;rm -rf\nsub_mode=bogus\nsub_lang=\nsub_scale=7\nav_offset_usb_ms=abc\nav_offset_bt_ms=99999\n"
        "av_offset_onboard_ms=-99999\nagc=maybe\nlibrary_movies=notanid\nlibrary_shows=\n");
    CHECK(jv_settings_load(&t, path) == 0, "malformed file still loads");
    CHECK(!strcmp(t.audio_lang, "default"), "bad language list -> default: '%s'", t.audio_lang);
    CHECK(t.sub_mode == JV_SUB_OFF && !strcmp(t.sub_lang, "hun") && t.sub_scale == 2, "bad mode/empty lang/bad scale -> default");
    CHECK(t.av_offset_ms[JV_SINK_USB] == 0, "non-numeric offset -> 0");
    CHECK(t.av_offset_ms[JV_SINK_BT] == JV_OFFSET_MAX_MS && t.av_offset_ms[JV_SINK_ONBOARD] == -JV_OFFSET_MAX_MS, "offsets clamped: %d %d",
          t.av_offset_ms[JV_SINK_BT], t.av_offset_ms[JV_SINK_ONBOARD]);
    CHECK(t.agc, "agc=maybe -> default on");
    CHECK(!t.library_movies[0], "bad library id -> all");

    put("audio_lang=abcdefghijklmnopqrstuvwxyzabcdefghijkl\nsub_scale=3\nagc=0\n");
    jv_settings_load(&t, path);
    CHECK(!strcmp(t.audio_lang, "default") && t.sub_scale == 3 && !t.agc, "over-long list rejected, valid keys kept");

    s.av_offset_ms[0] = 5000; s.sub_scale = 9;
    jv_settings_save(&s, path); jv_settings_load(&t, path);
    CHECK(t.av_offset_ms[0] == JV_OFFSET_MAX_MS && t.sub_scale == 2, "save clamps too");

    char cmd[200];
    snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
    if (system(cmd) != 0) printf("cleanup failed\n");
    printf("jv_settings_test: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
