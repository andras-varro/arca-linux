/*
 * Configuration Library — implementation.
 * See config.h for API documentation.
 */

#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <libgen.h>

/* ── Atomic file writes (see config.h) ───────────────────────────────────── */

/* Copy the parent directory of path into buf. "." if there is no '/'. */
static void parent_dir(const char *path, char *buf, size_t n) {
    char scratch[256];
    strncpy(scratch, path, sizeof(scratch) - 1);
    scratch[sizeof(scratch) - 1] = '\0';
    const char *d = dirname(scratch);
    strncpy(buf, d, n - 1);
    buf[n - 1] = '\0';
}

/* fsync a directory so a rename inside it is durable. Best effort: some
 * filesystems refuse O_RDONLY fsync on a directory, and that is not fatal. */
static void fsync_dir(const char *dir) {
    int fd = open(dir, O_RDONLY);
    if (fd < 0) return;
    (void)fsync(fd);
    close(fd);
}

FILE *file_write_atomic_open(const char *path, char *tmp_path, size_t tmp_sz) {
    if (!path || !tmp_path || tmp_sz == 0) return NULL;

    char dir[256];
    parent_dir(path, dir, sizeof(dir));
    mkdir(dir, 0755);   /* ok if it already exists */

    int n = snprintf(tmp_path, tmp_sz, "%s.tmp", path);
    if (n < 0 || (size_t)n >= tmp_sz) {
        tmp_path[0] = '\0';
        return NULL;    /* would truncate the temp name onto the real one */
    }

    return fopen(tmp_path, "w");
}

int file_write_atomic_commit(FILE *f, const char *tmp_path, const char *path) {
    if (!f) return -1;

    if (fflush(f) != 0 || fsync(fileno(f)) != 0) {
        fclose(f);
        unlink(tmp_path);
        return -1;
    }
    if (fclose(f) != 0) {
        unlink(tmp_path);
        return -1;
    }
    if (rename(tmp_path, path) != 0) {
        unlink(tmp_path);
        return -1;
    }

    /* The data is durable but the directory entry may not be. */
    char dir[256];
    parent_dir(path, dir, sizeof(dir));
    fsync_dir(dir);
    return 0;
}

void file_write_atomic_abort(FILE *f, const char *tmp_path) {
    if (f) fclose(f);
    if (tmp_path && tmp_path[0]) unlink(tmp_path);
}

/* ── Internal helpers ───────────────────────────────────────────────────── */

/** Trim leading and trailing whitespace in-place. Returns pointer to trimmed start. */
static char *trim(char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    if (*s == '\0') return s;
    char *end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end)) *end-- = '\0';
    return s;
}

/** Case-insensitive string compare. */
static int str_eq_nocase(const char *a, const char *b) {
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
            return 0;
        a++;
        b++;
    }
    return (*a == '\0' && *b == '\0');
}

/** Find index of key in entries, or -1 if not found. */
static int find_key(const Config *cfg, const char *key) {
    for (int i = 0; i < cfg->count; i++) {
        if (strcmp(cfg->entries[i].key, key) == 0)
            return i;
    }
    return -1;
}

/* ── Lifecycle ───────────────────────────────────────────────────────────── */

void config_init(Config *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    strncpy(cfg->filepath, CONFIG_FILE_PATH, sizeof(cfg->filepath) - 1);
    cfg->count = 0;
}

void config_init_path(Config *cfg, const char *path) {
    memset(cfg, 0, sizeof(*cfg));
    strncpy(cfg->filepath, path, sizeof(cfg->filepath) - 1);
    cfg->count = 0;
}

int config_load_sound_set(Config *cfg, const char *tag) {
    /* The longest real path is /opt/arca/soundsets/brick_breaker.sound at
     * 45 bytes against Config::filepath's 128, so this cannot truncate for any
     * tag a game passes — but snprintf bounds it anyway, because a truncated
     * path would open a DIFFERENT file rather than fail. */
    char path[128];
    snprintf(path, sizeof(path), "%s/%s%s", CONFIG_SOUND_SET_DIR,
             (tag && *tag) ? tag : "default", CONFIG_SOUND_SET_EXT);
    config_init_path(cfg, path);
    return config_load(cfg);
}

/* ── File I/O ────────────────────────────────────────────────────────────── */

int config_load(Config *cfg) {
    cfg->count = 0;

    FILE *f = fopen(cfg->filepath, "r");
    if (!f) return -1;

    char line[CONFIG_KEY_LEN + CONFIG_VAL_LEN + 16];
    while (fgets(line, sizeof(line), f)) {
        /* Strip newline */
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';

        char *trimmed = trim(line);

        /* Skip blank lines and comments */
        if (*trimmed == '\0' || *trimmed == '#')
            continue;

        /* Find '=' separator */
        char *eq = strchr(trimmed, '=');
        if (!eq) continue;

        /* Split into key and value */
        *eq = '\0';
        char *key = trim(trimmed);
        char *val = trim(eq + 1);

        if (*key == '\0') continue;

        /* Store entry */
        if (cfg->count < CONFIG_MAX_KEYS) {
            strncpy(cfg->entries[cfg->count].key, key, CONFIG_KEY_LEN - 1);
            cfg->entries[cfg->count].key[CONFIG_KEY_LEN - 1] = '\0';
            strncpy(cfg->entries[cfg->count].value, val, CONFIG_VAL_LEN - 1);
            cfg->entries[cfg->count].value[CONFIG_VAL_LEN - 1] = '\0';
            cfg->count++;
        }
    }

    fclose(f);
    return 0;
}

int config_save(const Config *cfg) {
    char tmp_path[160];
    FILE *f = file_write_atomic_open(cfg->filepath, tmp_path, sizeof(tmp_path));
    if (!f) return -1;

    if (fprintf(f, "# Configuration\n") < 0 ||
        fprintf(f, "# Auto-generated — edit with control_panel\n\n") < 0) {
        file_write_atomic_abort(f, tmp_path);
        return -1;
    }

    for (int i = 0; i < cfg->count; i++) {
        if (fprintf(f, "%s=%s\n", cfg->entries[i].key, cfg->entries[i].value) < 0) {
            file_write_atomic_abort(f, tmp_path);
            return -1;
        }
    }

    return file_write_atomic_commit(f, tmp_path, cfg->filepath);
}

/* ── Getters ─────────────────────────────────────────────────────────────── */

const char *config_get(const Config *cfg, const char *key, const char *default_val) {
    int idx = find_key(cfg, key);
    if (idx < 0) return default_val;
    return cfg->entries[idx].value;
}

int config_get_int(const Config *cfg, const char *key, int default_val) {
    const char *val = config_get(cfg, key, NULL);
    if (!val) return default_val;

    /* Verify the string is actually numeric (optional leading '-', then digits) */
    const char *p = val;
    if (*p == '-' || *p == '+') p++;
    if (*p == '\0') return default_val;
    while (*p) {
        if (!isdigit((unsigned char)*p)) return default_val;
        p++;
    }

    return atoi(val);
}

bool config_get_bool(const Config *cfg, const char *key, bool default_val) {
    const char *val = config_get(cfg, key, NULL);
    if (!val) return default_val;

    if (str_eq_nocase(val, "1") ||
        str_eq_nocase(val, "true") ||
        str_eq_nocase(val, "yes") ||
        str_eq_nocase(val, "on")) {
        return true;
    }

    if (str_eq_nocase(val, "0") ||
        str_eq_nocase(val, "false") ||
        str_eq_nocase(val, "no") ||
        str_eq_nocase(val, "off")) {
        return false;
    }

    return default_val;
}

/* ── Setters ─────────────────────────────────────────────────────────────── */

void config_set(Config *cfg, const char *key, const char *value) {
    int idx = find_key(cfg, key);
    if (idx >= 0) {
        /* Update existing */
        strncpy(cfg->entries[idx].value, value, CONFIG_VAL_LEN - 1);
        cfg->entries[idx].value[CONFIG_VAL_LEN - 1] = '\0';
        return;
    }

    /* Append new entry — silently fail if full */
    if (cfg->count >= CONFIG_MAX_KEYS) return;

    strncpy(cfg->entries[cfg->count].key, key, CONFIG_KEY_LEN - 1);
    cfg->entries[cfg->count].key[CONFIG_KEY_LEN - 1] = '\0';
    strncpy(cfg->entries[cfg->count].value, value, CONFIG_VAL_LEN - 1);
    cfg->entries[cfg->count].value[CONFIG_VAL_LEN - 1] = '\0';
    cfg->count++;
}

void config_set_int(Config *cfg, const char *key, int value) {
    char buf[CONFIG_VAL_LEN];
    snprintf(buf, sizeof(buf), "%d", value);
    config_set(cfg, key, buf);
}

void config_set_bool(Config *cfg, const char *key, bool value) {
    config_set(cfg, key, value ? "1" : "0");
}

/* ── Removal ─────────────────────────────────────────────────────────────── */

bool config_remove(Config *cfg, const char *key) {
    int idx = find_key(cfg, key);
    if (idx < 0) return false;

    /* Shift remaining entries down */
    for (int i = idx; i < cfg->count - 1; i++) {
        cfg->entries[i] = cfg->entries[i + 1];
    }
    cfg->count--;
    return true;
}

void config_clear(Config *cfg) {
    cfg->count = 0;
}

/* ── Convenience helpers ─────────────────────────────────────────────────── */

bool config_audio_enabled(const Config *cfg) {
    return config_get_bool(cfg, "audio_enabled", true);
}

bool config_music_enabled(const Config *cfg) {
    return config_get_bool(cfg, "music_enabled", true);
}

bool config_effects_enabled(const Config *cfg) {
    return config_get_bool(cfg, "effects_enabled", true);
}

const char *config_audio_device(const Config *cfg) {
    return config_get(cfg, "audio_device", "auto");
}

const char *config_audio_device_stored(void) {
    static char buf[CONFIG_VAL_LEN];
    Config cfg;
    config_init(&cfg);
    config_load(&cfg);          /* silent if the file is missing */
    snprintf(buf, sizeof(buf), "%s", config_audio_device(&cfg));
    return buf;
}

const char *config_audio_bt_addr(const Config *cfg) {
    return config_get(cfg, "audio_bt_addr", "");
}

const char *config_audio_bt_addr_stored(void) {
    static char buf[CONFIG_VAL_LEN];
    Config cfg;
    config_init(&cfg);
    config_load(&cfg);          /* silent if the file is missing */
    snprintf(buf, sizeof(buf), "%s", config_audio_bt_addr(&cfg));
    return buf;
}

bool config_led_enabled(const Config *cfg) {
    return config_get_bool(cfg, "led_enabled", true);
}

int config_led_brightness(const Config *cfg) {
    return config_get_int(cfg, "led_brightness", 100);
}

int config_backlight_brightness(const Config *cfg) {
    return config_get_int(cfg, "backlight_brightness", 100);
}

int config_blank_minutes(const Config *cfg) {
    int m = config_get_int(cfg, "blank_minutes", 10);
    return m < 0 ? 0 : m;
}

bool config_clock_24h(const Config *cfg) {
    return config_get_bool(cfg, "clock_24h", true);
}
