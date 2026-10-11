/* sys_settings.c — see sys_settings.h. */
#include "sys_settings.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

/* Check if a file/directory has safe permissions for SSH: owned by uid,
 * not group- or world-writable. Returns true if safe, false otherwise. */
static bool check_ssh_safe_perms(const char *path, uid_t allowed_uid) {
    struct stat st;
    if (stat(path, &st) != 0) return false;
    if (st.st_uid != allowed_uid) return false;
    if (st.st_mode & (S_IWGRP | S_IWOTH)) return false;
    return true;
}

/* Check if authorized_keys file looks like it has a valid key: plausible content
 * AND safe permissions for sshd's StrictModes check (owned by root, not group/world writable,
 * and the directories /home/root, /home/root/.ssh also satisfy the same). */
bool sys_authkeys_safe(const char *auth_keys_path, uid_t allowed_uid) {
    /* Check the three paths sshd validates: home dir, .ssh dir, authorized_keys file */
    if (!check_ssh_safe_perms(auth_keys_path, allowed_uid)) return false;

    /* Get the directory name of authorized_keys (.ssh dir) */
    char ssh_dir[256];
    snprintf(ssh_dir, sizeof(ssh_dir), "%s", auth_keys_path);
    char *last_slash = strrchr(ssh_dir, '/');
    if (!last_slash) return false;
    *last_slash = '\0';

    if (!check_ssh_safe_perms(ssh_dir, allowed_uid)) return false;

    /* Get the home directory (parent of .ssh) */
    last_slash = strrchr(ssh_dir, '/');
    if (!last_slash) return false;
    *last_slash = '\0';

    if (!check_ssh_safe_perms(ssh_dir, allowed_uid)) return false;

    return true;
}

/* One line's keyword and value, in place: kw / val point into line,
 * NUL-terminated.  false for a blank or comment line. */
static bool split_line(char *line, char **kw, char **val) {
    char *p = line;
    while (*p && isspace((unsigned char)*p)) p++;
    if (!*p || *p == '#') return false;
    *kw = p;
    while (*p && !isspace((unsigned char)*p) && *p != '=') p++;
    if (*p) *p++ = '\0';
    while (*p && (isspace((unsigned char)*p) || *p == '=')) p++;
    if (*p == '"') p++;
    *val = p;
    char *e = p + strlen(p);
    while (e > p && (isspace((unsigned char)e[-1]) || e[-1] == '"')) e--;
    *e = '\0';
    return true;
}

/* The first global value of keyword kw: NULL-free, copied into val (len bytes),
 * false when absent.  Stops at the first Match, as sshd's global section does. */
static bool first_value(const char *text, const char *kw, char *val, size_t len) {
    const char *p = text;
    while (*p) {
        char line[256];
        size_t n = strcspn(p, "\n");
        size_t c = n < sizeof(line) - 1 ? n : sizeof(line) - 1;
        memcpy(line, p, c);
        line[c] = '\0';
        p += n;
        if (*p == '\n') p++;

        char *k, *v;
        if (!split_line(line, &k, &v)) continue;
        if (strcasecmp(k, "Match") == 0) break;
        if (strcasecmp(k, kw) == 0) {
            snprintf(val, len, "%s", v);
            return true;
        }
    }
    return false;
}

SshMode sys_ssh_mode(const char *text) {
    if (!text) return SSH_MODE_UNKNOWN;
    char pw[32], pk[32];
    if (!first_value(text, "PasswordAuthentication", pw, sizeof(pw))) return SSH_MODE_UNKNOWN;
    /* key login off makes it something else: password only, or no way in */
    if (first_value(text, "PubkeyAuthentication", pk, sizeof(pk)) &&
        strcasecmp(pk, "yes") != 0) return SSH_MODE_UNKNOWN;
    if (strcasecmp(pw, "no") == 0)  return SSH_MODE_KEY_ONLY;
    if (strcasecmp(pw, "yes") == 0) return SSH_MODE_PASSWORD;
    return SSH_MODE_UNKNOWN;
}

SshMode sys_ssh_effective(SshMode cfg_mode, bool off_marker) {
    return off_marker ? SSH_MODE_OFF : cfg_mode;
}

SshRootLogin sys_ssh_root_login(const char *text) {
    if (!text) return SSH_ROOT_UNKNOWN;
    char v[48];
    if (!first_value(text, "PermitRootLogin", v, sizeof(v))) return SSH_ROOT_KEY;
    if (strcasecmp(v, "prohibit-password") == 0 || strcasecmp(v, "without-password") == 0)
        return SSH_ROOT_KEY;
    if (strcasecmp(v, "yes") == 0) return SSH_ROOT_PASSWORD;
    return SSH_ROOT_UNKNOWN;
}

/* ── Rewriting sshd_config ──────────────────────────────────────────────── */

typedef struct { char *p; size_t cap, len; bool over; } OutBuf;

static void ob_put(OutBuf *o, const char *s, size_t n) {
    if (o->len + n + 1 > o->cap) { o->over = true; return; }
    memcpy(o->p + o->len, s, n);
    o->len += n;
}

static void ob_line(OutBuf *o, const char *s, size_t n) {
    while (n && (s[n - 1] == '\r')) n--;          /* LF only */
    ob_put(o, s, n);
    ob_put(o, "\n", 1);
}

/* One directive to force: every ACTIVE global line of kw becomes "kw value";
 * add says whether one is inserted when there is none. */
typedef struct { const char *kw, *value; bool add; bool seen; } Rule;

static void rule_emit(OutBuf *o, const Rule *r) {
    char l[96];
    int n = snprintf(l, sizeof(l), "%s %s", r->kw, r->value);
    ob_line(o, l, (size_t)n);
}

/* Which rule an input line is, using split_line() on a copy: index + 1, -1 for
 * Match, 0 for anything else (incl. comments). */
static int line_rule(const char *s, size_t n, const Rule *rules, int nr) {
    char line[256];
    size_t c = n < sizeof(line) - 1 ? n : sizeof(line) - 1;
    memcpy(line, s, c);
    line[c] = '\0';
    char *kw, *val;
    if (!split_line(line, &kw, &val)) return 0;
    if (strcasecmp(kw, "Match") == 0) return -1;
    for (int i = 0; i < nr; i++)
        if (strcasecmp(kw, rules[i].kw) == 0) return i + 1;
    return 0;
}

static int rewrite(const char *text, Rule *rules, int nr, char *out, size_t out_len) {
    if (!text || !out || out_len < 2) return -1;
    OutBuf o = { out, out_len, 0, false };
    bool in_match = false;
    const char *p = text;
    while (*p) {
        size_t n = strcspn(p, "\n");
        int k = in_match ? 0 : line_rule(p, n, rules, nr);
        if (k < 0) {
            for (int i = 0; i < nr; i++)
                if (!rules[i].seen && rules[i].add) { rule_emit(&o, &rules[i]); rules[i].seen = true; }
            in_match = true;
        }
        if (k > 0) { rule_emit(&o, &rules[k - 1]); rules[k - 1].seen = true; }
        else       ob_line(&o, p, n);
        p += n;
        if (*p == '\n') p++;
    }
    for (int i = 0; i < nr; i++)
        if (!rules[i].seen && rules[i].add) rule_emit(&o, &rules[i]);
    if (o.over) return -1;
    out[o.len] = '\0';
    return (int)o.len;
}

int sys_ssh_set_password_auth(const char *text, bool password_yes,
                              char *out, size_t out_len) {
    Rule r[1] = { { "PasswordAuthentication", password_yes ? "yes" : "no", true, false } };
    return rewrite(text, r, 1, out, out_len);
}

int sys_ssh_set_mode(const char *text, SshMode mode, char *out, size_t out_len) {
    if (mode != SSH_MODE_KEY_ONLY && mode != SSH_MODE_PASSWORD) return -1;
    Rule r[2] = {
        { "PasswordAuthentication", mode == SSH_MODE_PASSWORD ? "yes" : "no", true, false },
        { "PubkeyAuthentication", "yes", false, false },
    };
    return rewrite(text, r, 2, out, out_len);
}

int sys_ssh_set_root_login(const char *text, SshRootLogin root, char *out, size_t out_len) {
    if (root != SSH_ROOT_KEY && root != SSH_ROOT_PASSWORD) return -1;
    Rule r[1] = { { "PermitRootLogin", root == SSH_ROOT_PASSWORD ? "yes" : "prohibit-password",
                    true, false } };
    return rewrite(text, r, 1, out, out_len);
}

const char *sys_ssh_refuse(SshMode mode, SshRootLogin root, bool has_key, bool has_password) {
    if (mode == SSH_MODE_UNKNOWN || root == SSH_ROOT_UNKNOWN) return "SETTING NOT UNDERSTOOD";
    if (root == SSH_ROOT_PASSWORD && !has_password) return "ROOT LOGIN NEEDS A PASSWORD FIRST";
    if (mode == SSH_MODE_KEY_ONLY && !has_key) return "NO AUTHORIZED KEY FOR ROOT";
    if (mode == SSH_MODE_PASSWORD) {
        if (!has_password) return "SET A PASSWORD FIRST";
        if (!has_key && root != SSH_ROOT_PASSWORD) return "NO KEY AND ROOT LOGIN IS KEY ONLY";
    }
    return NULL;
}

/* A lockout only counts when it takes access away.  A fresh card starts at Key only with
 * no key, which sys_ssh_refuse() already calls locked out, so judging the target alone
 * refused every way out of it (root login first: "no key"; Key + password first: "root
 * login is key only").  When the current state gives no SSH access (Off, or locked out),
 * a change cannot make it worse, and only the password and parse rules apply. */
const char *sys_ssh_refuse_change(SshMode cur_mode, SshRootLogin cur_root,
                                  SshMode mode, SshRootLogin root,
                                  bool has_key, bool has_password) {
    const char *why = sys_ssh_refuse(mode, root, has_key, has_password);
    if (why == NULL || mode == SSH_MODE_UNKNOWN || root == SSH_ROOT_UNKNOWN) return why;
    if (root == SSH_ROOT_PASSWORD && !has_password) return why;
    if (mode == SSH_MODE_PASSWORD && !has_password) return why;
    bool cur_open = cur_mode != SSH_MODE_OFF && cur_mode != SSH_MODE_UNKNOWN &&
                    sys_ssh_refuse(cur_mode, cur_root, has_key, has_password) == NULL;
    return cur_open ? why : NULL;
}

/* ── The root shadow line ───────────────────────────────────────────────── */

/* The root line of shadow text: its start, and the span of its password field
 * (colon to colon).  false when there is none. */
static bool root_line(const char *text, const char **line, const char **f0,
                      const char **f1, const char **eol) {
    const char *p = text;
    while (*p) {
        size_t n = strcspn(p, "\n");
        if (n > 5 && strncmp(p, "root:", 5) == 0) {
            const char *e = p + n;
            const char *a = p + 5;
            const char *b = memchr(a, ':', (size_t)(e - a));
            *line = p; *f0 = a; *f1 = b ? b : e; *eol = e;
            return true;
        }
        p += n;
        if (*p == '\n') p++;
    }
    return false;
}

bool sys_shadow_root_has_password(const char *text) {
    const char *l, *a, *b, *e;
    if (!text || !root_line(text, &l, &a, &b, &e)) return false;
    return b > a && *a != '*' && *a != '!';
}

bool sys_hash_plausible(const char *h) {
    if (!h || h[0] != '$') return false;
    size_t n = strlen(h);
    if (n < 13 || n > 255) return false;
    for (size_t i = 0; i < n; i++) {
        char c = h[i];
        if (!(isalnum((unsigned char)c) || c == '.' || c == '/' || c == '$')) return false;
    }
    return true;
}

int sys_shadow_set_root(const char *text, const char *hash, char *out, size_t out_len) {
    const char *l, *a, *b, *e;
    if (!text || !out || out_len < 2 || !sys_hash_plausible(hash) ||
        !root_line(text, &l, &a, &b, &e)) return -1;
    OutBuf o = { out, out_len, 0, false };
    const char *p = text;
    while (*p) {
        size_t n = strcspn(p, "\n");
        if (p == l) {
            ob_put(&o, "root:", 5);
            ob_put(&o, hash, strlen(hash));
            size_t rest = (size_t)(e - b);                 /* ":" and the later fields */
            while (rest && b[rest - 1] == '\r') rest--;
            ob_put(&o, b, rest);
            ob_put(&o, "\n", 1);
        } else {
            ob_line(&o, p, n);
        }
        p += n;
        if (*p == '\n') p++;
    }
    if (o.over) return -1;
    out[o.len] = '\0';
    return (int)o.len;
}

bool sys_hostname_valid(const char *name) {
    if (!name) return false;
    size_t n = strlen(name);
    if (n < 1 || n > 63 || name[0] == '-' || name[n - 1] == '-') return false;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)name[i];
        if (!(c < 128 && (isalnum(c) || c == '-'))) return false;
    }
    return true;
}

static bool is_b64(char c) {
    return isalnum((unsigned char)c) || c == '+' || c == '/' || c == '=';
}

static bool key_type(const char *t, size_t n) {
    return (n > 4 && strncmp(t, "ssh-", 4) == 0) ||
           (n > 11 && strncmp(t, "ecdsa-sha2-", 11) == 0) ||
           (n > 3 && strncmp(t, "sk-", 3) == 0);
}

bool sys_authkeys_plausible(const char *text) {
    if (!text) return false;
    const char *p = text;
    while (*p) {
        size_t n = strcspn(p, "\n");
        const char *e = p + n;
        const char *s = p;
        p = *e == '\n' ? e + 1 : e;
        while (s < e && isspace((unsigned char)*s)) s++;
        if (s == e || *s == '#') continue;
        /* tokens: [options] type blob [comment] */
        while (s < e) {
            const char *t = s;
            while (s < e && !isspace((unsigned char)*s)) s++;
            size_t tn = (size_t)(s - t);
            while (s < e && isspace((unsigned char)*s)) s++;
            if (!key_type(t, tn)) continue;
            const char *b = s;
            while (s < e && is_b64(*s)) s++;
            if ((size_t)(s - b) >= 20 && (s == e || isspace((unsigned char)*s))) return true;
            break;
        }
    }
    return false;
}

int sys_days_in_month(int y, int m) {
    static const int d[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (m < 1 || m > 12) return 0;
    if (m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) return 29;
    return d[m - 1];
}

static int wrap(int v, int lo, int hi) {
    return v > hi ? lo : v < lo ? hi : v;
}

void sys_dt_step(SysDateTime *dt, int field, int dir) {
    dir = dir < 0 ? -1 : 1;
    switch (field) {
    case SYS_DT_YEAR:
        dt->year += dir;
        if (dt->year < SYS_YEAR_MIN) dt->year = SYS_YEAR_MIN;
        if (dt->year > SYS_YEAR_MAX) dt->year = SYS_YEAR_MAX;
        break;
    case SYS_DT_MON:  dt->mon  = wrap(dt->mon + dir, 1, 12); break;
    case SYS_DT_DAY:  dt->day  = wrap(dt->day + dir, 1, sys_days_in_month(dt->year, dt->mon)); break;
    case SYS_DT_HOUR: dt->hour = wrap(dt->hour + dir, 0, 23); break;
    case SYS_DT_MIN:  dt->min  = wrap(dt->min + dir, 0, 59); break;
    default: break;
    }
    int dim = sys_days_in_month(dt->year, dt->mon);
    if (dim && dt->day > dim) dt->day = dim;
}

bool sys_dt_valid(const SysDateTime *dt) {
    return dt->year >= SYS_YEAR_MIN && dt->year <= SYS_YEAR_MAX &&
           dt->mon >= 1 && dt->mon <= 12 &&
           dt->day >= 1 && dt->day <= sys_days_in_month(dt->year, dt->mon) &&
           dt->hour >= 0 && dt->hour <= 23 && dt->min >= 0 && dt->min <= 59;
}

void sys_dt_format(const SysDateTime *dt, char *out, size_t len) {
    if (!len) return;
    if (!sys_dt_valid(dt)) { out[0] = '\0'; return; }
    snprintf(out, len, "%04d-%02d-%02d %02d:%02d:00",
             dt->year, dt->mon, dt->day, dt->hour, dt->min);
}

/* ── Timezone ───────────────────────────────────────────────────────────── */

static const char *const tz_zones[] = {
    "UTC", "America/New_York", "America/Chicago", "America/Denver",
    "America/Los_Angeles", "Europe/London", "Europe/Berlin",
    "Europe/Budapest", "Asia/Tokyo", "Australia/Sydney",
};
#define TZ_N ((int)(sizeof(tz_zones) / sizeof(tz_zones[0])))

int sys_tz_count(void) { return TZ_N; }

const char *sys_tz_name(int idx) {
    return idx >= 0 && idx < TZ_N ? tz_zones[idx] : NULL;
}

int sys_tz_from_link(const char *target) {
    if (!target) return -1;
    const char *p = strstr(target, "zoneinfo/");
    if (!p) return -1;
    p += strlen("zoneinfo/");
    for (int i = 0; i < TZ_N; i++)
        if (strcmp(p, tz_zones[i]) == 0) return i;
    return -1;
}

int sys_tz_step(int idx, int dir) {
    if (idx < 0 || idx >= TZ_N) return dir < 0 ? TZ_N - 1 : 0;
    return (idx + (dir < 0 ? TZ_N - 1 : 1)) % TZ_N;
}
