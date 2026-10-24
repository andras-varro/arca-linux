/*
 * jf_url.h — the Jellyfin client's wire strings: percent-encoding, the server
 * address, one builder per endpoint, the fresh playSessionId and the
 * Authorization header.  Pure: no socket, no framebuffer.
 *
 * Every builder writes a NUL-terminated string into `out` and returns its length,
 * or -1 when it does not fit `cap` or an id is not 32 hex digits (an id reaches a
 * URL path, so it is checked rather than escaped).  Paging is `start`/`limit`.
 *
 * ⚠️ The token travels ONLY in the Authorization header (`api_key=` in a query
 * is refused by the server, measured).  Nothing here puts it in a path.
 */

#ifndef ARCA_JF_URL_H
#define ARCA_JF_URL_H

#include <stdbool.h>
#include <stddef.h>

#define JF_HOST_MAX      64
#define JF_DEFAULT_PORT  8096
#define JF_ID_LEN        32     /**< hex digits of a Jellyfin id; buffers hold +1 */
#define JF_SAVE_IDS_MAX  100    /**< track ids a saved playlist carries */

/** Percent-encode `s` (RFC 3986 unreserved stay) into `out`.  Length, or -1 if it does not fit. */
int jf_url_encode(char *out, size_t cap, const char *s);

typedef enum { JF_SERVER_OK = 0, JF_SERVER_EMPTY, JF_SERVER_INSECURE, JF_SERVER_BAD } JfServerResult;

/**
 * Parse "host", "host:port", "1.2.3.4:8096", with an optional "http://" or
 * "https://" and a trailing "/".  On OK, `*tls` says whether the server is
 * reached over TLS, and the port defaults to 443 for TLS and JF_DEFAULT_PORT
 * for plain HTTP.  The scheme decides, else the host does: a private IPv4
 * literal (10/8, 172.16/12, 192.168/16) is plain http, anything else https.
 * "http://" to any other host is INSECURE (the caller asks for https).
 * A space, CR, LF or other character outside [A-Za-z0-9._-] in the host, or a
 * port outside 1..65535, is BAD.  `*port` and `*tls` are written only on OK.
 */
JfServerResult jf_parse_server(const char *in, char *host, size_t host_cap, int *port, bool *tls);

bool jf_id_valid(const char *id);

int jf_path_auth(char *out, size_t cap);
/** One item by id as a one-row list (Ids=): the seed of a MIX, with Genres, ProductionYear and ArtistItems. */
int jf_path_item_by_id(char *out, size_t cap, const char *uid, const char *id);
/** One item by id as a single object (GET /Users/<uid>/Items/<id>): its MediaSources come without asking (~5 KB); for the NOW PLAYING source line. */
int jf_path_item_detail(char *out, size_t cap, const char *uid, const char *id);
/** GET /Users/<uid>/Views: the user's libraries (jf_parse_items keeps the music ones as JF_ITEM_LIBRARY). */
int jf_path_views(char *out, size_t cap, const char *uid);
/** Scope an already built root path to a library: appends "&ParentId=<lib>".  Empty `lib` leaves the path byte-identical; an invalid id or no room returns -1; else the new length. */
int jf_path_scope(char *path, size_t cap, const char *lib);

/** Where the playSessionId's random bytes come from; NULL restores /dev/urandom.  Returns bytes read or -1. */
typedef int (*JfRandomFn)(unsigned char *buf, size_t n);
void jf_url_set_random(JfRandomFn fn);
/** 32 lowercase hex + NUL.  False if no randomness could be read. */
bool jf_session_id(char out[JF_ID_LEN + 1]);

/**
 * "Authorization: MediaBrowser Client=..., Device=..., DeviceId=..., Version=\"1\"[, Token=...]\r\n".
 * `client` is the app's name, supplied by the app (Ludfin passes "Ludfin").  `token` NULL or "" leaves the Token out.
 * -1 when `client`, `device` or `device_id` is empty, or any of the four
 * holds a quote, backslash, CR or LF.
 */
int jf_auth_header(char *out, size_t cap, const char *client, const char *device, const char *device_id, const char *token);

/** {"Username":..,"Pw":..} with JSON escaping.  The password is the only secret in it; the caller wipes `out`. */
int jf_auth_body(char *out, size_t cap, const char *user, const char *password);

/** "/Playlists": the POST that creates a playlist. */
int jf_path_playlist_create(char *out, size_t cap);

/** DELETE target: a playlist (or any item the user may delete): "/Items/<id>". */
int jf_path_playlist_delete(char *out, size_t cap, const char *id);
/** GET /Users/<uid>: one user object, whose Policy says what the user may delete. */
int jf_path_user(char *out, size_t cap, const char *uid);
/** GET /Items/<id>/Download: the original file bytes, honours Range (measured: 206 with Content-Range). -1 on a bad id or when it does not fit. */
int jf_path_download(char *out, size_t cap, const char *id);

/**
 * {"Name":..,"Ids":[..],"UserId":..,"MediaType":"Audio"} with the name JSON-escaped.  -1 when `uid` or an id is
 * not 32 hex, `n` is outside 1..JF_SAVE_IDS_MAX, or it does not fit.
 */
int jf_save_body(char *out, size_t cap, const char *name, const char *uid, const char *const *ids, int n);

#endif
