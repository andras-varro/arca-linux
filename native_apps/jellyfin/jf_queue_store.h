/*
 * jf_queue_store.h — the local, persistent play queue: a text file on the unit, never on the server.
 *
 * Format (UTF-8 text, LF lines):
 *   line 1   "ARCA-JF-QUEUE 1"            the magic and the version; any other first line = refused
 *   line 2   "cur<TAB>INDEX<TAB>POS_MS"   the track playing / last played, and where it was left
 *   then     one line per track: id, name, artist, album_artist, album, album_id, image_tag, artist_id (strings),
 *            runtime_s, norm_gain_mdb, year, index (integers), then the literal "E", all separated by TAB.
 * A string escapes backslash, TAB, LF and CR as \\ \t \n \r; everything else (UTF-8 included) is stored as is.  The closing
 * "E" is what tells a line that was cut short from a whole one: a line without it, with too few fields, or with a bad id is
 * skipped, and the lines after it still load.  The index is clamped into the entries that did load.  No credential is in it.
 *
 * Pure (no allocation beyond the one result buffer) except jf_queue_save / jf_queue_load, which are the file half.
 */
#ifndef ARCA_JF_QUEUE_STORE_H
#define ARCA_JF_QUEUE_STORE_H

#include <stddef.h>
#include "jf_app.h"
#include "jf_model.h"

#define JF_QUEUE_PATH JF_DATA_DIR "/queue.txt"

/** The file text for items[0..n): malloc'd and NUL-terminated (free it), *len = its length; NULL when out of memory.
 *  `cur` and `pos_ms` are clamped into range (cur to [0, n-1] or 0, pos to >= 0). */
char *jf_queue_serialize(const JfItem *items, int n, int cur, int pos_ms, size_t *len);

/** Parse a file text.  Returns the number of items stored in items[0..cap) (0 for an empty queue), or -1 when the text is
 *  not a version-1 queue file.  *cur is clamped into [0, n-1] (0 when n is 0); *pos_ms is >= 0.  Every item is a track. */
int jf_queue_parse(const char *buf, size_t len, JfItem *items, int cap, int *cur, int *pos_ms);

/** Write the queue atomically (temp file + rename), making the directory when it is missing.  0 on success, -1 on failure. */
int jf_queue_save(const char *path, const JfItem *items, int n, int cur, int pos_ms);

/** Read the file at `path`.  Same result as jf_queue_parse; -1 also when the file is missing or unreadable. */
int jf_queue_load(const char *path, JfItem *items, int cap, int *cur, int *pos_ms);

#endif
