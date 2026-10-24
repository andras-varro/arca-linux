/*
 * jv_model.c — see jv_model.h.  Lists are a visitor on jf_items_walk; the detail
 * is one object read with the same getters.
 */
#include <limits.h>
#include <string.h>
#include "jv_model.h"

static int clamp_i(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

static bool is_hex32(const char *s)
{
    if (strlen(s) != 32) return false;
    for (int i = 0; i < 32; i++)
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) return false;
    return true;
}

/* An id field into dst[33]; "" unless it is 32 lowercase hex. */
static void get_id(const char *s, const JsonTok *t, int n, int obj, const char *key, char *dst)
{
    char tmp[40];
    jf_get_str(s, t, n, obj, key, tmp, sizeof tmp);
    if (is_hex32(tmp)) memcpy(dst, tmp, 33);
    else dst[0] = '\0';
}

/* The row type of an element, JV_NONE for one this client does not show.  A Views entry has Type
 * CollectionFolder (or UserView) and its CollectionType says what it holds. */
static JvType type_of(const char *s, const JsonTok *t, int n, int o)
{
    char ty[24];
    jf_get_str(s, t, n, o, "Type", ty, sizeof ty);
    if (!strcmp(ty, "Movie"))   return JV_MOVIE;
    if (!strcmp(ty, "Series"))  return JV_SERIES;
    if (!strcmp(ty, "Season"))  return JV_SEASON;
    if (!strcmp(ty, "Episode")) return JV_EPISODE;
    if (!strcmp(ty, "BoxSet"))  return JV_BOXSET;
    if (!strcmp(ty, "Person"))  return JV_PERSON;
    if (!strcmp(ty, "Genre"))   return JV_GENRE;
    if (!strcmp(ty, "CollectionFolder") || !strcmp(ty, "UserView")) {
        char ct[16];
        jf_get_str(s, t, n, o, "CollectionType", ct, sizeof ct);
        if (!strcmp(ct, "movies"))  return JV_LIB_MOVIES;
        if (!strcmp(ct, "tvshows")) return JV_LIB_SHOWS;
    }
    return JV_NONE;
}

/* Everything of a row but the type; false (the row zeroed) when the Id is not 32 hex. */
static bool fill_item(JvItem *it, JvType ty, const char *s, const JsonTok *t, int n, int o)
{
    memset(it, 0, sizeof *it);
    if (!jf_base_fill(&it->b, s, t, n, o)) return false;
    it->type = ty;
    if (ty == JV_SEASON || ty == JV_EPISODE) {
        get_id(s, t, n, o, "SeriesId", it->series_id);
        jf_get_str(s, t, n, o, "SeriesName", it->series_name, sizeof it->series_name);
        jf_get_str(s, t, n, o, "SeriesPrimaryImageTag", it->series_image_tag, sizeof it->series_image_tag);
    }
    if (ty == JV_EPISODE) {
        get_id(s, t, n, o, "SeasonId", it->season_id);
        it->season_no  = clamp_i(jf_get_int(s, t, n, o, "ParentIndexNumber", 0), 0, 9999);
        it->episode_no = clamp_i(jf_get_int(s, t, n, o, "IndexNumber", 0), 0, 9999);
        it->episode_end = clamp_i(jf_get_int(s, t, n, o, "IndexNumberEnd", 0), 0, 9999);
    } else if (ty == JV_SEASON) {
        it->season_no = clamp_i(jf_get_int(s, t, n, o, "IndexNumber", 0), 0, 9999);
    }
    return true;
}

void jv_list_init(JvList *l, JvItem *storage, int cap)
{
    memset(l, 0, sizeof *l);
    l->items = storage;
    l->cap = cap;
}

void jv_list_clear(JvList *l)
{
    l->n = l->loaded = l->total = 0;
    l->full = false;
}

static bool items_visit(void *ctx, const char *s, const JsonTok *t, int n, int e)
{
    JvList *out = ctx;
    JvType ty = type_of(s, t, n, e);
    if (ty == JV_NONE) return true;
    if (out->n >= out->cap) { out->full = true; return true; }
    if (fill_item(&out->items[out->n], ty, s, t, n, e)) out->n++;
    return true;
}

int jv_parse_items(const char *body, size_t len, JsonTok *toks, int ntoks, void *dst)
{
    JvList *out = dst;
    int consumed = 0, total = -1;
    int r = jf_items_walk(body, len, toks, ntoks, items_visit, out, &consumed, &total);
    if (r != JF_PARSE_OK) return r;
    if (total >= 0) out->total = total;
    out->loaded = consumed > INT_MAX - out->loaded ? INT_MAX : out->loaded + consumed;
    return JF_PARSE_OK;
}

/* ── detail ─────────────────────────────────────────────────────────────── */

static void add_stream(JvStream *arr, int cap, int *n, int *dropped, const char *s, const JsonTok *t, int nt,
                       int o, bool audio)
{
    int idx = jf_get_int(s, t, nt, o, "Index", -1);
    if (idx < 0) return;                       /* no index, nothing to put in the URL */
    if (*n >= cap) { (*dropped)++; return; }
    JvStream *st = &arr[(*n)++];
    memset(st, 0, sizeof *st);
    st->index = idx;
    jf_get_str(s, t, nt, o, "Language", st->lang, sizeof st->lang);
    jf_get_str(s, t, nt, o, "Title", st->title, sizeof st->title);
    jf_get_str(s, t, nt, o, "Codec", st->codec, sizeof st->codec);
    if (audio) st->channels = clamp_i(jf_get_int(s, t, nt, o, "Channels", 0), 0, 32);
    jf_get_bool(s, t, nt, o, "IsDefault", &st->is_default);
    jf_get_bool(s, t, nt, o, "IsForced", &st->is_forced);
    jf_get_bool(s, t, nt, o, "IsExternal", &st->is_external);
}

/* Genre names joined with ", "; a name that does not fit whole is dropped. */
static void read_genres(const char *s, const JsonTok *t, int n, int obj, char *out, size_t cap)
{
    out[0] = '\0';
    int g = json_obj_get(s, t, n, obj, "Genres");
    if (g < 0 || t[g].type != JSON_ARRAY) return;
    size_t used = 0;
    for (int e = json_arr_first(t, n, g); e >= 0; e = json_arr_next(t, n, g, e)) {
        char nm[48];
        if (t[e].type != JSON_STRING) continue;
        int r = json_str(s, &t[e], nm, sizeof nm);
        if (r <= 0 || r >= (int)sizeof nm) continue;
        size_t need = (size_t)r + (used ? 2 : 0);
        if (used + need + 1 > cap) continue;
        if (used) { out[used++] = ','; out[used++] = ' '; }
        memcpy(out + used, nm, (size_t)r + 1);
        used += (size_t)r;
    }
}

static void read_people(JvDetail *d, const char *s, const JsonTok *t, int n, int obj)
{
    int p = json_obj_get(s, t, n, obj, "People");
    if (p < 0 || t[p].type != JSON_ARRAY) return;
    for (int pass = 0; pass < 2; pass++) {       /* directors first, then the cast */
        for (int e = json_arr_first(t, n, p); e >= 0 && d->n_people < JV_MAX_PEOPLE; e = json_arr_next(t, n, p, e)) {
            if (t[e].type != JSON_OBJECT) continue;
            char ty[16];
            jf_get_str(s, t, n, e, "Type", ty, sizeof ty);
            bool dir = !strcmp(ty, "Director");
            bool cast = !strcmp(ty, "Actor") || !strcmp(ty, "GuestStar");
            if (pass == 0 ? !dir : !cast) continue;
            JvPerson *pe = &d->people[d->n_people];
            memset(pe, 0, sizeof *pe);
            if (!jf_get_str(s, t, n, e, "Name", pe->name, sizeof pe->name)) continue;
            jf_get_str(s, t, n, e, "Role", pe->role, sizeof pe->role);
            get_id(s, t, n, e, "Id", pe->id);
            pe->director = dir;
            d->n_people++;
        }
    }
}

/* MediaSources[0]: only the first source is played. */
static int first_source(const char *s, const JsonTok *t, int n, int obj)
{
    int ms = json_obj_get(s, t, n, obj, "MediaSources");
    if (ms < 0 || t[ms].type != JSON_ARRAY) return -1;
    int m = json_arr_first(t, n, ms);
    return (m >= 0 && t[m].type == JSON_OBJECT) ? m : -1;
}

static void read_source(JvDetail *d, const char *s, const JsonTok *t, int n, int m)
{
    get_id(s, t, n, m, "Id", d->msid);
    jf_get_str(s, t, n, m, "Container", d->container, sizeof d->container);
    char *comma = strchr(d->container, ',');
    if (comma) *comma = '\0';
    int st = json_obj_get(s, t, n, m, "MediaStreams");
    if (st < 0 || t[st].type != JSON_ARRAY) return;
    for (int e = json_arr_first(t, n, st); e >= 0; e = json_arr_next(t, n, st, e)) {
        char ty[16];
        if (t[e].type != JSON_OBJECT) continue;
        jf_get_str(s, t, n, e, "Type", ty, sizeof ty);
        if (!strcmp(ty, "Audio"))
            add_stream(d->audio, JV_MAX_AUDIO, &d->n_audio, &d->audio_dropped, s, t, n, e, true);
        else if (!strcmp(ty, "Subtitle"))
            add_stream(d->subs, JV_MAX_SUBS, &d->n_subs, &d->subs_dropped, s, t, n, e, false);
        else if (!strcmp(ty, "Video") && d->width == 0) {
            d->width  = clamp_i(jf_get_int(s, t, n, e, "Width", 0), 0, 99999);
            d->height = clamp_i(jf_get_int(s, t, n, e, "Height", 0), 0, 99999);
        }
    }
}

int jv_parse_detail(const char *body, size_t len, JsonTok *toks, int ntoks, void *dst)
{
    JvDetail *d = dst;
    memset(d, 0, sizeof *d);
    int n = json_parse(body, len, toks, ntoks);
    if (n < 0) return n == JSON_ERR_PART ? JF_PARSE_TRUNC : n == JSON_ERR_NOMEM ? JF_PARSE_NOMEM : JF_PARSE_BAD;
    if (toks[0].type != JSON_OBJECT) return JF_PARSE_BAD;

    /* A type this client does not list still has a detail: type stays JV_NONE then. */
    if (!fill_item(&d->it, type_of(body, toks, n, 0), body, toks, n, 0)) { memset(d, 0, sizeof *d); return JF_PARSE_BAD; }

    jf_get_str(body, toks, n, 0, "Overview", d->overview, sizeof d->overview);
    read_genres(body, toks, n, 0, d->genres, sizeof d->genres);
    jf_get_str(body, toks, n, 0, "OfficialRating", d->rating, sizeof d->rating);
    int cr = json_obj_get(body, toks, n, 0, "CommunityRating");
    double v;
    if (cr >= 0 && json_num(body, &toks[cr], &v) && v > 0) d->community_x10 = clamp_i((int)(v * 10.0 + 0.5), 0, 100);
    d->width  = clamp_i(jf_get_int(body, toks, n, 0, "Width", 0), 0, 99999);
    d->height = clamp_i(jf_get_int(body, toks, n, 0, "Height", 0), 0, 99999);
    int m = first_source(body, toks, n, 0);
    if (m >= 0) {
        read_source(d, body, toks, n, m);
        if (d->it.b.runtime_s == 0)               /* the source's runtime when the item has none */
            d->it.b.runtime_s = (int)jf_get_ticks_s(body, toks, n, m, "RunTimeTicks");
    }
    read_people(d, body, toks, n, 0);
    return JF_PARSE_OK;
}

void jv_item_art(const JvItem *it, const char **id, const char **tag)
{
    if (it->b.image_tag[0]) { *id = it->b.id; *tag = it->b.image_tag; }
    else if (it->series_id[0] && it->series_image_tag[0]) { *id = it->series_id; *tag = it->series_image_tag; }
    else { *id = ""; *tag = ""; }
}
