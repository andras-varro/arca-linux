#include "jv_settings.h"

#include <string.h>

#include "config.h"
#include "jf_url.h"

static const char *const mode_names[] = { "off", "lang", "forced", "foreign" };

const char *jv_sub_mode_name(JvSubMode m)
{
    return (unsigned)m < 4u ? mode_names[m] : mode_names[0];
}

static const char *const off_keys[JV_SINK_COUNT] = { "av_offset_onboard_ms", "av_offset_usb_ms", "av_offset_bt_ms" };

void jv_settings_defaults(JvSettings *s)
{
    memset(s, 0, sizeof *s);
    strcpy(s->audio_lang, "default");
    s->sub_mode = JV_SUB_OFF;
    strcpy(s->sub_lang, "hun");
    s->sub_scale = 2;
    s->agc = true;
}

/* A language list is letters and commas only, and fits: else the default stands. */
static void take_langs(Config *cfg, const char *key, char *dst)
{
    const char *v = config_get(cfg, key, "");
    size_t n = strlen(v);
    if (n == 0 || n >= JV_LANGS_LEN) return;
    for (size_t i = 0; i < n; i++) {
        char c = v[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == ',')) return;
    }
    memcpy(dst, v, n + 1);
}

static void take_lib(Config *cfg, const char *key, char *dst)
{
    const char *v = config_get(cfg, key, "");
    if (jf_id_valid(v)) memcpy(dst, v, JF_ID_LEN + 1);
}

int jv_settings_load(JvSettings *s, const char *path)
{
    Config cfg;
    const char *m;
    int v;
    jv_settings_defaults(s);
    config_init_path(&cfg, path);
    if (config_load(&cfg) != 0) return -1;
    take_langs(&cfg, "audio_lang", s->audio_lang);
    take_langs(&cfg, "sub_lang", s->sub_lang);
    m = config_get(&cfg, "sub_mode", "");
    for (int i = 0; i < 4; i++) if (!strcmp(m, mode_names[i])) s->sub_mode = (JvSubMode)i;
    v = config_get_int(&cfg, "sub_scale", 2);
    s->sub_scale = v == 3 ? 3 : 2;
    for (int i = 0; i < JV_SINK_COUNT; i++) {
        v = config_get_int(&cfg, off_keys[i], 0);
        s->av_offset_ms[i] = v < -JV_OFFSET_MAX_MS ? -JV_OFFSET_MAX_MS : v > JV_OFFSET_MAX_MS ? JV_OFFSET_MAX_MS : v;
    }
    s->agc = config_get_bool(&cfg, "agc", true);
    take_lib(&cfg, "library_movies", s->library_movies);
    take_lib(&cfg, "library_shows", s->library_shows);
    return 0;
}

int jv_settings_save(const JvSettings *s, const char *path)
{
    Config cfg;
    config_init_path(&cfg, path);
    config_set(&cfg, "audio_lang", s->audio_lang);
    config_set(&cfg, "sub_mode", jv_sub_mode_name(s->sub_mode));
    config_set(&cfg, "sub_lang", s->sub_lang);
    config_set_int(&cfg, "sub_scale", s->sub_scale == 3 ? 3 : 2);
    for (int i = 0; i < JV_SINK_COUNT; i++) {
        int v = s->av_offset_ms[i];
        config_set_int(&cfg, off_keys[i], v < -JV_OFFSET_MAX_MS ? -JV_OFFSET_MAX_MS : v > JV_OFFSET_MAX_MS ? JV_OFFSET_MAX_MS : v);
    }
    config_set_bool(&cfg, "agc", s->agc);
    config_set(&cfg, "library_movies", s->library_movies);
    config_set(&cfg, "library_shows", s->library_shows);
    return config_save(&cfg) == 0 ? 0 : -1;
}
