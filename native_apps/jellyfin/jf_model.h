/*
 * jf_model.h — the Jellyfin client's reply parsing: item pages and the login
 * reply, turned into fixed-size structs.  Pure: no socket, no allocation, no
 * file.  The caller owns the body, the token array and the item storage.
 *
 * Every parse validates the whole document first (json_parse), so a body that
 * was cut short or is not JSON changes nothing: on any non-OK return the list
 * is untouched (the auth outputs are emptied instead).
 *
 * ⚠️ `loaded` counts server items CONSUMED, not items stored: it is the next
 * StartIndex.  An item of a type this client does not show, or with a bad Id, is
 * skipped but still counted, or the next page would start inside the same
 * items and the list would never reach the end.
 */

#ifndef ARCA_JF_MODEL_H
#define ARCA_JF_MODEL_H

#include <stdbool.h>
#include <stddef.h>
#include "json.h"

/** Token budget for one page (~20 B per JsonTok = ~100 KB); a page of 50 tracks with MediaSources noise fits. */
#define JF_TOKENS_PAGE  5000

typedef enum { JF_ITEM_NONE = 0, JF_ITEM_ARTIST, JF_ITEM_ALBUM, JF_ITEM_TRACK } JfItemType;

typedef struct {
    char id[33];
    char name[96];
    char artist[64];        /**< AlbumArtist, else Artists[0]; empty for an artist */
    char album[64];
    char image_tag[40];     /**< reserved, always empty for now */
    JfItemType type;
    int index;              /**< IndexNumber (track number), 0 if absent */
    int runtime_s;
    int norm_gain_mdb;      /**< NormalizationGain dB * 1000, rounded; 0 if absent */
} JfItem;

typedef struct {
    JfItem *items;
    int cap;
    int n;                  /**< items stored */
    int loaded;             /**< server items consumed so far = the next StartIndex */
    int total;              /**< TotalRecordCount of the last page that had one */
    bool full;              /**< an item was dropped because n == cap */
    bool pending;           /**< caller's flag: a page request is in flight */
    unsigned generation;    /**< caller's counter: bump to discard a stale reply */
} JfList;

typedef enum {
    JF_PARSE_OK = 0,
    JF_PARSE_BAD,           /**< not JSON, or no Items array / wrong auth shape */
    JF_PARSE_TRUNC,         /**< the body ended early */
    JF_PARSE_NOMEM          /**< `toks` too small for this document */
} JfParse;

void jf_list_init(JfList *l, JfItem *storage, int cap);
/** n = loaded = total = 0 and full = false; generation, pending and storage stay. */
void jf_list_clear(JfList *l);

/** Append a page's Items to `out`.  TotalRecordCount, when present, sets out->total. */
int jf_parse_items(const char *body, size_t len, JsonTok *toks, int ntoks, JfList *out);

/** AuthenticateByName reply -> token and user id; both emptied on failure. */
int jf_parse_auth(const char *body, size_t len, JsonTok *toks, int ntoks,
                  char *token, size_t token_cap, char *user_id, size_t uid_cap);

#endif
