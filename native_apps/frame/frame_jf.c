/*
 * frame_jf.c — see frame_jf.h.  Ids reach a URL path, so jf_id_valid() gates every builder.
 */
#include "frame_jf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "json.h"
#include "jf_url.h"

static int fin(int n, size_t cap) { return (n < 0 || (size_t)n >= cap) ? -1 : n; }

int fj_path_views(char *out, size_t cap, const char *uid) { return jf_path_views(out, cap, uid); }

static int items_path(char *out, size_t cap, const char *uid, const char *parent, int start, int limit, const char *mid)
{
    if (!out || cap == 0 || !jf_id_valid(uid) || !jf_id_valid(parent) || start < 0 || limit < 0) return -1;
    return fin(snprintf(out, cap, "/Users/%s/Items?ParentId=%s%s&SortBy=SortName&SortOrder=Ascending"
                        "&StartIndex=%d&Limit=%d", uid, parent, mid, start, limit), cap);
}

int fj_path_children(char *out, size_t cap, const char *uid, const char *parent, int start, int limit)
{
    return items_path(out, cap, uid, parent, start, limit, "&IncludeItemTypes=PhotoAlbum,Folder,Photo");
}

int fj_path_photos(char *out, size_t cap, const char *uid, const char *parent, int start, int limit)
{
    return items_path(out, cap, uid, parent, start, limit, "&Recursive=true&IncludeItemTypes=Photo");
}

int fj_path_image(char *out, size_t cap, const char *id, int max_w, int max_h)
{
    if (!out || cap == 0 || !jf_id_valid(id) || max_w < 1 || max_h < 1) return -1;
    return fin(snprintf(out, cap, "/Items/%s/Images/Primary?maxWidth=%d&maxHeight=%d&quality=90&format=Jpg",
                        id, max_w, max_h), cap);
}

int fj_path_original(char *out, size_t cap, const char *id)
{
    if (!out || cap == 0 || !jf_id_valid(id)) return -1;
    return fin(snprintf(out, cap, "/Items/%s/Download", id), cap);
}

int fj_key_from_id(char *out, size_t cap, const char *id)
{
    if (!out || cap < JF_ID_LEN + 8 || !jf_id_valid(id)) return -1;
    memcpy(out, "jf-", 3);
    for (int i = 0; i < JF_ID_LEN; i++) {
        char c = id[i];
        out[3 + i] = (c >= 'A' && c <= 'F') ? (char)(c - 'A' + 'a') : c;
    }
    memcpy(out + 3 + JF_ID_LEN, ".jpg", 5);
    return 3 + JF_ID_LEN + 4;
}

/* Parse into a heap token array sized by json_count(); NULL on a bad document. */
static JsonTok *tokens(const char *json, size_t len, int *ntok)
{
    if (!json) return NULL;
    int n = json_count(json, len);
    if (n < 1) return NULL;
    JsonTok *t = malloc((size_t)n * sizeof *t);
    if (!t) return NULL;
    if (json_parse(json, len, t, n) != n) { free(t); return NULL; }
    *ntok = n;
    return t;
}

static int get_str(const char *s, const JsonTok *t, int n, int obj, const char *key, char *out, size_t cap)
{
    out[0] = '\0';
    int i = json_obj_get(s, t, n, obj, key);
    if (i < 0 || t[i].type != JSON_STRING) return 0;
    int r = json_str(s, &t[i], out, cap);
    if (r < 0) { out[0] = '\0'; return 0; }
    return r;
}

static FjKind kind_of(const char *ty)
{
    if (!strcmp(ty, "Photo")) return FJ_PHOTO;
    if (!strcmp(ty, "PhotoAlbum") || !strcmp(ty, "Folder") || !strcmp(ty, "CollectionFolder") || !strcmp(ty, "BoxSet"))
        return FJ_ALBUM;
    return FJ_OTHER;
}

/* libs: Views reply (filter on CollectionType) rather than a Type-tagged item list. */
static int parse(const char *json, size_t len, FjItem *out, int max, int *total, int libs)
{
    int n = 0, count = 0, stored = 0;
    JsonTok *t = tokens(json, len, &n);
    if (!t || !out || max < 0 || t[0].type != JSON_OBJECT) { free(t); return -1; }
    int arr = json_obj_get(json, t, n, 0, "Items");
    if (arr < 0 || t[arr].type != JSON_ARRAY) { free(t); return -1; }
    for (int e = json_arr_first(t, n, arr); e >= 0; e = json_arr_next(t, n, arr, e)) {
        count++;
        if (t[e].type != JSON_OBJECT || stored >= max) continue;
        char id[48], ty[32], ct[24];
        FjItem *it = &out[stored];
        FjKind k;
        if (libs) {
            if (!get_str(json, t, n, e, "CollectionType", ct, sizeof ct)) continue;
            if (strcasecmp(ct, "photos") && strcasecmp(ct, "homevideos")) continue;
            k = FJ_ALBUM;
        } else {
            get_str(json, t, n, e, "Type", ty, sizeof ty);
            k = kind_of(ty);
        }
        if (get_str(json, t, n, e, "Id", id, sizeof id) != JF_ID_LEN || !jf_id_valid(id)) continue;
        memcpy(it->id, id, JF_ID_LEN + 1);
        get_str(json, t, n, e, "Name", it->name, sizeof it->name);
        it->kind = k;
        stored++;
    }
    if (total) {
        long long v;
        int ti = json_obj_get(json, t, n, 0, "TotalRecordCount");
        *total = (ti >= 0 && json_i64(json, &t[ti], &v) && v >= 0 && v <= 0x7fffffff) ? (int)v : count;
    }
    free(t);
    return stored;
}

int fj_parse_items(const char *json, size_t len, FjItem *out, int max, int *total)
{
    return parse(json, len, out, max, total, 0);
}

int fj_parse_libraries(const char *json, size_t len, FjItem *out, int max)
{
    return parse(json, len, out, max, NULL, 1);
}
