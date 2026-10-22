/*
 * jf_login.h — the Jellyfin client's saved login: server, user, the access token
 * and the per-unit device id, in one 0600 file.
 *
 * ⚠️ There is no password field and no "password" key, on purpose: the password
 * is typed once, sent once, and wiped (jf_wipe).  What stays on the unit is the
 * token, which the server can revoke.  The file is still a credential, so it is
 * written 0600 and the app's backup rules decide whether it ever leaves the unit.
 *
 * The API takes the path so a host test can use a temp dir; the app passes
 * JF_CONF_PATH.
 */

#ifndef ARCA_JF_LOGIN_H
#define ARCA_JF_LOGIN_H

#include <stddef.h>

#define JF_CONF_PATH     "/opt/jellyfin/jellyfin.conf"
#define JF_VOLUME_MAX    14   /**< volume_step runs 0..JF_VOLUME_MAX */
#define JF_VOLUME_DEFAULT 10

typedef struct {
    char server[64];
    char user[64];
    char user_id[33];
    char token[64];
    char device_id[64];
    int volume_step;
    char library[33];        /* chosen music library id; empty = all libraries */
    char library_name[64];
} JfLogin;

/**
 * Fill `l` from `path`.  A missing file returns -1 but `l` still holds defaults
 * (empty strings, volume_step 10).  device_id is never empty afterwards:
 * "arca-<hostname>", or "arca-unit".  A user_id that is not 32 hex is dropped and
 * volume_step is clamped to 0..JF_VOLUME_MAX.
 */
int jf_login_load(JfLogin *l, const char *path);

/** Write `l` (0600, atomic).  Empty token / user_id are left out.  0 or -1. */
int jf_login_save(const JfLogin *l, const char *path);

/** Forget the session: wipe token and user_id in `l` and rewrite the file without them. */
int jf_login_clear(JfLogin *l, const char *path);

/** Sign out: like jf_login_clear, and the chosen library is forgotten too.  Server and user stay (sign-in prefill). */
int jf_login_signout(JfLogin *l, const char *path);

/** Zero `n` bytes in a way the compiler may not drop (a password or token buffer). */
void jf_wipe(void *p, size_t n);

/** jf_wipe over the whole struct. */
void jf_login_wipe(JfLogin *l);

#endif
