/* Host regression for common/usb_watch.c, the /dev disk-node watcher shared by
 * file_manager and the Backup page.
 *
 *   cd native_apps && gcc -Wall -Wextra -I. -o build/usb_watch_test \
 *       tests/usb_watch_test.c common/usb_watch.c && ./build/usb_watch_test
 *
 * A  which /dev names count as a disk;
 * B  the signature is stable between two reads.
 * It cannot see a node appearing: that needs a device (or a changed /dev).
 */
#include "common/usb_watch.h"

#include <stdio.h>

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)

int main(void) {
    /* A */
    CHECK(usb_is_disk_name("sda") && usb_is_disk_name("sda1") && usb_is_disk_name("sdb12"));
    CHECK(usb_is_disk_name("sdaa"));
    CHECK(!usb_is_disk_name("sd") && !usb_is_disk_name("sd1") && !usb_is_disk_name("sda1x"));
    CHECK(!usb_is_disk_name("sdcard") && !usb_is_disk_name("mmcblk0") && !usb_is_disk_name("") && !usb_is_disk_name(NULL));
    /* B */
    CHECK(usb_disk_sig() == usb_disk_sig());
    printf("usb_watch_test: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
