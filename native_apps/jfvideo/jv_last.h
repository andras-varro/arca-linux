/*
 * jv_last.h — the last-played record: one small key=value file (common/config.c) that lets the
 * app start straight back into the film it was showing.  The path is the caller's.
 *
 * Tracks are kept as the SERVER's stream Index values (JvStream.index), not as positions in
 * detail.audio / detail.subs: a position shifts when the server re-orders or drops a stream, the
 * index is what the stream URL carries.  A missing file, a bad id or a malformed value reads as
 * "no record".  `attempted` is the crash-loop guard: set before an automatic resume, cleared once
 * playback reaches PLAYING, so a film that kills the app is tried once, not at every respawn.
 */

#ifndef ARCA_JV_LAST_H
#define ARCA_JV_LAST_H

#include <stdbool.h>

typedef struct {
    char      id[33];          /**< the item: 32 hex */
    int       position_s;      /**< seconds, >= 0 */
    int       audio_index;     /**< server stream index, -1 = none */
    int       sub_index;       /**< server stream index, -1 = off */
    long long saved_at;        /**< wall-clock seconds of the write (the loop stamps it); 0 = unknown */
    bool      attempted;       /**< an automatic resume began and has not reached PLAYING yet */
} JvLast;

/** 0 = a valid record was read into `r`; -1 = absent or invalid (`r` zeroed, tracks -1). */
int jv_last_load(JvLast *r, const char *path);

/** Writes every key (atomic, parent directory created).  0 or -1. */
int jv_last_save(const JvLast *r, const char *path);

/** Removes the file.  0 (also when it was absent) or -1. */
int jv_last_clear(const char *path);

#endif /* ARCA_JV_LAST_H */
