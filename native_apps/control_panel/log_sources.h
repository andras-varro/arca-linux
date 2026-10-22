/* log_sources.h — the Log page's extension point: /opt/arca/logs.d/<name>.item.
 *
 * Any component adds a log to the Control Panel's Log page by installing one
 * item file; the Control Panel is not edited.  Format, key=value lines:
 *
 *     name=JELLYFIN                 tab label, upper-cased, cut to LOGSRC_NAME_MAX
 *     path=/var/log/arca/jellyfin.log   absolute path of a plain text log file
 *
 * Unknown keys and '#' lines are ignored; an item without a name or without an
 * absolute path is skipped.  Pure text handling plus one directory scan, so a
 * host test drives it (tests/log_sources_test.c). */
#ifndef LOG_SOURCES_H
#define LOG_SOURCES_H

#define LOGSRC_DIR       "/opt/arca/logs.d"
#define LOGSRC_MAX       3      /* extra tabs the page lays out in its second row */
#define LOGSRC_NAME_MAX  12     /* 13 glyphs of 12 px fit a third of the narrowest content width */
#define LOGSRC_PATH_MAX  128

typedef struct {
    char name[LOGSRC_NAME_MAX + 1];
    char path[LOGSRC_PATH_MAX];
} LogSource;

/* Parses one item's text.  Returns 1 and fills *out, or 0 (item skipped). */
int logsrc_parse(const char *text, LogSource *out);

/* Reads every .item file of <dir> in file-name order and keeps the first `max` valid
 * ones.  A missing directory yields 0.  Returns the count kept. */
int logsrc_scan(const char *dir, LogSource *out, int max);

#endif
