/* Host-side regression for app_launcher's `args=` manifest field.
 *
 * Runs on the DEV MACHINE with native gcc, not on the device.  parse_args() is
 * pure logic — two libc string calls over a caller's `const char *`, no globals,
 * no fd, no exit() — so nothing about it needs a panel, a /dev/fb0 or a manifest
 * on disk.  What it DOES need is to be reachable: it is `static` inside a
 * main()-bearing translation unit, so this test `#include`s app_launcher.c
 * itself and the build line neutralises its entry point with
 * -Dmain=app_launcher_main_unused, then `#undef main` takes the name back for
 * the test's own.  That redirect is a test-only flag and changes no shipped
 * byte — the alternative, copying the eight lines in here, would test the copy
 * and stay green forever after the shipped function changed underneath it.  The
 * price of the include is the link line below: the whole launcher TU is
 * compiled, so every object app_launcher normally links has to be present even
 * though only parse_args() is ever called.
 *
 *   cd native_apps && gcc -Wall -Wextra -Wno-unused-parameter -I. -Dmain=app_launcher_main_unused \
 *       -o build/launcher_args_test tests/launcher_args_test.c \
 *       common/framebuffer.c common/touch_input.c common/hardware.c common/common.c \
 *       common/highscore.c common/keyboard.c common/audio.c common/audio_gen.c \
 *       common/audio_out.c common/audio_wav.c common/config.c common/gamepad.c common/input_slots.c \
 *       common/input_scan.c common/ppm.c common/icon_grid.c common/logger.c common/pointer.c common/status_line.c common/top_bar.c control_panel/cpu_load.c -lm && ./build/launcher_args_test
 *
 * What it asserts, and why: the four `args=` spellings app_launcher.c's own
 * manifest-format comment documents (fb,touch / fb / touch / none) each reach
 * the ArgMode the launcher then turns into the child's argv, and — the half
 * that is easy to "fix" — the matching is SUBSTRING, via strstr(), not
 * tokenised on commas.  So `offbeat` selects ARG_FB and `untouched` selects
 * ARG_TOUCH, while `framebuffer` selects neither because it holds no `fb`
 * bigram at all; a rewrite to token matching keeps group A green and breaks
 * group B alone, which is the only reason group B exists.  `none` is the one
 * spelling compared whole-string, so it loses to a substring the moment
 * anything sits beside it (`none,fb` is ARG_FB).  The unmatched fallback is
 * ARG_FB_TOUCH, which is also what a correct `fb,touch` returns — so a
 * misspelled manifest is indistinguishable from the default, deliberately, and
 * group D pins that.  Case sensitivity (group E) and a NULL argument (group F,
 * safe: the `!s` test short-circuits ahead of every dereference) are pinned as
 * the code actually behaves rather than as one might wish.  Every expectation
 * that reads like a bug carries a comment saying why it is that value; they are
 * pinned quirks, not defects to tidy.
 *
 * ⚠️ Every expected value below is a LITERAL, never the ArgMode enumerator it
 * describes.  Measured: written as `ARG_FB` the whole file passes with the enum
 * swapped to ARG_FB = 2 / ARG_TOUCH = 1, because subject and test move together
 * and parse_args() composes those values with `|=` — a defect the launcher's
 * argv would carry straight to the child.  Group A pins the four numbers
 * themselves for the same reason.
 *
 * NOT part of build-and-deploy.sh: that script cross-compiles for ARM and this
 * is a host binary.  Negative control: tests/measure_launcher_args_sabotage.sh,
 * one targeted defect per group, every one seen turning that group red.
 */
#include <stdio.h>

#include "app_launcher/app_launcher.c"

/* app_launcher.c's entry point was renamed by the build line; take the name
 * back for ours.  Nothing of this file may sit above that include. */
#undef main

/* The bit encoding, spelled out.  See the warning in the header: these must not
 * be the enumerators. */
#define WANT_NONE       0
#define WANT_FB         1
#define WANT_TOUCH      2
#define WANT_FB_TOUCH   3

static int fails = 0;

static const char *mode_name(int m) {
    switch (m) {
    case WANT_NONE:     return "ARG_NONE";
    case WANT_FB:       return "ARG_FB";
    case WANT_TOUCH:    return "ARG_TOUCH";
    case WANT_FB_TOUCH: return "ARG_FB_TOUCH";
    }
    return "ARG_<out of range>";
}

/* `what` opens with the group letter so the sabotage harness can attribute a
 * failure to one group rather than to the run as a whole. */
static void expect_mode(const char *what, const char *input, int want) {
    int got = (int)parse_args(input);
    if (got != want) {
        printf("  FAIL %-38s got %s(%d) want %s(%d)\n",
               what, mode_name(got), got, mode_name(want), want);
        fails++;
    } else {
        printf("  ok   %-38s %s(%d)\n", what, mode_name(got), got);
    }
}

static void expect_int(const char *what, int got, int want) {
    if (got != want) {
        printf("  FAIL %-38s got %d want %d\n", what, got, want);
        fails++;
    } else {
        printf("  ok   %-38s %d\n", what, got);
    }
}

int main(void) {
    printf("A  the four documented args= values\n");
    /* The encoding first: parse_args() ORs the two bits together, so the values
     * are load-bearing and not merely labels — ARG_FB_TOUCH has to BE the OR of
     * the other two or the composed result names something else. */
    expect_int("A  ARG_NONE == 0", (int)ARG_NONE, WANT_NONE);
    expect_int("A  ARG_FB == 1", (int)ARG_FB, WANT_FB);
    expect_int("A  ARG_TOUCH == 2", (int)ARG_TOUCH, WANT_TOUCH);
    expect_int("A  ARG_FB_TOUCH == 3", (int)ARG_FB_TOUCH, WANT_FB_TOUCH);
    expect_int("A  ARG_FB_TOUCH == FB | TOUCH",
               (int)ARG_FB_TOUCH, (int)ARG_FB | (int)ARG_TOUCH);
    /* Then the four spellings.  These are the whole documented vocabulary, and
     * each is what an installed .app file actually contains. */
    expect_mode("A  \"fb,touch\"", "fb,touch", WANT_FB_TOUCH);
    expect_mode("A  \"fb\"", "fb", WANT_FB);
    expect_mode("A  \"touch\"", "touch", WANT_TOUCH);
    expect_mode("A  \"none\"", "none", WANT_NONE);

    printf("\nB  substring, not token\n");
    /* strstr(), so any string CONTAINING a keyword selects it.  These four are
     * what separates the shipped behaviour from a comma-tokenising rewrite: a
     * tokeniser returns the ARG_FB_TOUCH fallback for every one of them. */
    expect_mode("B  \"offbeat\" holds \"fb\"", "offbeat", WANT_FB);
    expect_mode("B  \"fbdev\" holds \"fb\"", "fbdev", WANT_FB);
    expect_mode("B  \"untouched\" holds \"touch\"", "untouched", WANT_TOUCH);
    expect_mode("B  \"touchscreen\" holds \"touch\"", "touchscreen", WANT_TOUCH);
    /* The near-miss control, and NOT a typo: "framebuffer" spells
     * f-r-a-m-e-b-u-f-f-e-r, which contains no `f` immediately followed by a
     * `b`, so strstr() finds nothing and the fallback applies.  A sweep of
     * positive substring cases alone cannot tell strstr() from a prefix match
     * or a token match; this case can. */
    expect_mode("B  \"framebuffer\" has no \"fb\"", "framebuffer", WANT_FB_TOUCH);
    /* Same idea for the other keyword: holds "buf" and "ton", neither keyword. */
    expect_mode("B  \"softbufton\" holds neither", "softbufton", WANT_FB_TOUCH);

    printf("\nC  \"none\" is whole-string only; fb and touch OR together\n");
    /* The `none` test is a strcmp() over the WHOLE string and runs first, so it
     * can never act as a token: anything beside it drops through to the
     * substring pair and the `none` is then simply ignored. */
    expect_mode("C  \"none,fb\" -> the fb wins", "none,fb", WANT_FB);
    expect_mode("C  \"touch,none\" -> the touch wins", "touch,none", WANT_TOUCH);
    /* Doubling it loses ARG_NONE entirely: the strcmp() fails and neither
     * keyword is present, so this is the fallback, not "no arguments". */
    expect_mode("C  \"none,none\" -> fallback", "none,none", WANT_FB_TOUCH);
    /* The two bits are OR'd, so order carries no meaning and neither does the
     * separator — there is no precedence between fb and touch to get wrong. */
    expect_mode("C  \"touch,fb\" == \"fb,touch\"", "touch,fb", WANT_FB_TOUCH);
    expect_mode("C  \"fb touch\" (space) == both", "fb touch", WANT_FB_TOUCH);

    printf("\nD  the ARG_FB_TOUCH fallback\n");
    /* Empty and unmatched both land on ARG_FB_TOUCH — the same value a correct
     * "fb,touch" returns.  That is why a misspelled args= line is invisible on
     * the panel: the child is launched with both paths and simply works. */
    expect_mode("D  empty string", "", WANT_FB_TOUCH);
    expect_mode("D  unrecognised \"xyzzy\"", "xyzzy", WANT_FB_TOUCH);
    /* A trailing space defeats the whole-string strcmp(), so "none " is the
     * fallback and NOT ARG_NONE.  Live manifests survive that only because
     * load_manifest() runs trim_trailing() over the line first — parse_args()
     * trims nothing itself, and a second caller would owe the same. */
    expect_mode("D  \"none \" (trailing space)", "none ", WANT_FB_TOUCH);

    printf("\nE  matching is case-SENSITIVE\n");
    /* strcmp()/strstr(), not their case-insensitive spellings, so an upper-case
     * manifest silently gets the fallback instead of what it asked for. */
    expect_mode("E  \"FB\"", "FB", WANT_FB_TOUCH);
    expect_mode("E  \"TOUCH\"", "TOUCH", WANT_FB_TOUCH);
    expect_mode("E  \"Fb\"", "Fb", WANT_FB_TOUCH);
    /* Upper-case `none` is the expensive one: it does not mean ARG_NONE, it
     * means "pass both device paths" — the opposite of the intent. */
    expect_mode("E  \"NONE\" is not ARG_NONE", "NONE", WANT_FB_TOUCH);
    expect_mode("E  \"None\" is not ARG_NONE", "None", WANT_FB_TOUCH);

    printf("\nF  NULL is reachable and safe\n");
    /* load_manifest() only ever calls this with a buffer, but the guard is
     * real: `!s` is the first test and || short-circuits, so no dereference
     * happens.  This case can only fail by CRASHING, which is what makes it
     * worth having — delete the `!s` half and the run dies here instead of
     * printing FAIL, so the harness has to read the exit status too. */
    expect_mode("F  NULL", NULL, WANT_FB_TOUCH);

    printf("\n%s (%d failure%s)\n", fails ? "FAILED" : "PASSED",
           fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
