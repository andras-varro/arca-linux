# Package makefiles of this external tree would be included here; there are none yet.
#
# make legal-info aborts on the stock linux-headers package with a 4.14.52 header tree:
# its hash file carries the licence hashes of the 6.x trees, and its licence list names
# LICENSES/ files that arrived in 4.16. This file is included after every package/*/*.mk,
# so the two variables below replace the package's own. A hash file under
# BR2_GLOBAL_PATCH_DIR does not work: it is checked together with the package's own, and
# the first mismatching entry is fatal.
LINUX_HEADERS_HASH_FILES = $(BR2_EXTERNAL_ARCA_PATH)/package-hashes/linux-headers.hash
LINUX_HEADERS_LICENSE_FILES = COPYING
