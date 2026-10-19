/*
 * jf_api.h — the Jellyfin client's request queue: one HTTP request in flight at
 * a time over a four-slot ring, polled once per frame, never blocking.
 *
 * Four kinds.  AUTH is a POST (sign-in).  SAVE is a POST of a JSON body (create a playlist) whose
 * reply body is ignored: the status is the result.  LIST is a GET whose body is parsed
 * with jf_parse_items() into a caller-given JfList.  STREAM is a GET in
 * HTTP_STREAM mode: on a 200 the result carries the socket, positioned at the
 * first body byte (http_take_fd), for audio_music_start_fd().
 *
 * Order: AUTH > SAVE > STREAM > LIST, oldest first within a kind (a save is one short user action, so it
 * does not wait behind a stream).  An in-flight request
 * is never preempted, with one exception: SUPERSESSION.  A new LIST drops every
 * queued LIST and ends the in-flight one, a new STREAM does the same to STREAMs;
 * a dropped request delivers no result; a SAVE is never superseded, a second one just queues.  Every request carries a caller `tag`
 * that comes back in its result, so a result for a view the user has left can be
 * told from the current one.
 *
 * A GET that fails UNREACHABLE or SERVER is retried once after `retry_ms`; AUTH
 * and SAVE are never retried (a second sign-in with a wrong password is not harmless, a second save could
 * make a duplicate playlist).
 *
 * ⚠️ **Secrets.**  The Authorization header is built per request into a stack
 * buffer and wiped (volatile loop) right after http_begin(); the AUTH body (it
 * holds the password) and the reply body of an AUTH (it holds the token) are
 * wiped when the request ends.  Nothing here logs a header, a body, a token or a
 * password: a log line carries kind, status, elapsed ms and an item count.  The
 * token and user id in a JfResult are the caller's to wipe.
 *
 * This module never reads the config file: host, port and identity are set
 * through the setters.
 */

#ifndef ARCA_JF_API_H
#define ARCA_JF_API_H

#include <stdbool.h>
#include <stddef.h>

#include "http.h"
#include "json.h"
#include "jf_model.h"
#include "jf_url.h"
#include "logger.h"

#define JF_API_SLOTS      4
#define JF_API_BODY_BYTES (96 * 1024)
#define JF_API_PATH_MAX   512   /* = JF_PATH_MAX: a MIX pool path with 4 genre ids + artist + decade is 418 */
#define JF_API_BODY_OUT   320       /**< largest AUTH request body */
#define JF_API_POST_BYTES 4608      /**< largest request body: a SAVE of JF_SAVE_IDS_MAX ids (~3.6 KB) */
#define JF_API_TOKEN_MAX  64
#define JF_API_NAME_MAX   64

typedef enum { JF_KIND_NONE = 0, JF_KIND_AUTH, JF_KIND_LIST, JF_KIND_STREAM, JF_KIND_SAVE } JfKind;

typedef enum {
    JF_ERR_NONE = 0,
    JF_ERR_UNREACHABLE,   /**< no connection, timeout, dropped mid-reply          */
    JF_ERR_UNAUTH,        /**< 401                                                */
    JF_ERR_FORBIDDEN,     /**< 403                                                */
    JF_ERR_NOTFOUND,      /**< 404                                                */
    JF_ERR_SERVER,        /**< 5xx                                                */
    JF_ERR_BADREPLY       /**< other status, chunked, too big, malformed, bad JSON */
} JfErr;

typedef struct {
    JfKind   kind;
    unsigned tag;
    JfErr    err;
    int      status;      /**< HTTP status, 0 when none arrived                   */
    int      parse;       /**< LIST: a JfParse; JF_PARSE_OK otherwise             */
    JfList  *list;        /**< LIST: the caller's list, items appended on success */
    char     token[JF_API_TOKEN_MAX];   /**< AUTH: AccessToken; caller wipes      */
    char     user_id[JF_ID_LEN + 1];    /**< AUTH: User.Id; caller wipes          */
    int      fd;          /**< STREAM: the socket on success, else -1             */
} JfResult;

typedef struct {
    JfKind   kind;                      /**< JF_KIND_NONE = free slot             */
    unsigned tag;
    unsigned seq;                       /**< enqueue order, oldest first          */
    JfList  *list;
    char     path[JF_API_PATH_MAX];
    char     body[JF_API_POST_BYTES];   /**< AUTH and SAVE only; wiped when the request ends */
    size_t   body_len;
} JfSlot;

typedef struct {
    /* the caller may set these after jf_api_init() */
    int  idle_ms;           /**< default 5000   */
    int  list_total_ms;     /**< default 15000  */
    int  stream_total_ms;   /**< default 20000  */
    int  auth_total_ms;     /**< default 10000  */
    int  retry_ms;          /**< default 1500   */
    Logger *log;            /**< NULL = silent  */

    /* private */
    char host[JF_HOST_MAX + 1];
    int  port;
    char device[JF_API_NAME_MAX + 1];
    char device_id[JF_API_NAME_MAX + 1];
    char token[JF_API_TOKEN_MAX + 1];
    JfSlot   ring[JF_API_SLOTS];
    unsigned next_seq;
    JfSlot   cur;                       /**< the in-flight request (kind NONE = idle) */
    bool     retried, waiting;
    long long retry_at, began_ms;
    HttpReq  req;
    char    *body;                      /**< heap, JF_API_BODY_BYTES              */
    JsonTok *toks;                      /**< heap, JF_TOKENS_PAGE                 */
} JfApi;

/** 0, or -1 when the two buffers cannot be allocated. */
int  jf_api_init(JfApi *a);
/** Ends anything in flight, wipes the secrets and frees the buffers. */
void jf_api_free(JfApi *a);

void jf_api_set_server(JfApi *a, const char *host, int port);
/** The identity for the Authorization header; `token` NULL or "" = not signed in. */
void jf_api_set_auth(JfApi *a, const char *device, const char *device_id, const char *token);

/** Each returns 0, or -1 when the ring is full or an argument does not fit. */
int  jf_api_enqueue_auth(JfApi *a, unsigned tag, const char *user, const char *password);
int  jf_api_enqueue_list(JfApi *a, unsigned tag, const char *path, JfList *list);
int  jf_api_enqueue_stream(JfApi *a, unsigned tag, const char *path);
/** POST /Playlists: the first `n` (1..JF_SAVE_IDS_MAX) ids as a new playlist `name`.  The reply body is ignored. */
int  jf_api_enqueue_save(JfApi *a, unsigned tag, const char *name, const char *user_id, const char *const *ids, int n);

/** Drop every queued and in-flight request of `kind`; no result is delivered. */
void jf_api_cancel_kind(JfApi *a, JfKind kind);
/** True while anything is queued or in flight. */
bool jf_api_busy(const JfApi *a);

/** Once per frame.  Fills `*out` and returns true when a request ended; at most one per call. */
bool jf_api_poll(JfApi *a, JfResult *out);

/** Pure: HTTP status (0 = none) and http_error() text -> error class. */
JfErr jf_api_classify(int status, const char *http_err);

#endif
