#!/bin/bash
#
# product.sh — the product name for host scripts: PRODUCT_NAME and PRODUCT_STEM.
#
# Sourced, never executed:   . "$SCRIPT_DIR/lib/product.sh"
#
# The name is DATA in one repo file, rootfs/board/arca/os-release, which the rootfs build
# installs as the device's /etc/os-release (rootfs/board/arca/post-build.sh). The device's C
# code reads the installed copy (native_apps/common/product.c); this file reads the repo copy,
# so a banner, a release title and a download name cannot disagree with the device.
#
#   PRODUCT_NAME   the NAME= value, quotes stripped          Ludbox Linux
#   PRODUCT_STEM   its first word, lowercase, [a-z0-9] only  ludbox   (file names, tags)
#
# Unlike the C side there is NO fallback: a host script that cannot read the name would put a
# wrong one into a file name or a release title, so this returns 1 with a message instead
# (a caller under `set -e` stops). The file is parsed, never sourced, so its content cannot
# run anything.

_product_file="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/rootfs/board/arca/os-release"

PRODUCT_NAME=""
PRODUCT_STEM=""
if [ -r "$_product_file" ]; then
    PRODUCT_NAME="$(sed -n 's/^NAME=//p' "$_product_file" | head -n 1 | tr -d '\r')"
    # One pair of matching quotes, as product.c strips it.
    case "$PRODUCT_NAME" in
        \"*\") PRODUCT_NAME="${PRODUCT_NAME#\"}"; PRODUCT_NAME="${PRODUCT_NAME%\"}" ;;
        \'*\') PRODUCT_NAME="${PRODUCT_NAME#\'}"; PRODUCT_NAME="${PRODUCT_NAME%\'}" ;;
    esac
    PRODUCT_STEM="$(printf '%s' "${PRODUCT_NAME%% *}" | tr '[:upper:]' '[:lower:]' | tr -cd 'a-z0-9')"
fi
if [ -z "$PRODUCT_NAME" ] || [ -z "$PRODUCT_STEM" ]; then
    echo "product.sh: no usable NAME= in $_product_file" >&2
    unset _product_file
    PRODUCT_NAME="" PRODUCT_STEM=""
    return 1
fi
unset _product_file
export PRODUCT_NAME PRODUCT_STEM
