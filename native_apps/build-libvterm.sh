#!/bin/bash
# Cross-compile libvterm 0.3.3 for the device's soft-float userspace — a static
# libvterm.a and headers (vterm.h, vterm_keycodes.h) to LINK against.
# Nothing built here is deployed to the device — it is a build-time dependency only.
#
# Usage:
#   ./build-libvterm.sh            # build into arm-deps-softfp/ unless already there
#   ./build-libvterm.sh --force    # rebuild from scratch
#   ./build-libvterm.sh --prefix   # print the sysroot path, build nothing
#
# Output: arm-deps-softfp/usr/include/vterm.h, vterm_keycodes.h
#         arm-deps-softfp/usr/lib/libvterm.a
# Callers compile with -I<prefix>/usr/include and link -L<prefix>/usr/lib -lvterm.
#
# libvterm is a terminal emulator library; it ships as a Makefile that uses libtool.
# Here we compile the .c files directly with cross-gcc and create a static .a with ar,
# avoiding the libtool dependency. The tarball ships pre-generated .inc encoding tables
# in src/encoding/, so we do not need Perl (find-wide-chars.pl).
#
# Soft-float only — a hard-float binary cannot link against this library on the device.
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PREFIX="$SCRIPT_DIR/arm-deps-softfp"

VTERM_VER=0.3.3
VTERM_TARBALL="libvterm-$VTERM_VER.tar.gz"
VTERM_URL="https://www.leonerd.org.uk/code/libvterm/$VTERM_TARBALL"
VTERM_SHA256=09156f43dd2128bd347cbeebe50d9a571d32c64e0cf18d211197946aff7226e0

TC=arm-linux-gnueabi
ARMFLAGS_ARRAY=("-march=armv7-a" "-mtune=cortex-a8" "-mfpu=neon" "-mfloat-abi=softfp")

case "${1:-}" in
    --prefix) echo "$PREFIX"; exit 0 ;;
    --force)  rm -rf "${PREFIX:?}/usr/lib/libvterm.a" "${PREFIX:?}/usr/include/vterm"* ;;
    "") ;;
    *) echo "Usage: $0 [--force | --prefix]"; exit 1 ;;
esac

# The artifacts, not a stamp: a half-finished install leaves no libvterm.a.
if [ -f "$PREFIX/usr/lib/libvterm.a" ] && [ -f "$PREFIX/usr/include/vterm.h" ]; then
    echo "libvterm $VTERM_VER already built ($PREFIX/usr/lib/libvterm.a)"
    exit 0
fi

if ! command -v "$TC-gcc" >/dev/null 2>&1; then
    echo "$TC-gcc not found. Install every host prerequisite with setup-build-env.sh, at the repo root."
    exit 1
fi

# The tarball is cached beside the output, so a --force rebuild needs no network.
mkdir -p "$PREFIX/src"
if [ ! -f "$PREFIX/src/$VTERM_TARBALL" ]; then
    echo "Downloading $VTERM_URL"
    wget -q -O "$PREFIX/src/$VTERM_TARBALL.part" "$VTERM_URL"
    mv "$PREFIX/src/$VTERM_TARBALL.part" "$PREFIX/src/$VTERM_TARBALL"
fi
echo "$VTERM_SHA256  $PREFIX/src/$VTERM_TARBALL" | sha256sum -c --quiet - || {
    echo "SHA-256 mismatch on $PREFIX/src/$VTERM_TARBALL — delete it and rerun"
    exit 1
}

# Build on the Linux side: a configure + make over /mnt/c DrvFs is several times slower.
BUILD_DIR=$(mktemp -d "${TMPDIR:-/tmp}/libvterm-build.XXXXXX")
trap 'rm -rf "$BUILD_DIR"' EXIT
LOG="$BUILD_DIR/build.log"
tar xzf "$PREFIX/src/$VTERM_TARBALL" -C "$BUILD_DIR"
cd "$BUILD_DIR/libvterm-$VTERM_VER"

echo "Building libvterm $VTERM_VER for $TC (soft-float)..."
mkdir -p "$PREFIX/usr/lib" "$PREFIX/usr/include"

# Compile all .c files in src/ directly without libtool.
# The .inc encoding tables are pre-generated and included by encoding.c.
{
    for cfile in src/*.c; do
        "$TC-gcc" "${ARMFLAGS_ARRAY[@]}" -O2 -Wall -Iinclude -Isrc -std=c99 -c "$cfile" \
            -o "${cfile%.c}.o" || exit 1
    done
    "$TC-ar" rcs "$PREFIX/usr/lib/libvterm.a" src/*.o
} >"$LOG" 2>&1 || {
    tail -30 "$LOG"
    echo "libvterm build FAILED — the tail of the log is above"
    trap - EXIT
    echo "Build tree kept: $BUILD_DIR"
    exit 1
}

# Install headers.
cp include/vterm.h "$PREFIX/usr/include/"
cp include/vterm_keycodes.h "$PREFIX/usr/include/"

# Verify it's ARM soft-float and has no hardware divide instructions.
if "$TC-readelf" -A "$PREFIX/usr/lib/libvterm.a" | grep -q 'Tag_ABI_VFP_args'; then
    echo "Built libvterm.a is hard-float; the device cannot link it"
    exit 1
fi

IDIV_COUNT=$("$TC-objdump" -d "$PREFIX/usr/lib/libvterm.a" 2>/dev/null | grep -c 'sdiv\|udiv' || true)
if [ "$IDIV_COUNT" -gt 0 ]; then
    echo "libvterm.a contains $IDIV_COUNT sdiv/udiv instructions — Cortex-A8 cannot execute"
    exit 1
fi

echo "libvterm $VTERM_VER installed to $PREFIX/usr"
