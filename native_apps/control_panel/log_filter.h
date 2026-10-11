/* log_filter.h — the Log page's pure text handling: split the tail of a log
 * into lines, decide which lines are errors, keep a scroll position valid.
 *
 * Pure (no file, no framebuffer, no kernel call) so a host test can link it
 * alone.  The Log page reads the bytes; everything it then decides about them
 * is here. */
#ifndef LOG_FILTER_H
#define LOG_FILTER_H

typedef struct {
    int off;    /* byte offset of the line in the buffer */
    int len;    /* its length, without the newline or a trailing CR */
} LogLine;

/* Splits buf[0..len) into lines, oldest first, and writes at most `max` of
 * the NEWEST to out; returns how many.  Empty lines are skipped.  A last line
 * with no newline counts.  partial_head = 1 says the buffer began in the
 * middle of the log (a tail read), so the first line may be cut and is
 * dropped; a buffer that begins at a line start loses one line to this, which
 * is the price of not showing half a line. */
int log_split_tail(const char *buf, int len, int partial_head,
                   LogLine *out, int max);

/* The kernel priority of a "<N>" prefix (klogctl output carries one), 0..7,
 * or -1 when the line has none. */
int log_prio(const char *line, int len);

/* Bytes of "<N>" to skip when drawing a line: 3 for a kernel prefix, else 0. */
int log_prefix_len(const char *line, int len);

/* Nonzero when the line is error-ish: a kernel priority 0..3, or any of
 * "error", "fail", "warn", "oops", "bug:", "segfault", "timeout" anywhere in
 * it, in any letter case. */
int log_is_error(const char *line, int len);

/* The first visible row `top` clamped so a window of `rows` rows over `n`
 * lines shows no blank space below the last line: 0..max(0, n - rows). */
int log_clamp_top(int top, int n, int rows);

#endif
