/*
 * frame_jf.h — the picture frame's Jellyfin photo wire strings and reply parsers.  Pure: no socket, no
 * framebuffer, no file.  Every path builder writes a NUL-terminated string and returns its length, or -1
 * for an invalid id (not 32 hex digits), a negative start/limit, or a string that does not fit `cap`.
 */
#ifndef FRAME_JF_H
#define FRAME_JF_H

#include <stddef.h>

#define FJ_NAME_MAX 64

typedef enum { FJ_OTHER = 0, FJ_ALBUM, FJ_PHOTO } FjKind;   /* ALBUM: PhotoAlbum, Folder, CollectionFolder, BoxSet */

typedef struct { char id[33]; char name[FJ_NAME_MAX]; FjKind kind; int probe; int loose; } FjItem;   /* probe: a library not typed "photos", to be kept only if an Items query finds a Photo; loose: the album of the photos lying directly in a library */

int fj_path_views(char *out, size_t cap, const char *uid);
int fj_path_children(char *out, size_t cap, const char *uid, const char *parent, int start, int limit);
int fj_path_photos(char *out, size_t cap, const char *uid, const char *parent, int start, int limit);

/* Photos lying directly in `parent` (not in its sub-albums): the same list as fj_path_photos without Recursive. */
int fj_path_loose(char *out, size_t cap, const char *uid, const char *parent, int start, int limit);

/* A library whose top level holds loose photos (loose_count > 0) next to sub-albums gets one more album at the
 * front: the library itself, named as it is, marked loose, listing only its direct photos.  Returns the new count
 * (n unchanged when there is no loose photo or no room). */
int fj_add_loose(FjItem *items, int n, int cap, const FjItem *lib, int loose_count);
int fj_path_image(char *out, size_t cap, const char *id, int max_w, int max_h);   /* fill box: the server answers a size that covers it, not one that fits inside */
int fj_path_original(char *out, size_t cap, const char *id);

/* Library file name for a server item: "jf-<32 lowercase hex>.jpg".  -1 on an invalid id or no room (40 bytes). */
int fj_key_from_id(char *out, size_t cap, const char *id);

/* {"Items":[{"Id","Name","Type"}..],"TotalRecordCount":N}.  Returns the count stored (<= max), -1 on
 * malformed or truncated JSON or no Items array.  *total (may be NULL) = TotalRecordCount, else the Items count.
 * An entry with an invalid Id is skipped; a missing Name is ""; a long Name is cut to FJ_NAME_MAX-1 bytes;
 * an unknown Type is FJ_OTHER (the caller filters). */
int fj_parse_items(const char *json, size_t len, FjItem *out, int max, int *total);

/* A Views reply: keeps libraries typed "photos" (probe 0) and "homevideos", or with no CollectionType (probe 1: the caller asks
   the server for one Photo before listing it; CollectionType compared in any case), as FJ_ALBUM. */
int fj_parse_libraries(const char *json, size_t len, FjItem *out, int max);

#endif
