#include "jv_tracks.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

/* ISO 639-1, 639-2/B (the canonical form) and 639-2/T where it differs. */
static const struct { const char *a2, *b, *t; } langs[] = {
    {"hu","hun",0}, {"en","eng",0}, {"de","ger","deu"}, {"fr","fre","fra"}, {"es","spa",0}, {"it","ita",0},
    {"ja","jpn",0}, {"pt","por",0}, {"ru","rus",0}, {"nl","dut","nld"}, {"cs","cze","ces"}, {"pl","pol",0},
    {"sv","swe",0}, {"da","dan",0}, {"fi","fin",0}, {"no","nor",0}, {"tr","tur",0}, {"zh","chi","zho"},
    {"ko","kor",0}, {"el","gre","ell"}, {"ro","rum","ron"}, {"sk","slo","slk"}, {"hr","hrv",0}, {"sr","srp",0},
    {"bg","bul",0}, {"uk","ukr",0}, {"he","heb",0}, {"ar","ara",0}, {"sl","slv",0}, {"hi","hin",0},
};

static bool eqi(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
    return !*a && !*b;
}

bool jv_lang_norm(const char *code, char out[4])
{
    char t[4];
    size_t n = 0;
    out[0] = '\0';
    if (!code) return false;
    while (*code == ' ') code++;
    while (isalpha((unsigned char)code[n])) n++;     /* the letter run: "en-US" and "pt_BR" stop at the separator */
    if (n < 2 || n > 3) return false;
    for (size_t i = 0; i < n; i++) t[i] = (char)tolower((unsigned char)code[i]);
    t[n] = '\0';
    if (n == 3 && (!strcmp(t, "und") || !strcmp(t, "zxx") || !strcmp(t, "mul") || !strcmp(t, "mis"))) return false;
    for (size_t i = 0; i < sizeof langs / sizeof langs[0]; i++) {
        if ((n == 2 && !strcmp(t, langs[i].a2)) ||
            (n == 3 && (!strcmp(t, langs[i].b) || (langs[i].t && !strcmp(t, langs[i].t))))) {
            strcpy(out, langs[i].b);
            return true;
        }
    }
    strcpy(out, t);
    return true;
}

bool jv_lang_same(const char *a, const char *b)
{
    char x[4], y[4];
    return jv_lang_norm(a, x) && jv_lang_norm(b, y) && !strcmp(x, y);
}

static bool list_none(const char *list)
{
    while (list && *list == ' ') list++;
    return !list || !*list || !strncmp(list, "default", 7);
}

int jv_lang_rank(const char *list, const char *code)
{
    int rank = 0;
    const char *p = list;
    if (list_none(list)) return -1;
    while (*p) {
        char tok[16];
        size_t n = 0;
        while (*p && *p != ',') { if (n < sizeof tok - 1) tok[n++] = *p; p++; }
        tok[n] = '\0';
        if (*p == ',') p++;
        if (jv_lang_same(tok, code)) return rank;
        rank++;
    }
    return -1;
}

bool jv_sub_codec_text(const char *codec)
{
    static const char *const ok[] = { "srt", "subrip", "ass", "ssa", "webvtt", "vtt", "mov_text", "microdvd",
                                      "subviewer", "text" };
    if (!codec) return false;
    for (size_t i = 0; i < sizeof ok / sizeof ok[0]; i++) if (eqi(codec, ok[i])) return true;
    return false;
}

int jv_pick_audio(const JvStream *a, int n, const char *audio_langs)
{
    int best = -1, best_key = 0;
    if (n <= 0 || !a) return -1;
    for (int i = 0; i < n; i++) {
        int r = jv_lang_rank(audio_langs, a[i].lang);
        /* a ranked language beats an unranked one; inside a rank IsDefault beats not; ties keep the first */
        int key = (r < 0 ? 1000 : r * 2) + (a[i].is_default ? 0 : 1);
        if (best < 0 || key < best_key) { best = i; best_key = key; }
    }
    return best;
}

/* Lowest key wins, first on a tie.  key = rank*4 + (forced ? 2 : 0) + (default ? 0 : 1). */
static int pick_lang(const JvStream *s, int n, const char *langs_list, bool forced_only)
{
    int best = -1, best_key = 0;
    for (int i = 0; i < n; i++) {
        int r;
        if (!jv_sub_codec_text(s[i].codec)) continue;
        if (forced_only && !s[i].is_forced) continue;
        r = jv_lang_rank(langs_list, s[i].lang);
        if (r < 0) continue;
        int key = r * 4 + (s[i].is_forced ? 2 : 0) + (s[i].is_default ? 0 : 1);
        if (best < 0 || key < best_key) { best = i; best_key = key; }
    }
    return best;
}

int jv_pick_sub(const JvStream *s, int n, JvSubMode mode, const char *sub_langs, const char *audio_langs,
                const JvStream *audio)
{
    char alang[4];
    bool known = audio && jv_lang_norm(audio->lang, alang);
    if (n <= 0 || !s || mode == JV_SUB_OFF) return -1;
    if (mode == JV_SUB_FORCED) return pick_lang(s, n, known ? alang : sub_langs, true);
    if (mode == JV_SUB_FOREIGN && known) {
        const char *understood = list_none(audio_langs) ? sub_langs : audio_langs;
        if (jv_lang_rank(understood, alang) >= 0) return -1;
    }
    return pick_lang(s, n, sub_langs, false);
}

int jv_next_audio(int n, int cur)
{
    if (n <= 0) return -1;
    if (cur < 0 || cur + 1 >= n) return 0;
    return cur + 1;
}

int jv_next_sub(const JvStream *s, int n, int cur)
{
    int start = cur < 0 ? 0 : cur + 1;
    if (!s) return -1;
    for (int i = start; i < n; i++) if (jv_sub_codec_text(s[i].codec)) return i;
    return -1;
}

int jv_stream_label(char *out, size_t cap, const JvStream *s, bool audio)
{
    char lang[4], codec[16];
    char tail[24] = "";
    size_t i;
    int n;
    if (!jv_lang_norm(s->lang, lang)) strcpy(lang, "und");
    for (i = 0; lang[i]; i++) lang[i] = (char)toupper((unsigned char)lang[i]);
    for (i = 0; s->codec[i] && i < sizeof codec - 1; i++) codec[i] = (char)toupper((unsigned char)s->codec[i]);
    codec[i] = '\0';
    if (eqi(codec, "SUBRIP")) strcpy(codec, "SRT");
    if (audio && s->channels > 0) {
        const char *ch = s->channels == 1 ? "1.0" : s->channels == 2 ? "2.0" : s->channels == 6 ? "5.1"
                       : s->channels == 8 ? "7.1" : NULL;
        if (ch) snprintf(tail, sizeof tail, " %s", ch);
        else snprintf(tail, sizeof tail, " %dch", s->channels);
    }
    n = snprintf(out, cap, "%s %s%s%s%s", lang, codec[0] ? codec : "?", tail,
                 s->is_forced ? " FORCED" : "", s->is_external ? " EXT" : "");
    return (n < 0 || (size_t)n >= cap) ? -1 : n;
}
