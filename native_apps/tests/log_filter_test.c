/* Host regression for the Log page's pure text handling
 * (control_panel/log_filter.c).
 *
 *   cd native_apps && gcc -Wall -Wextra -I. -o build/log_filter_test \
 *       tests/log_filter_test.c control_panel/log_filter.c && ./build/log_filter_test
 *
 * What it asserts:
 *   A  every error word matches in any letter case, a kernel "<0>".."<3>"
 *      prefix matches on its own, "<4>".."<7>" and plain text do not;
 *   B  a tail read splits into lines oldest first with the newest last, drops
 *      the cut first line only when told the read began mid-log, skips empty
 *      lines, strips CR, counts an unterminated last line, and keeps only the
 *      newest `max` lines;
 *   C  the scroll position clamps to 0..n-rows.
 * "Failing before" = compile against a stub whose log_is_error returns 0. */
#include "control_panel/log_filter.h"

#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { fails++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static int err(const char *s) { return log_is_error(s, (int)strlen(s)); }

static void check_lines(const char *what, const char *buf, int partial, int max,
                        const char *const *want, int nwant) {
    LogLine ll[16];
    int n = log_split_tail(buf, (int)strlen(buf), partial, ll, max);
    CHECK(n == nwant, "%s: %d lines, want %d", what, n, nwant);
    for (int i = 0; i < n && i < nwant; i++)
        CHECK((int)strlen(want[i]) == ll[i].len && memcmp(buf + ll[i].off, want[i], ll[i].len) == 0,
              "%s: line %d wrong", what, i);
}

int main(void) {
    /* A */
    CHECK(err("kernel: ERROR reading"), "ERROR");
    CHECK(err("mmc0: Failed to init"), "Failed");
    CHECK(err("WARNING: CPU: 0"), "WARNING");
    CHECK(err("Oops: 17 [#1]"), "Oops");
    CHECK(err("BUG: scheduling while atomic"), "BUG:");
    CHECK(err("app[123]: segfault at 0"), "segfault");
    CHECK(err("i2c timeout"), "timeout");
    CHECK(err("<0>panic"), "<0>");
    CHECK(err("<3>quiet text"), "<3>");
    CHECK(!err("<4>quiet text"), "<4> is not an error");
    CHECK(!err("<6>usb 1-1: new device"), "<6>");
    CHECK(!err("<8>x"), "<8> is no prefix");
    CHECK(!err("Oct 10 app started ok"), "plain line");
    CHECK(!err("bug report filed"), "bug without colon");
    CHECK(!err(""), "empty");
    CHECK(log_prio("<5>x", 4) == 5 && log_prio("x", 1) == -1 && log_prio("<5", 2) == -1, "prio");
    CHECK(log_prefix_len("<5>x", 4) == 3 && log_prefix_len("x", 1) == 0, "prefix len");

    /* B */
    { const char *w[] = { "a", "bb", "ccc" };
      check_lines("plain", "a\nbb\nccc\n", 0, 16, w, 3);
      check_lines("no trailing newline", "a\nbb\nccc", 0, 16, w, 3); }
    { const char *w[] = { "bb", "ccc" };
      check_lines("partial head dropped", "cut\nbb\nccc\n", 1, 16, w, 2);
      check_lines("max keeps newest", "a\nbb\nccc\n", 0, 2, w, 2); }
    { const char *w[] = { "a", "b" };
      check_lines("empty lines and CR", "\na\r\n\n\nb\r\n\n", 0, 16, w, 2); }
    { LogLine ll[2];
      CHECK(log_split_tail("", 0, 0, ll, 2) == 0, "empty buffer");
      CHECK(log_split_tail("x\n", 2, 1, ll, 2) == 0, "only a cut line"); }

    /* C */
    CHECK(log_clamp_top(5, 100, 20) == 5, "clamp inside");
    CHECK(log_clamp_top(95, 100, 20) == 80, "clamp past end");
    CHECK(log_clamp_top(-3, 100, 20) == 0, "clamp negative");
    CHECK(log_clamp_top(7, 10, 20) == 0, "fewer lines than rows");

    printf("%s (%d failures)\n", fails ? "FAIL" : "PASS", fails);
    return fails != 0;
}
