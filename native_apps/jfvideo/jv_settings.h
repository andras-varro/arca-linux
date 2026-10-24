/*
 * jv_settings.h — the video app's own settings file (key=value through
 * common/config.c).  The path is the caller's; nothing here knows the app name.
 * A missing file or a malformed value gives the default for that key, so a
 * hand-edited file can never produce an out-of-range setting.
 */

#ifndef ARCA_JV_SETTINGS_H
#define ARCA_JV_SETTINGS_H

#include "jf_url.h"
#include "jv_tracks.h"

#define JV_OFFSET_MAX_MS   500   /**< A/V offset clamp, either sign */
#define JV_OFFSET_STEP_MS   20   /**< the UI steps by this; load does not snap */
#define JV_LANGS_LEN        32   /**< buffer of a language list, NUL included */

enum { JV_SINK_ONBOARD = 0, JV_SINK_USB, JV_SINK_BT, JV_SINK_COUNT };

typedef struct {
    char      audio_lang[JV_LANGS_LEN];   /**< "hun,eng" or "default" */
    JvSubMode sub_mode;
    char      sub_lang[JV_LANGS_LEN];
    int       sub_scale;                  /**< 2 or 3 */
    int       av_offset_ms[JV_SINK_COUNT];
    bool      agc;
    char      library_movies[JF_ID_LEN + 1];  /**< library id, "" = all */
    char      library_shows[JF_ID_LEN + 1];
} JvSettings;

void jv_settings_defaults(JvSettings *s);

/** Defaults first, then whatever the file holds that is valid.  0 = file read, -1 = absent (defaults kept). */
int jv_settings_load(JvSettings *s, const char *path);

/** Writes every key (atomic, parent directory created).  0 or -1. */
int jv_settings_save(const JvSettings *s, const char *path);

const char *jv_sub_mode_name(JvSubMode m);

#endif /* ARCA_JV_SETTINGS_H */
