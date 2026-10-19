/* Host regression for jellyfin/jf_login.c: the saved login file.
 *
 *   cd native_apps && gcc -Wall -Wextra -I common -I jellyfin -o build/jf_login_test \
 *       tests/jf_login_test.c jellyfin/jf_login.c jellyfin/jf_url.c common/config.c \
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
    CHECK(jf_login_load(&b, path) == 0, "load");
    CHECK(!strcmp(a.server, b.server) && !strcmp(a.user, b.user) && !strcmp(a.user_id, b.user_id)
          && !strcmp(a.token, b.token) && !strcmp(a.device_id, b.device_id) && a.volume_step == b.volume_step,
          "round trip equal: '%s' '%s' '%s' '%s' '%s' %d", b.server, b.user, b.user_id, b.token, b.device_id, b.volume_step);
    char *t = slurp(path);
    CHECK(!strstr(t, "password") && !strstr(t, "Password") && !strstr(t, "pw"), "no password key in the file");
    CHECK(strstr(t, "token=" TOK) && strstr(t, "user_id=" UID_), "token and id are in the file");

    /* an old file that somehow carries a password: a save drops it */
    put(path, "server=x\npassword=hunter2\ntoken=" TOK "\n");
    jf_login_load(&b, path);
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
    CHECK(jf_login_load(&b, path) == 0, "load after clear");
    CHECK(!strcmp(b.server, "192.0.2.10:8096") && !strcmp(b.user, "user1") && !strcmp(b.device_id, "arca-test")
          && b.volume_step == 7 && !b.token[0] && !b.user_id[0], "server/user/device_id/volume kept");
}

static void test_load_edges(void)
{
    JfLogin l;
    char missing[200];
    snprintf(missing, sizeof missing, "%s/none/jellyfin.conf", dir);
    memset(&l, 0xAA, sizeof l);
    CHECK(jf_login_load(&l, missing) == -1, "missing file -> -1");
    CHECK(!l.server[0] && !l.user[0] && !l.token[0] && !l.user_id[0] && l.volume_step == 10, "defaults filled");
    CHECK(!strncmp(l.device_id, "arca-", 5) && strlen(l.device_id) > 5, "device_id from host: '%s'", l.device_id);
    CHECK(access(missing, F_OK) != 0, "load did not create the file");

    put(path, "server=h\nvolume_step=99\nuser_id=nothex\ntoken=" TOK "\n");
    CHECK(jf_login_load(&l, path) == 0, "load odd file");
    CHECK(l.volume_step == 14, "volume 99 clamps to %d", l.volume_step);
    CHECK(l.user_id[0] == '\0', "invalid user_id dropped");
    CHECK(!strcmp(l.token, TOK) && !strncmp(l.device_id, "arca-", 5), "token kept, device_id defaulted");
    put(path, "volume_step=-4\nuser_id=" UID_ "0\n");
    jf_login_load(&l, path);
    CHECK(l.volume_step == 0 && l.user_id[0] == '\0', "negative volume -> 0, 33-char id dropped");
    put(path, "volume_step=junk\nuser_id=" UID_ "\ndevice_id=mine\n");
    jf_login_load(&l, path);
    CHECK(l.volume_step == 10 && !strcmp(l.user_id, UID_) && !strcmp(l.device_id, "mine"), "junk volume -> default, good id and stored device_id kept");

    /* save clamps too, so the file never holds an out-of-range step */
    fill(&l);
    l.volume_step = 40;
    jf_login_save(&l, path);
    CHECK(strstr(slurp(path), "volume_step=14"), "save clamps volume");

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

int main(void)
{
    snprintf(dir, sizeof dir, "/tmp/jf_login_test_XXXXXX");
    if (!mkdtemp(dir)) { perror("mkdtemp"); return 2; }
    snprintf(path, sizeof path, "%s/a/b/jellyfin.conf", dir);

    test_roundtrip();
    test_clear();
    test_load_edges();
    test_wipe();

    char cmd[200];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
    if (system(cmd) != 0) printf("note: could not remove %s\n", dir);
    printf("jf_login_test: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
