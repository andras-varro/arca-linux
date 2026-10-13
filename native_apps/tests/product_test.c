/*
 * product_test — product_name_from() reads NAME= out of an os-release file.
 *
 * The parser takes the value of the line that starts exactly "NAME=", strips
 * one pair of matching quotes and trailing blanks / CR, and falls back to
 * PRODUCT_NAME_FALLBACK for an unreadable file, a missing NAME=, or an empty
 * one.  The last group parses the SHIPPED rootfs/board/arca/os-release, the
 * one home of the name, so a quoting change there that the parser cannot read
 * fails here rather than on the panel.
 *
 * Build/run from native_apps/:
 *   gcc -Wall -I common tests/product_test.c common/product.c -o /tmp/t && /tmp/t
 */
#include "product.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int fails, total;
#define CHECK(c, ...) do { total++; if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

/* Parse `content` as an os-release file; the result is copied out. */
static const char *parse(const char *content) {
    static char out[128];
    char path[] = "/tmp/product_test_XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) { perror("mkstemp"); exit(2); }
    if (write(fd, content, strlen(content)) < 0) { perror("write"); exit(2); }
    close(fd);
    snprintf(out, sizeof(out), "%s", product_name_from(path));
    unlink(path);
    return out;
}

static void expect(const char *what, const char *content, const char *want) {
    const char *got = parse(content);
    CHECK(strcmp(got, want) == 0, "%s: got \"%s\", want \"%s\"", what, got, want);
}

int main(void) {
    expect("double quoted",      "NAME=\"Ludbox Linux\"\n", "Ludbox Linux");
    expect("single quoted",      "NAME='Some OS'\n", "Some OS");
    expect("unquoted",           "NAME=Plain\n", "Plain");
    expect("no trailing newline","NAME=\"Edge\"", "Edge");
    expect("CRLF",               "NAME=\"Crlf OS\"\r\n", "Crlf OS");
    expect("trailing blanks",    "NAME=\"Pad\"  \t\n", "Pad");
    expect("after other keys",   "ID=x\nVERSION=1\nNAME=\"Late\"\nPRETTY_NAME=\"No\"\n", "Late");
    expect("PRETTY_NAME first",  "PRETTY_NAME=\"Wrong\"\nNAME=\"Right\"\n", "Right");
    expect("first NAME wins",    "NAME=First\nNAME=Second\n", "First");
    expect("inner quote kept",   "NAME=\"A\"B\"\n", "A\"B");
    expect("mismatched quotes",  "NAME=\"Odd'\n", "\"Odd'");

    expect("no NAME line",       "ID=x\nPRETTY_NAME=\"Only\"\n", PRODUCT_NAME_FALLBACK);
    expect("PRETTY_NAME alone",  "PRETTY_NAME=\"Only\"\n", PRODUCT_NAME_FALLBACK);
    expect("commented NAME",     "#NAME=\"Hidden\"\n", PRODUCT_NAME_FALLBACK);
    expect("indented NAME",      " NAME=\"Hidden\"\n", PRODUCT_NAME_FALLBACK);
    expect("empty NAME",         "NAME=\"\"\n", PRODUCT_NAME_FALLBACK);
    expect("bare NAME=",         "NAME=\n", PRODUCT_NAME_FALLBACK);
    expect("empty file",         "", PRODUCT_NAME_FALLBACK);

    CHECK(strcmp(product_name_from("/nonexistent/product_test/os-release"),
                 PRODUCT_NAME_FALLBACK) == 0, "unreadable file falls back");
    CHECK(strcmp(product_name_from(NULL), PRODUCT_NAME_FALLBACK) == 0, "NULL path falls back");

    /* A long value is truncated, not overrun, and not mistaken for "no name". */
    {
        char big[400] = "NAME=";
        memset(big + 5, 'x', 300);
        strcpy(big + 305, "\n");
        const char *got = parse(big);
        CHECK(strlen(got) > 0 && strlen(got) < 64 && got[0] == 'x', "long value truncated, got %zu chars", strlen(got));
    }

    /* The one home of the name, as the device will read it. */
    {
        const char *got = product_name_from("../rootfs/board/arca/os-release");
        CHECK(strcmp(got, "Ludbox Linux") == 0, "shipped os-release: got \"%s\"", got);
    }

    /* product_name() is never empty whatever this host's /etc/os-release holds. */
    CHECK(product_name() && *product_name(), "product_name() non-empty");
    CHECK(product_name() == product_name(), "product_name() is cached (stable pointer)");

    printf("%d checks, %d failed\n", total, fails);
    return fails ? 1 : 0;
}
