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

/** Percent-encode `s` (RFC 3986 unreserved stay) into `out`.  Length, or -1 if it does not fit. */
int jf_url_encode(char *out, size_t cap, const char *s);

typedef enum { JF_SERVER_OK = 0, JF_SERVER_EMPTY, JF_SERVER_HTTPS, JF_SERVER_BAD } JfServerResult;

/**
 * Parse "host", "host:port", "1.2.3.4:8096", with an optional "http://" and a
 * trailing "/".  The port defaults to JF_DEFAULT_PORT.  "https://" is refused
 * (there is no TLS here); a space, CR, LF or other character outside
 * [A-Za-z0-9._-] in the host, or a port outside 1..65535, is BAD.
 */
JfServerResult jf_parse_server(const char *in, char *host, size_t host_cap, int *port);

bool jf_id_valid(const char *id);

int jf_path_auth(char *out, size_t cap);
int jf_path_artists(char *out, size_t cap, const char *uid, int start, int limit);
int jf_path_albums(char *out, size_t cap, const char *uid, const char *artist_id, int start, int limit);
int jf_path_tracks(char *out, size_t cap, const char *uid, const char *album_id, int start, int limit);
int jf_path_search(char *out, size_t cap, const char *uid, const char *term, int start, int limit);
/** `session_id` is 32 hex from jf_session_id(); a fresh one per request, or the server reuses an old transcode job. */
int jf_path_stream(char *out, size_t cap, const char *id, const char *session_id);

/** Where the playSessionId's random bytes come from; NULL restores /dev/urandom.  Returns bytes read or -1. */
typedef int (*JfRandomFn)(unsigned char *buf, size_t n);
void jf_url_set_random(JfRandomFn fn);
/** 32 lowercase hex + NUL.  False if no randomness could be read. */
bool jf_session_id(char out[JF_ID_LEN + 1]);

/**
 * "Authorization: MediaBrowser Client=..., Device=..., DeviceId=..., Version=\"1\"[, Token=...]\r\n".
 * `token` NULL or "" leaves the Token out.  -1 when `device`/`device_id`/`token` hold a quote, CR or LF.
 */
int jf_auth_header(char *out, size_t cap, const char *device, const char *device_id, const char *token);

/** {"Username":..,"Pw":..} with JSON escaping.  The password is the only secret in it; the caller wipes `out`. */
int jf_auth_body(char *out, size_t cap, const char *user, const char *password);

#endif
