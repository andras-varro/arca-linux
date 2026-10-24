/* jf_login.c — see jf_login.h. */

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "config.h"
#include "jf_login.h"
#include "jf_url.h"

/* Not memset: the compiler may drop a memset of a buffer it can see is not read again. */
void jf_wipe(void *p, size_t n)
{
    volatile unsigned char *q = p;
    while (n--) *q++ = 0;
}

void jf_login_wipe(JfLogin *l)
{
    jf_wipe(l, sizeof *l);
}

static void copy(char *dst, size_t cap, const char *src)
{
    snprintf(dst, cap, "%s", src);
}

/* "arca-" + hostname + `suffix`, reduced to the characters a header value may carry; the suffix is kept whole (the hostname is cut for it). */
static void default_device_id(char *out, size_t cap, const char *suffix)
{
    char host[48];
    size_t j = 0;
    if (gethostname(host, sizeof host) != 0) host[0] = '\0';
    host[sizeof host - 1] = '\0';
    char id[64] = "arca-";
    j = strlen(id);
    size_t sl = strlen(suffix);
    size_t room = sl < sizeof id - 6 ? sizeof id - 1 - sl : sizeof id - 1;   /* an absurd suffix gets none: it is the app's constant */
    for (size_t i = 0; host[i] && j < room; i++) {
        char c = host[i];
        if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-' || c == '.' || c == '_')
            id[j++] = c;
    }
    id[j] = '\0';
    if (j <= 5) { strcpy(id, "arca-unit"); j = strlen(id); }
    if (sl < sizeof id - j) strcpy(id + j, suffix);
    copy(out, cap, id);
}

int jf_login_load(JfLogin *l, const char *path, const char *id_suffix)
{
    Config cfg;
    memset(l, 0, sizeof *l);
    l->volume_step = JF_VOLUME_DEFAULT;

    config_init_path(&cfg, path);
    int rc = config_load(&cfg) == 0 ? 0 : -1;
    if (rc == 0) {
        copy(l->server, sizeof l->server, config_get(&cfg, "server", ""));
        copy(l->user, sizeof l->user, config_get(&cfg, "user", ""));
        copy(l->token, sizeof l->token, config_get(&cfg, "token", ""));
        copy(l->device_id, sizeof l->device_id, config_get(&cfg, "device_id", ""));
        const char *uid = config_get(&cfg, "user_id", "");
        if (jf_id_valid(uid)) copy(l->user_id, sizeof l->user_id, uid);
    }
    jf_wipe(&cfg, sizeof cfg);   /* it held the token */
    if (!l->device_id[0]) default_device_id(l->device_id, sizeof l->device_id, id_suffix ? id_suffix : "");
    return rc;
}

/* config.c makes one directory level; the path may be deeper (and a test's is). */
static void mkdir_parents(const char *path)
{
    char d[160];
    snprintf(d, sizeof d, "%s", path);
    for (char *p = d + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        mkdir(d, 0755);
        *p = '/';
    }
}

int jf_login_save(const JfLogin *l, const char *path)
{
    Config cfg;

    /* Fresh, not loaded: nothing foreign (an old password key) is carried along. */
    config_init_path(&cfg, path);
    config_set(&cfg, "server", l->server);
    config_set(&cfg, "user", l->user);
    if (l->user_id[0]) config_set(&cfg, "user_id", l->user_id);
    if (l->token[0]) config_set(&cfg, "token", l->token);
    config_set(&cfg, "device_id", l->device_id);

    mode_t old = umask(077);   /* the temp file is born 0600, not world-readable for a moment */
    mkdir_parents(path);
    int rc = config_save(&cfg);
    umask(old);
    if (rc == 0 && chmod(path, 0600) != 0) rc = -1;
    jf_wipe(&cfg, sizeof cfg);
    return rc == 0 ? 0 : -1;
}

int jf_settings_load(JfLogin *l, const char *path)
{
    Config cfg;
    l->library[0] = l->library_name[0] = '\0';
    l->volume_step = JF_VOLUME_DEFAULT;
    l->tap_action = JF_TAP_REPLACE;
    l->tags_on = 0;
    l->tags_cut[0] = '\0';

    config_init_path(&cfg, path);
    int rc = config_load(&cfg) == 0 ? 0 : -1;
    if (rc == 0) {
        const char *lib = config_get(&cfg, "library", "");
        if (jf_id_valid(lib)) {
            copy(l->library, sizeof l->library, lib);
            copy(l->library_name, sizeof l->library_name, config_get(&cfg, "library_name", ""));
        }
        int v = config_get_int(&cfg, "volume_step", JF_VOLUME_DEFAULT);
        l->volume_step = v < 0 ? 0 : v > JF_VOLUME_MAX ? JF_VOLUME_MAX : v;
        int ta = config_get_int(&cfg, "tap_action", JF_TAP_REPLACE);
        l->tap_action = ta >= 0 && ta < JF_TAP_COUNT ? ta : JF_TAP_REPLACE;
        l->tags_on = config_get_int(&cfg, "tags_read", 0) == 1;
        copy(l->tags_cut, sizeof l->tags_cut, config_get(&cfg, "tags_cut", ""));
    }
    return rc;
}

int jf_settings_save(const JfLogin *l, const char *path)
{
    Config cfg;
    int vs = l->volume_step < 0 ? 0 : l->volume_step > JF_VOLUME_MAX ? JF_VOLUME_MAX : l->volume_step;

    config_init_path(&cfg, path);
    if (l->library[0]) {
        config_set(&cfg, "library", l->library);
        config_set(&cfg, "library_name", l->library_name);
    }
    config_set_int(&cfg, "volume_step", vs);
    config_set_int(&cfg, "tap_action", l->tap_action >= 0 && l->tap_action < JF_TAP_COUNT ? l->tap_action : JF_TAP_REPLACE);
    config_set_int(&cfg, "tags_read", l->tags_on ? 1 : 0);
    if (l->tags_cut[0]) config_set(&cfg, "tags_cut", l->tags_cut);

    mkdir_parents(path);
    int rc = config_save(&cfg);
    return rc == 0 ? 0 : -1;
}

int jf_login_clear(JfLogin *l, const char *path)
{
    jf_wipe(l->token, sizeof l->token);
    jf_wipe(l->user_id, sizeof l->user_id);
    return jf_login_save(l, path);
}

int jf_login_signout(JfLogin *l, const char *path)
{
    jf_wipe(l->library, sizeof l->library);
    jf_wipe(l->library_name, sizeof l->library_name);
    return jf_login_clear(l, path);
}
