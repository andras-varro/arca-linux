#include "jv_last.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "jf_url.h"

static void blank(JvLast *r)
{
    memset(r, 0, sizeof *r);
    r->audio_index = -1;
    r->sub_index = -1;
}

int jv_last_load(JvLast *r, const char *path)
{
    Config cfg;
    blank(r);
    config_init_path(&cfg, path);
    if (config_load(&cfg) != 0) return -1;
    const char *id = config_get(&cfg, "id", "");
    if (!jf_id_valid(id)) return -1;
    memcpy(r->id, id, JF_ID_LEN + 1);
    int v = config_get_int(&cfg, "position_s", 0);
    r->position_s = v < 0 ? 0 : v;
    v = config_get_int(&cfg, "audio_index", -1);
    r->audio_index = v < -1 ? -1 : v;
    v = config_get_int(&cfg, "sub_index", -1);
    r->sub_index = v < -1 ? -1 : v;
    r->saved_at = strtoll(config_get(&cfg, "saved_at", "0"), NULL, 10);
    if (r->saved_at < 0) r->saved_at = 0;
    r->attempted = config_get_bool(&cfg, "attempted", false);
    return 0;
}

int jv_last_save(const JvLast *r, const char *path)
{
    Config cfg;
    char t[24];
    config_init_path(&cfg, path);
    config_set(&cfg, "id", r->id);
    config_set_int(&cfg, "position_s", r->position_s < 0 ? 0 : r->position_s);
    config_set_int(&cfg, "audio_index", r->audio_index < -1 ? -1 : r->audio_index);
    config_set_int(&cfg, "sub_index", r->sub_index < -1 ? -1 : r->sub_index);
    snprintf(t, sizeof t, "%lld", r->saved_at);
    config_set(&cfg, "saved_at", t);
    config_set_bool(&cfg, "attempted", r->attempted);
    return config_save(&cfg) == 0 ? 0 : -1;
}

int jv_last_clear(const char *path)
{
    return remove(path) == 0 || errno == ENOENT ? 0 : -1;
}
