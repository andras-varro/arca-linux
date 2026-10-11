/* sys_settings.h — the System and SSH pages' pure logic: the SSH mode and root
 * login read out of sshd_config text and rewritten, the lockout guard, the
 * root shadow line, the host-name check, and the date/time editor's field
 * arithmetic.
 *
 * Pure: functions take text or a struct, never a path, so a host test drives
 * them (tests/sys_settings_test.c).  ssh_page.c and system_page.c do the
 * reading and the setting.
 */
#ifndef SYS_SETTINGS_H
#define SYS_SETTINGS_H

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    SSH_MODE_UNKNOWN,    /* absent, a hybrid, or not understood */
    SSH_MODE_KEY_ONLY,   /* PasswordAuthentication no */
    SSH_MODE_PASSWORD,   /* PasswordAuthentication yes: password AND key */
    SSH_MODE_OFF         /* sshd stopped and kept from starting; never read from sshd_config */
} SshMode;

typedef enum {
    SSH_ROOT_UNKNOWN,    /* no, forced-commands-only, a value not ours */
    SSH_ROOT_KEY,        /* PermitRootLogin prohibit-password (sshd's default) */
    SSH_ROOT_PASSWORD    /* PermitRootLogin yes */
} SshRootLogin;

/* The mode an OpenSSH sshd_config text sets: KEY_ONLY for PasswordAuthentication
 * no, PASSWORD for yes.  A password mode never turns key login off, so a text
 * that also says PubkeyAuthentication no (password only, or nothing at all) is
 * UNKNOWN, as is an absent or unrecognised PasswordAuthentication and NULL.
 * Never OFF: that is a marker file, not a directive (sys_ssh_effective).
 * As sshd reads it: keywords and values are case-insensitive, `Keyword value`
 * or `Keyword=value`, '#' lines are comments, the FIRST occurrence of a
 * keyword wins, and a Match block ends the global section. */
SshMode sys_ssh_mode(const char *sshd_config_text);

/* The mode in force: OFF when the off marker exists, else cfg_mode. */
SshMode sys_ssh_effective(SshMode cfg_mode, bool off_marker);

/* The root login an sshd_config sets (first global PermitRootLogin).  Absent is
 * KEY, which is sshd's default.  prohibit-password and its old spelling
 * without-password are KEY, yes is PASSWORD, anything else UNKNOWN. */
SshRootLogin sys_ssh_root_login(const char *sshd_config_text);

/* Rewrite sshd_config text for a mode (KEY_ONLY or PASSWORD; anything else is
 * refused with -1): PasswordAuthentication is set no/yes, and an ACTIVE
 * PubkeyAuthentication line is set to yes so key login stays on (none is added:
 * sshd's default is yes).  PermitRootLogin is never touched.  Writes the new
 * text to out (NUL-terminated, LF only: any CR is dropped) and returns its
 * length, or -1 when out is too small or an argument is NULL.  Policy, from
 * sshd's first-match-wins: every ACTIVE line of a directive in the global
 * section (before the first Match) is rewritten, so no stale duplicate can still
 * win; commented lines and everything else are copied unchanged, as is
 * everything from the first Match on.  A directive with no active line that is
 * to be added goes just BEFORE the first Match line (an append would land inside
 * the Match block), else at the end. */
int sys_ssh_set_mode(const char *text, SshMode mode, char *out, size_t out_len);

/* The same for PermitRootLogin: KEY writes prohibit-password, PASSWORD yes,
 * UNKNOWN is refused.  Nothing else is touched. */
int sys_ssh_set_root_login(const char *text, SshRootLogin root, char *out, size_t out_len);

/* Rewrite sshd_config text so PasswordAuthentication is `yes` or `no` alone
 * (the primitive under sys_ssh_set_mode; same policy and return). */
int sys_ssh_set_password_auth(const char *text, bool password_yes,
                              char *out, size_t out_len);

/* The lockout guard, over the combination about to be written: NULL when it
 * is allowed, else a short reason (at most 40 characters) for a status line.
 * has_key: root has a plausible authorized key; has_password: root has a
 * password.  Rules: KEY_ONLY needs a key; PASSWORD needs a password and a way in
 * (a key, or root login yes); root login yes needs a password; UNKNOWN is never
 * written; OFF needs nothing. */
const char *sys_ssh_refuse(SshMode mode, SshRootLogin root, bool has_key, bool has_password);
/* The guard a panel tap uses: as sys_ssh_refuse() on the target, except that a lockout
 * is not refused when the current state already gives no SSH access. */
const char *sys_ssh_refuse_change(SshMode cur_mode, SshRootLogin cur_root,
                                  SshMode mode, SshRootLogin root,
                                  bool has_key, bool has_password);

/* True when authorized_keys text holds at least one plausible public-key
 * line: not blank, not '#', with a key-type token (ssh-*, ecdsa-sha2-*, sk-*)
 * followed by a base64 blob of 20+ characters.  It cannot prove the key is the
 * operator's, only that one exists. */
bool sys_authkeys_plausible(const char *text);

/* True when authorized_keys file satisfies sshd's StrictModes check:
 * the file, .ssh directory, and home directory are all owned by allowed_uid
 * with no group or world write permissions. Caller is responsible for reading
 * the file content separately if needed. */
#include <sys/types.h>
bool sys_authkeys_safe(const char *auth_keys_path, uid_t allowed_uid);

/* True when /etc/shadow text gives user root a usable password hash: the
 * second field is non-empty and starts neither with '*' nor with '!'. */
bool sys_shadow_root_has_password(const char *shadow_text);

/* True for a string fit to be a crypt hash in a shadow field: starts with '$',
 * 13..255 characters of [A-Za-z0-9./$] (no ':', no newline, no space). */
bool sys_hash_plausible(const char *hash);

/* shadow text with the password field of the root line replaced by hash; the
 * other fields and every other line are copied unchanged (LF only).  Returns
 * the length, or -1 for an implausible hash, no root line, out too small or a
 * NULL argument. */
int sys_shadow_set_root(const char *shadow_text, const char *hash,
                        char *out, size_t out_len);

/* A host name set-hostname accepts: one RFC-1123 label, 1..63 characters of
 * letters, digits and hyphens, not starting or ending with a hyphen. */
bool sys_hostname_valid(const char *name);

typedef struct { int year, mon, day, hour, min; } SysDateTime;
enum { SYS_DT_YEAR, SYS_DT_MON, SYS_DT_DAY, SYS_DT_HOUR, SYS_DT_MIN, SYS_DT_FIELDS };

#define SYS_YEAR_MIN 2024
#define SYS_YEAR_MAX 2099

/* 28..31 for month 1..12 of year y (Gregorian leap rule); 0 for a bad month. */
int sys_days_in_month(int year, int mon);

/* Step one field by dir (-1 or +1).  Month, hour, minute and day wrap; year
 * clamps to SYS_YEAR_MIN..MAX.  After any step the day is clamped into the
 * (possibly shorter) month, so the result is always valid. */
void sys_dt_step(SysDateTime *dt, int field, int dir);

bool sys_dt_valid(const SysDateTime *dt);

/* "YYYY-MM-DD HH:MM:00" — the argument `date -s` takes.  Empty if invalid. */
void sys_dt_format(const SysDateTime *dt, char *out, size_t len);


/* The curated timezone list the System page cycles through (names are
 * zoneinfo paths below /usr/share/zoneinfo). */
int sys_tz_count(void);
const char *sys_tz_name(int idx);                 /* NULL out of range */

/* Index of the zone a /etc/localtime link target names (anything ending
 * "zoneinfo/<zone>", absolute or relative), or -1 if it is not in the list or
 * the target is NULL. */
int sys_tz_from_link(const char *target);

/* Next (dir > 0) or previous (dir < 0) index, wrapping.  From -1 (zone not
 * in the list) a step lands on the first (+) or last (-) entry. */
int sys_tz_step(int idx, int dir);


#endif
