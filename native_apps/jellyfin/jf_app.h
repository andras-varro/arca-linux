/*
 * jf_app.h — the app's NAME, in one place.  The music client is called Ludfin; the
 * server it talks to is a Jellyfin server, and wording that names the server or the
 * protocol keeps that name.  Every user-visible title, the binary / log / lock name,
 * the config and data directories, and the client name reported to the server come
 * from here.  The shell side (build-and-deploy.sh GAMES_BINARIES, app-manifests.sh,
 * backup.d/ludfin*.item, logs.d/ludfin.item, cpu-budget.sh) spells the same `ludfin`.
 * The source directory and the jf_* prefixes are internal identifiers and keep the
 * server's name.
 */
#ifndef ARCA_JF_APP_H
#define ARCA_JF_APP_H

#define JF_APP_NAME     "Ludfin"     /**< as a name in prose, and the client / device name sent to the server */
#define JF_APP_TITLE    "LUDFIN"     /**< the same, as a bar / dialog title */
#define JF_APP_BIN      "ludfin"     /**< binary, instance lock, logger name (/var/log/arca/ludfin.log), directory names */

#define JF_CONF_DIR     "/opt/" JF_APP_BIN
#define JF_DATA_DIR     "/home/root/data/" JF_APP_BIN

/* The app's own files, passed at run time to the jf_shared layer, which knows no app name. */
#define JF_CONF_PATH     JF_CONF_DIR "/" JF_APP_BIN ".conf"   /**< the saved login (jf_login_load/save/clear take a path) */
#define JF_SETTINGS_PATH JF_DATA_DIR "/settings.conf"
#define JF_ART_CACHE_DIR JF_DATA_DIR "/art"                  /**< jf_art_init's cache_dir */

#endif
