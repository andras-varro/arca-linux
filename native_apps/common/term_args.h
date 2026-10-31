/*
 * term_args.h -- the command line of `terminal`, as a pure function.
 *
 *   terminal [fb [touch]] [--cwd DIR] [--exec PROG [ARG...]]
 *
 * fb and touch are the two positional arguments the launcher passes. --exec ends the
 * option list: everything after it is the program and its arguments, run instead of
 * the login shell (so a "--x" after PROG belongs to PROG). --cwd DIR is the working
 * directory of that program. Without --exec nothing changes. Host test:
 * tests/term_args_test.c.
 */
#ifndef TERM_ARGS_H
#define TERM_ARGS_H

#include <string.h>

typedef struct {
    const char  *fb;        /* never NULL */
    const char  *touch;     /* never NULL */
    const char  *cwd;       /* NULL: inherit */
    char *const *exec_argv; /* PROG, ARG..., NULL-terminated as argv itself is; NULL: login */
} TermArgs;

/* 0 on success; -1 on --exec or --cwd without a value, an unknown "--" option before
 * --exec, or a third positional argument. argv must be NULL-terminated (as main's is). */
static inline int term_args_parse(int argc, char *const argv[], TermArgs *out) {
    out->fb = "/dev/fb0";
    out->touch = "/dev/input/touchscreen0";
    out->cwd = NULL;
    out->exec_argv = NULL;
    int pos = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--exec") == 0) {
            if (i + 1 >= argc || argv[i + 1][0] == '\0') return -1;
            out->exec_argv = &argv[i + 1];
            return 0;
        }
        if (strcmp(a, "--cwd") == 0) {
            if (i + 1 >= argc) return -1;
            out->cwd = argv[++i];
            continue;
        }
        if (a[0] == '-' && a[1] == '-') return -1;
        if (pos == 0) out->fb = a;
        else if (pos == 1) out->touch = a;
        else return -1;
        pos++;
    }
    return 0;
}

/* Exit status of a finished child as terminal reports it: its own status, or 128 + the
 * signal that killed it (the shell's convention). */
static inline int term_exit_code(int wait_status) {
    if ((wait_status & 0x7f) == 0) return (wait_status >> 8) & 0xff;
    return 128 + (wait_status & 0x7f);
}

#endif
