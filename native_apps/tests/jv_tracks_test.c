/*
 * tests/jv_tracks_test.c — audio and subtitle selection, language folding, the cycle helpers and picker text.
 *
 * Build and run (host gcc, from native_apps/):
 *   gcc -std=c11 -Wall -Wextra -I jfvideo tests/jv_tracks_test.c jfvideo/jv_tracks.c -o build/jv_tracks_test
 */

#include <stdio.h>
#include <string.h>

#include "jv_tracks.h"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); \
    printf(__VA_ARGS__); printf("\n"); } } while (0)

static JvStream mk(int index, const char *lang, const char *codec, int flags)
{
    JvStream s;
    memset(&s, 0, sizeof s);
    s.index = index;
    snprintf(s.lang, sizeof s.lang, "%s", lang);
    snprintf(s.codec, sizeof s.codec, "%s", codec);
    s.is_default = flags & 1;
    s.is_forced = flags & 2;
    s.is_external = flags & 4;
    return s;
}

static void test_lang(void)
{
    char o[4];
    CHECK(jv_lang_norm("hu", o) && !strcmp(o, "hun"), "hu -> %s", o);
    CHECK(jv_lang_norm("HUN", o) && !strcmp(o, "hun"), "HUN -> %s", o);
    CHECK(jv_lang_norm("de", o) && !strcmp(o, "ger"), "de -> %s", o);
    CHECK(jv_lang_norm("deu", o) && !strcmp(o, "ger"), "deu -> %s", o);
    CHECK(jv_lang_norm("fra", o) && !strcmp(o, "fre"), "fra -> %s", o);
    CHECK(jv_lang_norm("en-US", o) && !strcmp(o, "eng"), "en-US -> %s", o);
    CHECK(jv_lang_norm("pt_BR", o) && !strcmp(o, "por"), "pt_BR -> %s", o);
    CHECK(jv_lang_norm("xyz", o) && !strcmp(o, "xyz"), "unknown 3-letter passes through");
    CHECK(!jv_lang_norm("", o) && o[0] == '\0', "empty is no language");
    CHECK(!jv_lang_norm(NULL, o), "NULL is no language");
    CHECK(!jv_lang_norm("und", o) && !jv_lang_norm("UND", o) && !jv_lang_norm("zxx", o), "und/zxx are no language");
    CHECK(!jv_lang_norm("english", o), "a word is not a code");
    CHECK(!jv_lang_norm("a", o) && !jv_lang_norm("12", o), "garbage is no language");
    CHECK(jv_lang_same("de", "ger") && jv_lang_same("fre", "fr") && !jv_lang_same("en", "hu"), "same");
    CHECK(!jv_lang_same("und", "und") && !jv_lang_same("", ""), "two undefined never match");
    CHECK(jv_lang_rank("hun,eng", "en") == 1 && jv_lang_rank("hun,eng", "hu") == 0, "rank");
    CHECK(jv_lang_rank("hun, ger", "deu") == 1, "rank with a space and a T code");
    CHECK(jv_lang_rank("hun,eng", "fre") == -1 && jv_lang_rank("hun,eng", "und") == -1, "rank miss");
    CHECK(jv_lang_rank("default", "hun") == -1 && jv_lang_rank("", "hun") == -1 && jv_lang_rank(NULL, "hun") == -1, "no list");
    CHECK(jv_sub_codec_text("subrip") && jv_sub_codec_text("SRT") && jv_sub_codec_text("ass") && jv_sub_codec_text("mov_text"), "text codecs");
    CHECK(!jv_sub_codec_text("pgssub") && !jv_sub_codec_text("dvdsub") && !jv_sub_codec_text("dvbsub") &&
          !jv_sub_codec_text("hdmv_pgs_subtitle") && !jv_sub_codec_text("") && !jv_sub_codec_text(NULL) &&
          !jv_sub_codec_text("weirdnew"), "image and unknown codecs are not text");
}

static void test_audio(void)
{
    JvStream a[4] = { mk(1, "eng", "ac3", 1), mk(2, "hun", "ac3", 0), mk(3, "ger", "aac", 0), mk(4, "hun", "aac", 1) };
    CHECK(jv_pick_audio(a, 4, "hun,eng") == 3, "language, then IsDefault inside it: %d", jv_pick_audio(a, 4, "hun,eng"));
    CHECK(jv_pick_audio(a, 4, "ger,hun") == 2, "list order beats IsDefault");
    CHECK(jv_pick_audio(a, 4, "fre,eng") == 0, "second list entry");
    CHECK(jv_pick_audio(a, 4, "jpn") == 0, "no match: IsDefault (first of several)");
    CHECK(jv_pick_audio(a, 4, "default") == 0 && jv_pick_audio(a, 4, "") == 0 && jv_pick_audio(a, 4, NULL) == 0, "no preference");
    a[0].is_default = false; a[3].is_default = false;
    CHECK(jv_pick_audio(a, 4, "jpn") == 0, "no default at all: first");
    CHECK(jv_pick_audio(a, 4, "hun") == 1, "no default in language: first of it");
    JvStream u[2] = { mk(1, "und", "ac3", 0), mk(2, "", "ac3", 1) };
    CHECK(jv_pick_audio(u, 2, "hun") == 1, "undefined languages never match: IsDefault");
    CHECK(jv_pick_audio(u, 0, "hun") == -1, "empty array");
    JvStream d[3] = { mk(1, "eng", "ac3", 1), mk(2, "eng", "ac3", 1), mk(3, "eng", "ac3", 1) };
    CHECK(jv_pick_audio(d, 3, "eng") == 0, "several IsDefault: first");
}

static void test_sub(void)
{
    JvStream s[6] = { mk(10, "eng", "subrip", 0), mk(11, "hun", "pgssub", 1), mk(12, "hun", "subrip", 0),
                      mk(13, "hun", "subrip", 2), mk(14, "ger", "ass", 0), mk(15, "hun", "srt", 1) };
    JvStream aeng = mk(1, "eng", "ac3", 1), ahun = mk(2, "hun", "ac3", 1), aund = mk(3, "und", "ac3", 1);
    CHECK(jv_pick_sub(s, 6, JV_SUB_OFF, "hun", "hun", &aeng) == -1, "off");
    CHECK(jv_pick_sub(s, 6, JV_SUB_LANG, "hun", "hun", &aeng) == 5, "lang: full IsDefault text track, PGS skipped: %d",
          jv_pick_sub(s, 6, JV_SUB_LANG, "hun", "hun", &aeng));
    CHECK(jv_pick_sub(s, 6, JV_SUB_LANG, "hun", "hun", &ahun) == 5, "lang shows even when audio matches");
    CHECK(jv_pick_sub(s, 6, JV_SUB_LANG, "ger,hun", "hun", NULL) == 4, "list order");
    CHECK(jv_pick_sub(s, 6, JV_SUB_LANG, "jpn", "hun", &aeng) == -1 && jv_pick_sub(s, 6, JV_SUB_LANG, "", "", NULL) == -1, "no language: none");
    CHECK(jv_pick_sub(s, 6, JV_SUB_FOREIGN, "hun", "hun,eng", &aeng) == -1, "foreign: audio is a listening language");
    CHECK(jv_pick_sub(s, 6, JV_SUB_FOREIGN, "hun", "hun", &aeng) == 5, "foreign: audio not listened to -> subs");
    CHECK(jv_pick_sub(s, 6, JV_SUB_FOREIGN, "hun", "default", &ahun) == -1, "foreign, default audio pref: audio in sub language -> none");
    CHECK(jv_pick_sub(s, 6, JV_SUB_FOREIGN, "hun", "hun", &aund) == 5, "foreign, unknown audio language: show");
    CHECK(jv_pick_sub(s, 6, JV_SUB_FORCED, "hun", "hun", &ahun) == 3, "forced of the audio language");
    CHECK(jv_pick_sub(s, 6, JV_SUB_FORCED, "hun", "hun", &aeng) == -1, "forced: no forced track in english");
    CHECK(jv_pick_sub(s, 6, JV_SUB_FORCED, "hun", "hun", &aund) == 3, "forced, unknown audio: sub list");
    JvStream f[2] = { mk(1, "hun", "subrip", 2), mk(2, "hun", "pgssub", 0) };
    CHECK(jv_pick_sub(f, 2, JV_SUB_LANG, "hun", "", NULL) == 0, "only a forced track of the language: used as fallback");
    JvStream ext[2] = { mk(1, "hun", "subrip", 0), mk(2, "hun", "subrip", 4 | 1) };
    CHECK(jv_pick_sub(ext, 2, JV_SUB_LANG, "hun", "", NULL) == 1, "IsDefault external beats the first");
    CHECK(jv_pick_sub(s, 0, JV_SUB_LANG, "hun", "", NULL) == -1, "empty");
}

static void test_cycle(void)
{
    JvStream s[4] = { mk(1, "eng", "pgssub", 0), mk(2, "hun", "subrip", 0), mk(3, "eng", "dvdsub", 0), mk(4, "ger", "ass", 0) };
    CHECK(jv_next_audio(3, 0) == 1 && jv_next_audio(3, 2) == 0 && jv_next_audio(3, -1) == 0 && jv_next_audio(0, 0) == -1, "audio cycle");
    CHECK(jv_next_audio(1, 0) == 0, "single audio stays");
    int c = -1;
    c = jv_next_sub(s, 4, c); CHECK(c == 1, "off -> first text: %d", c);
    c = jv_next_sub(s, 4, c); CHECK(c == 3, "skips the DVD track: %d", c);
    c = jv_next_sub(s, 4, c); CHECK(c == -1, "last -> off: %d", c);
    CHECK(jv_next_sub(s, 1, -1) == -1 && jv_next_sub(s, 0, -1) == -1, "only image tracks: stays off");
}

static void test_label(void)
{
    char b[64];
    JvStream a = mk(1, "hu", "ac3", 0); a.channels = 6;
    CHECK(jv_stream_label(b, sizeof b, &a, true) == (int)strlen("HUN AC3 5.1") && !strcmp(b, "HUN AC3 5.1"), "audio label '%s'", b);
    JvStream s = mk(2, "eng", "subrip", 2 | 4);
    jv_stream_label(b, sizeof b, &s, false);
    CHECK(!strcmp(b, "ENG SRT FORCED EXT"), "sub label '%s'", b);
    JvStream u = mk(3, "", "dts", 0); u.channels = 3;
    jv_stream_label(b, sizeof b, &u, true);
    CHECK(!strcmp(b, "UND DTS 3ch"), "undefined label '%s'", b);
    CHECK(jv_stream_label(b, 11, &a, true) == -1 && jv_stream_label(b, 12, &a, true) == 11, "cap edge");
}

int main(void)
{
    test_lang(); test_audio(); test_sub(); test_cycle(); test_label();
    printf("jv_tracks_test: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
