/*
 * jv_app.h — the video client's NAME, in one place (the same macro names as the music app's
 * jellyfin/jf_app.h, so code copied between the two reads the same).  The server it talks to is a
 * Jellyfin server, and wording that names the server keeps that name.
 */
#ifndef ARCA_JV_APP_H
#define ARCA_JV_APP_H

#define JF_APP_NAME     "Ludkino"    /**< prose, and the client / device name sent to the server */
#define JF_APP_TITLE    "LUDKINO"    /**< as a bar / dialog title */
#define JF_APP_BIN      "ludkino"    /**< binary, instance lock, logger name, directory names */

#define JF_CONF_DIR     "/opt/" JF_APP_BIN
#define JF_DATA_DIR     "/home/root/data/" JF_APP_BIN

#define JF_CONF_PATH     JF_CONF_DIR "/" JF_APP_BIN ".conf"   /**< the saved login */
#define JF_SETTINGS_PATH JF_DATA_DIR "/settings.conf"
#define JF_LAST_PATH     JF_DATA_DIR "/last.conf"      /**< the last-played record (jv_last.h) */
#define JF_ART_CACHE_DIR JF_DATA_DIR "/art"

/** Appended to the default device id so this app and the music app present two ids to the server. */
#define JV_DEVICE_ID_SUFFIX "-v"
/** The music app's login file: read for server and user only, never written. */
#define JV_REUSE_LOGIN_PATH "/opt/ludfin/ludfin.conf"

#endif
