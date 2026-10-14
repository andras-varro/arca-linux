#!/bin/bash
# build.sh - Build the rw20 U-Boot (MLO + u-boot.img) from a pristine U-Boot v2026.10 tree.
# Run in WSL (Ubuntu 20.04). The steps and the reasons for them are in uboot/BUILD.md.
#
# Prerequisite: gcc-10-arm-linux-gnueabihf (U-Boot 2026.10 refuses the stock gcc 9).
#
# Every build exports the tag fresh with `git archive` and applies uboot/rw20-board.patch to that
# copy, so a dirty source tree is never used. Writes MLO and u-boot.img to the output dir, and
# nothing inside the repo. It never touches a device, and never writes p1 or any unit.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

UBOOT_TAG="v2026.10"
# The commit the tag must point at: a tag can be moved, and the source offer ships this one.
UBOOT_COMMIT="5508406582f6f7120dce0c4b3059819a41788db2"
UBOOT_URL="https://source.denx.de/u-boot/u-boot.git"
PATCH="${SCRIPT_DIR}/rw20-board.patch"
DEFCONFIG="rw20_defconfig"
CROSS_COMPILE="arm-linux-gnueabihf-"

OUT_DIR="${HOME}/rw-uboot-out"
WORK_DIR="${HOME}/rw-uboot-build"
SRC_REPO=""

usage() {
    cat <<'USAGE'
Usage: uboot/build.sh [--out <dir>] [--work <dir>] [--src <git-dir>]

Build rw20_defconfig from U-Boot v2026.10 with uboot/rw20-board.patch, then copy MLO and u-boot.img
to <dir> and print their sizes and md5s. Then the zero sdiv/udiv gate (native_apps/check-arm-safe.sh)
runs on spl/u-boot-spl and u-boot.

  --out <dir>    where MLO and u-boot.img are written (default: ~/rw-uboot-out)
  --work <dir>   scratch: the source copy, the toolchain links, the build tree and build.log
                 (default: ~/rw-uboot-build). Wiped and rebuilt on every run.
  --src <dir>    an existing U-Boot git checkout that has the tag v2026.10. It is only read
                 (git archive). Default: clone UBOOT_URL once into <work>/u-boot-clone.
  -h, --help     this text

Needs WSL with gcc-10-arm-linux-gnueabihf (apt-get install, as root). Run it in WSL, not Git Bash.
USAGE
}

die() {
    echo "build.sh: $*" >&2
    exit 1
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --out)   [ "$#" -ge 2 ] || die "--out needs a directory"; OUT_DIR="$2"; shift 2 ;;
        --work)  [ "$#" -ge 2 ] || die "--work needs a directory"; WORK_DIR="$2"; shift 2 ;;
        --src)   [ "$#" -ge 2 ] || die "--src needs a directory"; SRC_REPO="$2"; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "build.sh: unknown argument: $1" >&2; usage >&2; exit 2 ;;
    esac
done

# ── preflight ───────────────────────────────────────────────────────────────
[ -f "$PATCH" ] || die "missing $PATCH"
command -v git >/dev/null 2>&1 || die "git not found"
command -v make >/dev/null 2>&1 || die "make not found (install build-essential)"
command -v "${CROSS_COMPILE}gcc-10" >/dev/null 2>&1 \
    || die "${CROSS_COMPILE}gcc-10 not found: sudo apt-get install gcc-10-arm-linux-gnueabihf"

# ── source: a read-only tag export, never a working tree ───────────────────
mkdir -p "$WORK_DIR"
if [ -z "$SRC_REPO" ]; then
    SRC_REPO="${WORK_DIR}/u-boot-clone"
    if [ ! -d "${SRC_REPO}/.git" ]; then
        git clone --depth 1 --branch "$UBOOT_TAG" "$UBOOT_URL" "$SRC_REPO"
    fi
fi
git -C "$SRC_REPO" rev-parse -q --verify "refs/tags/${UBOOT_TAG}^{commit}" >/dev/null \
    || die "no tag ${UBOOT_TAG} in ${SRC_REPO}"
[ "$(git -C "$SRC_REPO" rev-parse "refs/tags/${UBOOT_TAG}^{commit}")" = "$UBOOT_COMMIT" ] \
    || die "tag ${UBOOT_TAG} in ${SRC_REPO} is not commit ${UBOOT_COMMIT}"

TREE="${WORK_DIR}/u-boot"
BUILD="${WORK_DIR}/build"
LOG="${WORK_DIR}/build.log"
rm -rf "$TREE" "$BUILD"
mkdir -p "$TREE" "$BUILD"
git -C "$SRC_REPO" archive "$UBOOT_TAG" | tar -x -C "$TREE"
(
    cd "$TREE"
    git apply --check "$PATCH"
    git apply "$PATCH"
)
echo "source: ${UBOOT_TAG} + $(basename "$PATCH") in ${TREE}"

# ── toolchain: U-Boot's version check reads ${CROSS_COMPILE}gcc, so that name must be gcc-10 ──
TC="${WORK_DIR}/tc/bin"
mkdir -p "$TC"
for f in /usr/bin/arm-linux-gnueabihf-*; do
    ln -sfn "$f" "${TC}/$(basename "$f")"
done
ln -sfn /usr/bin/arm-linux-gnueabihf-gcc-10 "${TC}/arm-linux-gnueabihf-gcc"
ln -sfn /usr/bin/arm-linux-gnueabihf-cpp-10 "${TC}/arm-linux-gnueabihf-cpp"
export PATH="${TC}:${PATH}"
GCC_VER="$("${CROSS_COMPILE}gcc" -dumpversion)"
case "$GCC_VER" in
    10*|1[1-9]*|[2-9][0-9]*) ;;
    *) die "${CROSS_COMPILE}gcc is ${GCC_VER}; U-Boot needs 10 or newer" ;;
esac

# ── build ───────────────────────────────────────────────────────────────────
JOBS="$(nproc 2>/dev/null || echo 4)"
echo "building ${DEFCONFIG} with gcc ${GCC_VER}, log: ${LOG}"
if ! { make -C "$TREE" O="$BUILD" "$DEFCONFIG" \
        && make -C "$TREE" O="$BUILD" -j"$JOBS" CROSS_COMPILE="$CROSS_COMPILE"; } > "$LOG" 2>&1; then
    tail -n 40 "$LOG" >&2
    die "make failed (full log: ${LOG})"
fi

for f in MLO u-boot.img spl/u-boot-spl u-boot; do
    [ -f "${BUILD}/${f}" ] || die "build produced no ${f}"
done

# ── gate: no hardware divide in the ELFs that the images come from ───────────
# check-arm-safe.sh exits 0 only when every file is checked and has zero sdiv/udiv (exit 2 = unverifiable).
if ! OBJDUMP="${CROSS_COMPILE}objdump" "${REPO_DIR}/native_apps/check-arm-safe.sh" \
        "${BUILD}/spl/u-boot-spl" "${BUILD}/u-boot"; then
    die "sdiv/udiv gate failed (see output above); images not copied"
fi

# ── publish to the output dir ───────────────────────────────────────────────
mkdir -p "$OUT_DIR"
cp "${BUILD}/MLO" "${BUILD}/u-boot.img" "$OUT_DIR/"
for f in MLO u-boot.img; do
    printf '%s  %s bytes  md5 %s\n' "$f" "$(stat -c %s "${OUT_DIR}/${f}")" \
        "$(md5sum "${OUT_DIR}/${f}" | cut -d' ' -f1)"
done
echo "written to ${OUT_DIR}"
