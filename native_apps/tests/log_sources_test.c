/* Host regression for the Log page's extension point (control_panel/log_sources.c).
 *
 *   cd native_apps && gcc -Wall -Wextra -I. -o build/log_sources_test \
 *       tests/log_sources_test.c control_panel/log_sources.c && ./build/log_sources_test
 *
 * What it asserts:
 *   A  parse: name and path read, the name upper-cased and cut to LOGSRC_NAME_MAX,
 *      blanks and CR trimmed, unknown keys and '#' lines ignored, last key wins;
 *   B  skipped: no name, no path, empty name, a relative path, an over-long path;
 *   C  scan: file-name order whatever the creation order, non-.item files and
 *      invalid items do not count, the cap keeps the first LOGSRC_MAX valid ones,
 *      a missing directory yields 0.
 * New file, so no pre-change source: "failing" = a copy of log_sources.c with the
 * path check removed (see the sabotage run recorded with the change). */
#include "control_panel/log_sources.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { fails++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void put(const char *dir, const char *file, const char *text) {
    char p[300];
    snprintf(p, sizeof p, "%s/%s", dir, file);
    FILE *f = fopen(p, "w");
    if (!f) { printf("cannot write %s\n", p); exit(2); }
    fputs(text, f);
    fclose(f);
}

int main(void) {
    LogSource s;

    /* A */
    CHECK(logsrc_parse("name=Jelly\npath=/var/log/a.log\n", &s), "plain item");
    CHECK(strcmp(s.name, "JELLY") == 0 && strcmp(s.path, "/var/log/a.log") == 0, "fields: %s %s", s.name, s.path);
    CHECK(logsrc_parse("  name =  abc  \r\n path= /x/y \r\n", &s) && strcmp(s.name, "ABC") == 0
          && strcmp(s.path, "/x/y") == 0, "blanks and CR trimmed: '%s' '%s'", s.name, s.path);
    CHECK(logsrc_parse("# name=NO\nfoo=bar\nname=OK\npath=/p\nreboot=no", &s) && strcmp(s.name, "OK") == 0,
          "unknown keys and comments ignored, unterminated last line read");
    CHECK(logsrc_parse("name=averyveryverylongname\npath=/p\n", &s) && strlen(s.name) == LOGSRC_NAME_MAX,
          "name cut to %d, got %zu", LOGSRC_NAME_MAX, strlen(s.name));
    CHECK(logsrc_parse("name=A\npath=/p\nname=B\n", &s) && strcmp(s.name, "B") == 0, "last name wins");

    /* B */
    CHECK(!logsrc_parse("path=/p\n", &s), "no name is skipped");
    CHECK(!logsrc_parse("name=A\n", &s), "no path is skipped");
    CHECK(!logsrc_parse("name=\npath=/p\n", &s), "empty name is skipped");
    CHECK(!logsrc_parse("name=A\npath=rel/p\n", &s), "relative path is skipped");
    {
        char big[400] = "name=A\npath=/";
        memset(big + strlen(big), 'x', 200);
        strcat(big, "\n");
        CHECK(!logsrc_parse(big, &s), "over-long path is skipped");
    }
    CHECK(!logsrc_parse("", &s), "empty text is skipped");

    /* C */
    char dir[] = "/tmp/lsrcXXXXXX";
    if (!mkdtemp(dir)) { printf("mkdtemp failed\n"); return 2; }
    put(dir, "30-c.item", "name=C\npath=/c\n");
    put(dir, "10-a.item", "name=A\npath=/a\n");
    put(dir, "20-bad.item", "name=NOPATH\n");
    put(dir, "15-note.txt", "name=TXT\npath=/t\n");
    put(dir, "25-b.item", "name=B\npath=/b\n");
    put(dir, "40-d.item", "name=D\npath=/d\n");
    put(dir, "50-e.item", "name=E\npath=/e\n");
    LogSource out[8];
    int n = logsrc_scan(dir, out, 8);
    CHECK(n == 5, "scan kept %d, want 5", n);
    static const char *const want[] = { "A", "B", "C", "D", "E" };
    for (int i = 0; i < n && i < 5; i++) CHECK(strcmp(out[i].name, want[i]) == 0, "order %d: %s want %s", i, out[i].name, want[i]);
    n = logsrc_scan(dir, out, LOGSRC_MAX);
    CHECK(n == LOGSRC_MAX && strcmp(out[LOGSRC_MAX - 1].name, "C") == 0, "cap: %d kept, last %s", n, out[n > 0 ? n - 1 : 0].name);
    CHECK(logsrc_scan("/nonexistent-lsrc-dir", out, 8) == 0, "missing directory");

    char cmd[320];
    snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
    if (system(cmd) != 0) printf("note: could not remove %s\n", dir);

    printf(fails ? "log_sources_test: %d FAILED\n" : "log_sources_test: all passed\n", fails);
    return fails ? 1 : 0;
}
