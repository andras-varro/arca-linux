/* Host regression for jf_shared/jf_login.c: the saved login file and the settings file.
 *
 *   cd native_apps && gcc -Wall -Wextra -I common -I jellyfin -I jf_shared -o build/jf_login_test \
 *       tests/jf_login_test.c jf_shared/jf_login.c jf_shared/jf_url.c common/config.c \
 *       && ./build/jf_login_test
 *
 * The fixture directory comes from mkdtemp under /tmp and the file sits in a
 * subdirectory that does not exist yet, which is what proves the parent is
 * created.  /tmp matters: /mnt/c (DrvFs) reports every file 0777 and discards
 * chmod, so the 0600 check only means something on a real filesystem.  Run it
 * inside one WSL call.  Fake token and ids only.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "jf_login.h"

static int checks, fails;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); \
    printf(__VA_ARGS__); printf("\n"); } } while (0)

#define UID_ "00000000000000000000000000000002"
#define TOK "fedcba9876543210fedcba9876543210"

static char dir[64];
static char path[160];
static char spath[160];

static char *slurp(const char *p)
{
    static char buf[4096];
    FILE *f = fopen(p, "r");
    size_t n = 0;
    buf[0] = '\0';
    if (f) { n = fread(buf, 1, sizeof buf - 1, f); fclose(f); }
    buf[n] = '\0';
    return buf;
}

static int mode_of(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0 ? (int)(st.st_mode & 0777) : -1;
}

static void put(const char *p, const char *text)
{
    FILE *f = fopen(p, "w");
    if (f) { fputs(text, f); fclose(f); }
}

static void fill(JfLogin *l)
{
    memset(l, 0, sizeof *l);
    snprintf(l->server, sizeof l->server, "192.0.2.10:8096");
    snprintf(l->user, sizeof l->user, "user1");
    snprintf(l->user_id, sizeof l->user_id, "%s", UID_);
    snprintf(l->token, sizeof l->token, "%s", TOK);
    snprintf(l->device_id, sizeof l->device_id, "arca-test");
    l->volume_step = 7;
}

static void test_roundtrip(void)
{
    JfLogin a, b;
    fill(&a);
    CHECK(access(path, F_OK) != 0, "the nested dir does not exist yet");
    CHECK(jf_login_save(&a, path) == 0, "save creates the parent and writes");
    CHECK(mode_of(path) == 0600, "mode after save %o", mode_of(path));
    CHECK(jf_login_load(&b, path, "") == 0, "load");
    CHECK(!strcmp(a.server, b.server) && !strcmp(a.user, b.user) && !strcmp(a.user_id, b.user_id)
          && !strcmp(a.token, b.token) && !strcmp(a.device_id, b.device_id),
          "round trip equal: '%s' '%s' '%s' '%s' '%s'", b.server, b.user, b.user_id, b.token, b.device_id);
    char *t = slurp(path);
    CHECK(!strstr(t, "password") && !strstr(t, "Password") && !strstr(t, "pw"), "no password key in the file");
    CHECK(strstr(t, "token=" TOK) && strstr(t, "user_id=" UID_), "token and id are in the file");

    /* an old file that somehow carries a password: a save drops it */
    put(path, "server=x\npassword=hunter2\ntoken=" TOK "\n");
    jf_login_load(&b, path, "");
    jf_login_save(&b, path);
    CHECK(!strstr(slurp(path), "hunter2") && !strstr(slurp(path), "password"), "save does not carry a foreign password key");
    CHECK(mode_of(path) == 0600, "mode after re-save %o", mode_of(path));

    /* an existing wide-open file is narrowed */
    chmod(path, 0644);
    jf_login_save(&a, path);
    CHECK(mode_of(path) == 0600, "0644 file is narrowed to %o", mode_of(path));
    CHECK(!strstr(slurp(path), "hunter2"), "stale value gone");

    /* a leftover temp file keeps its old mode through fopen("w") and the rename:
     * umask alone cannot cover that, the chmod after the write does */
    char tmp[200];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    put(tmp, "stale\n");
    chmod(tmp, 0644);
    jf_login_save(&a, path);
    CHECK(mode_of(path) == 0600, "leftover 0644 temp file: final mode %o", mode_of(path));
}

static void test_clear(void)
{
    JfLogin a, b;
    fill(&a);
    jf_login_save(&a, path);
    CHECK(jf_login_clear(&a, path) == 0, "clear");
    CHECK(a.token[0] == '\0' && a.user_id[0] == '\0', "struct token/user_id emptied");
    CHECK(a.token[5] == '\0' && a.token[40] == '\0', "token bytes wiped, not just terminated");
    CHECK(mode_of(path) == 0600, "mode after clear %o", mode_of(path));
    char *t = slurp(path);
    CHECK(!strstr(t, TOK) && !strstr(t, UID_) && !strstr(t, "token") && !strstr(t, "user_id"), "no token/user_id text after clear");
    CHECK(!strstr(t, "password"), "no password after clear");
    CHECK(jf_login_load(&b, path, "") == 0, "load after clear");
    CHECK(!strcmp(b.server, "192.0.2.10:8096") && !strcmp(b.user, "user1") && !strcmp(b.device_id, "arca-test")
          && !b.token[0] && !b.user_id[0], "server/user/device_id kept");
}

static void test_load_edges(void)
{
    JfLogin l;
    char missing[200];
    snprintf(missing, sizeof missing, "%s/none/ludfin.conf", dir);
    memset(&l, 0xAA, sizeof l);
    CHECK(jf_login_load(&l, missing, "") == -1, "missing file -> -1");
    CHECK(!l.server[0] && !l.user[0] && !l.token[0] && !l.user_id[0] && l.volume_step == 10, "defaults filled");
    CHECK(!strncmp(l.device_id, "arca-", 5) && strlen(l.device_id) > 5, "device_id from host: '%s'", l.device_id);
    CHECK(access(missing, F_OK) != 0, "load did not create the file");

    put(path, "server=h\nvolume_step=99\nuser_id=nothex\ntoken=" TOK "\n");
    CHECK(jf_login_load(&l, path, "") == 0, "load odd file");
    CHECK(l.volume_step == 10 && !l.library[0], "a settings key in the login file is ignored, volume %d", l.volume_step);
    CHECK(l.user_id[0] == '\0', "invalid user_id dropped");
    CHECK(!strcmp(l.token, TOK) && !strncmp(l.device_id, "arca-", 5), "token kept, device_id defaulted");
    put(path, "user_id=" UID_ "0\n");
    jf_login_load(&l, path, "");
    CHECK(l.user_id[0] == '\0', "33-char id dropped");
    put(path, "user_id=" UID_ "\ndevice_id=mine\n");
    jf_login_load(&l, path, "");
    CHECK(!strcmp(l.user_id, UID_) && !strcmp(l.device_id, "mine"), "good id and stored device_id kept");

    /* empty token / user_id are omitted, not written empty */
    fill(&l);
    l.token[0] = l.user_id[0] = '\0';
    jf_login_save(&l, path);
    CHECK(!strstr(slurp(path), "token") && !strstr(slurp(path), "user_id"), "empty token/user_id omitted");
}

static void test_wipe(void)
{
    unsigned char b[40];
    memset(b, 0xA5, sizeof b);
    jf_wipe(b, 30);
    int z = 0, kept = 0;
    for (int i = 0; i < 30; i++) z += b[i] == 0;
    for (int i = 30; i < 40; i++) kept += b[i] == 0xA5;
    CHECK(z == 30 && kept == 10, "jf_wipe zeroes exactly n bytes (%d, %d)", z, kept);
    jf_wipe(b, 0);
    CHECK(b[35] == 0xA5, "jf_wipe n=0 touches nothing");

    JfLogin l;
    fill(&l);
    jf_login_wipe(&l);
    const unsigned char *p = (const unsigned char *)&l;
    int nz = 0;
    for (size_t i = 0; i < sizeof l; i++) nz += p[i] != 0;
    CHECK(nz == 0, "jf_login_wipe leaves %d non-zero bytes", nz);
}

#define LIB1 "00000000000000000000000000000a01"
/* The library choice and the volume live in the settings file, not the login file.  They survive a save/load,
 * a sign-out forgets the library and keeps server and user, and a load drops a library that is not 32 hex. */
static void test_library(void)
{
    JfLogin a, b;
    memset(&a, 0, sizeof a);
    snprintf(a.server, sizeof a.server, "10.0.0.1:8096"); snprintf(a.user, sizeof a.user, "bob");
    snprintf(a.token, sizeof a.token, "tok"); snprintf(a.device_id, sizeof a.device_id, "dev");
    snprintf(a.user_id, sizeof a.user_id, LIB1);
    snprintf(a.library, sizeof a.library, LIB1); snprintf(a.library_name, sizeof a.library_name, "Classical");
    a.volume_step = 5;
    CHECK(jf_login_save(&a, path) == 0 && jf_settings_save(&a, spath) == 0, "save");
    jf_login_load(&b, path, ""); jf_settings_load(&b, spath);
    CHECK(!strcmp(b.library, LIB1) && !strcmp(b.library_name, "Classical") && b.volume_step == 5,
          "library round trip: '%s' '%s' %d", b.library, b.library_name, b.volume_step);
    a.volume_step = 6;
    jf_settings_save(&a, spath); jf_settings_load(&b, spath);
    CHECK(!strcmp(b.library, LIB1) && b.volume_step == 6, "a settings save after a volume change keeps the library");
    CHECK(jf_login_signout(&a, path) == 0, "signout");
    CHECK(jf_settings_save(&a, spath) == 0, "signout settings save");
    CHECK(!a.library[0] && !a.library_name[0] && !a.token[0], "signout empties library and token in memory");
    jf_login_load(&b, path, ""); jf_settings_load(&b, spath);
    CHECK(!b.library[0] && !b.library_name[0] && !b.token[0] && !strcmp(b.server, "10.0.0.1:8096") && !strcmp(b.user, "bob"),
          "signout files: no library/token, server and user kept");
    FILE *f = fopen(spath, "w");
    fputs("library=notahexid\nlibrary_name=X\n", f); fclose(f);
    jf_settings_load(&b, spath);
    CHECK(!b.library[0] && !b.library_name[0], "an invalid library id is dropped on load");

    /* tap_action: default REPLACE, round trip, unknown / missing falls back to the default */
    memset(&b, 0, sizeof b);
    remove(spath);
    jf_settings_load(&b, spath);
    CHECK(b.tap_action == JF_TAP_REPLACE, "tap_action defaults to REPLACE with no file (%d)", b.tap_action);
    for (int t = 0; t < JF_TAP_COUNT; t++) {
        a.tap_action = t;
        jf_settings_save(&a, spath);
        b.tap_action = -7;
        jf_settings_load(&b, spath);
        CHECK(b.tap_action == t, "tap_action %d round trip (got %d)", t, b.tap_action);
    }
    f = fopen(spath, "w");
    fputs("tap_action=9\n", f); fclose(f);
    jf_settings_load(&b, spath);
    CHECK(b.tap_action == JF_TAP_REPLACE, "an unknown tap_action falls back (%d)", b.tap_action);
    f = fopen(spath, "w");
    fputs("tap_action=junk\nvolume_step=3\n", f); fclose(f);
    jf_settings_load(&b, spath);
    CHECK(b.tap_action == JF_TAP_REPLACE && b.volume_step == 3, "a non-number tap_action falls back (%d)", b.tap_action);
    f = fopen(spath, "w");
    fputs("volume_step=3\n", f); fclose(f);
    b.tap_action = JF_TAP_ADD_ONLY;
    jf_settings_load(&b, spath);
    CHECK(b.tap_action == JF_TAP_REPLACE, "a missing tap_action resets to the default (%d)", b.tap_action);

    /* tags_read / tags_cut: default off and no cut-off; round trip; a stale value is reset by a file without the keys */
    memset(&b, 0, sizeof b);
    remove(spath);
    jf_settings_load(&b, spath);
    CHECK(b.tags_on == 0 && b.tags_cut[0] == 0, "tag reads default OFF with no cut-off (%d '%s')", b.tags_on, b.tags_cut);
    memset(&a, 0, sizeof a);
    a.tags_on = 1;
    snprintf(a.tags_cut, sizeof a.tags_cut, "192.168.50.54:8096");
    jf_settings_save(&a, spath);
    b.tags_on = 0; b.tags_cut[0] = 0;
    jf_settings_load(&b, spath);
    CHECK(b.tags_on == 1 && strcmp(b.tags_cut, "192.168.50.54:8096") == 0, "tags_read and tags_cut round trip (%d '%s')", b.tags_on, b.tags_cut);
    a.tags_cut[0] = 0;
    jf_settings_save(&a, spath);
    jf_settings_load(&b, spath);
    CHECK(b.tags_on == 1 && b.tags_cut[0] == 0, "a cleared cut-off is not written back (%d '%s')", b.tags_on, b.tags_cut);
    f = fopen(spath, "w");
    fputs("tags_read=7\n", f); fclose(f);
    jf_settings_load(&b, spath);
    CHECK(b.tags_on == 0, "an out-of-range tags_read is OFF (%d)", b.tags_on);
    f = fopen(spath, "w");
    fputs("volume_step=3\n", f); fclose(f);
    b.tags_on = 1; snprintf(b.tags_cut, sizeof b.tags_cut, "x");
    jf_settings_load(&b, spath);
    CHECK(b.tags_on == 0 && b.tags_cut[0] == 0, "missing keys reset tags_on / tags_cut");
}

/* The split itself: the login file never names a settings key, the settings file never names a token, and an
 * old login file that still carries them is ignored for them. */
static void test_settings_split(void)
{
    JfLogin a, b;
    fill(&a);
    snprintf(a.library, sizeof a.library, LIB1); snprintf(a.library_name, sizeof a.library_name, "Classical");
    remove(spath);
    CHECK(jf_login_save(&a, path) == 0 && jf_settings_save(&a, spath) == 0, "save both files");
    char *lt = slurp(path);
    CHECK(!strstr(lt, "library") && !strstr(lt, "volume_step"), "login file names no settings key");
    CHECK(strstr(lt, "token=" TOK), "login file keeps the token");
    char *st = slurp(spath);
    CHECK(strstr(st, "library=" LIB1) && strstr(st, "library_name=Classical") && strstr(st, "volume_step=7"),
          "settings file holds library, name and volume");
    CHECK(!strstr(st, TOK) && !strstr(st, "token") && !strstr(st, "user_id"), "settings file holds no token or user id");
    CHECK(jf_settings_load(&b, spath) == 0 && !strcmp(b.library, LIB1) && b.volume_step == 7, "settings load");

    a.volume_step = 40;
    jf_settings_save(&a, spath);
    CHECK(strstr(slurp(spath), "volume_step=14"), "settings save clamps volume");

    /* an old login file that still carries the settings keys: ignored; no settings file: defaults, -1 */
    put(path, "server=h\nuser=u\nlibrary=" LIB1 "\nlibrary_name=Old\nvolume_step=3\ntoken=" TOK "\n");
    remove(spath);
    CHECK(jf_login_load(&b, path, "") == 0 && !b.library[0] && b.volume_step == 10 && !strcmp(b.token, TOK),
          "old login keys ignored by the login load");
    CHECK(jf_settings_load(&b, spath) == -1 && !b.library[0] && !b.library_name[0] && b.volume_step == 10,
          "missing settings file -> -1 with defaults");
}

static void test_id_suffix(void)
{
    JfLogin a, b, c;
    char none[300];
    snprintf(none, sizeof none, "%s/a/b/absent.conf", dir);
    CHECK(jf_login_load(&a, none, "") == -1, "absent file");
    CHECK(jf_login_load(&b, none, "-v") == -1, "absent file, suffix");
    CHECK(!strncmp(a.device_id, "arca-", 5), "default id: %s", a.device_id);
    CHECK(!strcmp(b.device_id, strcat(strcpy(c.device_id, a.device_id), "-v")), "suffix appended: %s vs %s", a.device_id, b.device_id);
    CHECK(strcmp(a.device_id, b.device_id) != 0, "the two apps get different ids");
    /* a saved id is kept as it is: the suffix only shapes the default */
    snprintf(a.device_id, sizeof a.device_id, "%s", "fixed-id");
    CHECK(jf_login_save(&a, path) == 0, "save");
    CHECK(jf_login_load(&c, path, "-v") == 0 && !strcmp(c.device_id, "fixed-id"), "saved id wins: %s", c.device_id);
    /* a long hostname leaves room for the suffix: the id still ends with it and fits */
    CHECK(strlen(b.device_id) < sizeof b.device_id, "fits");
}

int main(void)
{
    snprintf(dir, sizeof dir, "/tmp/jf_login_test_XXXXXX");
    if (!mkdtemp(dir)) { perror("mkdtemp"); return 2; }
    snprintf(path, sizeof path, "%s/a/b/ludfin.conf", dir);
    snprintf(spath, sizeof spath, "%s/a/b/settings.conf", dir);

    test_roundtrip();
    test_clear();
    test_load_edges();
    test_wipe();
    test_library();
    test_settings_split();
    test_id_suffix();

    char cmd[200];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
    if (system(cmd) != 0) printf("note: could not remove %s\n", dir);
    printf("jf_login_test: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
