/*
 * jv_url.h — the video client's endpoint builders: lists, detail, the MPEG
 * stream, the SRT subtitle, playback reports and the encoding-job cleanup.
 * Same contract as jf_url.h: a NUL-terminated string in `out`, its length
 * returned, or -1 when it does not fit `cap` or an id is not 32 hex digits.
 * The generic half (encoding, ids, views, scope, session id) is jf_shared/jf_url.h.
 *
 * The token is NOT here: it travels only in the Authorization header, and the
 * server refuses it in a query.  The DeviceId in a stream URL names the device
 * so the encoding job can be found and deleted again; it is percent-encoded.
 *
 * Time is milliseconds at this interface and 100 ns ticks on the wire.
 */

#ifndef ARCA_JV_URL_H
#define ARCA_JV_URL_H

#include "jf_url.h"

#define JV_STREAM_MAX_W   400   /**< widest picture asked of the server (the decoder's budget) */
#define JV_STREAM_MIN_W    64
#define JV_STREAM_H       240   /**< maxHeight; the width limit decides for 16:9 */
#define JV_STREAM_VBITRATE 800000

/** Lists.  `lib` is a library id or ""/NULL for all libraries; page = start/limit. */
int jv_path_resume(char *out, size_t cap, const char *uid, int limit);
int jv_path_next_up(char *out, size_t cap, const char *uid, int limit);
int jv_path_movies(char *out, size_t cap, const char *uid, const char *lib, int start, int limit);
int jv_path_shows(char *out, size_t cap, const char *uid, const char *lib, int start, int limit);
int jv_path_seasons(char *out, size_t cap, const char *uid, const char *series_id);
int jv_path_episodes(char *out, size_t cap, const char *uid, const char *series_id, const char *season_id);
int jv_path_similar(char *out, size_t cap, const char *uid, const char *id, int limit);
/** Search is three requests: items, people, genres. */
int jv_path_search_items(char *out, size_t cap, const char *uid, const char *term, int limit);
int jv_path_search_people(char *out, size_t cap, const char *term, int limit);
int jv_path_search_genres(char *out, size_t cap, const char *term, int limit);
int jv_path_by_person(char *out, size_t cap, const char *uid, const char *person_id, int start, int limit);
int jv_path_by_genre(char *out, size_t cap, const char *uid, const char *genre_id, int start, int limit);
/** One item with People, Overview, Genres and MediaSources (the streams). */
int jv_path_detail(char *out, size_t cap, const char *uid, const char *id);

/**
 * The MPEG-1 + MP2 program stream.  `channels` 1 or 2 (anything else is 1), `rate` the mix bus rate in Hz
 * (8000..48000), `max_w` JV_STREAM_MIN_W..JV_STREAM_MAX_W, `audio_index` the server's stream index (<0 leaves it out
 * and the server picks), `start_ms` >= 0 (0 leaves StartTimeTicks out), `session_id` 32 hex from jf_session_id()
 * and fresh for every request, `device_id` non-empty.
 */
int jv_path_stream(char *out, size_t cap, const char *id, const char *media_source_id, int channels, int rate,
                   int max_w, int audio_index, long start_ms, const char *session_id, const char *device_id);
/** The subtitle track as SRT (the server converts every text codec); `index` is the stream index, >= 0. */
int jv_path_subtitle(char *out, size_t cap, const char *id, const char *media_source_id, int index);
/** DELETE target that ends a transcode job; both values are checked (device id percent-encoded). */
int jv_path_end_job(char *out, size_t cap, const char *device_id, const char *session_id);

/** POST targets of the three playback reports. */
int jv_path_playing(char *out, size_t cap);
int jv_path_progress(char *out, size_t cap);
int jv_path_stopped(char *out, size_t cap);

typedef struct {
    const char *item_id;          /**< 32 hex */
    const char *media_source_id;  /**< 32 hex */
    const char *session_id;       /**< 32 hex */
    long position_ms;             /**< >= 0 */
    int audio_index;              /**< <0 sends 0 */
    int sub_index;                /**< <0 = off, sent as -1 */
    int paused;
    const char *event;            /**< progress only: NULL, "timeupdate", "pause", "unpause" or "seek" */
} JvReport;

/** JSON bodies.  Started and progress carry every field; stopped only item, source, session and position. */
int jv_body_started(char *out, size_t cap, const JvReport *r);
int jv_body_progress(char *out, size_t cap, const JvReport *r);
int jv_body_stopped(char *out, size_t cap, const JvReport *r);

#endif /* ARCA_JV_URL_H */
