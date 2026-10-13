#!/bin/bash
# Cross-compile BlueZ 5.66 for the device's soft-float userspace and stage a
# device-rooted runtime tree, ready for a later deploy step to copy over.
#
# Usage (inside WSL):
#   ./build-bluez.sh            # build whatever is missing, then (re)stage
#   ./build-bluez.sh --force    # drop this script's outputs and rebuild them all
#   ./build-bluez.sh --stage    # print the staged runtime tree's path, build nothing
#
# Two kinds of output, both under arm-deps-softfp/ (gitignored):
#
#   arm-deps-softfp/usr/        LINK-TIME sysroot, shared with build-alsa-lib.sh.
#                               zlib, libffi, glib, expat, dbus, ncurses and readline
#                               are built here for their headers, .pc files and a .so
#                               to link against — NONE of them is deployed. The device
#                               ships every one of those runtimes itself, and each is
#                               pinned to the device's own version so a symbol that
#                               resolves here resolves there. libbluetooth + headers +
#                               bluez.pc land here too, for anything linking BlueZ.
#   arm-deps-softfp/bluez-stage/  the RUNTIME tree, rooted at / on the device:
#                               bluetoothd, bluetoothctl, btmgmt, hciconfig, hcitool,
#                               libbluetooth.so.3, the D-Bus policy and main.conf,
#                               stripped, plus MANIFEST.txt (path, bytes, md5).
#   arm-deps-softfp/bluez-install/  full unstripped `make install` — what the divide
#                               gate reads, since a stripped binary cannot be judged.
#
# Device versions the pins match: libdbus-1.so.3.19.11 is dbus 1.12.16 (libtool age 19,
# revision 11); libglib-2.0.so.0.6200.6 is glib 2.62.6; libz 1.2.11; libreadline.so.8.
# bluetoothctl links libreadline.so.8 DYNAMICALLY, so the device's readline and its
# terminal library are what runs; readline 8.0 is the lowest 8.x, so nothing links
# against a symbol a newer 8.x added. glib uses its internal PCRE and no libmount, so
# the stubs NEED nothing the device's copies may lack — none of that reaches BlueZ's
# own NEEDED list anyway, which the verification at the end prints.
#
# It lives beside build-alsa-lib.sh because the sysroot is that one: a bluez-alsa build
# needs libasound and libbluetooth + dbus in one prefix. Soft-float only
# (../SYSTEM_ANALYSIS.md#63-cross-compiled-dependencies-must-be-built-from-source).
# Needs meson, ninja and pkg-config on the host (setup-build-env.sh lists the set).
set -e
set -o pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PREFIX="$SCRIPT_DIR/arm-deps-softfp"
SYSROOT="$PREFIX"                 # DESTDIR of every --prefix=/usr install below
STAGE="$PREFIX/bluez-stage"
INSTALL="$PREFIX/bluez-install"
SRC="$PREFIX/src"

TC=arm-linux-gnueabi
# Same flags as build-and-deploy.sh / build-alsa-lib.sh: armv7-a implies no idiv.
ARMFLAGS="-march=armv7-a -mtune=cortex-a8 -mfpu=neon -mfloat-abi=softfp"
CFLAGS_ARM="-O2 $ARMFLAGS"

# name|version|tarball|url|sha256 — the tarballs are cached in $SRC, so --force needs no network.
PKGS="
zlib|1.2.11|zlib-1.2.11.tar.gz|https://zlib.net/fossils/zlib-1.2.11.tar.gz|c3e5e9fdd5004dcb542feda5ee4f0ff0744628baf8ed2dd5d66f8ca1197cb1a1
libffi|3.3|libffi-3.3.tar.gz|https://github.com/libffi/libffi/releases/download/v3.3/libffi-3.3.tar.gz|72fba7922703ddfa7a028d513ac15a85c8d54c8d67f55fa5a4802885dc652056
glib|2.62.6|glib-2.62.6.tar.xz|https://download.gnome.org/sources/glib/2.62/glib-2.62.6.tar.xz|104fa26fbefae8024ff898330c671ec23ad075c1c0bce45c325c6d5657d58b9c
expat|2.2.9|expat-2.2.9.tar.xz|https://github.com/libexpat/libexpat/releases/download/R_2_2_9/expat-2.2.9.tar.xz|1ea6965b15c2106b6bbe883397271c80dfa0331cdf821b2c319591b55eadc0a4
dbus|1.12.16|dbus-1.12.16.tar.gz|https://dbus.freedesktop.org/releases/dbus/dbus-1.12.16.tar.gz|54a22d2fa42f2eb2a871f32811c6005b531b9613b1b93a0d269b05e7549fec80
ncurses|6.2|ncurses-6.2.tar.gz|https://invisible-island.net/archives/ncurses/ncurses-6.2.tar.gz|30306e0c76e0f9f1f0de987cf1c82a5c21e1ce6568b9227f7da5b71cbea86c9d
readline|8.0|readline-8.0.tar.gz|https://mirrors.kernel.org/gnu/readline/readline-8.0.tar.gz|e339f51971478d369f8a053a330a190781acb9864cf4c541060f12078948e461
bluez|5.66|bluez-5.66.tar.xz|https://www.kernel.org/pub/linux/bluetooth/bluez-5.66.tar.xz|39fea64b590c9492984a0c27a89fc203e1cdc74866086efb8f4698677ab2b574
"

case "${1:-}" in
    --stage) echo "$STAGE"; exit 0 ;;
    --force) FORCE=1 ;;
    "") FORCE=0 ;;
    *) echo "Usage: $0 [--force | --stage]"; exit 1 ;;
esac

for tool in "$TC-gcc" meson ninja pkg-config; do
    command -v "$tool" >/dev/null 2>&1 || {
        echo "$tool not found. Install every host prerequisite with setup-build-env.sh, at the repo root."
        exit 1
    }
done

# The cross environment every build below sees. PKG_CONFIG_LIBDIR (not _PATH) so the
# host's own .pc files can never answer; SYSROOT_DIR prefixes the .pc's /usr paths.
export PKG_CONFIG_LIBDIR="$SYSROOT/usr/lib/pkgconfig:$SYSROOT/usr/share/pkgconfig"
export PKG_CONFIG_SYSROOT_DIR="$SYSROOT"
unset PKG_CONFIG_PATH
CPPFLAGS_ARM="-I$SYSROOT/usr/include"
LDFLAGS_ARM="-L$SYSROOT/usr/lib -Wl,-rpath-link,$SYSROOT/usr/lib"

# Build on the Linux side: configure + make over /mnt/c DrvFs is several times slower.
BUILD_ROOT="${ARCA_BLUEZ_BUILD_DIR:-$HOME/rw-bluez-build}"
mkdir -p "$BUILD_ROOT" "$SRC"
LOG="$BUILD_ROOT/build.log"
: >"$LOG"

pkg_field() {   # pkg_field <name> <field#>
    printf '%s\n' "$PKGS" | awk -F'|' -v n="$1" -v f="$2" '$1 == n { print $f }'
}

# Fetch + verify + unpack a fresh source tree; prints its directory.
unpack() {
    local name=$1 ver tarball url sha dir
    ver=$(pkg_field "$name" 2); tarball=$(pkg_field "$name" 3)
    url=$(pkg_field "$name" 4); sha=$(pkg_field "$name" 5)
    if [ ! -s "$SRC/$tarball" ]; then
        echo "Downloading $url" >&2
        curl -fsSL -o "$SRC/$tarball.part" "$url"
        mv "$SRC/$tarball.part" "$SRC/$tarball"
    fi
    echo "$sha  $SRC/$tarball" | sha256sum -c --quiet - >&2 || {
        echo "SHA-256 mismatch on $SRC/$tarball — delete it and rerun" >&2
        exit 1
    }
    dir="$BUILD_ROOT/$name-$ver"
    rm -rf "$dir"
    tar xf "$SRC/$tarball" -C "$BUILD_ROOT"
    echo "$dir"
}

# run <label> <cmd...> — all output to the log; on failure show its tail and keep the tree.
run() {
    local label=$1; shift
    echo "== $label: $*" >>"$LOG"
    if ! "$@" >>"$LOG" 2>&1; then
        tail -40 "$LOG"
        echo "$label FAILED — full log: $LOG (build tree kept under $BUILD_ROOT)"
        exit 1
    fi
}

# Libtool archives name /usr/lib paths, which would drag the HOST's libraries into a
# cross link. Nothing here needs them.
drop_la() { find "$SYSROOT/usr/lib" -maxdepth 1 -name "$1*.la" -delete; }

have() { [ "$FORCE" = 0 ] && [ -e "$SYSROOT/usr/$1" ]; }

AUTOTOOLS_ENV=("CC=$TC-gcc" "AR=$TC-ar" "RANLIB=$TC-ranlib" "STRIP=$TC-strip"
               "CFLAGS=$CFLAGS_ARM" "CPPFLAGS=$CPPFLAGS_ARM" "LDFLAGS=$LDFLAGS_ARM")

if [ "$FORCE" = 1 ]; then
    rm -rf "$STAGE" "$INSTALL"
fi

# ── zlib (glib/gio needs it) ────────────────────────────────────────────────────
if ! have lib/libz.so; then
    echo "Building zlib..."
    d=$(unpack zlib); cd "$d"
    run zlib-configure env CHOST="$TC" CC="$TC-gcc" CFLAGS="$CFLAGS_ARM" ./configure --prefix=/usr
    run zlib-make make -j"$(nproc)"
    run zlib-install make install DESTDIR="$SYSROOT"
fi

# ── libffi (gobject needs it) ───────────────────────────────────────────────────
if ! have lib/libffi.so; then
    echo "Building libffi..."
    d=$(unpack libffi); cd "$d"
    run libffi-configure env "${AUTOTOOLS_ENV[@]}" ./configure --host="$TC" --prefix=/usr \
        --disable-static --disable-docs --disable-multi-os-directory
    run libffi-make make -j"$(nproc)"
    run libffi-install make install DESTDIR="$SYSROOT"
    drop_la libffi
fi

# ── glib 2.62.6 (meson) ─────────────────────────────────────────────────────────
if ! have lib/libglib-2.0.so.0; then
    echo "Building glib (a few minutes)..."
    d=$(unpack glib); cd "$d"
    cat >cross.txt <<EOF
[binaries]
c = '$TC-gcc'
cpp = '$TC-g++'
ar = '$TC-ar'
strip = '$TC-strip'
pkgconfig = 'pkg-config'

[properties]
c_args = [$(read -ra a <<<"$CFLAGS_ARM -I$SYSROOT/usr/include"; printf "'%s', " "${a[@]}" | sed 's/, $//')]
c_link_args = ['-L$SYSROOT/usr/lib', '-Wl,-rpath-link,$SYSROOT/usr/lib']
have_c99_vsnprintf = true
have_c99_snprintf = true
have_unix98_printf = true

[host_machine]
system = 'linux'
cpu_family = 'arm'
cpu = 'cortex-a8'
endian = 'little'
EOF
    run glib-setup meson setup build --cross-file cross.txt --prefix=/usr --libdir=lib \
        --buildtype=release -Dinternal_pcre=true -Dlibmount=false -Dselinux=disabled \
        -Dxattr=false -Dman=false -Dgtk_doc=false -Dinstalled_tests=false -Dfam=false \
        -Dnls=disabled
    run glib-build ninja -C build
    run glib-install env DESTDIR="$SYSROOT" ninja -C build install
fi

# ── expat (dbus's configure requires it) ────────────────────────────────────────
if ! have lib/libexpat.so; then
    echo "Building expat..."
    d=$(unpack expat); cd "$d"
    run expat-configure env "${AUTOTOOLS_ENV[@]}" ./configure --host="$TC" --prefix=/usr \
        --disable-static --without-docbook --without-examples --without-tests
    run expat-make make -j"$(nproc)"
    run expat-install make install DESTDIR="$SYSROOT"
    drop_la libexpat
fi

# ── dbus 1.12.16 (libdbus-1 headers + .so; the daemon it also builds is discarded) ──
if ! have lib/libdbus-1.so.3; then
    echo "Building dbus..."
    d=$(unpack dbus); cd "$d"
    run dbus-configure env "${AUTOTOOLS_ENV[@]}" ./configure --host="$TC" --prefix=/usr \
        --sysconfdir=/etc --localstatedir=/var --disable-static --disable-systemd \
        --disable-selinux --disable-libaudit --disable-apparmor --disable-tests \
        --disable-xml-docs --disable-doxygen-docs --disable-ducktype-docs --without-x \
        --disable-launchd --disable-asserts
    run dbus-make make -j"$(nproc)"
    run dbus-install make install DESTDIR="$SYSROOT"
    drop_la libdbus-1
fi

# ── ncurses 6.2 (only so libreadline.so resolves its terminal symbols at link time) ──
if ! have lib/libtinfo.so; then
    echo "Building ncurses..."
    d=$(unpack ncurses); cd "$d"
    run ncurses-configure env "${AUTOTOOLS_ENV[@]}" BUILD_CC=gcc ./configure --host="$TC" \
        --prefix=/usr --with-shared --without-normal --with-termlib --without-debug \
        --without-ada --without-cxx-binding --without-manpages --without-progs \
        --without-tests --disable-db-install --disable-stripping
    run ncurses-make make -j"$(nproc)"
    run ncurses-install make install DESTDIR="$SYSROOT"
fi

# ── readline 8.0 (headers + libreadline.so.8 to link bluetoothctl against) ──────
if ! have lib/libreadline.so.8; then
    echo "Building readline..."
    d=$(unpack readline); cd "$d"
    run readline-configure env "${AUTOTOOLS_ENV[@]}" bash_cv_termcap_lib=libtinfo \
        bash_cv_wcwidth_broken=no bash_cv_func_sigsetjmp=present \
        ./configure --host="$TC" --prefix=/usr --disable-static --with-curses
    run readline-make make -j"$(nproc)" SHLIB_LIBS=-ltinfo
    run readline-install make install DESTDIR="$SYSROOT"
fi

# The two libraries the device must supply under exactly these sonames.
for so in libglib-2.0.so libdbus-1.so libreadline.so; do
    "$TC-readelf" -d "$SYSROOT/usr/lib/$so" | grep SONAME >>"$LOG"
done

# ── BlueZ 5.66 ──────────────────────────────────────────────────────────────────
if [ "$FORCE" = 1 ] || [ ! -f "$INSTALL/usr/libexec/bluetooth/bluetoothd" ]; then
    echo "Building BlueZ..."
    d=$(unpack bluez); cd "$d"
    rm -rf "$INSTALL"
    # client = bluetoothctl; tools gives btmgmt; deprecated gives hciconfig/hcitool.
    # udev, systemd, cups, obex, mesh, monitor and manpages each need a host or target
    # dependency the device lacks or nothing here wants.
    run bluez-configure env "${AUTOTOOLS_ENV[@]}" ./configure --host="$TC" --prefix=/usr \
        --sysconfdir=/etc --localstatedir=/var --enable-library --enable-client \
        --enable-tools --enable-deprecated --disable-udev --disable-systemd \
        --disable-cups --disable-obex --disable-mesh --disable-monitor \
        --disable-manpages --disable-testing \
        --with-dbusconfdir=/etc --with-dbussystembusdir=/usr/share/dbus-1/system-services
    run bluez-make make -j"$(nproc)"
    run bluez-install make install DESTDIR="$INSTALL"
    # btmgmt is built but not installed by 5.66's Makefile.
    install -m755 tools/btmgmt "$INSTALL/usr/bin/btmgmt"
    # The link-time copy of libbluetooth, for bluez-alsa and friends.
    cp -a "$INSTALL"/usr/lib/libbluetooth.so* "$SYSROOT/usr/lib/"
    cp -a "$INSTALL/usr/include/bluetooth" "$SYSROOT/usr/include/"
    cp -a "$INSTALL/usr/lib/pkgconfig/bluez.pc" "$SYSROOT/usr/lib/pkgconfig/"
fi

# ── Stage the runtime set, stripped ─────────────────────────────────────────────
RUNTIME_ELF="
usr/libexec/bluetooth/bluetoothd
usr/bin/bluetoothctl
usr/bin/btmgmt
usr/bin/hciconfig
usr/bin/hcitool
usr/lib/libbluetooth.so.3
"
rm -rf "$STAGE"
for f in $RUNTIME_ELF; do
    src="$INSTALL/$f"
    src=$(readlink -f "$src")               # libbluetooth.so.3 is a symlink
    mkdir -p "$STAGE/$(dirname "$f")"
    "$TC-strip" --strip-unneeded -o "$STAGE/$f" "$src"
done
mkdir -p "$STAGE/etc/dbus-1/system.d" "$STAGE/etc/bluetooth"
cp "$INSTALL/etc/dbus-1/system.d/bluetooth.conf" "$STAGE/etc/dbus-1/system.d/"
cat >"$STAGE/etc/bluetooth/main.conf" <<'EOF'
# Minimal: power the controller up as soon as bluetoothd sees it.
[Policy]
AutoEnable=true
EOF

# ── Verify ──────────────────────────────────────────────────────────────────────
fail=0
# What the device supplies (glibc 2.31 set + the libraries the header lists).
ALLOWED=" libc.so.6 libm.so.6 libpthread.so.0 librt.so.1 libdl.so.2 ld-linux.so.3
 libglib-2.0.so.0 libgio-2.0.so.0 libgobject-2.0.so.0 libdbus-1.so.3 libreadline.so.8
 libbluetooth.so.3 "
ALLOWED=" ${ALLOWED//$'\n'/ } "   # the case match below needs a space on each side of every name
glibc_max=0
for f in $RUNTIME_ELF; do
    elf="$STAGE/$f"
    if "$TC-readelf" -A "$elf" | grep -q 'Tag_ABI_VFP_args'; then
        echo "HARD-FLOAT: $f"; fail=1
    fi
    "$TC-readelf" -l "$elf" | grep -q 'interpreter: /lib/ld-linux.so.3' || \
        [ "${f##*/}" = libbluetooth.so.3 ] || { echo "WRONG INTERPRETER: $f"; fail=1; }
    needed=$("$TC-readelf" -d "$elf" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p' | tr '\n' ' ')
    echo "NEEDED $f: $needed"
    for n in $needed; do
        case "$ALLOWED" in *" $n "*) ;; *) echo "  NOT ON DEVICE: $n"; fail=1 ;; esac
    done
    v=$("$TC-objdump" -T "$elf" | grep -o 'GLIBC_[0-9.]*' | sed 's/GLIBC_//' | sort -V | tail -1)
    [ -n "$v" ] && glibc_max=$(printf '%s\n%s\n' "$glibc_max" "$v" | sort -V | tail -1)
done
echo "GLIBC max symbol version: $glibc_max"
if [ "$(printf '%s\n2.31\n' "$glibc_max" | sort -V | tail -1)" != 2.31 ]; then
    echo "  newer than the device's glibc 2.31"; fail=1
fi

# The divide gate reads the unstripped install, never the stripped stage.
UNSTRIPPED=()
for f in $RUNTIME_ELF; do UNSTRIPPED+=("$(readlink -f "$INSTALL/$f")"); done
OBJDUMP="$TC-objdump" bash "$SCRIPT_DIR/check-arm-safe.sh" "${UNSTRIPPED[@]}" || fail=1

( cd "$STAGE" && find . -type f | sort | while read -r p; do
      printf '%-45s %9d  %s\n' "${p#.}" "$(stat -c %s "$p")" "$(md5sum <"$p" | cut -d' ' -f1)"
  done ) >"$BUILD_ROOT/MANIFEST.txt"
mv "$BUILD_ROOT/MANIFEST.txt" "$STAGE/MANIFEST.txt"
cat "$STAGE/MANIFEST.txt"
echo "total bytes: $(du -cb "$STAGE" --exclude=MANIFEST.txt | tail -1 | cut -f1)"

if [ "$fail" != 0 ]; then
    echo "BlueZ staged at $STAGE but VERIFICATION FAILED — see above"
    exit 1
fi
rm -rf "$BUILD_ROOT"
echo "BlueZ 5.66 staged at $STAGE"
