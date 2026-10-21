#!/bin/bash
# Stage the OpenSSL headers and the two shared libraries to LINK against, from the
# Buildroot tree that built the device's own root filesystem.  Nothing here is
# deployed: the device ships /usr/lib/libssl.so.3 and libcrypto.so.3 (OpenSSH needs
# them), and those are the runtime.  Used by common/http.c (TLS) and so by the
# jellyfin binary only.
#
# Usage:
#   ./build-openssl-deps.sh                  # stage into arm-deps-softfp/ unless already there
#   ./build-openssl-deps.sh --force          # restage
#   ./build-openssl-deps.sh --prefix         # print the sysroot path, copy nothing
#   ./build-openssl-deps.sh --from <dir>     # Buildroot staging dir (default $BR_STAGING,
#                                            # else ~/br-rw/staging)
#
# Output: arm-deps-softfp/usr/include/openssl/*.h and arm-deps-softfp/usr/lib/
# libssl.so.3, libcrypto.so.3, plus libssl.so / libcrypto.so.  The unversioned names
# are COPIES, not symlinks (a DrvFs checkout cannot hold symlinks); the linker reads
# the soname out of each, so -lssl -lcrypto records DT_NEEDED libssl.so.3 and
# libcrypto.so.3.  The static .a files are never copied: linking them would put
# 4 MB of OpenSSL into the binary and bypass the device's copy.
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PREFIX="$SCRIPT_DIR/arm-deps-softfp"
STAGING="${BR_STAGING:-$HOME/br-rw/staging}"

while [ $# -gt 0 ]; do
    case "$1" in
        --prefix) echo "$PREFIX"; exit 0 ;;
        --force)  rm -rf "${PREFIX:?}/usr/include/openssl" "$PREFIX/usr/lib/libssl.so" \
                         "$PREFIX/usr/lib/libssl.so.3" "$PREFIX/usr/lib/libcrypto.so" \
                         "$PREFIX/usr/lib/libcrypto.so.3"; shift ;;
        --from)   STAGING="${2:?--from needs a directory}"; shift 2 ;;
        -h|--help) sed -n '2,/^set -e/p' "$0" | sed '$d;s/^# \{0,1\}//'; exit 0 ;;
        *) echo "Usage: $0 [--force | --prefix | --from <staging dir> | --help]"; exit 1 ;;
    esac
done

# The artifacts, not a stamp.
if [ -f "$PREFIX/usr/lib/libssl.so" ] && [ -f "$PREFIX/usr/lib/libssl.so.3" ] \
   && [ -f "$PREFIX/usr/lib/libcrypto.so" ] && [ -f "$PREFIX/usr/lib/libcrypto.so.3" ] \
   && [ -f "$PREFIX/usr/include/openssl/ssl.h" ]; then
    echo "OpenSSL link files already staged ($PREFIX/usr/lib/libssl.so)"
    exit 0
fi

for f in usr/include/openssl/ssl.h usr/lib/libssl.so.3 usr/lib/libcrypto.so.3; do
    if [ ! -f "$STAGING/$f" ]; then
        echo "$STAGING/$f not found."
        echo "That is the Buildroot staging dir of the root filesystem build:"
        echo "run rootfs/build-rootfs.sh (it builds ~/br-rw), or point BR_STAGING / --from at one."
        exit 1
    fi
done

mkdir -p "$PREFIX/usr/include" "$PREFIX/usr/lib"
rm -rf "$PREFIX/usr/include/openssl"
cp -rL "$STAGING/usr/include/openssl" "$PREFIX/usr/include/openssl"
for lib in libssl libcrypto; do
    cp -L "$STAGING/usr/lib/$lib.so.3" "$PREFIX/usr/lib/$lib.so.3"
    cp -L "$STAGING/usr/lib/$lib.so.3" "$PREFIX/usr/lib/$lib.so"
done
echo "OpenSSL staged from $STAGING into $PREFIX"
