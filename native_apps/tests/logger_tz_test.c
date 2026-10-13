/*
 * logger_tz_test — a log line's timestamp follows a zone change without a
 * restart.
 *
 * A long-running app logs for its whole life, while the Control Panel's System
 * page rewrites /etc/localtime from another process; glibc's localtime_r()
 * reads the zone only on its first call, so logger_write() must tzset() first.
 * Stand-in for the device's /etc/localtime swap: the TZ variable, which tzset()
 * re-reads on a changed value (on the device TZ is unset and tzset() stats the
 * file).
 *
 * Build/run from native_apps/:
 *   gcc -Wall -I common tests/logger_tz_test.c common/logger.c -o /tmp/t && /tmp/t
 */
#include "logger.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

/* The hour of the last line written to fp, or -1. */
static int last_hour(FILE *fp) {
    char line[512], last[512] = "";
    rewind(fp);
    while (fgets(line, sizeof(line), fp)) memcpy(last, line, sizeof(last));
    int y, mo, d, h, mi;
    return sscanf(last, "%d-%d-%d %d:%d", &y, &mo, &d, &h, &mi) == 5 ? h : -1;
}

int main(void) {
    /* A file-backed Logger without logger_init(), which would open LOG_DIR. */
    Logger log;
    memset(&log, 0, sizeof(log));
    log.fp = tmpfile();
    log.min_level = LOG_LEVEL_INFO;
    snprintf(log.app_name, sizeof(log.app_name), "tz_test");
    CHECK(log.fp != NULL, "tmpfile");
    if (!log.fp) return 1;

    setenv("TZ", "UTC0", 1);
    LOG_INFO(&log, "before");
    int h0 = last_hour(log.fp);
    setenv("TZ", "XXX-5", 1);                 /* five hours east of UTC0 */
    fseek(log.fp, 0, SEEK_END);
    LOG_INFO(&log, "after");
    int h1 = last_hour(log.fp);
    CHECK(h0 >= 0 && h1 >= 0, "parse hours (%d, %d)", h0, h1);
    CHECK(h1 == (h0 + 5) % 24, "zone change UTC0 -> UTC+5 must move the stamp 5 h (%d -> %d)", h0, h1);
    unsetenv("TZ");
    fclose(log.fp);

    if (fails) { printf("logger_tz_test: %d FAILED\n", fails); return 1; }
    printf("logger_tz_test: all passed\n");
    return 0;
}
