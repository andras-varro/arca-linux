/*
 * jf_login.h — the Ludfin client's saved login: server, user, the access token
 * and the per-unit device id, in one 0600 file.  The non-secret settings (the
 * chosen library and the volume step) live in their own file (the app's JF_SETTINGS_PATH),
 * so the backup rules can take them without the token.
 *
 * ⚠️ There is no password field and no "password" key, on purpose: the password
 * is typed once, sent once, and wiped (jf_wipe).  What stays on the unit is the
 * token, which the server can revoke.  The file is still a credential, so it is
 * written 0600 and the app's backup rules decide whether it ever leaves the unit.
 *
 * The API takes the path so a host test can use a temp dir; the app passes
 * its own saved-login path (Ludfin's JF_CONF_PATH); any other app's file loads the same way.
 */

#ifndef ARCA_JF_LOGIN_H
#define ARCA_JF_LOGIN_H

#include <stddef.h>

#define JF_VOLUME_MAX    14   /**< volume_step runs 0..JF_VOLUME_MAX */
#define JF_VOLUME_DEFAULT 10
/** user_mdb for a volume step: 0 -> -100000 (mute, whatever the track's norm), else (step-10)*3000. */
int jf_volume_mdb(int step);
/** What a tap on a track row (the PLAY button of its bar) does; one setting, persisted as tap_action. */
typedef enum { JF_TAP_REPLACE = 0, JF_TAP_ADD_PLAY, JF_TAP_ADD_ONLY, JF_TAP_COUNT } JfTapAction;

typedef struct {
    char server[64];
    char user[64];
    char user_id[33];
    char token[64];
    char device_id[64];
    int volume_step;
    int tap_action;          /* JfTapAction; an unknown or missing value is JF_TAP_REPLACE */
    int tags_on;             /* 1 = read each playing track's file tags by a range request (default 0); persisted as tags_read */
    char tags_cut[64];       /* the `server` string a 200-instead-of-206 answer switched tag reads off for (persisted as tags_cut); "" = none */
    char library[33];        /* chosen music library id; empty = all libraries */
    char library_name[64];
} JfLogin;

/**
 * Fill `l` from `path`.  A missing file returns -1 but `l` still holds defaults
 * (empty strings, volume_step 10).  device_id is never empty afterwards:
 * "arca-<hostname>" + `id_suffix` (NULL or "" for none; a second app on the unit passes its own so the two do not share a
 * DeviceId, the key of the server's access token), or "arca-unit" + suffix.  A saved device_id is kept as it is.  A user_id that is not 32 hex is dropped.
 * The settings keys (library, library_name, volume_step) are not read here; a
 * ludfin.conf that still carries them is ignored for them.
 */
int jf_login_load(JfLogin *l, const char *path, const char *id_suffix);

/** Write `l` (0600, atomic).  Empty token / user_id are left out.  0 or -1. */
int jf_login_save(const JfLogin *l, const char *path);

/**
 * Fill l->library, l->library_name and l->volume_step from the settings file at
 * `path`.  Call after jf_login_load.  A missing file returns -1 with the defaults
 * (no library, volume_step 10).  An invalid library id is dropped and volume_step
 * is clamped to 0..JF_VOLUME_MAX.
 */
int jf_settings_load(JfLogin *l, const char *path);

/** Write l->library, l->library_name and l->volume_step to `path`.  0 or -1. */
int jf_settings_save(const JfLogin *l, const char *path);

/** Forget the session: wipe token and user_id in `l` and rewrite the file without them. */
int jf_login_clear(JfLogin *l, const char *path);

/** Sign out: like jf_login_clear, and the chosen library is forgotten too.  Server and user stay (sign-in prefill). */
int jf_login_signout(JfLogin *l, const char *path);

/** Zero `n` bytes in a way the compiler may not drop (a password or token buffer). */
void jf_wipe(void *p, size_t n);

/** jf_wipe over the whole struct. */
void jf_login_wipe(JfLogin *l);

#endif
