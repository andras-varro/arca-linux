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

/** Token budget for one page (~20 B per JsonTok = ~160 KB heap).  Measured on the live server: a pool page of 40 tracks with Fields=Genres needs up to 5259 tokens (4 of 50 pages failed at 5000: UNEXPECTED REPLY), an instant mix of 50 up to 4399. */
#define JF_TOKENS_PAGE  8000

/** Bytes of a track's genre names; JF_MIX_OPT_MAX in jf_core.h counts what fits. */
#define JF_GENRES_BYTES 160

typedef enum { JF_ITEM_NONE = 0, JF_ITEM_ARTIST, JF_ITEM_ALBUM, JF_ITEM_TRACK,
               JF_ITEM_PLAYLIST, JF_ITEM_GENRE,
               JF_ITEM_LIBRARY   /* a music library (CollectionType "music") of the Views reply */ } JfItemType;

typedef struct {
    char id[33];
    char name[96];
    char artist[64];        /**< Artists[0], else AlbumArtist when Artists is empty; empty for an artist */
    char album_artist[64];  /**< AlbumArtist of a track or album, empty if absent */
    char album[64];
    char album_id[33];      /**< a track's AlbumId; an album's own Id; empty for other types or when absent */
    char image_tag[40];     /**< a track's AlbumPrimaryImageTag, an album's ImageTags.Primary; empty if absent (EnableImages=false requests carry none) */
    JfItemType type;
    int index;              /**< IndexNumber (track number), 0 if absent */
    int runtime_s;
    int norm_gain_mdb;      /**< NormalizationGain dB * 1000, rounded; 0 if absent */
    int year;               /**< ProductionYear, 0 if absent */
    int count;              /**< SongCount of a genre (Fields=ItemCounts), 0 if absent */
    char artist_id[33];     /**< ArtistItems[0].Id of a track, empty if absent */
    char genres[JF_GENRES_BYTES];  /**< a track's Genres (Fields=Genres): each name whole, each ended by a newline; one that does not fit is dropped */
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

/**
 * The key two genre spellings share: ASCII-lowercased, every run of whitespace, '-' and '_' one space, the ends
 * trimmed, a trailing " music" cut ("Hip-Hop" and "Hip Hop Music" are both "hip hop").  Writes at most cap-1 bytes;
 * "" when nothing is left.  Returns the length.
 */
int jf_genre_key(char *out, size_t cap, const char *name);

void jf_list_init(JfList *l, JfItem *storage, int cap);
/** n = loaded = total = 0 and full = false; generation, pending and storage stay. */
void jf_list_clear(JfList *l);

/** Append a page's Items to `out`.  TotalRecordCount, when present, sets out->total. */
int jf_parse_items(const char *body, size_t len, JsonTok *toks, int ntoks, JfList *out);

/** AuthenticateByName reply -> token and user id; both emptied on failure. */
int jf_parse_auth(const char *body, size_t len, JsonTok *toks, int ntoks,
                  char *token, size_t token_cap, char *user_id, size_t uid_cap);


/** Characters of an artist name a list row's detail text shows; longer is cut to JF_ROW_ARTIST_CHARS-2 characters + "..". */
#define JF_ROW_ARTIST_CHARS 22

/**
 * Does a list row show the track's artist?  Queue, search, mixes and playlists: always for a track.  An ALBUM's own
 * track list (`album_list` true): only when the track's artist differs (ASCII case-insensitive) from the album
 * artist, i.e. on a compilation.  False for anything that is not a track.
 */
bool jf_row_shows_artist(bool album_list, const JfItem *it);

/**
 * The detail text of a track row: "ARTIST  M:SS" (artist cut on a UTF-8 boundary to JF_ROW_ARTIST_CHARS characters,
 * the cut marked ".."), or "M:SS" when `show_artist` is false or the artist is empty.  A track without a runtime
 * has no time part.  Always NUL-terminated within `n` (n >= 1).
 */
void jf_row_detail(const JfItem *it, bool show_artist, char *out, int n);

#endif
