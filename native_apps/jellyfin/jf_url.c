#include "jf_url.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static JfRandomFn rand_fn;

static bool unreserved(unsigned char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
           c == '-' || c == '_' || c == '.' || c == '~';
}

int jf_url_encode(char *out, size_t cap, const char *s)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t n = 0;
    if (!out || cap == 0 || !s) return -1;
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        size_t need = unreserved(c) ? 1 : 3;
        if (n + need + 1 > cap) { out[0] = '\0'; return -1; }
        if (need == 1) out[n++] = (char)c;
        else { out[n++] = '%'; out[n++] = hex[c >> 4]; out[n++] = hex[c & 15]; }
    }
    out[n] = '\0';
    return (int)n;
}

JfServerResult jf_parse_server(const char *in, char *host, size_t host_cap, int *port)
{
    if (!in || !host || host_cap == 0 || !port) return JF_SERVER_BAD;
    host[0] = '\0';
    if (strncmp(in, "https://", 8) == 0) return JF_SERVER_HTTPS;
    if (strncmp(in, "http://", 7) == 0) in += 7;
    size_t len = strlen(in);
    while (len > 0 && in[len - 1] == '/') len--;
    if (len == 0) return JF_SERVER_EMPTY;

    const char *colon = memchr(in, ':', len);
    size_t hlen = colon ? (size_t)(colon - in) : len;
    if (hlen == 0 || hlen + 1 > host_cap) return JF_SERVER_BAD;
    for (size_t i = 0; i < hlen; i++) {
        char c = in[i];
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                  c == '.' || c == '-' || c == '_';
        if (!ok) return JF_SERVER_BAD;
    }
    int p = JF_DEFAULT_PORT;
    if (colon) {
        size_t plen = len - hlen - 1;
        if (plen == 0 || plen > 5) return JF_SERVER_BAD;
        p = 0;
        for (size_t i = 0; i < plen; i++) {
            char c = colon[1 + i];
            if (c < '0' || c > '9') return JF_SERVER_BAD;
            p = p * 10 + (c - '0');
        }
        if (p < 1 || p > 65535) return JF_SERVER_BAD;
    }
    memcpy(host, in, hlen);
    host[hlen] = '\0';
    *port = p;
    return JF_SERVER_OK;
}

bool jf_id_valid(const char *id)
{
    if (!id) return false;
    for (int i = 0; i < JF_ID_LEN; i++) {
        char c = id[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
    }
    return id[JF_ID_LEN] == '\0';
}

static int fin(int n, size_t cap) { return (n < 0 || (size_t)n >= cap) ? -1 : n; }

int jf_path_auth(char *out, size_t cap)
{
    return fin(snprintf(out, cap, "/Users/AuthenticateByName"), cap);
}

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
                        "/Items?userId=%s&IncludeItemTypes=Playlist&Recursive=true"
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

int jf_path_genre_albums(char *out, size_t cap, const char *uid, const char *genre_id, int start, int limit)
{
    if (!jf_id_valid(uid) || !jf_id_valid(genre_id)) return -1;
    return fin(snprintf(out, cap,
                        "/Items?userId=%s&GenreIds=%s&IncludeItemTypes=MusicAlbum&Recursive=true"
                        "&SortBy=SortName&StartIndex=%d&Limit=%d", uid, genre_id, start, limit), cap);
}

int jf_path_stream(char *out, size_t cap, const char *id, const char *session_id)
{
    if (!jf_id_valid(id) || !jf_id_valid(session_id)) return -1;
    return fin(snprintf(out, cap,
                        "/Audio/%s/stream?static=false&container=wav&audioCodec=pcm_s16le"
                        "&audioSampleRate=44100&audioChannels=1&playSessionId=%s", id, session_id), cap);
}

void jf_url_set_random(JfRandomFn fn) { rand_fn = fn; }

static int urandom_read(unsigned char *buf, size_t n)
{
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) return -1;
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, buf + got, n - got);
        if (r <= 0) { close(fd); return -1; }
        got += (size_t)r;
    }
    close(fd);
    return (int)got;
}

bool jf_session_id(char out[JF_ID_LEN + 1])
{
    static const char hex[] = "0123456789abcdef";
    unsigned char r[JF_ID_LEN / 2];
    int got = (rand_fn ? rand_fn : urandom_read)(r, sizeof(r));
    if (got != (int)sizeof(r)) { out[0] = '\0'; return false; }
    for (size_t i = 0; i < sizeof(r); i++) {
        out[2 * i]     = hex[r[i] >> 4];
        out[2 * i + 1] = hex[r[i] & 15];
    }
    out[JF_ID_LEN] = '\0';
    return true;
}

static bool header_safe(const char *s)
{
    for (; *s; s++) if (*s == '"' || *s == '\r' || *s == '\n' || *s == '\\') return false;
    return true;
}

int jf_auth_header(char *out, size_t cap, const char *device, const char *device_id, const char *token)
{
    /* an empty DeviceId is answered 500 by AuthenticateByName */
    if (!device || !device_id || !*device || !*device_id || !header_safe(device) || !header_safe(device_id)) return -1;
    if (token && *token) {
        if (!header_safe(token)) return -1;
        return fin(snprintf(out, cap,
                            "Authorization: MediaBrowser Client=\"Arca\", Device=\"%s\", DeviceId=\"%s\", "
                            "Version=\"1\", Token=\"%s\"\r\n", device, device_id, token), cap);
    }
    return fin(snprintf(out, cap,
                        "Authorization: MediaBrowser Client=\"Arca\", Device=\"%s\", DeviceId=\"%s\", "
                        "Version=\"1\"\r\n", device, device_id), cap);
}

static int json_put(char *out, size_t cap, size_t n, const char *s)
{
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        char tmp[8];
        int tl;
        if (c == '"' || c == '\\') tl = snprintf(tmp, sizeof(tmp), "\\%c", c);
        else if (c < 0x20)         tl = snprintf(tmp, sizeof(tmp), "\\u%04x", c);
        else { tmp[0] = (char)c; tmp[1] = '\0'; tl = 1; }
        if (n + (size_t)tl + 1 > cap) return -1;
        memcpy(out + n, tmp, (size_t)tl);
        n += (size_t)tl;
    }
    out[n] = '\0';
    return (int)n;
}

static int json_lit(char *out, size_t cap, size_t n, const char *s)
{
    size_t l = strlen(s);
    if (n + l + 1 > cap) return -1;
    memcpy(out + n, s, l + 1);
    return (int)(n + l);
}

int jf_auth_body(char *out, size_t cap, const char *user, const char *password)
{
    if (!out || cap == 0 || !user || !password) return -1;
    int n = json_lit(out, cap, 0, "{\"Username\":\"");
    if (n >= 0) n = json_put(out, cap, (size_t)n, user);
    if (n >= 0) n = json_lit(out, cap, (size_t)n, "\",\"Pw\":\"");
    if (n >= 0) n = json_put(out, cap, (size_t)n, password);
    if (n >= 0) n = json_lit(out, cap, (size_t)n, "\"}");
    if (n < 0) out[0] = '\0';
    return n;
}

int jf_path_instant_mix(char *out, size_t cap, const char *uid, const char *track_id, int limit)
{
    if (!jf_id_valid(uid) || !jf_id_valid(track_id)) return -1;
    return fin(snprintf(out, cap, "/Items/%s/InstantMix?userId=%s&Limit=%d"
                        "&EnableImages=false&EnableUserData=false", track_id, uid, limit), cap);
}

int jf_path_item_by_id(char *out, size_t cap, const char *uid, const char *id)
{
    if (!jf_id_valid(uid) || !jf_id_valid(id)) return -1;
    return fin(snprintf(out, cap, "/Items?userId=%s&Ids=%s&Fields=Genres"
                        "&EnableImages=false&EnableUserData=false", uid, id), cap);
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

int jf_path_playlist_create(char *out, size_t cap)
{
    return fin(snprintf(out, cap, "/Playlists"), cap);
}

int jf_save_body(char *out, size_t cap, const char *name, const char *uid, const char *const *ids, int n)
{
    if (!out || cap == 0) return -1;
    out[0] = '\0';
    if (!name || !jf_id_valid(uid) || !ids || n < 1 || n > JF_SAVE_IDS_MAX) return -1;
    for (int i = 0; i < n; i++) if (!jf_id_valid(ids[i])) return -1;
    int r = json_lit(out, cap, 0, "{\"Name\":\"");
    if (r >= 0) r = json_put(out, cap, (size_t)r, name);
    if (r >= 0) r = json_lit(out, cap, (size_t)r, "\",\"Ids\":[");
    for (int i = 0; r >= 0 && i < n; i++) {
        if (i) r = json_lit(out, cap, (size_t)r, ",");
        if (r >= 0) r = json_lit(out, cap, (size_t)r, "\"");
        if (r >= 0) r = json_lit(out, cap, (size_t)r, ids[i]);
        if (r >= 0) r = json_lit(out, cap, (size_t)r, "\"");
    }
    if (r >= 0) r = json_lit(out, cap, (size_t)r, "],\"UserId\":\"");
    if (r >= 0) r = json_lit(out, cap, (size_t)r, uid);
    if (r >= 0) r = json_lit(out, cap, (size_t)r, "\",\"MediaType\":\"Audio\"}");
    if (r < 0) out[0] = '\0';
    return r;
}
