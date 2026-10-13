/* Host-side regression for the one /etc/input_config.conf parser and the
 * hot-plug fingerprint gate, both in common/input_scan.c.
 *
 * The parser used to exist three times (gamepad.c, vnc_client, ScummVM) and the
 * copies disagreed: one skipped an empty value, another mapped it to button 0.
 * Every row here pins one rule of the one copy left:
 *   - every key the file may carry lands in its own field, and nowhere else;
 *   - each range is rejected on BOTH sides, at the exact boundary;
 *   - a blank line, a comment, a line with no '=', an empty key, an empty
 *     value and an unknown key change nothing — the unknown key is README's
 *     `gamepad_btn_a`, which no parser has ever read;
 *   - a CRLF line parses as its LF twin;
 *   - input_config_load() applies a file on top of what it is given and
 *     reports -1 for a missing file.
 * The gate rows: a first check only records a baseline, an unchanged
 * fingerprint never asks for a rescan, a changed one asks exactly once, and a
 * check is not due again until INPUT_SIG_CHECK_MS has passed (wrap included).
 *
 * CONTROLS.  Every rejection row is paired with an acceptance row one step
 * inside the same boundary, so a parser that rejected everything fails half of
 * them.  The field-isolation row compares the whole struct, so a key written to
 * the wrong field cannot pass by also being right.
 *
 * Build (this line is the CTEST_ROWS row in tests/run-all.sh):
 *   cd native_apps && gcc -Wall -Wextra -Wno-unused-parameter -I common \
 *       -o build/input_config_test tests/input_config_test.c common/input_scan.c -lm
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "input_scan.h"

static int g_pass, g_fail;

static void check(int cond, const char *what) {
    if (cond) { g_pass++; printf("  PASS  %s\n", what); }
    else      { g_fail++; printf("  FAIL  %s\n", what); }
}

static bool same(const InputConfig *a, const InputConfig *b) {
    return memcmp(a, b, sizeof(*a)) == 0;
}

/* Apply `line` to fresh defaults; true if it was applied AND only `want`
 * differs from the defaults afterwards (want == NULL: nothing may differ). */
static bool applies_only(const char *line, void (*want)(InputConfig *)) {
    InputConfig got, exp;
    input_config_defaults(&got);
    input_config_defaults(&exp);
    bool applied = input_config_parse_line(&got, line);
    if (want) want(&exp);
    return applied == (want != NULL) && same(&got, &exp);
}

/* One setter per key, for applies_only(). */
#define SETTER(name, stmt) static void name(InputConfig *c) { stmt; }
SETTER(w_sens,  c->mouse_sensitivity = 3.5f)
SETTER(w_accel, c->mouse_acceleration = 4.0f)
SETTER(w_low,   c->mouse_low_threshold = 7)
SETTER(w_high,  c->mouse_high_threshold = 120)
SETTER(w_dz,    c->gamepad_deadzone = 40)
SETTER(w_jump,  c->gamepad_btn_jump = 300)
SETTER(w_run,   c->gamepad_btn_run = 301)
SETTER(w_act,   c->gamepad_btn_action = 302)
SETTER(w_pause, c->gamepad_btn_pause = 303)
SETTER(w_back,  c->gamepad_btn_back = 309)
SETTER(w_north, c->gamepad_btn_north = 312)
SETTER(w_tl,    c->gamepad_btn_tl = 313)
SETTER(w_tr,    c->gamepad_btn_tr = 316)
SETTER(w_hx,    c->gamepad_hat_x = 6)
SETTER(w_hy,    c->gamepad_hat_y = 7)
SETTER(w_lx,    c->gamepad_stick_lx = 8)
SETTER(w_ly,    c->gamepad_stick_ly = 9)
SETTER(w_rx,    c->gamepad_stick_rx = 10)
SETTER(w_ry,    c->gamepad_stick_ry = 11)
/* Boundary acceptances, one step inside each range. */
SETTER(w_sens_lo,  c->mouse_sensitivity = 0.11f)
SETTER(w_sens_hi,  c->mouse_sensitivity = 19.9f)
SETTER(w_accel_lo, c->mouse_acceleration = 0.11f)
SETTER(w_accel_hi, c->mouse_acceleration = 19.9f)
SETTER(w_low_lo,   c->mouse_low_threshold = 0)
SETTER(w_low_hi,   c->mouse_low_threshold = 99)
SETTER(w_high_lo,  c->mouse_high_threshold = 1)
SETTER(w_high_hi,  c->mouse_high_threshold = 499)
SETTER(w_dz_lo,    c->gamepad_deadzone = 0)
SETTER(w_dz_hi,    c->gamepad_deadzone = 100)

static void test_defaults(void) {
    InputConfig c;
    memset(&c, 0x5a, sizeof(c));
    input_config_defaults(&c);
    check(c.mouse_sensitivity == 1.5f && c.mouse_acceleration == 2.0f &&
          c.mouse_low_threshold == 3 && c.mouse_high_threshold == 15,
          "defaults: mouse 1.5 / 2.0 / 3 / 15");
    check(c.gamepad_deadzone == 25, "defaults: deadzone 25 %");
    check(c.gamepad_btn_jump == BTN_SOUTH && c.gamepad_btn_run == BTN_EAST &&
          c.gamepad_btn_action == BTN_WEST && c.gamepad_btn_pause == BTN_START &&
          c.gamepad_btn_back == BTN_SELECT && c.gamepad_btn_north == BTN_NORTH &&
          c.gamepad_btn_tl == BTN_TL && c.gamepad_btn_tr == BTN_TR,
          "defaults: native button codes");
    check(c.gamepad_hat_x == ABS_HAT0X && c.gamepad_hat_y == ABS_HAT0Y &&
          c.gamepad_stick_lx == ABS_X && c.gamepad_stick_ly == ABS_Y &&
          c.gamepad_stick_rx == ABS_RX && c.gamepad_stick_ry == ABS_RY,
          "defaults: axis codes");
}

static void test_every_key(void) {
    static const struct { const char *line; void (*want)(InputConfig *); } rows[] = {
        { "mouse_sensitivity=3.5", w_sens },   { "mouse_acceleration=4", w_accel },
        { "mouse_low_threshold=7", w_low },    { "mouse_high_threshold=120", w_high },
        { "gamepad_deadzone=40", w_dz },
        { "gamepad_btn_jump=300", w_jump },    { "gamepad_btn_run=301", w_run },
        { "gamepad_btn_action=302", w_act },   { "gamepad_btn_pause=303", w_pause },
        { "gamepad_btn_back=309", w_back },    { "gamepad_btn_north=312", w_north },
        { "gamepad_btn_tl=313", w_tl },        { "gamepad_btn_tr=316", w_tr },
        { "gamepad_hat_x=6", w_hx },           { "gamepad_hat_y=7", w_hy },
        { "gamepad_stick_lx=8", w_lx },        { "gamepad_stick_ly=9", w_ly },
        { "gamepad_stick_rx=10", w_rx },       { "gamepad_stick_ry=11", w_ry },
    };
    char what[96];
    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        snprintf(what, sizeof(what), "key: '%s' sets its field only", rows[i].line);
        check(applies_only(rows[i].line, rows[i].want), what);
    }
}

static void test_ranges(void) {
    static const struct { const char *line; void (*want)(InputConfig *); } rows[] = {
        { "mouse_sensitivity=0.1",    NULL }, { "mouse_sensitivity=0.11",   w_sens_lo },
        { "mouse_sensitivity=20",     NULL }, { "mouse_sensitivity=19.9",   w_sens_hi },
        { "mouse_acceleration=0.1",   NULL }, { "mouse_acceleration=0.11",  w_accel_lo },
        { "mouse_acceleration=20",    NULL }, { "mouse_acceleration=19.9",  w_accel_hi },
        { "mouse_low_threshold=-1",   NULL }, { "mouse_low_threshold=0",    w_low_lo },
        { "mouse_low_threshold=100",  NULL }, { "mouse_low_threshold=99",   w_low_hi },
        { "mouse_high_threshold=0",   NULL }, { "mouse_high_threshold=1",   w_high_lo },
        { "mouse_high_threshold=500", NULL }, { "mouse_high_threshold=499", w_high_hi },
        { "gamepad_deadzone=-1",      NULL }, { "gamepad_deadzone=0",       w_dz_lo },
        { "gamepad_deadzone=101",     NULL }, { "gamepad_deadzone=100",     w_dz_hi },
    };
    char what[96];
    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        snprintf(what, sizeof(what), "range: '%s' %s", rows[i].line,
                 rows[i].want ? "accepted" : "rejected");
        check(applies_only(rows[i].line, rows[i].want), what);
    }
}

static void test_line_shapes(void) {
    check(applies_only("", NULL), "shape: empty line ignored");
    check(applies_only("   \t\r\n", NULL), "shape: whitespace-only line ignored");
    check(applies_only("# mouse_sensitivity=3.5", NULL), "shape: comment ignored");
    check(applies_only("   #gamepad_btn_jump=300", NULL), "shape: indented comment ignored");
    check(applies_only("mouse_sensitivity 3.5", NULL), "shape: no '=' ignored");
    check(applies_only("gamepad_btn_jump=", NULL), "shape: empty value ignored (not button 0)");
    check(applies_only("gamepad_btn_jump=   \r\n", NULL), "shape: blank value ignored");
    check(applies_only("=300", NULL), "shape: empty key ignored");
    check(applies_only("gamepad_btn_a=300", NULL), "shape: unknown key gamepad_btn_a ignored");
    check(applies_only("mouse_sensitivity_x=3.5", NULL), "shape: key with a known prefix ignored");
    check(applies_only("mouse_sensitivity=3.5\r\n", w_sens), "shape: CRLF line parses");
    check(applies_only("gamepad_btn_jump=300\r", w_jump), "shape: bare CR parses");
    check(applies_only("  gamepad_deadzone \t=\t 40  \n", w_dz), "shape: spaces around key and value");
    check(!input_config_parse_line(NULL, "gamepad_deadzone=40"), "shape: NULL cfg refused");
    InputConfig c;
    input_config_defaults(&c);
    check(!input_config_parse_line(&c, NULL), "shape: NULL line refused");
}

static void test_load(void) {
    char path[] = "/tmp/input_config_test_XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) { check(0, "load: temp file created"); return; }
    static const char body[] =
        "# Arca Input Configuration\r\n"
        "mouse_sensitivity=3.5\r\n"
        "gamepad_btn_a=1\n"
        "gamepad_deadzone=\n"
        "gamepad_deadzone=40\n"
        "gamepad_btn_jump=300\n";
    if (write(fd, body, sizeof(body) - 1) != (ssize_t)(sizeof(body) - 1)) {
        close(fd); unlink(path); check(0, "load: temp file written"); return;
    }
    close(fd);

    InputConfig got, exp;
    input_config_defaults(&got);
    got.mouse_high_threshold = 77;            /* a caller's value the file omits */
    int n = input_config_load(&got, path);
    input_config_defaults(&exp);
    exp.mouse_high_threshold = 77;
    w_sens(&exp); w_dz(&exp); w_jump(&exp);
    check(n == 3, "load: returns the number of lines applied (3)");
    check(same(&got, &exp), "load: applies on top of the caller's values");
    unlink(path);

    input_config_defaults(&got);
    input_config_defaults(&exp);
    check(input_config_load(&got, "/nonexistent/input_config.conf") == -1,
          "load: missing file returns -1");
    check(same(&got, &exp), "load: missing file changes nothing");
}

static void test_sig_gate(void) {
    InputSigGate g;
    memset(&g, 0, sizeof(g));
    check(input_sig_gate_due(&g, 0), "gate: a fresh gate is due at once");
    check(!input_sig_gate_feed(&g, 5000, 111), "gate: the first fingerprint is a baseline, no rescan");
    check(!input_sig_gate_due(&g, 5999), "gate: not due 999 ms later");
    check(input_sig_gate_due(&g, 6000), "gate: due 1000 ms later");
    check(!input_sig_gate_feed(&g, 6000, 111), "gate: unchanged fingerprint, no rescan");
    check(input_sig_gate_feed(&g, 7000, 222), "gate: changed fingerprint, rescan");
    check(!input_sig_gate_feed(&g, 8000, 222), "gate: the change is reported once");

    /* A full scan in between makes its own fingerprint the baseline. */
    input_sig_gate_baseline(&g, 333);
    check(!input_sig_gate_feed(&g, 9000, 333), "gate: baseline from a full scan, no rescan");
    check(!input_sig_gate_due(&g, 9500), "gate: baseline does not consume a check");

    /* A baseline set before any check (gamepad_init) still detects a change
     * seen at the first check. */
    memset(&g, 0, sizeof(g));
    input_sig_gate_baseline(&g, 10);
    check(input_sig_gate_due(&g, 123), "gate: baseline-only gate is due at once");
    check(input_sig_gate_feed(&g, 123, 11), "gate: change since the baseline is reported");

    /* uint32_t millisecond wrap. */
    memset(&g, 0, sizeof(g));
    input_sig_gate_feed(&g, 0xFFFFFF00u, 1);
    check(!input_sig_gate_due(&g, 0x00000100u), "gate: 512 ms across the wrap is not due");
    check(input_sig_gate_due(&g, 0x00000300u), "gate: 1024 ms across the wrap is due");
}

int main(void) {
    test_defaults();
    test_every_key();
    test_ranges();
    test_line_shapes();
    test_load();
    test_sig_gate();
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
