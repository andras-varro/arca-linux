/* jf_model.c — see jf_model.h. */

#include <limits.h>
#include <string.h>
#include "jf_model.h"

/* Not jf_id_valid(): the model test links only json.c, and an id here is a
 * fixed-size field of a reply, not a caller's string. */
static bool hex32(const char *s)
{
    for (int i = 0; i < 32; i++) {
        char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
    }
    return s[32] == '\0';
}

static int map_err(int n)
{
    if (n == JSON_ERR_PART) return JF_PARSE_TRUNC;
    if (n == JSON_ERR_NOMEM) return JF_PARSE_NOMEM;
    return JF_PARSE_BAD;
}

/* A string field into dst; absent, null or non-string leaves "".  json_str cuts
 * on character boundaries, so a long name is shortened, never split. */
static bool sfield(const char *s, const JsonTok *t, int n, int obj, const char *key,
                   char *dst, size_t cap)
{
    int i = json_obj_get(s, t, n, obj, key);
    dst[0] = '\0';
    if (i < 0 || t[i].type != JSON_STRING) return false;
    if (json_str(s, &t[i], dst, cap) < 0) { dst[0] = '\0'; return false; }
    return dst[0] != '\0';
}

static int clamp_i(long long v, long long lo, long long hi)
{
    return (int)(v < lo ? lo : v > hi ? hi : v);
}

/* Round half away from zero; clamped first so the cast cannot overflow. */
static int gain_mdb(double x)
{
    double v = x * 1000.0;
    if (v > 100000.0) return 100000;
    if (v < -100000.0) return -100000;
    return (int)(v < 0 ? v - 0.5 : v + 0.5);
}

static JfItemType type_of(const char *s, const JsonTok *t, int n, int o)
{
    int i = json_obj_get(s, t, n, o, "Type");
    if (i < 0 || t[i].type != JSON_STRING) return JF_ITEM_NONE;
    char ty[16];   /* "MusicGenre" is the longest type kept (10) */
    int r = json_str(s, &t[i], ty, sizeof ty);
    if (r < 0 || r >= (int)sizeof ty) return JF_ITEM_NONE;
    if (!strcmp(ty, "MusicArtist")) return JF_ITEM_ARTIST;
    if (!strcmp(ty, "MusicAlbum")) return JF_ITEM_ALBUM;
    if (!strcmp(ty, "Audio")) return JF_ITEM_TRACK;
    if (!strcmp(ty, "Playlist")) return JF_ITEM_PLAYLIST;
    if (!strcmp(ty, "MusicGenre")) return JF_ITEM_GENRE;
    return JF_ITEM_NONE;
}

static void read_item(const char *s, const JsonTok *t, int n, int o, JfItem *it)
{
    long long v;
    double d;
    int i, e;

    sfield(s, t, n, o, "Name", it->name, sizeof it->name);

    i = json_obj_get(s, t, n, o, "IndexNumber");
    if (i >= 0 && json_i64(s, &t[i], &v)) it->index = clamp_i(v, 0, INT_MAX);
    i = json_obj_get(s, t, n, o, "RunTimeTicks");
    if (i >= 0 && json_i64(s, &t[i], &v) && v > 0) it->runtime_s = clamp_i(v / 10000000LL, 0, INT_MAX);
    i = json_obj_get(s, t, n, o, "NormalizationGain");
    if (i >= 0 && json_num(s, &t[i], &d)) it->norm_gain_mdb = gain_mdb(d);

    i = json_obj_get(s, t, n, o, "SongCount");
    if (i >= 0 && json_i64(s, &t[i], &v)) it->count = clamp_i(v, 0, INT_MAX);

    if (it->type == JF_ITEM_ARTIST || it->type == JF_ITEM_GENRE || it->type == JF_ITEM_PLAYLIST) return;
    i = json_obj_get(s, t, n, o, "ProductionYear");
    if (i >= 0 && json_i64(s, &t[i], &v)) it->year = clamp_i(v, 0, 9999);
    i = json_obj_get(s, t, n, o, "ArtistItems");
    e = i >= 0 ? json_arr_first(t, n, i) : -1;
    if (e >= 0 && t[e].type == JSON_OBJECT) {
        char aid[40];
        int al = -1;
        int ai = json_obj_get(s, t, n, e, "Id");
        if (ai >= 0 && t[ai].type == JSON_STRING) al = json_str(s, &t[ai], aid, sizeof aid);
        if (al == 32 && hex32(aid)) memcpy(it->artist_id, aid, 33);
    }
    i = json_obj_get(s, t, n, o, "Genres");
    if (i >= 0 && t[i].type == JSON_ARRAY) {
        size_t used = 0;
        for (int g = json_arr_first(t, n, i); g >= 0; g = json_arr_next(t, n, i, g)) {
            char nm[JF_GENRES_BYTES];
            if (t[g].type != JSON_STRING || json_str(s, &t[g], nm, sizeof nm) <= 0) continue;
            size_t l = strlen(nm);
            if (strchr(nm, '\n') || used + l + 1 >= sizeof it->genres) continue;   /* whole names only */
            memcpy(it->genres + used, nm, l);
            used += l;
            it->genres[used++] = '\n';
            it->genres[used] = '\0';
        }
    }
    sfield(s, t, n, o, "Album", it->album, sizeof it->album);
    if (sfield(s, t, n, o, "AlbumArtist", it->artist, sizeof it->artist)) return;
    i = json_obj_get(s, t, n, o, "Artists");
    e = i >= 0 ? json_arr_first(t, n, i) : -1;   /* only [0]: some tracks list dozens */
    if (e >= 0 && t[e].type == JSON_STRING && json_str(s, &t[e], it->artist, sizeof it->artist) < 0)
        it->artist[0] = '\0';
}

int jf_parse_items(const char *body, size_t len, JsonTok *toks, int ntoks, JfList *out)
{
    int n = json_parse(body, len, toks, ntoks);
    if (n < 0) return map_err(n);
    if (toks[0].type != JSON_OBJECT) return JF_PARSE_BAD;
    int arr = json_obj_get(body, toks, n, 0, "Items");
    if (arr < 0 || toks[arr].type != JSON_ARRAY) return JF_PARSE_BAD;

    int tr = json_obj_get(body, toks, n, 0, "TotalRecordCount");
    long long tv;
    if (tr >= 0 && json_i64(body, &toks[tr], &tv)) out->total = clamp_i(tv, 0, INT_MAX);

    for (int e = json_arr_first(toks, n, arr); e >= 0; e = json_arr_next(toks, n, arr, e)) {
        if (out->loaded < INT_MAX) out->loaded++;
        if (toks[e].type != JSON_OBJECT) continue;
        JfItemType ty = type_of(body, toks, n, e);
        if (ty == JF_ITEM_NONE) continue;
        char id[40];
        int il = -1;
        int ii = json_obj_get(body, toks, n, e, "Id");
        if (ii >= 0 && toks[ii].type == JSON_STRING) il = json_str(body, &toks[ii], id, sizeof id);
        if (il != 32 || !hex32(id)) continue;
        if (out->n >= out->cap) { out->full = true; continue; }

        JfItem *it = &out->items[out->n++];
        memset(it, 0, sizeof *it);
        it->type = ty;
        memcpy(it->id, id, 33);
        read_item(body, toks, n, e, it);
    }
    return JF_PARSE_OK;
}

int jf_parse_auth(const char *body, size_t len, JsonTok *toks, int ntoks,
                  char *token, size_t token_cap, char *user_id, size_t uid_cap)
{
    if (token_cap) token[0] = '\0';
    if (uid_cap) user_id[0] = '\0';
    if (!token_cap || uid_cap < 33) return JF_PARSE_BAD;

    int n = json_parse(body, len, toks, ntoks);
    if (n < 0) return map_err(n);
    if (toks[0].type != JSON_OBJECT) return JF_PARSE_BAD;

    int a = json_obj_get(body, toks, n, 0, "AccessToken");
    int u = json_obj_get(body, toks, n, 0, "User");
    if (a < 0 || toks[a].type != JSON_STRING || u < 0 || toks[u].type != JSON_OBJECT) return JF_PARSE_BAD;
    int ui = json_obj_get(body, toks, n, u, "Id");
    if (ui < 0 || toks[ui].type != JSON_STRING) return JF_PARSE_BAD;

    int tl = json_str(body, &toks[a], token, token_cap);
    int il = json_str(body, &toks[ui], user_id, uid_cap);
    if (tl <= 0 || (size_t)tl >= token_cap || il != 32 || !hex32(user_id)) {
        token[0] = '\0';
        user_id[0] = '\0';
        return JF_PARSE_BAD;
    }
    return JF_PARSE_OK;
}

int jf_genre_key(char *out, size_t cap, const char *name)
{
    if (!out || cap == 0) return 0;
    size_t w = 0;
    bool gap = false;
    for (const char *p = name ? name : ""; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '-' || c == '_' || c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v') { gap = true; continue; }
        if (gap && w > 0) { if (w + 1 >= cap) break; out[w++] = ' '; }
        gap = false;
        if (w + 1 >= cap) break;
        out[w++] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : (char)c;
    }
    out[w] = '\0';
    if (w > 6 && !strcmp(out + w - 6, " music")) { w -= 6; out[w] = '\0'; }
    return (int)w;
}

void jf_list_init(JfList *l, JfItem *storage, int cap)
{
    memset(l, 0, sizeof *l);
    l->items = storage;
    l->cap = cap < 0 ? 0 : cap;
}

void jf_list_clear(JfList *l)
{
    l->n = l->loaded = l->total = 0;
    l->full = false;
}
