/**
 * Logger implementation for Arca native apps
 *
 * See logger.h for usage documentation.
 */

#include "logger.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <errno.h>

/* The Logger LIB_LOG messages from the shared library go to: the first one
 * logger_init() opened in this process, until logger_close() closes it. */
static Logger *lib_sink;

/* ── Helpers ────────────────────────────────────────────────────────── */

static const char *level_tag(LogLevel level) {
    switch (level) {
        case LOG_LEVEL_DEBUG: return "DEBUG";
        case LOG_LEVEL_INFO:  return "INFO ";
        case LOG_LEVEL_WARN:  return "WARN ";
        case LOG_LEVEL_ERROR: return "ERROR";
        default:              return "?????";
    }
}

/**
 * Ensure the log directory exists.
 * Returns 0 on success, -1 on failure.
 */
static int ensure_log_dir(void) {
    struct stat st;

    if (stat(LOG_DIR, &st) == 0 && S_ISDIR(st.st_mode))
        return 0;

    /* Try to create it (one level deep is enough) */
    if (mkdir(LOG_DIR, 0755) == 0)
        return 0;

    /* mkdir failed — maybe a parent is missing, or read-only FS */
    return -1;
}

/**
 * Rotate the log file if it exceeds LOG_MAX_SIZE.
 * Keeps one backup: <name>.log → <name>.log.1
 */
static void maybe_rotate(Logger *log) {
    if (!log->fp)
        return;

    if (log->bytes_written < LOG_MAX_SIZE)
        return;

    /* Get actual file size (other writers / buffering) */
    long pos = ftell(log->fp);
    if (pos < LOG_MAX_SIZE && pos >= 0) {
        log->bytes_written = pos;
        return;
    }

    fclose(log->fp);

    /* Rename current → .1 (overwrites previous backup) */
    char backup[LOG_PATH_MAX + 4];
    snprintf(backup, sizeof(backup), "%s.1", log->path);
    rename(log->path, backup);

    /* Open a fresh file */
    log->fp = fopen(log->path, "a");
    log->bytes_written = 0;

    if (log->fp) {
        /* Disable full buffering — use line buffering for crash safety */
        setvbuf(log->fp, NULL, _IOLBF, 0);
    }
}

/* ── Public API ─────────────────────────────────────────────────────── */

int logger_init(Logger *log, const char *app_name,
                LogLevel min_level, bool echo_stderr) {
    memset(log, 0, sizeof(*log));

    /* Copy app name */
    strncpy(log->app_name, app_name, LOG_APP_NAME_MAX - 1);
    log->app_name[LOG_APP_NAME_MAX - 1] = '\0';

    log->min_level   = min_level;
    log->echo_stderr = echo_stderr;
    log->fp          = NULL;

    /* Before the early returns: a stderr-only Logger still takes messages. */
    if (!lib_sink)
        lib_sink = log;

    /* Build path: /var/log/arca/<app_name>.log */
    snprintf(log->path, sizeof(log->path), "%s/%s.log", LOG_DIR, app_name);

    /* Ensure directory exists */
    if (ensure_log_dir() < 0) {
        /* Can't create dir — stderr only */
        if (echo_stderr)
            fprintf(stderr, "[logger] Cannot create %s: %s (stderr-only mode)\n",
                    LOG_DIR, strerror(errno));
        return -1;
    }

    /* Open log file (append mode) */
    log->fp = fopen(log->path, "a");
    if (!log->fp) {
        if (echo_stderr)
            fprintf(stderr, "[logger] Cannot open %s: %s (stderr-only mode)\n",
                    log->path, strerror(errno));
        return -1;
    }

    /* Line-buffered for crash safety */
    setvbuf(log->fp, NULL, _IOLBF, 0);

    /* Seed byte counter from current file size */
    fseek(log->fp, 0, SEEK_END);
    log->bytes_written = ftell(log->fp);

    /* Opening banner */
    LOG_INFO(log, "=== %s started (pid %d) ===", app_name, (int)getpid());

    return 0;
}

void logger_write(Logger *log, LogLevel level,
                  const char *file, int line,
                  const char *fmt, ...) {
    if (!log)
        return;

    if (level < log->min_level)
        return;

    /* Timestamp */
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    localtime_r(&ts.tv_sec, &tm);

    char timebuf[32];
    int tlen = strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", &tm);
    /* Append milliseconds */
    snprintf(timebuf + tlen, sizeof(timebuf) - tlen, ".%03ld",
             ts.tv_nsec / 1000000);

    /* Build message */
    char msgbuf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msgbuf, sizeof(msgbuf), fmt, ap);
    va_end(ap);

    /* Extract just the filename from the path */
    const char *basename = file;
    const char *p;
    for (p = file; *p; p++) {
        if (*p == '/' || *p == '\\')
            basename = p + 1;
    }

    /* Format: 2026-03-03 08:55:14.123 [INFO ] vnc_client  vnc_client.c:324  Connection lost */
    const char *tag = level_tag(level);

    /* Write to file */
    if (log->fp) {
        int n = fprintf(log->fp, "%s [%s] %-14s %s:%d  %s\n",
                        timebuf, tag, log->app_name, basename, line, msgbuf);
        if (n > 0)
            log->bytes_written += n;

        /* Check rotation */
        maybe_rotate(log);
    }

    /* Echo to stderr */
    if (log->echo_stderr) {
        fprintf(stderr, "%s [%s] %s:%d  %s\n",
                timebuf, tag, basename, line, msgbuf);
    }
}

void logger_close(Logger *log) {
    if (!log)
        return;

    if (lib_sink == log)
        lib_sink = NULL;

    if (log->fp) {
        LOG_INFO(log, "=== %s shutting down (pid %d) ===",
                 log->app_name, (int)getpid());
        fclose(log->fp);
        log->fp = NULL;
    }
}

void logger_lib_write(LogLevel level, const char *file, int line,
                      const char *fmt, ...) {
    char msgbuf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msgbuf, sizeof(msgbuf), fmt, ap);
    va_end(ap);

    if (lib_sink) {
        logger_write(lib_sink, level, file, line, "%s", msgbuf);
        return;
    }
    /* No Logger open: the stdout fallback LIB_LOG uses without logger.o. */
    if (level >= LOG_LEVEL_INFO)
        printf("%s\n", msgbuf);
}
