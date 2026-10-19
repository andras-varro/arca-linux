/* usb_watch.h — has a USB disk appeared or gone?  One readdir of /dev, no fork,
 * cheap enough for once a second.  Shared by file_manager and control_panel's
 * Backup page, which both mount (`arca-storage mount`) only when it changes.
 * usb_is_disk_name() is pure; host test tests/usb_watch_test.c.
 */
#ifndef USB_WATCH_H
#define USB_WATCH_H

#include <stdbool.h>

/* Is this a /dev entry that is a USB/SCSI disk or one of its partitions:
 * "sd" + 1-2 letters + optional digits ("sda", "sdb2")?  Not "sd", "sda1x",
 * "sdcard", "mmcblk0". */
bool usb_is_disk_name(const char *name);

/* A number for the disk nodes in /dev: 0 = none, and it changes when one
 * appears, goes or is replaced by another. */
unsigned usb_disk_sig(void);

#endif
