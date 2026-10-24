#include "jf_url_music.h"

#include <stdio.h>
#include <string.h>

static int fin(int n, size_t cap) { return (n < 0 || (size_t)n >= cap) ? -1 : n; }

int jf_path_artists(char *out, size_t cap, const char *uid, int start, int limit)
{
    if (!jf_id_valid(uid)) return -1;
    return fin(snprintf(out, cap, "/Artists/AlbumArtists?userId=%s&StartIndex=%d&Limit=%d",
                        uid, start, limit), cap);
}

int jf_path_albums(char *out, size_t cap, const char *uid, const char *artist_id, int start, int limit)
{
    if (!jf_id_valid(uid) || !jf_id_valid(artist_id)) return -1;
    return fin(snprintf(out, cap,
                        "/Items?userId=%s&ArtistIds=%s&IncludeItemTypes=MusicAlbum&Recursive=true"
                        "&SortBy=SortName&StartIndex=%d&Limit=%d", uid, artist_id, start, limit), cap);
}

int jf_path_tracks(char *out, size_t cap, const char *uid, const char *album_id, int start, int limit)
{
    if (!jf_id_valid(uid) || !jf_id_valid(album_id)) return -1;
    return fin(snprintf(out, cap,
                        "/Items?userId=%s&ParentId=%s&IncludeItemTypes=Audio"
                        "&SortBy=ParentIndexNumber,IndexNumber&StartIndex=%d&Limit=%d",
                        uid, album_id, start, limit), cap);
}

int jf_path_search(char *out, size_t cap, const char *uid, const char *term, int start, int limit)
{
    char enc[256];
    if (!jf_id_valid(uid) || !term || jf_url_encode(enc, sizeof(enc), term) < 0) return -1;
    return fin(snprintf(out, cap,
                        "/Items?userId=%s&IncludeItemTypes=Audio,MusicAlbum,MusicArtist&Recursive=true"
                        "&SearchTerm=%s&StartIndex=%d&Limit=%d", uid, enc, start, limit), cap);
}

int jf_path_albums_all(char *out, size_t cap, const char *uid, int start, int limit)
{
    if (!jf_id_valid(uid)) return -1;
    return fin(snprintf(out, cap,
                        "/Items?userId=%s&IncludeItemTypes=MusicAlbum&Recursive=true"
                        "&SortBy=SortName&StartIndex=%d&Limit=%d", uid, start, limit), cap);
}

int jf_path_playlists(char *out, size_t cap, const char *uid, int start, int limit)
{
    if (!jf_id_valid(uid)) return -1;
    return fin(snprintf(out, cap,
                        "/Items?userId=%s&IncludeItemTypes=Playlist&Recursive=true&Fields=ChildCount"
                        "&StartIndex=%d&Limit=%d", uid, start, limit), cap);
}

int jf_path_genres(char *out, size_t cap, const char *uid, int start, int limit)
{
    if (!jf_id_valid(uid)) return -1;
    return fin(snprintf(out, cap, "/MusicGenres?userId=%s&StartIndex=%d&Limit=%d", uid, start, limit), cap);
}

int jf_path_songs(char *out, size_t cap, const char *uid, int start, int limit)
{
    if (!jf_id_valid(uid)) return -1;
    return fin(snprintf(out, cap,
                        "/Items?userId=%s&IncludeItemTypes=Audio&Recursive=true"
                        "&SortBy=SortName&StartIndex=%d&Limit=%d", uid, start, limit), cap);
}

int jf_path_playlist_items(char *out, size_t cap, const char *uid, const char *playlist_id, int start, int limit)
{
    if (!jf_id_valid(uid) || !jf_id_valid(playlist_id)) return -1;
    return fin(snprintf(out, cap, "/Playlists/%s/Items?userId=%s&StartIndex=%d&Limit=%d",
                        playlist_id, uid, start, limit), cap);
}

int jf_path_genre_tracks(char *out, size_t cap, const char *uid, const char *genre_id, int start, int limit)
{
    if (!jf_id_valid(uid) || !jf_id_valid(genre_id)) return -1;   /* an empty GenreIds= lists the whole library */
    return fin(snprintf(out, cap,
                        "/Items?userId=%s&GenreIds=%s&IncludeItemTypes=Audio&Recursive=true"
                        "&SortBy=SortName&StartIndex=%d&Limit=%d", uid, genre_id, start, limit), cap);
}

int jf_path_stream(char *out, size_t cap, const char *id, const char *session_id, int channels)
{
    if (!jf_id_valid(id) || !jf_id_valid(session_id)) return -1;
    return fin(snprintf(out, cap,
                        "/Audio/%s/stream?static=false&container=wav&audioCodec=pcm_s16le"
                        "&audioSampleRate=44100&audioChannels=%d&playSessionId=%s",
                        id, channels == 2 ? 2 : 1, session_id), cap);
}

int jf_path_stream_at(char *out, size_t cap, const char *id, const char *session_id, long start_ms, int channels)
{
    if (start_ms < 0) return -1;
    int n = jf_path_stream(out, cap, id, session_id, channels);
    if (n < 0 || start_ms == 0) return n;
    return fin(snprintf(out + n, cap - (size_t)n, "&StartTimeTicks=%lld", (long long)start_ms * 10000LL) + n, cap);
}

int jf_path_instant_mix(char *out, size_t cap, const char *uid, const char *track_id, int limit)
{
    if (!jf_id_valid(uid) || !jf_id_valid(track_id)) return -1;
    return fin(snprintf(out, cap, "/Items/%s/InstantMix?userId=%s&Limit=%d"
                        "&EnableImages=false&EnableUserData=false", track_id, uid, limit), cap);
}

int jf_path_genre_index(char *out, size_t cap, const char *uid, int start, int limit)
{
    if (!jf_id_valid(uid)) return -1;
    return fin(snprintf(out, cap, "/MusicGenres?userId=%s&StartIndex=%d&Limit=%d&Fields=ItemCounts"
                        "&EnableImages=false&EnableUserData=false", uid, start, limit), cap);
}

int jf_path_mix_pool(char *out, size_t cap, const char *uid, const char *const *genre_ids, int n_genre_ids,
                     const char *artist_id, int decade, bool random, int start, int limit)
{
    if (!jf_id_valid(uid) || n_genre_ids < 0 || (artist_id && artist_id[0] && !jf_id_valid(artist_id))) return -1;
    if (decade != 0 && (decade < 1000 || decade > 9990 || decade % 10 != 0)) return -1;
    size_t w = 0;
    int r = snprintf(out, cap, "/Items?userId=%s&IncludeItemTypes=Audio&Recursive=true", uid);
    if (r < 0 || (size_t)r >= cap) return -1;
    w = (size_t)r;
    for (int k = 0; k < n_genre_ids; k++) {
        if (!genre_ids[k] || !jf_id_valid(genre_ids[k])) return -1;
        r = snprintf(out + w, cap - w, "%s%s", k == 0 ? "&GenreIds=" : ",", genre_ids[k]);   /* commas: the server ORs them, a '|' is ignored */
        if (r < 0 || (size_t)r >= cap - w) return -1;
        w += (size_t)r;
    }
    if (artist_id && artist_id[0]) {
        r = snprintf(out + w, cap - w, "&ArtistIds=%s", artist_id);
        if (r < 0 || (size_t)r >= cap - w) return -1;
        w += (size_t)r;
    }
    if (decade) {
        for (int y = 0; y < 10; y++) {
            r = snprintf(out + w, cap - w, "%s%d", y == 0 ? "&Years=" : ",", decade + y);
            if (r < 0 || (size_t)r >= cap - w) return -1;
            w += (size_t)r;
        }
    }
    r = snprintf(out + w, cap - w, "&SortBy=%s&StartIndex=%d&Limit=%d&Fields=Genres"
                 "&EnableImages=false&EnableUserData=false", random ? "Random" : "SortName", start, limit);
    if (r < 0 || (size_t)r >= cap - w) return -1;
    return (int)(w + (size_t)r);
}

int jf_path_playlist_add(char *out, size_t cap, const char *uid, const char *playlist_id,
                         const char *const *ids, int n)
{
    if (!out || cap == 0 || !jf_id_valid(uid) || !jf_id_valid(playlist_id) || !ids || n < 1) return -1;
    int r = snprintf(out, cap, "/Playlists/%s/Items?ids=", playlist_id);
    if (r < 0 || (size_t)r >= cap) return -1;
    size_t w = (size_t)r;
    for (int i = 0; i < n; i++) {
        if (!jf_id_valid(ids[i])) return -1;
        r = snprintf(out + w, cap - w, "%s%s", i ? "," : "", ids[i]);
        if (r < 0 || (size_t)r >= cap - w) return -1;
        w += (size_t)r;
    }
    r = snprintf(out + w, cap - w, "&userId=%s", uid);
    if (r < 0 || (size_t)r >= cap - w) return -1;
    return (int)(w + (size_t)r);
}

int jf_path_playlist_ids(char *out, size_t cap, const char *uid, const char *playlist_id, int start, int limit)
{
    if (!jf_id_valid(uid) || !jf_id_valid(playlist_id)) return -1;
    return fin(snprintf(out, cap, "/Playlists/%s/Items?userId=%s&StartIndex=%d&Limit=%d"
                        "&EnableImages=false&EnableUserData=false&Fields=", playlist_id, uid, start, limit), cap);
}

int jf_path_artist_albums(char *out, size_t cap, const char *uid, const char *artist_id, int start, int limit)
{
    if (!jf_id_valid(uid) || !jf_id_valid(artist_id)) return -1;
    return fin(snprintf(out, cap,
                        "/Items?userId=%s&AlbumArtistIds=%s&IncludeItemTypes=MusicAlbum&Recursive=true"
                        "&SortBy=SortName&StartIndex=%d&Limit=%d", uid, artist_id, start, limit), cap);
}

/* The artist's audio, album by album: shared by the tracks and the whole-artist queries. */
static int artist_audio(char *out, size_t cap, const char *uid, const char *artist_id, int start, int limit)
{
    if (!jf_id_valid(uid) || !jf_id_valid(artist_id)) return -1;
    return fin(snprintf(out, cap,
                        "/Items?userId=%s&ArtistIds=%s&IncludeItemTypes=Audio&Recursive=true"
                        "&SortBy=Album,ParentIndexNumber,IndexNumber&StartIndex=%d&Limit=%d",
                        uid, artist_id, start, limit), cap);
}

int jf_path_artist_tracks(char *out, size_t cap, const char *uid, const char *artist_id, int start, int limit)
{
    return artist_audio(out, cap, uid, artist_id, start, limit);
}

int jf_path_artist_all(char *out, size_t cap, const char *uid, const char *artist_id, int start, int limit)
{
    return artist_audio(out, cap, uid, artist_id, start, limit);
}
