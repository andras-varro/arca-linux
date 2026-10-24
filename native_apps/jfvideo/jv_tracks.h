/*
 * jv_tracks.h — which audio and subtitle stream to start with, and how to step
 * to the next one.  Pure: it works on arrays the caller has already parsed
 * from a detail reply, and returns a POSITION in that array (not the server's
 * stream index; read `index` from the entry for the URL).  -1 = none.
 *
 * Languages are compared by a canonical 3-letter code, so "hu", "hun" and
 * "HUN" are the same, and "de", "ger" and "deu" are the same.  A list is a
 * comma-separated string in preference order ("hun,eng"); "default" or an
 * empty list means "no language preference".
 */

#ifndef ARCA_JV_TRACKS_H
#define ARCA_JV_TRACKS_H

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    int  index;            /**< the server's stream index (goes into the URL) */
    char lang[8];          /**< as the server reports it: "hun", "en", "" ... */
    char title[48];        /**< display title, may be empty */
    char codec[16];        /**< "ac3", "subrip", "pgssub" ... */
    int  channels;         /**< audio only; 0 when unknown */
    bool is_default;
    bool is_external;
    bool is_forced;
} JvStream;

typedef enum {
    JV_SUB_OFF = 0,        /**< never pick one */
    JV_SUB_LANG,           /**< always the preferred subtitle language */
    JV_SUB_FORCED,         /**< only forced tracks (foreign-speech signs) in the audio's language */
    JV_SUB_FOREIGN         /**< the preferred language, but only when the audio is NOT in a language you listen to */
} JvSubMode;

/**
 * Canonical lowercase 3-letter code of `code` into `out` (ISO 639-2/B; a 639-1 code, a 639-2/T code and a region
 * suffix such as "en-US" or "pt_BR" are folded onto it; an unknown code passes through lowercased, cut at 3).
 * Returns false and an empty `out` for no language: empty, "und", "zxx", "mul", "mis" or a non-letter start.
 */
bool jv_lang_norm(const char *code, char out[4]);

/** Same language after normalisation.  Two undefined languages never match. */
bool jv_lang_same(const char *a, const char *b);

/** Position of `code` in the comma list (0 = most preferred), or -1.  "default"/empty list: -1. */
int jv_lang_rank(const char *list, const char *code);

/** True for a subtitle codec the server can hand over as SRT text; false for image formats (PGS, DVD, DVB) and unknown codecs. */
bool jv_sub_codec_text(const char *codec);

/**
 * Audio: the first list language that has a track wins; inside it IsDefault, then the first.  No match (or no
 * preference): IsDefault, then the first.  -1 only for n == 0.
 */
int jv_pick_audio(const JvStream *a, int n, const char *audio_langs);

/**
 * Subtitle for `mode` (see JvSubMode).  `audio` is the chosen audio stream (NULL = unknown), `audio_langs` the
 * listening languages, `sub_langs` the subtitle preference list.  Only text codecs qualify.
 * LANG and FOREIGN take the best-ranked language, a full track before a forced one, IsDefault, then the first;
 * a forced track is used only if nothing else of that language exists.  FORCED takes the forced tracks of the
 * audio's language (the sub list when the audio language is unknown).  FOREIGN with an unknown audio language
 * behaves as LANG (cannot tell, so show).  -1 = none.
 */
int jv_pick_sub(const JvStream *s, int n, JvSubMode mode, const char *sub_langs, const char *audio_langs,
                const JvStream *audio);

/** Next audio position after `cur` (wraps); -1 when n == 0, 0 when `cur` is out of range. */
int jv_next_audio(int n, int cur);

/** Next subtitle position after `cur` in the cycle OFF(-1) -> first text track -> ... -> last -> OFF.  Image tracks are skipped. */
int jv_next_sub(const JvStream *s, int n, int cur);

/** Picker text: "HUN AC3 5.1" for audio, "ENG SRT FORCED" for a subtitle; EXT is added for an external file. Returns the length, or -1 if it does not fit. */
int jv_stream_label(char *out, size_t cap, const JvStream *s, bool audio);

#endif /* ARCA_JV_TRACKS_H */
