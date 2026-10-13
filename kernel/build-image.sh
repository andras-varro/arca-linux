#!/bin/bash
# build-image.sh - Build a bootable uImage from the vanilla 4.14.52 tree for Arca
# Run this script in WSL (Linux). The recipe and the reasoning behind each step are in
# kernel/README.md.
#
# Prerequisites: every host package this needs comes from ../setup-build-env.sh, the one
# home for them (the cross-compiler is its `core` group; mkimage its `kmod` group). The device
# tree is compiled by the kernel tree's own scripts/dtc, so it needs nothing more.
#
# Produces: <out>/zImage-with-dtb and <out>/uImage-test. It never writes p1: installing the image
# as uImage-system is a manual operator step (kernel/README.md), and nothing scripted writes it.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

KERNEL_VERSION="4.14.52"
KERNEL_TARBALL="${REPO_DIR}/usb_host/linux-${KERNEL_VERSION}.tar.xz"
DEVICE_CONFIG="${REPO_DIR}/usb_host/device_config"
PATCH_DIR="${SCRIPT_DIR}/patches"
CONFIG_CHANGES="${SCRIPT_DIR}/config-changes"
# Our own board description (kernel/dts/rw20.dts), compiled against the tree's mainline dtsi files.
DTS_SOURCE="${SCRIPT_DIR}/dts/rw20.dts"
CROSS_COMPILE="arm-linux-gnueabihf-"
ARCH="arm"

WORK_DIR="${HOME}/rw-kbuild-image"
OUT_DIR=""
DTB=""
DTB_MODE="build"
REUSE=0

usage() {
    cat <<'USAGE'
Usage: kernel/build-image.sh [--work <dir>] [--out <dir>] [--dtb <file>] [--reuse]

Extract a fresh 4.14.52 tree from usb_host/linux-4.14.52.tar.xz, apply kernel/patches/*.patch
in sorted order, configure from usb_host/device_config + kernel/config-changes + olddefconfig,
compile kernel/dts/rw20.dtb, build zImage, append the DTB and wrap it with mkimage into
uImage-test. Stage that under a NEW name on p1.

The DTB, by default: kernel/dts/rw20.dts, copied into the tree's arch/arm/boot/dts and built
there with `make rw20.dtb`, so the tree's own cpp include path and scripts/dtc apply.

  --work <dir>   Build tree parent, in the WSL filesystem (default: ~/rw-kbuild-image).
                 Refused under /mnt: a kernel build on DrvFs is many times slower.
  --out <dir>    Where zImage-with-dtb and uImage-test go (default: the work dir).
  --dtb <file>   Append this DTB instead of building rw20.dts.
  --reuse        Keep an existing tree, applying only patches not yet in it. Fast rebuilds.
  -h, --help     This text.

Writes <work>/dropped-symbols.txt: every symbol =y/=m in device_config that the new
.config does not carry as =y/=m. Nothing in the repo is written. Timestamps are pinned
to the repo's last commit, so the same inputs give the same uImage.
USAGE
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --work)       shift; WORK_DIR="${1:?--work needs a directory}" ;;
        --out)        shift; OUT_DIR="${1:?--out needs a directory}" ;;
        --dtb)        shift; DTB="${1:?--dtb needs a file}"; DTB_MODE="file" ;;
        --reuse)      REUSE=1 ;;
        -h|--help)    usage; exit 0 ;;
        *)            echo "Unknown argument: $1" >&2; usage >&2; exit 2 ;;
    esac
    shift
done
OUT_DIR="${OUT_DIR:-$WORK_DIR}"
KERNEL_DIR="${WORK_DIR}/linux-${KERNEL_VERSION}"

echo "[0/6] Checking prerequisites..."
missing=0
for tool in "${CROSS_COMPILE}gcc" mkimage bc bison flex; do
    command -v "$tool" &>/dev/null || { echo "ERROR: ${tool} not found."; missing=1; }
done
if [ "$missing" -ne 0 ]; then
    echo "  Install them with: ./setup-build-env.sh --install-deps   (kmod group: mkimage, bc, bison, flex)"
    exit 1
fi
[ -f "$KERNEL_TARBALL" ] || { echo "ERROR: ${KERNEL_TARBALL} not found."; exit 1; }
# gitignored, so a fresh clone lacks it: it comes off a unit.
if [ ! -f "$DEVICE_CONFIG" ]; then
    echo "ERROR: ${DEVICE_CONFIG} not found (gitignored; it is the unit's own /proc/config.gz)."
    echo "  In usb_host/: scp root@<ip>:/proc/config.gz device_config.gz && gunzip device_config.gz"
    exit 1
fi
[ "$DTB_MODE" = file ] || [ -f "$DTS_SOURCE" ] || { echo "ERROR: ${DTS_SOURCE} not found."; exit 1; }
case "$(realpath -m "$WORK_DIR")" in
    /mnt/*) echo "ERROR: work dir ${WORK_DIR} is on DrvFs. Use a path under \$HOME."; exit 1 ;;
esac
mapfile -t PATCHES < <(find "$PATCH_DIR" -maxdepth 1 -name '*.patch' | sort)
[ "${#PATCHES[@]}" -gt 0 ] || { echo "ERROR: no patches in ${PATCH_DIR} (the image boots with no network without them)."; exit 1; }
# Pin every timestamp and build-identity string the image embeds, so the same inputs give the
# same bytes: the kernel's UTS version string (read literally), and mkimage's header time.
EPOCH=$(git -C "$REPO_DIR" log -1 --format=%ct 2>/dev/null || stat -c %Y "$KERNEL_TARBALL")
KBUILD_BUILD_TIMESTAMP="$(date -u -d "@${EPOCH}")"
export SOURCE_DATE_EPOCH="$EPOCH" KBUILD_BUILD_TIMESTAMP KBUILD_BUILD_VERSION=1
export KBUILD_BUILD_USER=arca KBUILD_BUILD_HOST=build
echo "  Cross compiler: $(${CROSS_COMPILE}gcc --version | head -1)"
echo "  Build epoch:    ${EPOCH} (${KBUILD_BUILD_TIMESTAMP})"

# The device tree is compiled in step 5, before the kernel build, because it needs the tree
# and its .config; only the choice is made here.
echo; echo "[1/6] Device tree (${DTB_MODE})..."
mkdir -p "$WORK_DIR"
case "$DTB_MODE" in
    file)   [ -f "$DTB" ] || { echo "ERROR: DTB ${DTB} not found."; exit 1; }
            DTB_SOURCE="--dtb ${DTB}" ;;
    build)  DTB="${KERNEL_DIR}/arch/${ARCH}/boot/dts/rw20.dtb"
            DTB_SOURCE="kernel/dts/rw20.dts, built in the tree" ;;
esac

echo; echo "[2/6] Extracting kernel source..."
if [ "$REUSE" -eq 1 ] && [ -f "${KERNEL_DIR}/Makefile" ]; then
    echo "  --reuse: keeping ${KERNEL_DIR}"
else
    rm -rf "$KERNEL_DIR"
    tar xf "$KERNEL_TARBALL" -C "$WORK_DIR"
    echo "  Extracted into ${KERNEL_DIR}"
fi

# A patch counts as applied only when it reverses cleanly afterwards. Under --reuse a patch that
# already reverses is skipped; one that neither reverses nor applies was edited since, so refuse.
# ⚠️ The reverse probe needs --forward: `-R --batch` on an UNpatched tree prints "Unreversed patch
# detected! Ignoring -R." and exits 0 (measured), so every check here would pass vacuously.
echo; echo "[3/6] Applying ${#PATCHES[@]} patch(es) from ${PATCH_DIR}..."
for p in "${PATCHES[@]}"; do
    if patch -d "$KERNEL_DIR" -p1 -R --forward --dry-run -s < "$p" &>/dev/null; then
        if [ "$REUSE" -eq 0 ]; then echo "ERROR: $(basename "$p") reverses on a fresh tree."; exit 1; fi
        echo "  Already applied: $(basename "$p")"
        continue
    fi
    if ! patch -d "$KERNEL_DIR" -p1 --forward --batch --no-backup-if-mismatch < "$p"; then
        echo "ERROR: $(basename "$p") did not apply."
        [ "$REUSE" -eq 0 ] || echo "  Under --reuse a changed patch needs a fresh tree: drop --reuse."
        exit 1
    fi
    patch -d "$KERNEL_DIR" -p1 -R --forward --dry-run -s < "$p" >/dev/null \
        || { echo "ERROR: $(basename "$p") reported success but does not reverse."; exit 1; }
    echo "  Applied: $(basename "$p")"
done

echo; echo "[4/6] Configuring from device_config + olddefconfig..."
cp "$DEVICE_CONFIG" "${KERNEL_DIR}/.config"
# kernel/config-changes: `<verb> <SYMBOL> [value]`, the verb being scripts/config's.
while read -r verb sym val; do
    case "$verb" in ''|'#'*) continue ;; esac
    if [ "$verb" = set-str ]; then
        "${KERNEL_DIR}/scripts/config" --file "${KERNEL_DIR}/.config" --set-str "$sym" "$val"
    else
        "${KERNEL_DIR}/scripts/config" --file "${KERNEL_DIR}/.config" "--${verb}" "$sym"
    fi
    echo "  config-changes: ${verb} ${sym}${val:+ ${val}}"
done < "$CONFIG_CHANGES"
make -C "$KERNEL_DIR" ARCH=$ARCH CROSS_COMPILE=$CROSS_COMPILE olddefconfig
# Asserted after olddefconfig, which drops an added symbol whose dependency is missing.
while read -r verb sym val; do
    case "$verb" in
        ''|'#'*) continue ;;
        enable)  want="CONFIG_${sym}=y" ;;
        disable) want="# CONFIG_${sym} is not set" ;;
        set-str) want="CONFIG_${sym}=\"${val}\"" ;;
        *)       echo "ERROR: config-changes: unknown verb ${verb}"; exit 1 ;;
    esac
    grep -qxF "$want" "${KERNEL_DIR}/.config" \
        || { echo "ERROR: .config lacks '${want}' after olddefconfig"; exit 1; }
done < "$CONFIG_CHANGES"
# Lowercase is legal in a symbol name (CONFIG_VFPv3), so the class is [A-Za-z0-9_].
sym_re='^CONFIG_[A-Za-z0-9_]+=[ym]$'
DROPPED="${WORK_DIR}/dropped-symbols.txt"
comm -23 <(grep -E "$sym_re" "$DEVICE_CONFIG" | cut -d= -f1 | sort -u) \
         <(grep -E "$sym_re" "${KERNEL_DIR}/.config" | cut -d= -f1 | sort -u) > "$DROPPED"
echo "  Dropped by olddefconfig ($(wc -l < "$DROPPED") symbols, saved to ${DROPPED}):"
sed 's/^/    /' "$DROPPED"

echo; echo "[5/6] Building the device tree and zImage with -j$(nproc)..."
# The tree's own rule for a .dtb (cpp with the tree's include path, then scripts/dtc), run on a
# copy of rw20.dts dropped in beside the mainline dtsi files it includes. Before the kernel build,
# so a DTS error costs seconds. Always copied: under --reuse the tree may hold an older rw20.dts.
if [ "$DTB_MODE" = build ]; then
    cp "$DTS_SOURCE" "${KERNEL_DIR}/arch/${ARCH}/boot/dts/rw20.dts"
    rm -f "$DTB"
    make -C "$KERNEL_DIR" ARCH=$ARCH CROSS_COMPILE=$CROSS_COMPILE rw20.dtb
    [ -s "$DTB" ] || { echo "ERROR: rw20.dtb was not produced."; exit 1; }
    echo "  Built: $(basename "$DTB") ($(stat -c %s "$DTB") bytes)"
fi
start=$SECONDS
make -C "$KERNEL_DIR" ARCH=$ARCH CROSS_COMPILE=$CROSS_COMPILE -j"$(nproc)" zImage
echo "  zImage built in $((SECONDS - start)) s."

# -C none, never -C gzip: -C describes the blob handed over, and a zImage self-extracts.
echo; echo "[6/6] Appending DTB and wrapping with mkimage..."
mkdir -p "$OUT_DIR"
ZIMAGE="${KERNEL_DIR}/arch/arm/boot/zImage"
cat "$ZIMAGE" "$DTB" > "${OUT_DIR}/zImage-with-dtb"
mkimage -A arm -O linux -T kernel -C none -a 0x80008000 -e 0x80008000 -n '' \
        -d "${OUT_DIR}/zImage-with-dtb" "${OUT_DIR}/uImage-test"

echo; echo "  Image built. DTB source: ${DTB_SOURCE}"
echo "  md5sums:"
md5sum "$ZIMAGE" "$DTB" "${OUT_DIR}/uImage-test" | sed 's/^/    /'
echo "  Install by hand: back up the running uImage-system on p1, then copy this over it (kernel/README.md)."
