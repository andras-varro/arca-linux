/* Host regression for control_panel/backup_parse.c, the reader of the lines
 * device-files/arca-backup prints (the BACKUP page's pure half).
 *
 *   cd native_apps && gcc -Wall -Wextra -I. -o build/backup_parse_test \
 *       tests/backup_parse_test.c control_panel/backup_parse.c && ./build/backup_parse_test
 *
 * A  item lines: flags, names with spaces, and every malformed shape refused;
 * B  archive / info / unknown / imported / done lines;
 * C  an id or file name that could be an option or a path is refused;
 * D  the last "error" line of a failed run;
 * E  which items start marked (secret never, unit-bound only on the same unit).
 * It cannot see the page: drawing, focus and the engine itself.
 */
#include "control_panel/backup_parse.h"

#include <stdio.h>
#include <string.h>

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)

int main(void) {
    BkItem it;
    /* A */
    CHECK(bk_parse_item("item scores unit=no secret=no reboot=no Game scores and settings\n", &it));
    CHECK(!strcmp(it.id, "scores") && !it.unit && !it.secret && !it.reboot);
    CHECK(!strcmp(it.name, "Game scores and settings"));
    CHECK(bk_parse_item("item ssh-keys unit=yes secret=yes reboot=yes SSH keys", &it));
    CHECK(it.unit && it.secret && it.reboot && !strcmp(it.name, "SSH keys"));
    CHECK(bk_parse_item("item a unit=no secret=no reboot=no", &it) && !strcmp(it.name, "a"));
    CHECK(!bk_parse_item("item a unit=maybe secret=no reboot=no x", &it));
    CHECK(!bk_parse_item("item a secret=no unit=no reboot=no x", &it));
    CHECK(!bk_parse_item("item a unit=no secret=no x", &it));
    CHECK(!bk_parse_item("items a unit=no secret=no reboot=no x", &it));
    CHECK(!bk_parse_item("item ../x unit=no secret=no reboot=no x", &it));
    CHECK(!bk_parse_item("", &it) && !bk_parse_item(NULL, &it));
    CHECK(bk_is_disk_name("sda") && bk_is_disk_name("sda1") && bk_is_disk_name("sdb12"));
    CHECK(bk_is_disk_name("sdaa"));
    CHECK(!bk_is_disk_name("sd") && !bk_is_disk_name("sd1") && !bk_is_disk_name("sda1x"));
    CHECK(!bk_is_disk_name("sdcard") && !bk_is_disk_name("mmcblk0") && !bk_is_disk_name("") && !bk_is_disk_name(NULL));
    /* B */
    char a[64], h[32], v[32], d[32];
    CHECK(bk_parse_archive("archive ludbox-backup-arca-1a2b-2026-10-18.tar.gz\n", a, sizeof a));
    CHECK(!strcmp(a, "ludbox-backup-arca-1a2b-2026-10-18.tar.gz"));
    CHECK(!bk_parse_archive("archive a/b.tgz", a, sizeof a));
    CHECK(!bk_parse_archive("archive averyverylongname.tar.gz", a, 8));
    CHECK(bk_parse_info("info host=arca-1a2b version=1.2 date=2026-10-18", h, sizeof h, v, sizeof v, d, sizeof d));
    CHECK(!strcmp(h, "arca-1a2b") && !strcmp(v, "1.2") && !strcmp(d, "2026-10-18"));
    CHECK(!bk_parse_info("info host=x date=y version=z", h, sizeof h, v, sizeof v, d, sizeof d));
    CHECK(!bk_parse_info("info host=x version=1", h, sizeof h, v, sizeof v, d, sizeof d));
    CHECK(bk_parse_unknown("unknown wifi", a, sizeof a) && !strcmp(a, "wifi"));
    CHECK(bk_parse_imported("imported scores\n", a, sizeof a) && !strcmp(a, "scores"));
    CHECK(!bk_parse_imported("imported", a, sizeof a));
    bool rb = false;
    CHECK(bk_parse_done("done reboot=yes", &rb) && rb);
    CHECK(bk_parse_done("done reboot=no\n", &rb) && !rb);
    CHECK(!bk_parse_done("done reboot=2", &rb));
    CHECK(!bk_parse_done("done", &rb));
    /* C */
    CHECK(bk_name_ok("scores") && bk_name_ok("a.b_c-d"));
    CHECK(!bk_name_ok("") && !bk_name_ok("-rf") && !bk_name_ok(".") && !bk_name_ok("..") &&
          !bk_name_ok("a b") && !bk_name_ok("a/b") && !bk_name_ok(NULL));
    /* D */
    char e[40];
    CHECK(bk_last_error("exported x\nerror first\nerror stick is full\n", e, sizeof e) && !strcmp(e, "stick is full"));
    CHECK(!bk_last_error("exported x\n", e, sizeof e));
    CHECK(bk_last_error("error a very long reason that does not fit in the buffer at all", e, 10) && strlen(e) == 9);
    /* E */
    BkItem plain = { "p", "P", false, false, false }, sec = { "s", "S", false, true, false },
           unit = { "u", "U", true, false, false };
    CHECK(bk_start_marked(&plain, false) && bk_start_marked(&plain, true));
    CHECK(!bk_start_marked(&sec, false) && !bk_start_marked(&sec, true));
    CHECK(bk_start_marked(&unit, false) && !bk_start_marked(&unit, true));

    printf("backup_parse_test: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
