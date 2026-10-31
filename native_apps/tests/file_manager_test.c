/* Host regression for the file manager's logic (file_manager/fm_core.c).
 *
 *   cd native_apps && gcc -Wall -Wextra -Wno-unused-parameter -I. -o build/file_manager_test \
 *       tests/file_manager_test.c file_manager/fm_core.c -lm && ./build/file_manager_test
 *
 * What it asserts:
 *   A  path normalise / join / parent / within (whole components: "/a/bc" is
 *      not below "/a/b"), name rules, shell-safe check;
 *   B  the transfer check: same path, a folder into itself or below itself,
 *      a prefix that is only a prefix;
 *   C  size text (at most 5 characters, K/M/G boundaries);
 *   D  sort: folders first whatever the key; name ignores case; size and date
 *      put the largest / newest first;
 *   E  a real directory read: types from lstat, a symlink is a LINK, sorted;
 *   F  the arca-storage line parser: every verb, and the hostile or garbled
 *      lines that must not become a root;
 *   G  rename (refuses to replace) and mkdir;
 *   H  copy as a state machine: bounded work per step, byte-identical result,
 *      nested folders, a symlink copied as a link and never followed;
 *   I  the overwrite conflict: ask, skip, overwrite, and the two "all" forms;
 *   J  refusals at init: into itself, onto itself, target not a folder;
 *   K  move: same-filesystem rename, a merge into an existing folder, and the
 *      copy-then-delete path across filesystems when /dev/shm is another one;
 *   L  delete: recursive, and a link to a folder is removed without touching
 *      what it points at;
 *   M  cancel removes the half-written target;
 *   N  a write error (the file-size limit stands in for a full stick) stops the
 *      job as FM_J_ERROR with err_fatal, leaves no partial file, and cancels.
 *   O  expert mode's pure pieces: mode text (setuid/setgid/sticky, device
 *      types), the long detail line in a fixed zone, the link-to-folder
 *      resolver, fm_dir_read's mode and link_dir, pseudo and protected paths.
 * "Failing before" = compile against a sabotaged COPY of fm_core.c (see the
 * sabotage notes in the commit that added this file): each group has a case
 * that a broken fm_core.c turns red. */
#include "file_manager/fm_core.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <unistd.h>

static int fails, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { fails++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ── helpers ─────────────────────────────────────────────────────────────── */

static char TMP[256];

static void P(char *out, const char *a, const char *b) { snprintf(out, FM_PATH_MAX, "%s/%s", a, b); }

static void mk(const char *path) { mkdir(path, 0755); }

static void put(const char *path, long n, unsigned seed) {
    FILE *f = fopen(path, "wb");
    if (!f) { printf("FAIL: cannot create %s\n", path); fails++; return; }
    for (long i = 0; i < n; i++) fputc((int)((i * 31 + seed) & 0xff), f);
    fclose(f);
}

static bool same_content(const char *a, const char *b, long *len) {
    FILE *fa = fopen(a, "rb"), *fb = fopen(b, "rb");
    bool ok = fa && fb;
    long n = 0;
    while (ok) {
        int ca = fgetc(fa), cb = fgetc(fb);
        if (ca != cb) ok = false;
        if (ca == EOF) break;
        n++;
    }
    if (fa) fclose(fa);
    if (fb) fclose(fb);
    if (len) *len = n;
    return ok;
}

static bool exists(const char *p) { struct stat st; return lstat(p, &st) == 0; }
static bool is_link(const char *p) { struct stat st; return lstat(p, &st) == 0 && S_ISLNK(st.st_mode); }
static bool is_dir(const char *p) { struct stat st; return lstat(p, &st) == 0 && S_ISDIR(st.st_mode); }

static void rmtree(const char *p) {
    char cmd[FM_PATH_MAX + 16];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", p);
    if (system(cmd) != 0) { /* nothing to clean */ }
}

/* Run a job to the end.  conflict[]: the answers for successive conflicts
 * (the last repeats).  Errors are acknowledged as skip.  Returns the end state;
 * *steps counts fm_job_step calls, *max_step the most bytes one call added. */
static FmJobState drive(FmJob *j, long long budget, const FmDecision *conflict, int nconf,
                        int *steps, long long *max_step, int *nerr) {
    int ci = 0;
    if (steps) *steps = 0;
    if (max_step) *max_step = 0;
    if (nerr) *nerr = 0;
    for (int guard = 0; guard < 2000000; guard++) {
        long long before = j->bytes_done;
        FmJobState s = fm_job_step(j, budget);
        if (steps) (*steps)++;
        if (max_step && j->bytes_done - before > *max_step) *max_step = j->bytes_done - before;
        if (s == FM_J_DONE || s == FM_J_CANCELLED) return s;
        if (s == FM_J_CONFLICT) {
            FmDecision d = nconf ? conflict[ci < nconf ? ci : nconf - 1] : FM_DECIDE_SKIP;
            ci++;
            fm_job_decide(j, d);
        } else if (s == FM_J_ERROR) {
            if (nerr) (*nerr)++;
            fm_job_ack_error(j, false);
        }
    }
    return FM_J_RUN;     /* never finished */
}

static int run(FmOp op, const char *sd, const char *dd, const char *const *names, int n,
               long long budget, const FmDecision *cf, int ncf, FmJob *out_job, int *steps,
               long long *max_step, int *nerr, FmJobState *end) {
    char err[200];
    if (fm_job_init(out_job, op, sd, dd, names, n, err, sizeof err) < 0) { printf("  (init refused: %s)\n", err); return -1; }
    *end = drive(out_job, budget, cf, ncf, steps, max_step, nerr);
    return 0;
}

/* ── A ───────────────────────────────────────────────────────────────────── */

static void group_a(void) {
    static const struct { const char *in, *want; } norm[] = {
        { "/", "/" }, { "//", "/" }, { "/a/b/", "/a/b" }, { "/a//b", "/a/b" },
        { "/a/./b", "/a/b" }, { "/a/b/..", "/a" }, { "/a/../..", "/" }, { "/..", "/" },
        { "/a/b/../../c", "/c" }, { "/home/root/data/./x/../y", "/home/root/data/y" },
    };
    char o[FM_PATH_MAX];
    for (unsigned i = 0; i < sizeof norm / sizeof norm[0]; i++) {
        bool ok = fm_path_normalise(norm[i].in, o, sizeof o);
        CHECK(ok && !strcmp(o, norm[i].want), "A normalise '%s' -> '%s' (want '%s')", norm[i].in, ok ? o : "(false)", norm[i].want);
    }
    CHECK(!fm_path_normalise("a/b", o, sizeof o), "A relative path refused");
    CHECK(!fm_path_normalise("", o, sizeof o), "A empty path refused");
    CHECK(!fm_path_normalise("/abcdefghij", o, 8), "A overflow refused");

    CHECK(fm_path_join("/a/b", "c", o, sizeof o) && !strcmp(o, "/a/b/c"), "A join");
    CHECK(fm_path_join("/", "c", o, sizeof o) && !strcmp(o, "/c"), "A join at root");
    CHECK(!fm_path_join("/a", "..", o, sizeof o), "A join refuses ..");
    CHECK(!fm_path_join("/a", "b/c", o, sizeof o), "A join refuses a slash in the name");
    CHECK(!fm_path_join("/a", "", o, sizeof o), "A join refuses an empty name");

    CHECK(fm_path_parent("/a/b/c", o, sizeof o) && !strcmp(o, "/a/b"), "A parent");
    CHECK(fm_path_parent("/a", o, sizeof o) && !strcmp(o, "/"), "A parent of /a is /");
    CHECK(!fm_path_parent("/", o, sizeof o), "A / has no parent");

    CHECK(fm_path_within("/a/b", "/a/b"), "A within: equal");
    CHECK(fm_path_within("/a/b", "/a/b/c/d"), "A within: below");
    CHECK(!fm_path_within("/a/b", "/a/bc"), "A within: '/a/bc' is not below '/a/b'");
    CHECK(!fm_path_within("/a/b", "/a"), "A within: the parent is not below");
    CHECK(!fm_path_within("/a/b", "/a/b/../c"), "A within: a .. that leaves the root");
    CHECK(fm_path_within("/a/b", "/a/b/c/../d"), "A within: a .. that stays");
    CHECK(!fm_path_within("/home/root/data", "/home/root/data/../.ssh"), "A within: escape through ..");

    CHECK(fm_name_ok("file.txt") && fm_name_ok("a b") && fm_name_ok(".hidden"), "A names ok");
    CHECK(!fm_name_ok("") && !fm_name_ok(".") && !fm_name_ok("..") && !fm_name_ok("a/b") &&
          !fm_name_ok("a\nb") && !fm_name_ok(NULL), "A names refused");
    char longname[300];
    memset(longname, 'x', sizeof longname); longname[299] = '\0';
    CHECK(!fm_name_ok(longname), "A 299-byte name refused");

    CHECK(fm_path_shell_safe("/media/usb0") && fm_path_shell_safe("/a_b-c.d/e"), "A shell-safe ok");
    CHECK(!fm_path_shell_safe("/media/usb 0") && !fm_path_shell_safe("/a;b") &&
          !fm_path_shell_safe("/a$(x)") && !fm_path_shell_safe("/a'b") && !fm_path_shell_safe(""), "A shell-unsafe refused");
    CHECK(!strcmp(fm_base("/a/b/c.txt"), "c.txt") && !strcmp(fm_base("x"), "x"), "A base");
}

/* ── B ───────────────────────────────────────────────────────────────────── */

static void group_b(void) {
    CHECK(fm_check_transfer("/d/a", "f", "/d/b", false) == FM_OK, "B file to another folder");
    CHECK(fm_check_transfer("/d/a", "f", "/d/a", false) == FM_ERR_SAME, "B file onto itself");
    CHECK(fm_check_transfer("/d/a", "f", "/d/a/", false) == FM_ERR_SAME, "B file onto itself, trailing slash");
    CHECK(fm_check_transfer("/d", "a", "/d/a", true) == FM_ERR_INTO_SELF, "B folder into itself");
    CHECK(fm_check_transfer("/d", "a", "/d/a/sub/deep", true) == FM_ERR_INTO_SELF, "B folder below itself");
    CHECK(fm_check_transfer("/d", "a", "/d/ab", true) == FM_OK, "B folder into a sibling that only shares a prefix");
    CHECK(fm_check_transfer("/d", "a", "/d/a/../ab", true) == FM_OK, "B .. resolved before the check");
    CHECK(fm_check_transfer("/d", "a", "/d/x/../a/y", true) == FM_ERR_INTO_SELF, "B .. into itself");
    CHECK(fm_check_transfer("/d", "a", "/d", true) == FM_ERR_SAME, "B folder onto itself (same parent)");
    CHECK(fm_check_transfer("/d", "a/b", "/e", false) == FM_ERR_NAME, "B bad name");
    CHECK(fm_check_transfer("/d", "a", "/d/a", false) == FM_OK, "B a FILE named like the folder below it is not 'into itself'");
}

/* ── C ───────────────────────────────────────────────────────────────────── */

static void group_c(void) {
    static const struct { long long b; const char *want; } t[] = {
        { 0, "0B" }, { 1, "1B" }, { 1023, "1023B" }, { 1024, "1.0K" }, { 1536, "1.5K" },
        { 10 * 1024, "10K" }, { 1023 * 1024, "1023K" }, { 1024 * 1024, "1.0M" },
        { 5LL * 1024 * 1024 + 300000, "5.2M" }, { 100LL * 1024 * 1024, "100M" },
        { 1024LL * 1024 * 1024, "1.0G" }, { 3LL * 1024 * 1024 * 1024 + 512LL * 1024 * 1024, "3.5G" },
        { 1024LL * 1024 * 1024 * 1024, "1.0T" }, { -5, "0B" },
    };
    for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++) {
        char o[16];
        fm_format_size(t[i].b, o, sizeof o);
        CHECK(!strcmp(o, t[i].want), "C size %lld -> '%s' (want '%s')", t[i].b, o, t[i].want);
        CHECK(strlen(o) <= 5, "C size %lld is %zu characters", t[i].b, strlen(o));
    }
}

/* ── D ───────────────────────────────────────────────────────────────────── */

static void group_d(void) {
    FmEntry d1 = { "Zdir", FM_T_DIR, 0, 10, 0, false }, d2 = { "adir", FM_T_DIR, 0, 99, 0, false };
    FmEntry f1 = { "b.txt", FM_T_FILE, 500, 100, 0, false }, f2 = { "A.txt", FM_T_FILE, 900, 50, 0, false },
            f3 = { "c.txt", FM_T_FILE, 500, 200, 0, false };
    FmSort s;
    for (s = FM_SORT_NAME; s < FM_SORT_COUNT; s++) {
        CHECK(fm_entry_cmp(&d1, &f2, s) < 0 && fm_entry_cmp(&f2, &d1, s) > 0, "D folder before file, sort %s", fm_sort_label(s));
        CHECK(fm_entry_cmp(&d2, &d1, s) < 0, "D folders by name under %s (adir before Zdir)", fm_sort_label(s));
    }
    CHECK(fm_entry_cmp(&f2, &f1, FM_SORT_NAME) < 0, "D name: 'A.txt' before 'b.txt' (case-insensitive)");
    CHECK(fm_entry_cmp(&f2, &f1, FM_SORT_SIZE) < 0, "D size: 900 before 500 (largest first)");
    CHECK(fm_entry_cmp(&f1, &f3, FM_SORT_SIZE) < 0, "D size tie falls to name");
    CHECK(fm_entry_cmp(&f3, &f1, FM_SORT_DATE) < 0, "D date: 200 before 100 (newest first)");
    CHECK(fm_entry_cmp(&f1, &f1, FM_SORT_NAME) == 0, "D equal to itself");
    FmEntry lo = { "a", FM_T_FILE, 0, 0, 0, false }, up = { "A", FM_T_FILE, 0, 0, 0, false };
    CHECK(fm_entry_cmp(&lo, &up, FM_SORT_NAME) != 0, "D 'a' and 'A' are not equal (bytes break the tie)");
    CHECK(!strcmp(fm_sort_label(FM_SORT_NAME), "NAME") && !strcmp(fm_sort_label(FM_SORT_SIZE), "SIZE") &&
          !strcmp(fm_sort_label(FM_SORT_DATE), "DATE"), "D labels");

    char o[16];
    fm_entry_detail(&d1, o, sizeof o); CHECK(!strcmp(o, "<DIR>"), "D detail of a folder");
    fm_entry_detail(&f1, o, sizeof o); CHECK(!strcmp(o, "500B"), "D detail of a file");
}

/* ── E ───────────────────────────────────────────────────────────────────── */

static void group_e(void) {
    char base[FM_PATH_MAX], p[FM_PATH_MAX], q[FM_PATH_MAX];
    P(base, TMP, "e"); mk(base);
    P(p, base, "Beta.txt"); put(p, 100, 1);
    P(p, base, "alpha.bin"); put(p, 3000, 2);
    P(p, base, "sub"); mk(p);
    P(p, base, "link"); P(q, base, "sub"); (void)!symlink(q, p);
    FmDir d;
    CHECK(fm_dir_read(base, FM_SORT_NAME, &d) == 0, "E read ok");
    CHECK(d.n == 4, "E 4 entries (got %d)", d.n);
    if (d.n == 4) {
        CHECK(!strcmp(d.e[0].name, "sub") && d.e[0].type == FM_T_DIR, "E folder first");
        int il = fm_dir_find(&d, "link"), ia = fm_dir_find(&d, "alpha.bin");
        CHECK(il >= 0 && d.e[il].type == FM_T_LINK, "E a symlink to a folder is a LINK, not a DIR");
        CHECK(ia >= 0 && d.e[ia].type == FM_T_FILE && d.e[ia].size == 3000, "E file size");
        CHECK(fm_dir_find(&d, "nope") == -1, "E find miss");
        CHECK(!strcmp(d.e[1].name, "alpha.bin") && !strcmp(d.e[2].name, "Beta.txt"), "E sorted by name");
        fm_dir_sort(&d, FM_SORT_SIZE);
        CHECK(!strcmp(d.e[0].name, "sub") && !strcmp(d.e[1].name, "alpha.bin"), "E re-sorted by size");
    }
    fm_dir_free(&d);
    CHECK(d.e == NULL && d.n == 0, "E free clears");
    CHECK(fm_dir_read("/nonexistent-dir-xyz", FM_SORT_NAME, &d) < 0, "E unreadable folder reports an error");

    long long fr = 0, tot = 0;
    CHECK(fm_space(base, &fr, &tot) && tot > 0 && fr >= 0 && fr <= tot, "E free space <= total");
    CHECK(!fm_space("/nonexistent-dir-xyz", &fr, &tot), "E space of a missing path fails");
}

/* ── F ───────────────────────────────────────────────────────────────────── */

static void group_f(void) {
    FmStLine l;
    l = fm_parse_storage_line("MOUNTED /media/usb0 /dev/sda1");
    CHECK(l.kind == FM_ST_MOUNTED && !strcmp(l.mp, "/media/usb0") && !strcmp(l.dev, "/dev/sda1"), "F MOUNTED");
    l = fm_parse_storage_line("ALREADY /media/usb1 /dev/sdb\n");
    CHECK(l.kind == FM_ST_ALREADY && !strcmp(l.mp, "/media/usb1") && !strcmp(l.dev, "/dev/sdb"), "F ALREADY, newline");
    l = fm_parse_storage_line("UNSUPPORTED /dev/sda2 exFAT");
    CHECK(l.kind == FM_ST_UNSUPPORTED && !strcmp(l.dev, "/dev/sda2") && !strcmp(l.extra, "exFAT"), "F UNSUPPORTED");
    l = fm_parse_storage_line("FAILED /dev/sda1 mount: wrong fs type, bad option\r\n");
    CHECK(l.kind == FM_ST_FAILED && !strcmp(l.dev, "/dev/sda1") && !strcmp(l.extra, "mount: wrong fs type, bad option"), "F FAILED keeps the whole reason (got '%s')", l.extra);
    l = fm_parse_storage_line("GONE /media/usb0");
    CHECK(l.kind == FM_ST_GONE && !strcmp(l.mp, "/media/usb0"), "F GONE");
    l = fm_parse_storage_line("EJECTED /media/usb0");
    CHECK(l.kind == FM_ST_EJECTED && !strcmp(l.mp, "/media/usb0"), "F EJECTED");
    l = fm_parse_storage_line("BUSY /media/usb0");
    CHECK(l.kind == FM_ST_BUSY && !strcmp(l.mp, "/media/usb0"), "F BUSY");
    l = fm_parse_storage_line("  MOUNTED   /media/usb2    /dev/sdc1  ");
    CHECK(l.kind == FM_ST_MOUNTED && !strcmp(l.mp, "/media/usb2"), "F extra spaces");

    static const char *bad[] = {
        "", "   ", "\n", "MOUNTED", "MOUNTED /media/usb0", "MOUNTED media/usb0 /dev/sda1",
        "MOUNTED / /dev/sda1", "MOUNTED /media/../etc /dev/sda1", "MOUNTED /media//usb0 /dev/sda1",
        "MOUNTED /media/usb0/ /dev/sda1", "MOUNTED /media/usb0;reboot /dev/sda1",
        "MOUNTED /media/usb0 sda1", "MOUNTED /media/usb0 /tmp/sda1", "MOUNTED /media/usb0 /dev/",
        "UNSUPPORTED sda2 exFAT", "UNSUPPORTED /dev/sda2", "FAILED", "FAILED sda1 x", "GONE", "GONE /",
        "EJECTED /media/usb 0x", "mounted /media/usb0 /dev/sda1", "HELLO /media/usb0",
        "MOUNTED /media/$(id) /dev/sda1", "BUSY relative",
    };
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        l = fm_parse_storage_line(bad[i]);
        CHECK(l.kind == FM_ST_NONE, "F line '%s' must give no root (kind %d)", bad[i], (int)l.kind);
    }
    l = fm_parse_storage_line(NULL);
    CHECK(l.kind == FM_ST_NONE, "F NULL line");
}

/* ── G ───────────────────────────────────────────────────────────────────── */

static void group_g(void) {
    char base[FM_PATH_MAX], p[FM_PATH_MAX], q[FM_PATH_MAX];
    P(base, TMP, "g"); mk(base);
    P(p, base, "one"); put(p, 10, 1);
    P(p, base, "two"); put(p, 20, 2);
    CHECK(fm_rename(base, "one", "uno") == 0, "G rename");
    P(p, base, "uno"); P(q, base, "one");
    CHECK(exists(p) && !exists(q), "G renamed on disk");
    CHECK(fm_rename(base, "uno", "two") == EEXIST, "G rename onto an existing name is refused");
    P(p, base, "two");
    long n = 0; FILE *f = fopen(p, "rb"); if (f) { fseek(f, 0, SEEK_END); n = ftell(f); fclose(f); }
    CHECK(n == 20, "G the existing file was not replaced");
    CHECK(fm_rename(base, "uno", "a/b") == EINVAL && fm_rename(base, "uno", "..") == EINVAL &&
          fm_rename(base, "uno", "") == EINVAL, "G bad new names");
    CHECK(fm_rename(base, "uno", "uno") == 0, "G rename to itself is a no-op");
    CHECK(fm_rename(base, "missing", "x") != 0, "G rename of a missing file fails");
    CHECK(fm_mkdir(base, "newdir") == 0, "G mkdir");
    P(p, base, "newdir"); CHECK(is_dir(p), "G mkdir made a folder");
    CHECK(fm_mkdir(base, "newdir") == EEXIST, "G mkdir twice");
    CHECK(fm_mkdir(base, "x/y") == EINVAL, "G mkdir bad name");
}

/* ── H ───────────────────────────────────────────────────────────────────── */

static void group_h(void) {
    char s[FM_PATH_MAX], d[FM_PATH_MAX], a[FM_PATH_MAX], b[FM_PATH_MAX];
    P(s, TMP, "hs"); P(d, TMP, "hd"); mk(s); mk(d);
    P(a, s, "big.bin"); put(a, 300000, 7);
    P(a, s, "tree"); mk(a);
    P(a, s, "tree/one.txt"); put(a, 1000, 3);
    P(a, s, "tree/inner"); mk(a);
    P(a, s, "tree/inner/two.txt"); put(a, 70000, 4);
    P(a, s, "tree/empty"); mk(a);
    P(a, s, "outside"); mk(a);
    P(a, s, "outside/secret.txt"); put(a, 50, 9);
    P(a, s, "tree/lnk"); P(b, s, "outside"); (void)!symlink(b, a);        /* link to a folder */
    P(a, s, "tree/flnk"); (void)!symlink("one.txt", a);                    /* relative link to a file */

    const char *names[] = { "big.bin", "tree" };
    FmJob j; FmJobState end; int steps, nerr; long long maxs;
    CHECK(run(FM_OP_COPY, s, d, names, 2, 10000, NULL, 0, &j, &steps, &maxs, &nerr, &end) == 0, "H init");
    CHECK(end == FM_J_DONE && nerr == 0, "H copy finished (state %d, errors %d)", (int)end, nerr);
    CHECK(steps > 30, "H a 300 KB copy at 10000 per step takes many steps (took %d)", steps);
    CHECK(maxs <= 10000, "H no step moved more than its budget (max %lld)", maxs);
    CHECK(j.bytes_done == 300000 + 1000 + 70000, "H bytes_done %lld", j.bytes_done);
    CHECK(j.files_done == 5, "H files_done %d (3 files + 2 links)", j.files_done);
    fm_job_free(&j);
    P(a, s, "big.bin"); P(b, d, "big.bin");
    long len;
    CHECK(same_content(a, b, &len) && len == 300000, "H big file identical");
    P(a, s, "tree/inner/two.txt"); P(b, d, "tree/inner/two.txt");
    CHECK(same_content(a, b, &len) && len == 70000, "H nested file identical");
    P(b, d, "tree/empty"); CHECK(is_dir(b), "H empty folder copied");
    P(b, d, "tree/lnk");
    CHECK(is_link(b), "H a link to a folder was copied as a LINK, not followed into a folder");
    char tgt[FM_PATH_MAX] = "";
    ssize_t r = readlink(b, tgt, sizeof tgt - 1); if (r > 0) tgt[r] = '\0';
    P(a, s, "outside");
    CHECK(!strcmp(tgt, a), "H the link keeps its target text");
    P(b, d, "outside"); CHECK(!exists(b), "H the folder the link points at was not copied");
    P(b, d, "tree/flnk"); r = readlink(b, tgt, sizeof tgt - 1); if (r > 0) tgt[r] = '\0'; else tgt[0] = '\0';
    CHECK(is_link(b) && !strcmp(tgt, "one.txt"), "H a relative link stays relative");
    P(a, s, "tree/one.txt"); CHECK(exists(a), "H copy leaves the source");

    /* one big step finishes small jobs at once */
    P(a, s, "solo.txt"); put(a, 5000, 1);
    const char *one[] = { "solo.txt" };
    CHECK(run(FM_OP_COPY, s, d, one, 1, 1LL << 40, NULL, 0, &j, &steps, &maxs, &nerr, &end) == 0 && end == FM_J_DONE && steps == 1,
          "H a huge budget finishes a small copy in one step (steps %d)", steps);
    fm_job_free(&j);
}

/* ── I ───────────────────────────────────────────────────────────────────── */

static void group_i(void) {
    char s[FM_PATH_MAX], d[FM_PATH_MAX], a[FM_PATH_MAX], b[FM_PATH_MAX];
    P(s, TMP, "is"); P(d, TMP, "id"); mk(s); mk(d);
    const char *nm[] = { "f1", "f2" };
    FmJob j; FmJobState end; int steps, nerr; long long maxs;
    char x[FM_PATH_MAX];
    long len;

#define SETUP() do { P(a, s, "f1"); put(a, 100, 1); P(a, s, "f2"); put(a, 200, 2); \
                     P(b, d, "f1"); put(b, 11, 5); P(b, d, "f2"); put(b, 22, 6); } while (0)
    FmDecision skip[] = { FM_DECIDE_SKIP };
    SETUP();
    CHECK(run(FM_OP_COPY, s, d, nm, 2, 1LL << 20, skip, 1, &j, &steps, &maxs, &nerr, &end) == 0 && end == FM_J_DONE, "I skip run");
    CHECK(j.skipped == 2 && j.files_done == 0, "I skip: skipped %d done %d", j.skipped, j.files_done);
    fm_job_free(&j);
    P(x, d, "f1");
    { struct stat sb; CHECK(stat(x, &sb) == 0 && sb.st_size == 11, "I skip leaves the old target"); }

    /* the first answer is asked about, not assumed */
    SETUP();
    CHECK(fm_job_init(&j, FM_OP_COPY, s, d, nm, 2, x, sizeof x) == 0, "I init");
    FmJobState st = fm_job_step(&j, 1LL << 20);
    CHECK(st == FM_J_CONFLICT, "I the job stops and asks (state %d)", (int)st);
    P(b, d, "f1"); { struct stat sb; stat(b, &sb); CHECK(sb.st_size == 11, "I nothing was touched while it waits"); }
    fm_job_decide(&j, FM_DECIDE_OVERWRITE);
    st = fm_job_step(&j, 1LL << 20);
    CHECK(st == FM_J_CONFLICT, "I the second file asks again after a single OVERWRITE (state %d)", (int)st);
    fm_job_decide(&j, FM_DECIDE_SKIP);
    st = fm_job_step(&j, 1LL << 20);
    CHECK(st == FM_J_DONE, "I done after the second answer");
    fm_job_free(&j);
    P(a, s, "f1"); P(b, d, "f1");
    CHECK(same_content(a, b, &len) && len == 100, "I overwrite replaced f1");
    P(b, d, "f2"); { struct stat sb; stat(b, &sb); CHECK(sb.st_size == 22, "I skip kept f2"); }

    FmDecision ow_all[] = { FM_DECIDE_OVERWRITE_ALL };
    SETUP();
    CHECK(run(FM_OP_COPY, s, d, nm, 2, 1LL << 20, ow_all, 1, &j, &steps, &maxs, &nerr, &end) == 0 && end == FM_J_DONE, "I overwrite-all run");
    fm_job_free(&j);
    P(a, s, "f2"); P(b, d, "f2");
    CHECK(same_content(a, b, &len) && len == 200, "I overwrite-all replaced the second without asking again");

    FmDecision sk_all[] = { FM_DECIDE_SKIP_ALL };
    SETUP();
    int asked = 0;
    CHECK(fm_job_init(&j, FM_OP_COPY, s, d, nm, 2, x, sizeof x) == 0, "I init 2");
    for (;;) {
        st = fm_job_step(&j, 1LL << 20);
        if (st == FM_J_CONFLICT) { asked++; fm_job_decide(&j, sk_all[0]); continue; }
        break;
    }
    CHECK(st == FM_J_DONE && asked == 1 && j.skipped == 2, "I skip-all asked once (asked %d, skipped %d)", asked, j.skipped);
    fm_job_free(&j);

    /* a folder where a file is expected is an error, not an overwrite */
    P(a, s, "f1"); P(b, d, "f1"); unlink(b); mk(b);
    const char *one[] = { "f1" };
    CHECK(fm_job_init(&j, FM_OP_COPY, s, d, one, 1, x, sizeof x) == 0, "I init dir-vs-file");
    st = fm_job_step(&j, 1LL << 20);
    CHECK(st == FM_J_ERROR, "I a folder in the way is an error (state %d)", (int)st);
    fm_job_free(&j);
#undef SETUP
}

/* ── J ───────────────────────────────────────────────────────────────────── */

static void group_j(void) {
    char s[FM_PATH_MAX], d[FM_PATH_MAX], a[FM_PATH_MAX], err[200];
    P(s, TMP, "js"); mk(s);
    P(a, s, "dir"); mk(a);
    P(a, s, "dir/sub"); mk(a);
    P(a, s, "file"); put(a, 10, 1);
    const char *dir[] = { "dir" }, *file[] = { "file" };
    FmJob j;
    P(d, s, "dir/sub");
    CHECK(fm_job_init(&j, FM_OP_COPY, s, d, dir, 1, err, sizeof err) < 0, "J copy a folder into its own subfolder refused (%s)", err);
    CHECK(strstr(err, "ITSELF") != NULL, "J the sentence says why: '%s'", err);
    CHECK(fm_job_init(&j, FM_OP_MOVE, s, d, dir, 1, err, sizeof err) < 0, "J move a folder into its own subfolder refused");
    P(d, s, "dir");
    CHECK(fm_job_init(&j, FM_OP_COPY, s, s, file, 1, err, sizeof err) < 0, "J copy a file onto itself refused (%s)", err);
    CHECK(fm_job_init(&j, FM_OP_COPY, s, s, dir, 1, err, sizeof err) < 0, "J copy a folder onto itself refused");
    P(d, s, "file");
    CHECK(fm_job_init(&j, FM_OP_COPY, s, d, dir, 1, err, sizeof err) < 0 && strstr(err, "NOT A FOLDER"), "J target that is a file refused (%s)", err);
    P(d, s, "missing");
    CHECK(fm_job_init(&j, FM_OP_COPY, s, d, file, 1, err, sizeof err) < 0, "J missing target refused");
    const char *bad[] = { "../x" };
    CHECK(fm_job_init(&j, FM_OP_DELETE, s, NULL, bad, 1, err, sizeof err) < 0, "J a name with .. refused");
    const char *gone[] = { "nothere" };
    CHECK(fm_job_init(&j, FM_OP_DELETE, s, NULL, gone, 1, err, sizeof err) < 0, "J a missing source refused");
    CHECK(fm_job_init(&j, FM_OP_DELETE, s, NULL, file, 0, err, sizeof err) < 0, "J nothing to do refused");
    P(d, s, "dir/sub");
    CHECK(fm_job_init(&j, FM_OP_COPY, s, d, file, 1, err, sizeof err) == 0, "J a FILE into that subfolder is fine");
    fm_job_free(&j);
}

/* ── K ───────────────────────────────────────────────────────────────────── */

static void group_k(void) {
    char s[FM_PATH_MAX], d[FM_PATH_MAX], a[FM_PATH_MAX], b[FM_PATH_MAX];
    P(s, TMP, "ks"); P(d, TMP, "kd"); mk(s); mk(d);
    P(a, s, "m.bin"); put(a, 90000, 5);
    P(a, s, "mt"); mk(a);
    P(a, s, "mt/x.txt"); put(a, 300, 6);
    P(a, s, "mt/y"); mk(a);
    P(a, s, "mt/y/z.txt"); put(a, 400, 7);
    P(a, d, "mt"); mk(a);                       /* the target folder exists: a merge */
    P(a, d, "mt/x.txt"); put(a, 5, 1);          /* and holds a clashing file */
    const char *nm[] = { "m.bin", "mt" };
    FmJob j; FmJobState end; int steps, nerr; long long maxs;
    FmDecision ow[] = { FM_DECIDE_OVERWRITE };
    CHECK(run(FM_OP_MOVE, s, d, nm, 2, 1LL << 20, ow, 1, &j, &steps, &maxs, &nerr, &end) == 0 && end == FM_J_DONE && nerr == 0,
          "K same-filesystem move (state %d, errors %d)", (int)end, nerr);
    fm_job_free(&j);
    P(a, s, "m.bin"); P(b, d, "m.bin");
    long len; struct stat sb;
    CHECK(!exists(a) && stat(b, &sb) == 0 && sb.st_size == 90000, "K the file moved");
    P(b, d, "mt/x.txt"); CHECK(stat(b, &sb) == 0 && sb.st_size == 300, "K the clash was overwritten");
    P(b, d, "mt/y/z.txt"); CHECK(stat(b, &sb) == 0 && sb.st_size == 400, "K the nested file arrived");
    P(a, s, "mt"); CHECK(!exists(a), "K the emptied source folder was removed");

    /* skip one: the source keeps that file and its folder */
    P(a, s, "mt"); mk(a);
    P(a, s, "mt/x.txt"); put(a, 300, 6);
    P(a, d, "mt/x.txt"); put(a, 5, 1);
    const char *one[] = { "mt" };
    FmDecision sk[] = { FM_DECIDE_SKIP };
    CHECK(run(FM_OP_MOVE, s, d, one, 1, 1LL << 20, sk, 1, &j, &steps, &maxs, &nerr, &end) == 0 && end == FM_J_DONE, "K skip run");
    fm_job_free(&j);
    P(a, s, "mt/x.txt"); CHECK(exists(a), "K a skipped file stays in the source");
    P(a, s, "mt"); CHECK(is_dir(a), "K ... and so does its folder (no data lost)");

    /* across filesystems: /dev/shm is tmpfs when it exists and is not where TMP is */
    struct stat s1, s2;
    char shm[] = "/dev/shm/fm_test_XXXXXX";
    if (stat(TMP, &s1) == 0 && mkdtemp(shm) && stat(shm, &s2) == 0 && s1.st_dev != s2.st_dev) {
        P(a, s, "x.bin"); put(a, 200000, 3);
        P(a, s, "xt"); mk(a);
        P(a, s, "xt/q.txt"); put(a, 777, 4);
        const char *xn[] = { "x.bin", "xt" };
        CHECK(run(FM_OP_MOVE, s, shm, xn, 2, 50000, NULL, 0, &j, &steps, &maxs, &nerr, &end) == 0 && end == FM_J_DONE && nerr == 0,
              "K cross-filesystem move (state %d, errors %d)", (int)end, nerr);
        CHECK(steps > 3 && maxs <= 50000, "K ... done in bounded steps (%d steps, max %lld)", steps, maxs);
        fm_job_free(&j);
        P(a, s, "x.bin"); P(b, shm, "x.bin");
        CHECK(!exists(a) && stat(b, &sb) == 0 && sb.st_size == 200000, "K cross-fs file moved");
        P(a, s, "xt"); P(b, shm, "xt/q.txt");
        CHECK(!exists(a) && stat(b, &sb) == 0 && sb.st_size == 777, "K cross-fs folder moved and the source removed");
        rmtree(shm);
    } else {
        printf("note: no second filesystem at /dev/shm; the cross-filesystem move is not exercised\n");
        if (shm[0] && strcmp(shm, "/dev/shm/fm_test_XXXXXX")) rmtree(shm);
    }
    (void)len;
}

/* ── L ───────────────────────────────────────────────────────────────────── */

static void group_l(void) {
    char s[FM_PATH_MAX], a[FM_PATH_MAX], b[FM_PATH_MAX];
    P(s, TMP, "ls"); mk(s);
    P(a, s, "keep"); mk(a);
    P(a, s, "keep/precious.txt"); put(a, 10, 1);
    P(a, s, "del"); mk(a);
    P(a, s, "del/a.txt"); put(a, 10, 1);
    P(a, s, "del/sub"); mk(a);
    P(a, s, "del/sub/b.txt"); put(a, 10, 1);
    P(a, s, "del/sub/deeper"); mk(a);
    P(a, s, "del/link"); P(b, s, "keep"); (void)!symlink(b, a);
    P(a, s, "gone.txt"); put(a, 3, 1);
    const char *nm[] = { "del", "gone.txt" };
    FmJob j; FmJobState end; int steps, nerr; long long maxs;
    CHECK(run(FM_OP_DELETE, s, NULL, nm, 2, 4096, NULL, 0, &j, &steps, &maxs, &nerr, &end) == 0 && end == FM_J_DONE && nerr == 0,
          "L delete (state %d, errors %d)", (int)end, nerr);
    fm_job_free(&j);
    P(a, s, "del"); P(b, s, "gone.txt");
    CHECK(!exists(a) && !exists(b), "L folder and file are gone");
    P(a, s, "keep/precious.txt");
    CHECK(exists(a), "L the folder a link pointed at was NOT touched");
}

/* ── M ───────────────────────────────────────────────────────────────────── */

static void group_m(void) {
    char s[FM_PATH_MAX], d[FM_PATH_MAX], a[FM_PATH_MAX], b[FM_PATH_MAX], err[200];
    P(s, TMP, "ms"); P(d, TMP, "md"); mk(s); mk(d);
    P(a, s, "big"); put(a, 400000, 2);
    const char *nm[] = { "big" };
    FmJob j;
    CHECK(fm_job_init(&j, FM_OP_COPY, s, d, nm, 1, err, sizeof err) == 0, "M init");
    int guard = 0;
    while (j.bytes_done < 100000 && guard++ < 1000) fm_job_step(&j, 30000);
    P(b, d, "big");
    CHECK(exists(b), "M the target exists while copying");
    CHECK(fm_job_current(&j) && strstr(fm_job_current(&j), "big"), "M current path names the file");
    fm_job_cancel(&j);
    CHECK(j.state == FM_J_CANCELLED, "M state is cancelled");
    CHECK(!exists(b), "M the half-written target was removed");
    P(a, s, "big"); CHECK(exists(a), "M the source is untouched");
    CHECK(fm_job_step(&j, 1 << 20) == FM_J_CANCELLED, "M stepping a cancelled job does nothing");
    fm_job_free(&j);
}

/* ── N ───────────────────────────────────────────────────────────────────── */

static void group_n(void) {
    char s[FM_PATH_MAX], d[FM_PATH_MAX], a[FM_PATH_MAX], b[FM_PATH_MAX], err[200];
    P(s, TMP, "ns"); P(d, TMP, "nd"); mk(s); mk(d);
    P(a, s, "toobig"); put(a, 200000, 2);
    P(a, s, "small"); put(a, 1000, 3);
    const char *nm[] = { "toobig", "small" };
    FmJob j;
    struct rlimit old, lim;
    getrlimit(RLIMIT_FSIZE, &old);
    signal(SIGXFSZ, SIG_IGN);
    lim = old; lim.rlim_cur = 50000;
    CHECK(fm_job_init(&j, FM_OP_COPY, s, d, nm, 2, err, sizeof err) == 0, "N init");
    setrlimit(RLIMIT_FSIZE, &lim);
    FmJobState st = FM_J_RUN;
    for (int g = 0; g < 100 && st == FM_J_RUN; g++) st = fm_job_step(&j, 30000);
    setrlimit(RLIMIT_FSIZE, &old);
    CHECK(st == FM_J_ERROR, "N a failing write stops the job with an error (state %d)", (int)st);
    CHECK(j.err_fatal, "N a full-disk style error is fatal (errno %d)", j.err_no);
    CHECK(strstr(j.err_msg, "toobig") != NULL, "N the message names the file: '%s'", j.err_msg);
    P(b, d, "toobig");
    CHECK(!exists(b), "N no partial file is left");
    CHECK(j.errors == 1, "N one error counted");
    fm_job_ack_error(&j, true);
    CHECK(j.state == FM_J_CANCELLED, "N cancel from the error dialog");
    fm_job_free(&j);

    /* continue-after-error: the second item still copies */
    CHECK(fm_job_init(&j, FM_OP_COPY, s, d, nm, 2, err, sizeof err) == 0, "N init 2");
    setrlimit(RLIMIT_FSIZE, &lim);
    int nerr = 0;
    FmJobState end = drive(&j, 30000, NULL, 0, NULL, NULL, &nerr);
    setrlimit(RLIMIT_FSIZE, &old);
    CHECK(end == FM_J_DONE && nerr == 1, "N skipping the failed item lets the job finish (state %d, errors %d)", (int)end, nerr);
    P(a, s, "small"); P(b, d, "small");
    CHECK(same_content(a, b, NULL), "N the item after the failure was copied");
    fm_job_free(&j);
}

/* ── O: expert mode's pure pieces (mode text, long detail, link target,
 *      pseudo and protected paths) ──────────────────────────────────────── */

static void group_o(void) {
    char m[11], o[128];

    /* mode strings */
    fm_mode_string(S_IFREG | 0644, m); CHECK(!strcmp(m, "-rw-r--r--"), "O mode 0644 file -> '%s'", m);
    fm_mode_string(S_IFDIR | 0755, m); CHECK(!strcmp(m, "drwxr-xr-x"), "O mode 0755 dir -> '%s'", m);
    fm_mode_string(S_IFLNK | 0777, m); CHECK(!strcmp(m, "lrwxrwxrwx"), "O mode symlink -> '%s'", m);
    fm_mode_string(S_IFREG | 04755, m); CHECK(!strcmp(m, "-rwsr-xr-x"), "O setuid with x is 's' -> '%s'", m);
    fm_mode_string(S_IFREG | 02755, m); CHECK(!strcmp(m, "-rwxr-sr-x"), "O setgid with x is 's' -> '%s'", m);
    fm_mode_string(S_IFREG | 04644, m); CHECK(!strcmp(m, "-rwSr--r--"), "O setuid without x is 'S' -> '%s'", m);
    fm_mode_string(S_IFDIR | 01777, m); CHECK(!strcmp(m, "drwxrwxrwt"), "O sticky with x is 't' -> '%s'", m);
    fm_mode_string(S_IFDIR | 01776, m); CHECK(!strcmp(m, "drwxrwxrwT"), "O sticky without x is 'T' -> '%s'", m);
    fm_mode_string(S_IFCHR | 0600, m); CHECK(!strcmp(m, "crw-------"), "O char device -> '%s'", m);
    fm_mode_string(S_IFBLK | 0600, m); CHECK(m[0] == 'b', "O block device type 'b' -> '%s'", m);
    fm_mode_string(S_IFIFO | 0600, m); CHECK(m[0] == 'p', "O fifo type 'p' -> '%s'", m);
    fm_mode_string(S_IFSOCK | 0600, m); CHECK(m[0] == 's', "O socket type 's' -> '%s'", m);
    fm_mode_string(0, m); CHECK(!strcmp(m, "?---------"), "O unknown type '?' -> '%s'", m);

    /* detail_long, in a fixed zone so the clock text is fixed */
    setenv("TZ", "UTC", 1); tzset();
    struct tm tmv;
    memset(&tmv, 0, sizeof tmv);
    tmv.tm_year = 126; tmv.tm_mon = 9; tmv.tm_mday = 18; tmv.tm_hour = 14; tmv.tm_min = 8;
    long long when = (long long)timegm(&tmv);
    FmEntry fe = { "f", FM_T_FILE, 1536, when, S_IFREG | 0644, false };
    fm_entry_detail_long(&fe, o, sizeof o);
    CHECK(!strcmp(o, "-rw-r--r-- 1.5K 2026-10-18 14:08"), "O detail_long file -> '%s'", o);
    FmEntry fd = { "d", FM_T_DIR, 4096, when, S_IFDIR | 0755, false };
    fm_entry_detail_long(&fd, o, sizeof o);
    CHECK(!strcmp(o, "drwxr-xr-x <DIR> 2026-10-18 14:08"), "O detail_long folder -> '%s'", o);
    FmEntry fl = { "l", FM_T_LINK, 7, when, S_IFLNK | 0777, true };
    fm_entry_detail_long(&fl, o, sizeof o);
    CHECK(!strcmp(o, "lrwxrwxrwx <LNK> 2026-10-18 14:08"), "O detail_long link -> '%s'", o);
    setenv("TZ", "UTC-2", 1); tzset();          /* UTC+2: the clock text follows the zone */
    fm_entry_detail_long(&fe, o, sizeof o);
    CHECK(!strcmp(o, "-rw-r--r-- 1.5K 2026-10-18 16:08"), "O detail_long uses localtime -> '%s'", o);
    setenv("TZ", "UTC", 1); tzset();
    /* truncation: always NUL-terminated, never overrun */
    char small[10];
    memset(small, 'Z', sizeof small);
    fm_entry_detail_long(&fe, small, sizeof small);
    CHECK(small[sizeof small - 1] == '\0' && !strncmp(small, "-rw-r--r-", 9),
          "O detail_long truncated at 10 is NUL-terminated -> '%s'", small);
    fm_entry_detail_long(&fe, small, 0);       /* must not write or crash */
    memset(small, 'Z', sizeof small);
    fm_entry_detail_long(&fe, small, 0);
    CHECK(small[0] == 'Z', "O detail_long with n=0 writes nothing");

    /* link target: a symlink to a folder, to a file, dangling, and too small */
    char a[FM_PATH_MAX], l[FM_PATH_MAX], f[FM_PATH_MAX], lf[FM_PATH_MAX], dead[FM_PATH_MAX], ld[FM_PATH_MAX];
    P(a, TMP, "lt_dir"); mk(a);
    P(l, TMP, "lt_link_dir"); (void)!symlink(a, l);
    P(f, TMP, "lt_file"); put(f, 10, 3);
    P(lf, TMP, "lt_link_file"); (void)!symlink(f, lf);
    P(dead, TMP, "nowhere");
    P(ld, TMP, "lt_link_dead"); (void)!symlink(dead, ld);
    char canon[FM_PATH_MAX];
    char *cr = realpath(a, NULL);
    CHECK(fm_link_dir_target(l, o, sizeof o) == 0 && cr && !strcmp(o, cr),
          "O link to folder: 0 and canonical '%s' (got '%s')", cr ? cr : "?", o);
    if (cr) free(cr);
    int rc = fm_link_dir_target(lf, canon, sizeof canon);
    CHECK(rc == -ENOTDIR, "O link to a file -> -ENOTDIR (got %d)", rc);
    rc = fm_link_dir_target(ld, canon, sizeof canon);
    CHECK(rc < 0, "O dangling link -> negative (got %d)", rc);
    rc = fm_link_dir_target(l, canon, 3);
    CHECK(rc == -ENAMETOOLONG, "O target that does not fit -> -ENAMETOOLONG (got %d)", rc);

    /* fm_dir_read fills mode and link_dir */
    char dir[FM_PATH_MAX];
    P(dir, TMP, "expdir"); mk(dir);
    char p1[FM_PATH_MAX];
    P(p1, dir, "sub"); mk(p1);
    P(p1, dir, "plain.txt"); put(p1, 20, 4);
    P(p1, dir, "suid.bin"); put(p1, 20, 5); chmod(p1, 04755);
    P(p1, dir, "lsub"); (void)!symlink("sub", p1);
    P(p1, dir, "lplain"); (void)!symlink("plain.txt", p1);
    P(p1, dir, "ldead"); (void)!symlink("missing", p1);
    FmDir d;
    CHECK(fm_dir_read(dir, FM_SORT_NAME, &d) == 0, "O dir_read ok");
    int i;
    i = fm_dir_find(&d, "lsub");
    CHECK(i >= 0 && d.e[i].type == FM_T_LINK && d.e[i].link_dir, "O link to folder: link_dir true");
    fm_mode_string(i >= 0 ? d.e[i].mode : 0, m);
    CHECK(i >= 0 && m[0] == 'l', "O link mode from lstat is 'l' (got '%s')", m);
    i = fm_dir_find(&d, "lplain");
    CHECK(i >= 0 && d.e[i].type == FM_T_LINK && !d.e[i].link_dir, "O link to a file: link_dir false");
    i = fm_dir_find(&d, "ldead");
    CHECK(i >= 0 && d.e[i].type == FM_T_LINK && !d.e[i].link_dir, "O dangling link: link_dir false");
    i = fm_dir_find(&d, "sub");
    CHECK(i >= 0 && d.e[i].type == FM_T_DIR && !d.e[i].link_dir, "O real folder: link_dir false");
    CHECK(i >= 0 && (d.e[i].mode & S_IFMT) == S_IFDIR, "O real folder mode is a directory");
    i = fm_dir_find(&d, "suid.bin");
    fm_mode_string(i >= 0 ? d.e[i].mode : 0, m);
    CHECK(i >= 0 && !strcmp(m, "-rwsr-xr-x"), "O dir_read setuid file mode -> '%s'", m);
    i = fm_dir_find(&d, "plain.txt");
    CHECK(i >= 0 && (d.e[i].mode & S_IFMT) == S_IFREG && !d.e[i].link_dir, "O plain file mode is a regular file");
    fm_dir_free(&d);

    /* pseudo filesystems: whole components, normalised first */
    struct { const char *p; bool want; } pse[] = {
        {"/proc", true}, {"/proc/1", true}, {"/sys/class", true}, {"/dev", true},
        {"/dev/input/event0", true}, {"/proc/../etc", false}, {"/devices", false},
        {"/devx", false}, {"/home/root/data", false}, {"/", false},
    };
    for (size_t k = 0; k < sizeof pse / sizeof pse[0]; k++)
        CHECK(fm_path_pseudo(pse[k].p) == pse[k].want, "O pseudo '%s' want %d", pse[k].p, (int)pse[k].want);

    /* protected: the root, its direct children, pseudo, and anything unparsable */
    struct { const char *p; bool want; } prot[] = {
        {"/", true}, {"/etc", true}, {"/media", true}, {"/etc/passwd", false},
        {"/proc/1", true}, {"/media/usb0", false}, {"/home/root/data", false},
        {"", true}, {"garbage", true}, {"/a/../..", true}, {"//etc//", true},
        {"/devices", true},            /* a direct child of / */
    };
    for (size_t k = 0; k < sizeof prot / sizeof prot[0]; k++)
        CHECK(fm_path_protected(prot[k].p) == prot[k].want, "O protected '%s' want %d", prot[k].p, (int)prot[k].want);
    unsetenv("TZ"); tzset();
}


/* ── P: OPEN ─────────────────────────────────────────────────────────────── */
static void group_p(void) {
    const char *txt[] = { "a.txt", "A.TXT", "notes.md", "os-release", "fstab", ".profile", "x.conf", "run.sh", "m.json" };
    for (size_t k = 0; k < sizeof txt / sizeof txt[0]; k++) CHECK(fm_name_is_text(txt[k]), "P text '%s'", txt[k]);
    const char *bin[] = { "a.bin", "pic.jpg", "x.ko", "game.zip", "a.png", "v.mp4" };
    for (size_t k = 0; k < sizeof bin / sizeof bin[0]; k++) CHECK(!fm_name_is_text(bin[k]), "P not text '%s'", bin[k]);
    CHECK(fm_name_is_image("a.jpg") && fm_name_is_image("B.JPEG") && fm_name_is_image("c.Png"), "P image names");
    CHECK(!fm_name_is_image("a.gif") && !fm_name_is_image("jpg") && !fm_name_is_image("a.txt"), "P not image names");

    unsigned char t[8] = "hello", z[8] = { 'a', 0, 'b' };
    CHECK(fm_sniff_text(t, 5, 5), "P sniff text");
    CHECK(fm_sniff_text(t, 0, 0), "P sniff empty");
    CHECK(!fm_sniff_text(z, 3, 3), "P sniff NUL");
    CHECK(!fm_sniff_text(t, 5, FM_TEXT_MAX), "P sniff too big");
    CHECK(fm_sniff_text(t, 5, FM_TEXT_MAX - 1), "P sniff just under");

    const char *media[] = { "/media/usb0" };
    CHECK(fm_open_editable("/home/root/data/a.txt", media, 1), "P edit data");
    CHECK(fm_open_editable("/home/root/data/x/y.txt", NULL, 0), "P edit data deep");
    CHECK(fm_open_editable("/media/usb0/a.txt", media, 1), "P edit media");
    CHECK(!fm_open_editable("/media/usb01/a.txt", media, 1), "P media prefix only");
    CHECK(!fm_open_editable("/home/root/database/a", media, 1), "P data prefix only");
    CHECK(!fm_open_editable("/etc/os-release", media, 1), "P view etc");

    FmOpenCmd c;
    CHECK(fm_open_cmd(&c, false, true, "/dev/fb0", "/dev/input/touchscreen0", "/home/root/data/a.txt"), "P cmd ok");
    const char *want_e[] = { "/opt/games/terminal", "/dev/fb0", "/dev/input/touchscreen0", "--cwd", "/home/root/data",
                             "--exec", "nano", "--", "/home/root/data/a.txt" };
    CHECK(c.ac == 9 && c.av[9] == NULL, "P edit argc %d", c.ac);
    for (int i = 0; i < 9 && i < c.ac; i++) CHECK(!strcmp(c.av[i], want_e[i]), "P edit av[%d]=%s", i, c.av[i]);
    CHECK(fm_open_cmd(&c, false, false, "/dev/fb0", "/dev/input/touchscreen0", "/etc/os-release"), "P view ok");
    CHECK(c.ac == 10 && !strcmp(c.av[7], "-v") && !strcmp(c.av[8], "--") && !strcmp(c.av[9], "/etc/os-release") &&
          !strcmp(c.av[4], "/etc"), "P view argv");
    CHECK(fm_open_cmd(&c, true, false, "/dev/fb0", "/t", "/home/root/data/pictures/a.jpg"), "P img ok");
    CHECK(c.ac == 7 && !strcmp(c.av[0], "/opt/games/frame") && !strcmp(c.av[5], "--show") &&
          !strcmp(c.av[6], "/home/root/data/pictures/a.jpg"), "P img argv");
    CHECK(!fm_open_cmd(&c, false, true, "/dev/fb0", "/t", "relative.txt"), "P relative refused");

    /* EDIT: writable nano on any location; text-like regular files only, never a picture or a non-file. */
    CHECK(fm_edit_ok(FM_T_FILE, "notes.txt"), "P editok text");
    CHECK(fm_edit_ok(FM_T_FILE, "Makefile"), "P editok no extension");
    CHECK(fm_edit_ok(FM_T_FILE, "4D-Sports.zip.opt"), "P editok any extension");
    CHECK(fm_edit_ok(FM_T_FILE, "app.bin"), "P editok binary name allowed");
    CHECK(!fm_edit_ok(FM_T_DIR, "docs"), "P editok dir refused");
    CHECK(!fm_edit_ok(FM_T_LINK, "l.txt"), "P editok link refused");
    CHECK(!fm_edit_ok(FM_T_OTHER, "dev.txt"), "P editok other refused");
    CHECK(fm_open_cmd(&c, false, true, "/dev/fb0", "/t", "/opt/games/arca_config.conf"), "P edit opt ok");
    CHECK(c.ac == 9 && !strcmp(c.av[6], "nano") && !strcmp(c.av[7], "--") && !strcmp(c.av[8], "/opt/games/arca_config.conf") &&
          !strcmp(c.av[4], "/opt/games"), "P edit opt argv has no -v");
}
/* ── main ────────────────────────────────────────────────────────────────── */

int main(void) {
    snprintf(TMP, sizeof TMP, "/tmp/fm_test_XXXXXX");
    if (!mkdtemp(TMP)) { printf("FAIL: cannot make a temp directory\n"); return 1; }

    group_a(); group_b(); group_c(); group_d(); group_e(); group_f();
    group_g(); group_h(); group_i(); group_j(); group_k(); group_l();
    group_m(); group_n(); group_o(); group_p();

    rmtree(TMP);
    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
