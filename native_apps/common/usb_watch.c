/* usb_watch.c — see usb_watch.h. */
#include "usb_watch.h"

#include <dirent.h>
#include <stddef.h>

bool usb_is_disk_name(const char *name) {
    if (!name || name[0] != 's' || name[1] != 'd') return false;
    const char *p = name + 2;
    int letters = 0;
    while (*p >= 'a' && *p <= 'z') { p++; letters++; }
    if (letters < 1 || letters > 2) return false;
    while (*p >= '0' && *p <= '9') p++;
    return *p == '\0';
}

unsigned usb_disk_sig(void) {
    DIR *d = opendir("/dev");
    if (!d) return 0;
    unsigned sig = 0;
    for (struct dirent *e; (e = readdir(d)) != NULL; ) {
        if (!usb_is_disk_name(e->d_name)) continue;
        unsigned h = 5381;
        for (const char *p = e->d_name; *p; p++) h = h * 33 + (unsigned char)*p;
        sig += h | 1;          /* a sum: the directory order does not matter */
    }
    closedir(d);
    return sig;
}
