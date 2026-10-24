/*
 * vid_srt.h — SubRip (.srt) subtitle parser and word wrapper for the video client.
 *
 * Pure: no I/O, no fonts.  The caller hands in the whole file as bytes; the parser
 * returns cues sorted by start time, with markup removed, in ONE text arena (a cue's
 * text is a NUL-terminated string inside it, lines joined with '\n').  The wrapper
 * takes a glyph-width callback so it knows nothing about the framebuffer font.
 *
 * Input rules (all measured by tests/vid_srt_test.c):
 *   - UTF-8, optional BOM; CRLF, LF and lone CR may be mixed in one file.
 *   - A cue is started by any line containing "-->"; index lines are ignored and may
 *     be missing.  Times are "H:MM:SS,mmm" with ',' or '.', 1-3 ms digits taken as a
 *     plain number of milliseconds (",5" is 5 ms, not 500).  Anything after the end
 *     time (position tags such as "X1:100") is ignored.
 *   - A blank line ends a cue.  A digits-only line right before a "-->" line is the
 *     next cue's index, so it ends the previous cue even without a blank line.
 *   - Removed from the text: <tag> / </tag> (a '<' followed by a letter or '/' and a
 *     '>' within 40 bytes) and {ASS overrides}.  Decoded: &amp; &lt; &gt; &quot;
 *     &nbsp;.  Each line is trimmed and empty lines are dropped.
 *   - Dropped: cues with end <= start, cues with no text left, malformed blocks (they
 *     are skipped to the next blank line, the rest of the file still parses), and
 *     every cue past VID_SRT_MAX_CUES.
 *   - If the starts are not in order the cues are stable-sorted by start.
 *
 * No integer division anywhere: the target (Cortex-A8) has no divide instruction.
 */
#ifndef ARCA_VID_SRT_H
#define ARCA_VID_SRT_H

#include <stddef.h>
#include <stdint.h>

#define VID_SRT_MAX_CUES 20000

typedef struct {
    int      start, end;     /**< milliseconds, end exclusive             */
    uint32_t off;            /**< text offset in the arena                */
} VidSrtCue;

typedef struct {
    VidSrtCue *cue;
    int        count, cap;
    char      *text;         /**< the arena; stable once parse() returned */
    size_t     tlen, tcap;
    int        cursor;       /**< last cue found: the fast path of find() */
} VidSrt;

/** Parse `len` bytes at `data`.  Initialises *s (do not pass one that still owns memory:
 *  call vid_srt_free() first).  Returns the cue count (>= 0, 0 for empty or hopeless
 *  input) or -1 on allocation failure, in which case *s is left empty and freed. */
int  vid_srt_parse(VidSrt *s, const char *data, size_t len);
void vid_srt_free(VidSrt *s);

int  vid_srt_count(const VidSrt *s);
/** Text of cue idx (NUL-terminated, '\n' between lines), or "" when idx is out of range. */
const char *vid_srt_text(const VidSrt *s, int idx);
/** Start / end of cue idx in ms, 0 when idx is out of range. */
int  vid_srt_start_ms(const VidSrt *s, int idx);
int  vid_srt_end_ms(const VidSrt *s, int idx);

/** Index of the cue showing at `ms` (start <= ms < end), or -1.  Where cues overlap the
 *  latest-starting one that covers `ms` wins (up to 8 earlier cues are looked at).
 *  Remembers the last answer, so playing forwards costs O(1); a seek costs a binary search. */
int  vid_srt_find(VidSrt *s, int ms);

/* ---- wrapping ---------------------------------------------------------------- */

/** Width in pixels of the glyph for code point `cp`. */
typedef int (*VidSrtGlyphW)(uint32_t cp, void *user);
/** One output line: `len` bytes at `p`, a view into the wrapped text (no copy). */
typedef struct { const char *p; int len; } VidSrtLine;

/** Break `text` into lines no wider than `max_px`: hard breaks at '\n', soft breaks at
 *  spaces, and a word wider than a whole line is cut between glyphs (never inside a
 *  UTF-8 sequence; every line holds at least one glyph, so it always terminates).
 *  Trailing spaces are left out of a line's len.  Returns the number of lines stored;
 *  lines past `max_lines` are dropped. */
int  vid_srt_wrap(const char *text, int max_px, VidSrtGlyphW gw, void *user,
                  VidSrtLine *lines, int max_lines);

#endif
