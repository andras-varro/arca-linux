#!/bin/bash
# build-rootfs.sh - Build the Arca root filesystem tarball with Buildroot.
# Run this script in WSL (Linux). Source, downloads and build tree all live on WSL's
# native filesystem (~), never under /mnt: a Buildroot build on DrvFs is many times
# slower, and build output must not land in the repository.
#
# Prerequisites: a normal Buildroot host set (gcc, g++, make, patch, perl, python3,
# cpio, unzip, rsync, bc, file, wget, libncurses-dev). The first run takes about an hour.
#
# Produces: ~/br-rw-out/rootfs.tar. Nothing here writes a device or a card.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

BR_VERSION="2025.02.18"
BR_SHA256="0620c699b3a1da41cc854bc8584692b913e57e284d3328121c674a43b8707ab1"
BR_URL="https://buildroot.org/downloads/buildroot-${BR_VERSION}.tar.gz"

DL_DIR="${HOME}/br-dl"
SRC_PARENT="${HOME}/br-src"
BR_SRC="${SRC_PARENT}/buildroot-${BR_VERSION}"
BUILD_DIR="${HOME}/br-rw"
OUT_DIR="${HOME}/br-rw-out"

usage() {
    cat <<'USAGE'
Usage: rootfs/build-rootfs.sh [--help]

Download Buildroot (pinned version, sha256-checked) into ~/br-dl, extract it to
~/br-src, build arca_defconfig out of tree in ~/br-rw with this directory as
BR2_EXTERNAL, and copy the result to ~/br-rw-out/rootfs.tar.

Re-running is incremental: the download, the extraction and the build tree are reused.
USAGE
}

case "${1:-}" in
    -h|--help) usage; exit 0 ;;
    "") ;;
    *) echo "unknown argument: $1" >&2; usage >&2; exit 2 ;;
esac

case "$HOME" in
    /mnt/*) echo "HOME is under /mnt: build on WSL's native filesystem" >&2; exit 1 ;;
esac

mkdir -p "$DL_DIR" "$SRC_PARENT" "$OUT_DIR"

# Buildroot refuses a PATH containing a space, and WSL appends the Windows PATH
# (Program Files, ...) by default. Keep only the Linux-side entries.
PATH="$(printf '%s' "$PATH" | tr ':' '\n' | grep -v '^/mnt/' | paste -sd: -)"
export PATH

TARBALL="${DL_DIR}/buildroot-${BR_VERSION}.tar.gz"
if [ ! -f "$TARBALL" ]; then
    wget -O "${TARBALL}.part" "$BR_URL"
    mv "${TARBALL}.part" "$TARBALL"
fi
echo "${BR_SHA256}  ${TARBALL}" | sha256sum -c -

if [ ! -f "${BR_SRC}/Makefile" ]; then
    rm -rf "$BR_SRC"
    tar -xzf "$TARBALL" -C "$SRC_PARENT"
fi

cd "$BR_SRC"
make O="$BUILD_DIR" BR2_EXTERNAL="${SCRIPT_DIR}" BR2_DL_DIR="$DL_DIR" arca_defconfig
# defconfig silently drops a symbol whose dependency is unset (bash needs
# BUSYBOX_SHOW_OTHERS), so every set line must survive into .config.
# BR2_DL_DIR is excluded: the command line overrides it with an expanded path.
dropped=$(grep -E '^BR2_[A-Z0-9_]+=' "${SCRIPT_DIR}/configs/arca_defconfig" | grep -v '^BR2_DL_DIR=' |
    grep -vxF -f "${BUILD_DIR}/.config" || true)
if [ -n "$dropped" ]; then
    echo "arca_defconfig lines that did not reach .config (an unmet dependency):" >&2
    echo "$dropped" >&2
    exit 1
fi
make O="$BUILD_DIR" BR2_DL_DIR="$DL_DIR" -j"$(nproc)"

cp "${BUILD_DIR}/images/rootfs.tar" "${OUT_DIR}/rootfs.tar"
ls -l "${OUT_DIR}/rootfs.tar"

# A tarball that lacks the boot-critical files is a failed build, whatever make said.
"${SCRIPT_DIR}/check-rootfs.sh" "${OUT_DIR}/rootfs.tar"
