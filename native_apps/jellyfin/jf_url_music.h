/*
 * jf_url_music.h — Ludfin's music-only endpoint builders (artists, albums, tracks,
 * genres, instant mix, the audio stream, playlist edits).  Same contract as jf_url.h:
 * a NUL-terminated string in `out`, its length returned, or -1 when it does not fit
 * `cap` or an id is not 32 hex digits.  The generic half is in jf_shared/jf_url.h.
 */

#ifndef ARCA_JF_URL_MUSIC_H
#define ARCA_JF_URL_MUSIC_H

#include "jf_url.h"

#define JF_MIX_LIMIT     50     /**< items asked of an instant mix: 100 is ~93 KB and ~6600 tokens, over the page buffers */

int jf_path_artists(char *out, size_t cap, const char *uid, int start, int limit);
int jf_path_albums(char *out, size_t cap, const char *uid, const char *artist_id, int start, int limit);
int jf_path_tracks(char *out, size_t cap, const char *uid, const char *album_id, int start, int limit);
int jf_path_search(char *out, size_t cap, const char *uid, const char *term, int start, int limit);
/** The root browse lists: every album, playlists, genres, every song. */
int jf_path_albums_all(char *out, size_t cap, const char *uid, int start, int limit);
int jf_path_playlists(char *out, size_t cap, const char *uid, int start, int limit);
int jf_path_genres(char *out, size_t cap, const char *uid, int start, int limit);
int jf_path_songs(char *out, size_t cap, const char *uid, int start, int limit);
/** A playlist's tracks, in playlist order. */
int jf_path_playlist_items(char *out, size_t cap, const char *uid, const char *playlist_id, int start, int limit);
/** The tracks whose own genre is `genre_id` (GenreIds on Audio: measured to match the track's Genres, TotalRecordCount is the track count). */
int jf_path_genre_tracks(char *out, size_t cap, const char *uid, const char *genre_id, int start, int limit);
/** An instant mix seeded by one track: Audio items, the seed first, parsed like any list; images and user data left out (-30% bytes). */
int jf_path_instant_mix(char *out, size_t cap, const char *uid, const char *track_id, int limit);
/** The genre index with SongCount (Fields=ItemCounts; the plain list has none): 100 per page is ~38 KB and ~3700 tokens. */
int jf_path_genre_index(char *out, size_t cap, const char *uid, int start, int limit);
/**
 * Audio items for a MIX.  `genre_ids` joined by ',' (the server ORs them; a '|' is ignored and returns the whole library),
 * `artist_id` NULL/"" = any artist, `decade` 0 = any year else the decade's first year (1980 -> Years=1980,...,1989),
 * `random` = SortBy=Random else SortName (a stable order to page with `start`).  Fields=Genres, no images, no user data.
 */
int jf_path_mix_pool(char *out, size_t cap, const char *uid, const char *const *genre_ids, int n_genre_ids,
                     const char *artist_id, int decade, bool random, int start, int limit);
/** `session_id` is 32 hex from jf_session_id(); a fresh one per request, or the server reuses an old transcode job. */
/** `channels` is the audioChannels asked of the server: 2 for a stereo sink, anything else 1 (the device has no downmix for a mono sink to spare). */
int jf_path_stream(char *out, size_t cap, const char *id, const char *session_id, int channels);
/** The same stream starting `start_ms` into the track (StartTimeTicks, 100 ns ticks; measured to seek on Jellyfin 12.1.0 with a fresh session id).  0 = jf_path_stream.  -1 for start_ms < 0. */
int jf_path_stream_at(char *out, size_t cap, const char *id, const char *session_id, long start_ms, int channels);
/**
 * POST target that adds `n` (1..) tracks to a playlist: "/Playlists/<pl>/Items?ids=a,b,..&userId=<uid>".  The ids go in the
 * query, 33 bytes each, so the path cap decides how many fit: 10 fit JF_API_PATH_MAX; a larger add is several requests
 * (callers batch 10).  -1 when an id is bad, `n` < 1, or the path does not fit `cap`.
 */
int jf_path_playlist_add(char *out, size_t cap, const char *uid, const char *playlist_id,
                         const char *const *ids, int n);
/** One page of a playlist's tracks, the fields the playlist view needs and nothing else. */
int jf_path_playlist_ids(char *out, size_t cap, const char *uid, const char *playlist_id, int start, int limit);
/** An artist's albums (AlbumArtistIds: the albums the artist is the album artist of). */
int jf_path_artist_albums(char *out, size_t cap, const char *uid, const char *artist_id, int start, int limit);
/** An artist's tracks, in album order (ArtistIds: every track the artist performs on, compilations included). */
int jf_path_artist_tracks(char *out, size_t cap, const char *uid, const char *artist_id, int start, int limit);
/** The same query as jf_path_artist_tracks, for the whole artist; one implementation behind both names. */
int jf_path_artist_all(char *out, size_t cap, const char *uid, const char *artist_id, int start, int limit);

#endif
