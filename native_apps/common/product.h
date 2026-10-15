/**
 * product.h — the product name and image version, read from the device's /etc/os-release.
 *
 * The name is data in ONE repo file, rootfs/board/arca/os-release, which the
 * rootfs build installs as /etc/os-release.  Nothing in C spells it: a title,
 * banner or config header asks product_name().  The host-side twin is
 * lib/product.sh.
 */
#ifndef PRODUCT_H
#define PRODUCT_H

/* What product_name() returns when NAME= cannot be read.  Deliberately not the
 * product's name: a build that cannot read its own name must not carry a
 * second copy of it. */
#define PRODUCT_NAME_FALLBACK "Linux"

/* The NAME= value of /etc/os-release, quotes stripped, read once and kept.
 * Never NULL, never empty.  The pointer stays valid for the process. */
const char *product_name(void);

/* The same parse of an arbitrary os-release file, re-read on every call (the
 * test seam).  Returns PRODUCT_NAME_FALLBACK when the file is unreadable, has
 * no NAME= line, or its NAME is empty.  The result lives in a static buffer
 * that the next call overwrites. */
const char *product_name_from(const char *path);

/* The VERSION_ID= value of /etc/os-release (the image's own version, appended by
 * rootfs/board/arca/post-build.sh from `git describe`), quotes stripped, read once
 * and kept.  NULL when the file has no VERSION_ID= line or its value is empty: a
 * caller omits what it cannot show.  The pointer stays valid for the process. */
const char *product_version(void);

/* The same parse of an arbitrary os-release file, re-read on every call (the test
 * seam).  NULL when the file is unreadable, has no VERSION_ID= line, or the value is
 * empty.  The result lives in a static buffer that the next call overwrites. */
const char *product_version_from(const char *path);

#endif
