/*
 * jf_tags.h — the tags at the head of a track's file, parsed from a byte range.
 *
 * Pure: bytes in, JfTags out; no I/O, no allocation.  The Jellyfin API already gives artist, album artist, genres, year
 * and composer per track; the file's own tags add what it does not: conductor, performers with their roles, work.
 *
 * Formats: ID3v2.3 and v2.4 (MP3) and FLAC (Vorbis comment block).  Not covered, reported as `have` false: ID3v2.2,
 * MP4/M4A, Ogg (Vorbis/Opus), WMA, a file with no tag at the head.  ID3v1 at the end of a file is out of reach of a head range.
 *
 * The caller reads JF_TAGS_CAP bytes first.  If the tag runs past what it was given, `need` says how many bytes of the
 * file the rest of the tag takes; the caller may read once more up to JF_TAGS_HARD (a tag whose cover art comes first can
 * be megabytes: that one is given up on, `have` false).
 */

#ifndef ARCA_JF_TAGS_H
#define ARCA_JF_TAGS_H

#include <stdbool.h>
#include <stddef.h>

#define JF_TAGS_CAP        65536    /**< first range read */
#define JF_TAGS_HARD       131072   /**< the most a track's tags are ever read (one extension, no more) */
#define JF_TAGS_PERF_MAX   6
#define JF_TAGS_GENRE_MAX  5
#define JF_TAGS_NAME_MAX   256

typedef struct {
    bool have;               /**< a supported tag was found and read (fields may still be empty) */
    bool truncated;          /**< the tag runs past the bytes given: the fields hold what fit */
    size_t need;             /**< truncated: how many bytes from the start of the file hold the whole tag (0 = unknown) */
    char artist[JF_TAGS_NAME_MAX];
    char album_artist[JF_TAGS_NAME_MAX];
    char conductor[JF_TAGS_NAME_MAX];
    char work[JF_TAGS_NAME_MAX];
    int  year;               /**< 0 when absent */
    struct { char name[64]; char role[32]; } perf[JF_TAGS_PERF_MAX];
    int  perf_n;
    char genre[JF_TAGS_GENRE_MAX][40];
    int  genre_n;
} JfTags;

/** Parse the head of a file.  `out` is cleared first.  Returns out->have. */
bool jf_tags_parse(const unsigned char *buf, size_t len, JfTags *out);

/** The display rule for the album artist: false when empty, equal to the artist (any case) or "Various Artists". */
bool jf_tags_show_album_artist(const char *artist, const char *album_artist);

#endif
