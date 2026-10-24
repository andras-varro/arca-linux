/*
 * jv_model.h — the video client's reply parsing: item pages (movies, shows,
 * seasons, episodes, libraries, search hits) and one item's detail (overview,
 * people, the audio and subtitle streams of its first media source).  Pure: no
 * socket, no allocation, no file.  The caller owns the body, the token array
 * and the storage.
 *
 * Built on the shared walker (jf_shared/jf_model.h): a list is a visitor on
 * jf_items_walk, so the same rules hold as for the music client: the whole body
 * is validated first, a non-OK return changes nothing, and `loaded` counts
 * server items CONSUMED (the next StartIndex), skipped ones included.  A detail
 * reply is a bare object, not an Items page, so it is parsed with json_parse
 * and the same field getters; on a non-OK return the JvDetail is zeroed.
 *
 * Both parsers have the JfParseFn shape (body, len, toks, ntoks, dst) so they can
 * be handed to a jf_api PARSE request as they are.
 *
 * Storage is fixed: nothing here grows.  What does not fit is dropped and
 * counted (list: `full`; detail: audio_dropped, subs_dropped), never truncated
 * into a wrong entry.
 */

#ifndef ARCA_JV_MODEL_H
#define ARCA_JV_MODEL_H

#include <stdbool.h>
#include <stddef.h>
#include "jf_model.h"
#include "jv_tracks.h"      /* JvStream: the track structs the selection logic reads */

/** Token budget of a detail reply.  Measured on a real movie with 18 people, 5 streams and chapters: 18.4 KB, 1269 tokens. */
#define JV_TOKENS_DETAIL  3000

#define JV_MAX_AUDIO   16   /**< audio streams kept of the first media source */
#define JV_MAX_SUBS    24   /**< subtitle streams kept (image ones included: jv_tracks skips them) */
#define JV_MAX_PEOPLE  16   /**< directors first, then cast */

/** What a row is.  Types the client does not show are skipped (but counted). */
typedef enum {
    JV_NONE = 0,
    JV_LIB_MOVIES,          /**< a Views entry with CollectionType "movies" */
    JV_LIB_SHOWS,           /**< a Views entry with CollectionType "tvshows" */
    JV_MOVIE, JV_SERIES, JV_SEASON, JV_EPISODE,
    JV_BOXSET, JV_PERSON, JV_GENRE
} JvType;

typedef struct {                    /* one list row */
    JfBase b;                       /**< id, name, year, runtime_s, Primary tag, played, position_s, unplayed */
    JvType type;
    char series_id[33];             /**< SeriesId (season, episode), else "" */
    char series_name[64];
    char series_image_tag[40];      /**< SeriesPrimaryImageTag: the poster a season or episode without its own falls back to */
    char season_id[33];             /**< an episode's SeasonId */
    int  season_no;                 /**< an episode's ParentIndexNumber, a season's IndexNumber; 0 if absent */
    int  episode_no;                /**< an episode's IndexNumber; 0 if absent */
    int  episode_end;               /**< IndexNumberEnd of a double episode; 0 otherwise */
} JvItem;

typedef struct {
    JvItem *items;
    int cap;
    int n;                  /**< items stored */
    int loaded;             /**< server items consumed so far = the next StartIndex */
    int total;              /**< TotalRecordCount of the last page that had one */
    bool full;              /**< an item was dropped because n == cap */
    bool pending;           /**< caller's flag: a page request is in flight */
    unsigned generation;    /**< caller's counter: bump to discard a stale reply */
} JvList;

typedef struct { char name[48], role[48], id[33]; bool director; } JvPerson;

typedef struct {                    /* one at a time, ~7 KB */
    JvItem it;
    char overview[1200];
    char genres[128];               /**< names joined by ", "; one that does not fit whole is dropped */
    char rating[12];                /**< OfficialRating ("R", "12"), "" if absent */
    int  community_x10;             /**< CommunityRating * 10 rounded, 0 if absent */
    char msid[33];                  /**< MediaSources[0].Id, "" when the item has no media source */
    char container[16];             /**< MediaSources[0].Container (first of a comma list) */
    int  width, height;             /**< the picture, 0 if unknown */
    JvStream audio[JV_MAX_AUDIO];  int n_audio, audio_dropped;
    JvStream subs[JV_MAX_SUBS];    int n_subs,  subs_dropped;
    JvPerson people[JV_MAX_PEOPLE]; int n_people;
} JvDetail;

void jv_list_init(JvList *l, JvItem *storage, int cap);
/** n = loaded = total = 0 and full = false; generation, pending and storage stay. */
void jv_list_clear(JvList *l);

/** Append a page's Items to a JvList (`dst`).  TotalRecordCount, when present, sets total.  A JfParseFn. */
int jv_parse_items(const char *body, size_t len, JsonTok *toks, int ntoks, void *dst);

/** One item's reply (GET /Users/<uid>/Items/<id>) into a JvDetail (`dst`); zeroed on any non-OK return.  A JfParseFn. */
int jv_parse_detail(const char *body, size_t len, JsonTok *toks, int ntoks, void *dst);

/**
 * Which image to ask for: the item's own Primary when it has one, else (season, episode) the series' poster.
 * `id`/`tag` are set to "" when there is no image at all.  The pointers stay valid as long as `it` does.
 */
void jv_item_art(const JvItem *it, const char **id, const char **tag);

#endif /* ARCA_JV_MODEL_H */
