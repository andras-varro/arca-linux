/*
 * jf_art.h — album art for the Jellyfin client: request, fetch without blocking,
 * decode once, cache on disk, hand the screen an LvIcon.
 *
 * Use:
 *   jf_art_init(dir, size);                       once; size is the square, e.g. 160
 *   jf_art_set_server(host, port, dev, dev_id, token);   when signed in (copies; wipe with jf_art_shutdown)
 *   jf_art_request(album_id, tag);                when a track/album comes on screen; cheap, repeatable
 *   jf_art_poll(now_ms);                          once per frame
 *   const LvIcon *ic = jf_art_get(album_id, tag); NULL until the tile is ready
 *
 * One request is in flight at a time, on its own socket (separate from jf_api's
 * queue, so a music stream or a list fetch is never held up).  http_poll() is the
 * non-blocking client, so the network never stalls a frame.  The two accepted
 * stalls are one disk read of a cached JPEG (a JPEG file of about 15-20 KB) and
 * one JPEG decode, each at most once per image and at most one per jf_art_poll().
 *
 * A reply is refused when it is not a 200, is larger than JF_ART_MAX_BYTES, or does
 * not decode; the entry is then FAILED and is asked for again no sooner than
 * JF_ART_RETRY_MS later.  The auth token is held in this module's memory only
 * and never logged.
 */
#ifndef ARCA_JF_ART_H
#define ARCA_JF_ART_H

#include <stdbool.h>
#include <stddef.h>

#include "list_view.h"

#define JF_ART_MAX_BYTES  (256 * 1024)   /**< largest reply accepted                 */
#define JF_ART_TOTAL_MS   8000           /**< whole-request deadline                 */
#define JF_ART_RETRY_MS   60000          /**< a failed entry is retried after this   */
#define JF_ART_MEM_SLOTS  6              /**< decoded tiles held in memory           */
#define JF_ART_CACHE_DIR  "/home/root/data/jellyfin/art"

/** The request path (pure): "/Items/<id>/Images/Primary?maxWidth=N&maxHeight=N&quality=90[&tag=T]".  Length or -1 (bad id/tag/size, no room). */
int jf_art_path(char *out, size_t cap, const char *album_id, const char *tag, int size);

bool jf_art_init(const char *cache_dir, int size);
void jf_art_set_server(const char *host, int port, const char *device, const char *device_id, const char *token);
void jf_art_request(const char *album_id, const char *tag);
void jf_art_poll(long long now_ms);
const LvIcon *jf_art_get(const char *album_id, const char *tag);
/** True while a tile is wanted or loading (lets a caller keep redrawing). */
bool jf_art_busy(void);
/** Bumps each time a tile becomes ready: a caller redraws when it changes. */
unsigned jf_art_generation(void);
/** Free everything, close the socket, wipe the token. */
void jf_art_shutdown(void);

#endif
