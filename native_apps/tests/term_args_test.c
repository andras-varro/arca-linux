/* Host test for common/term_args.h.
 * Build: gcc -Wall -Wextra -I.. -o /tmp/term_args_test tests/term_args_test.c */
#include <stdio.h>
#include <string.h>
#include "../common/term_args.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static int run(TermArgs *t, char *const *v) {
    int n = 0;
    while (v[n]) n++;
    return term_args_parse(n, v, t);
}

int main(void) {
    TermArgs t;
    { char *v[] = {"terminal", NULL};
      CHECK(run(&t, v) == 0); CHECK(!strcmp(t.fb, "/dev/fb0")); CHECK(!strcmp(t.touch, "/dev/input/touchscreen0"));
      CHECK(t.exec_argv == NULL); CHECK(t.cwd == NULL); }
    { char *v[] = {"terminal", "/dev/fb1", "/dev/input/t", NULL};
      CHECK(run(&t, v) == 0); CHECK(!strcmp(t.fb, "/dev/fb1")); CHECK(!strcmp(t.touch, "/dev/input/t")); CHECK(t.exec_argv == NULL); }
    { char *v[] = {"terminal", "/dev/fb0", "/dev/input/touchscreen0", "--exec", "nano", "-v", "--x", "/etc/os-release", NULL};
      CHECK(run(&t, v) == 0); CHECK(t.exec_argv && !strcmp(t.exec_argv[0], "nano"));
      CHECK(!strcmp(t.exec_argv[1], "-v")); CHECK(!strcmp(t.exec_argv[2], "--x"));
      CHECK(!strcmp(t.exec_argv[3], "/etc/os-release")); CHECK(t.exec_argv[4] == NULL); }
    { char *v[] = {"terminal", "--cwd", "/tmp", "--exec", "nano", NULL};
      CHECK(run(&t, v) == 0); CHECK(!strcmp(t.cwd, "/tmp")); CHECK(!strcmp(t.fb, "/dev/fb0")); CHECK(!strcmp(t.exec_argv[0], "nano")); }
    { char *v[] = {"terminal", "--exec", NULL};  CHECK(run(&t, v) == -1); }
    { char *v[] = {"terminal", "--exec", "", NULL};  CHECK(run(&t, v) == -1); }
    { char *v[] = {"terminal", "--cwd", NULL};  CHECK(run(&t, v) == -1); }
    { char *v[] = {"terminal", "--bogus", NULL};  CHECK(run(&t, v) == -1); }
    { char *v[] = {"terminal", "a", "b", "c", NULL};  CHECK(run(&t, v) == -1); }
    CHECK(term_exit_code(0) == 0);
    CHECK(term_exit_code(3 << 8) == 3);
    CHECK(term_exit_code(9) == 137);
    printf(fails ? "term_args_test: %d FAILED\n" : "term_args_test: PASS\n", fails);
    return fails != 0;
}
