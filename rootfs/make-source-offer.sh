#!/bin/bash
# make-source-offer.sh - Assemble the complete corresponding source of a card image
# (GPLv2 section 3(a)): one tar, written outside the repository, to be published next to
# the image under the same release tag.
#
# Run in WSL. Nothing is rebuilt and nothing touches a device: everything it reads is a
# local cache (the Buildroot build tree, the dependency tarballs the component scripts
# fetch, the U-Boot clone, the ScummVM tree), plus one download (the Ubuntu glibc source
# the static vnc_client and devmem_write embed). Every third-party member is checked
# against the sha256 or commit that the component's own build script pins, and the
# result is graded by rootfs/check-source-offer.sh before the tar is written.
#
# See --help.

# shellcheck source-path=SCRIPTDIR
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/.." && pwd)"

# shellcheck source=../lib/product.sh
. "$REPO/lib/product.sh"

# The Ubuntu source package of the glibc the host cross-toolchain links statically into
# vnc_client and devmem_write. archive.ubuntu.com no longer serves 2.31-0ubuntu9.9
# (superseded); Launchpad keeps every published file. The .dsc carries the hashes of the
# other two, so only its own hash is pinned here.
UBUNTU_GLIBC_VERSION="2.31-0ubuntu9.9"
UBUNTU_GLIBC_BASE="https://launchpad.net/ubuntu/+archive/primary/+sourcefiles/glibc/${UBUNTU_GLIBC_VERSION}"
UBUNTU_GLIBC_DSC_SHA256="49304b4d1b2904c4548bfb36be492f139ded302c5733ba1dc4bbe32b56bac6ed"

usage() {
    cat <<'USAGE'
Usage: rootfs/make-source-offer.sh --tag <tag> --out <dir> [options]

Writes <dir>/<product>-source-<tag>.tar and its .sha256: the source of everything on the
card image that is not our own code (our own code is the repo archive inside it).

  --tag <tag>             release tag the image is published under (required)
  --out <dir>             where to write; must be outside the repository (required)
  --rootfs-build <dir>    Buildroot output tree (default ~/br-rw)
  --legal-info-dir <dir>  use an existing legal-info tree instead of running
                          'make legal-info' (default <rootfs-build>/legal-info after a run)
  --cache <dir>           download cache for the Ubuntu glibc source (default ~/ludbox-source-cache)
  --uboot-src <dir>       U-Boot git checkout holding the tag (default ~/rw-uboot-build/u-boot-clone)
  --scummvm-src <dir>     ScummVM git tree at the pinned commit (default <repo>/scummvm)
  --image <file>          a file of the image the source belongs to; its md5 is recorded
                          (repeatable: MLO, u-boot.img, uImage-system, rootfs.tar, the .img)
  --allow-dev             development build: accept a dirty tree or an unpushed commit. The
                          result says so in README-SOURCE.txt and cannot be published.
  --help                  this text

Refuses a dirty tree, and a commit that is not on a remote branch, unless --allow-dev.
USAGE
}

die() { echo "make-source-offer: $*" >&2; exit 1; }
info() { echo "  -> $*"; }

TAG="" OUT="" RB="${HOME}/br-rw" LEGAL="" CACHE="${HOME}/ludbox-source-cache"
UBOOT_SRC="${HOME}/rw-uboot-build/u-boot-clone" SCUMMVM_SRC="${REPO}/scummvm"
DEV=0
IMAGES=()
while [ "$#" -gt 0 ]; do
    case "$1" in
        -h|--help) usage; exit 0 ;;
        --tag)            [ "$#" -ge 2 ] || die "--tag needs a value"; TAG="$2"; shift 2 ;;
        --out)            [ "$#" -ge 2 ] || die "--out needs a value"; OUT="$2"; shift 2 ;;
        --rootfs-build)   [ "$#" -ge 2 ] || die "--rootfs-build needs a value"; RB="$2"; shift 2 ;;
        --legal-info-dir) [ "$#" -ge 2 ] || die "--legal-info-dir needs a value"; LEGAL="$2"; shift 2 ;;
        --cache)          [ "$#" -ge 2 ] || die "--cache needs a value"; CACHE="$2"; shift 2 ;;
        --uboot-src)      [ "$#" -ge 2 ] || die "--uboot-src needs a value"; UBOOT_SRC="$2"; shift 2 ;;
        --scummvm-src)    [ "$#" -ge 2 ] || die "--scummvm-src needs a value"; SCUMMVM_SRC="$2"; shift 2 ;;
        --image)          [ "$#" -ge 2 ] || die "--image needs a value"; IMAGES+=("$2"); shift 2 ;;
        --allow-dev)      DEV=1; shift ;;
        *) echo "unknown argument: $1" >&2; usage >&2; exit 2 ;;
    esac
done

# ── local refusals, before any work ─────────────────────────────────────────
[ -n "$TAG" ] || die "--tag is required"
case "$TAG" in *[!A-Za-z0-9._-]*|"") die "--tag may contain only letters, digits, . _ -" ;; esac
[ -n "$OUT" ] || die "--out is required"
# Resolve the target without creating it: a refusal must leave nothing behind.
OUT_PARENT="$(cd "$(dirname "$OUT")" 2>/dev/null && pwd -P)" || die "the parent of --out does not exist"
OUT_ABS="$OUT_PARENT/$(basename "$OUT")"
REPO_REAL="$(cd "$REPO" && pwd -P)"
case "$OUT_ABS/" in
    "$REPO_REAL"/*) die "--out is inside the repository ($OUT_ABS): the offer is ~0.7 GB and group D of doc_check counts what sits in the tree" ;;
esac
case "$HOME" in /mnt/*) die "HOME is under /mnt: run in WSL, on its native filesystem" ;; esac

for t in git tar xz sha256sum md5sum curl; do
    command -v "$t" >/dev/null 2>&1 || die "$t not found"
done

if ! git -C "$REPO" diff --quiet 2>/dev/null || ! git -C "$REPO" diff --cached --quiet 2>/dev/null; then
    [ "$DEV" -eq 1 ] || die "the working tree has uncommitted changes: the offer would not match any commit. Commit, or pass --allow-dev."
    DIRTY=1
else
    DIRTY=0
fi
REV="$(git -C "$REPO" rev-parse HEAD)"
if [ "$DEV" -eq 0 ] && [ -z "$(git -C "$REPO" branch -r --contains HEAD 2>/dev/null)" ]; then
    die "HEAD ($REV) is not on any remote branch: the repo archive in the offer would name an unpublished commit. Push first, or pass --allow-dev."
fi

# ── pins, read from the build scripts that own them ─────────────────────────
# pin_hex <file> <needle> <digits>: the first hex string of that length on the first line
# containing the needle. The check reads the same pins out of the repo archive it is
# handed, with the same function (rootfs/check-source-offer.sh), so the two cannot disagree.
# shellcheck source=check-source-offer.sh
. "$SCRIPT_DIR/check-source-offer.sh"

UBOOT_TAG="$(sed -n 's/^UBOOT_TAG="\(.*\)"$/\1/p' "$REPO/uboot/build.sh" | head -n 1)"
UBOOT_COMMIT="$(pin_hex "$REPO/uboot/build.sh" "UBOOT_COMMIT=" 40)"
SCUMMVM_COMMIT="$(pin_hex "$REPO/scummvm-arca/upstream-commit" "" 40)"
BR_VERSION="$(sed -n 's/^BR_VERSION="\(.*\)"$/\1/p' "$REPO/rootfs/build-rootfs.sh" | head -n 1)"
if [ -z "$UBOOT_TAG" ] || [ -z "$UBOOT_COMMIT" ]; then die "uboot/build.sh pins no UBOOT_TAG / UBOOT_COMMIT"; fi
[ -n "$SCUMMVM_COMMIT" ] || die "scummvm-arca/upstream-commit holds no commit"
[ -n "$BR_VERSION" ] || die "rootfs/build-rootfs.sh has no BR_VERSION"

# The sources must be what the pins say before anything is copied.
[ "$(git -C "$SCUMMVM_SRC" rev-parse HEAD 2>/dev/null || true)" = "$SCUMMVM_COMMIT" ] \
    || die "$SCUMMVM_SRC is not at the pinned ScummVM commit $SCUMMVM_COMMIT"
[ "$(git -C "$UBOOT_SRC" rev-parse -q --verify "refs/tags/${UBOOT_TAG}^{commit}" 2>/dev/null || true)" = "$UBOOT_COMMIT" ] \
    || die "$UBOOT_SRC has no tag $UBOOT_TAG at the pinned commit $UBOOT_COMMIT"

mkdir -p "$OUT_ABS"
# Buildroot: PATH without the Windows entries, as build-rootfs.sh does.
if [ -z "$LEGAL" ]; then
    [ -d "$RB" ] || die "--rootfs-build $RB does not exist (run rootfs/build-rootfs.sh first)"
    BR_SRC="${HOME}/br-src/buildroot-${BR_VERSION}"
    [ -f "$BR_SRC/Makefile" ] || die "no Buildroot tree at $BR_SRC (run rootfs/build-rootfs.sh first)"
    info "make legal-info"
    ( PATH="$(printf '%s' "$PATH" | tr ':' '\n' | grep -v '^/mnt/' | paste -sd: -)"; export PATH
      cd "$BR_SRC" && make O="$RB" BR2_DL_DIR="${HOME}/br-dl" legal-info ) >"$OUT_ABS/legal-info.log" 2>&1 \
        || die "make legal-info failed; see $OUT_ABS/legal-info.log"
    LEGAL="$RB/legal-info"
fi
[ -f "$LEGAL/manifest.csv" ] || die "$LEGAL/manifest.csv missing"
BR_FILELIST="$(dirname "$LEGAL")/build/packages-file-list.txt"
[ -f "$BR_FILELIST" ] || BR_FILELIST=""

# ── assemble ────────────────────────────────────────────────────────────────
NAME="${PRODUCT_STEM}-source-${TAG}"
[ "$DIRTY" -eq 0 ] || NAME="${NAME}-dev"
STAGE="$OUT_ABS/$NAME"
rm -rf "$STAGE" "$OUT_ABS/$NAME.tar" "$OUT_ABS/$NAME.tar.sha256"
mkdir -p "$STAGE"/{repo,buildroot,kernel,uboot,scummvm,vnc_client,bluetooth,native_apps,scummvm-deps,host-libc,licenses}
CSV="$STAGE/SOURCES.csv"
echo "component,version,licence,binaries,member,url,sha256,pin" >"$CSV"

# add <component> <version> <licence> <binaries> <member> <url> <source file> <pin>
add() {
    local comp="$1" ver="$2" lic="$3" bins="$4" member="$5" url="$6" src="$7" pin="$8" sum
    lic="${lic//,/;}"; bins="${bins//,/;}"
    [ -f "$src" ] || die "$comp: $src is not there (run the component's build script once to fill its cache)"
    cp "$src" "$STAGE/$member"
    sum="$(sha256sum "$STAGE/$member" | cut -d' ' -f1)"
    printf '%s,%s,%s,%s,%s,%s,%s,%s\n' "$comp" "$ver" "$lic" "$bins" "$member" "$url" "$sum" "$pin" >>"$CSV"
}
# add_made: the same row for a member that was just written into the stage by a pipeline
add_made() {
    local comp="$1" ver="$2" lic="$3" bins="$4" member="$5" url="$6" pin="$7" sum
    sum="$(sha256sum "$STAGE/$member" | cut -d' ' -f1)"
    lic="${lic//,/;}"; bins="${bins//,/;}"
    printf '%s,%s,%s,%s,%s,%s,%s,%s\n' "$comp" "$ver" "$lic" "$bins" "$member" "$url" "$sum" "$pin" >>"$CSV"
}

info "repo archive"
if [ "$DIRTY" -eq 0 ]; then
    git -C "$REPO" archive --format=tar --prefix="arca-${TAG}/" HEAD -- . ':!HardwarePhotos' | xz -T0 -6 >"$STAGE/repo/arca-${TAG}.tar.xz"
else
    # A development build packs the tracked files as they are on disk (staged additions
    # included), so a change that is not yet committed is in the archive.
    ( cd "$REPO" && git ls-files -z -c -- . ':!HardwarePhotos' | tar --null -T - --transform "s|^|arca-${TAG}/|" -cf - 2>/dev/null ) \
        | xz -T0 -6 >"$STAGE/repo/arca-${TAG}.tar.xz"
fi
add_made "arca" "$TAG" "MIT;mixed" "native_apps games and tools, app_launcher, devmem_write, scripts, patches, device tree, kernel config" \
    "repo/arca-${TAG}.tar.xz" "this release" "none:"

info "Buildroot"
BR_TARBALL="${HOME}/br-dl/buildroot-${BR_VERSION}.tar.gz"
add "buildroot" "$BR_VERSION" "GPL-2.0-or-later;mixed" "build system of the root filesystem" \
    "buildroot/buildroot-${BR_VERSION}.tar.gz" "https://buildroot.org/downloads/buildroot-${BR_VERSION}.tar.gz" \
    "$BR_TARBALL" "grep:rootfs/build-rootfs.sh:BR_SHA256="
cp -a "$LEGAL" "$STAGE/buildroot/legal-info"
rm -rf "$STAGE/buildroot/legal-info/host-sources" "$STAGE/buildroot/legal-info/host-licenses" "$STAGE/buildroot/legal-info/host-manifest.csv"
[ -z "$BR_FILELIST" ] || cp "$BR_FILELIST" "$STAGE/buildroot/packages-file-list.txt"
# The kernel tarball is shipped once, under kernel/; the legal-info copy becomes a pointer.
for d in "$STAGE"/buildroot/legal-info/sources/linux-headers-*; do
    [ -d "$d" ] || continue
    rm -rf "$d"; mkdir -p "$d"
    echo "see ../../../../kernel/linux-4.14.52.tar.xz" >"$d/POINTER.txt"
done
echo "Buildroot's own packages: sources/ and licenses/ here, listed in manifest.csv. The host tools Buildroot compiled to build the image (compilers, make, python) are not part of the image and are not offered." \
    >"$STAGE/buildroot/legal-info/NOT-INCLUDED.txt"

info "kernel"
add "linux" "4.14.52" "GPL-2.0-only" "uImage-system, uImage-fallback, every .ko" "kernel/linux-4.14.52.tar.xz" \
    "https://cdn.kernel.org/pub/linux/kernel/v4.x/linux-4.14.52.tar.xz" "$REPO/usb_host/linux-4.14.52.tar.xz" \
    "grep:rootfs/package-hashes/linux-headers.hash:linux-4.14.52.tar.xz"

info "U-Boot $UBOOT_TAG"
git -C "$UBOOT_SRC" archive --format=tar --prefix="u-boot-${UBOOT_TAG}/" "refs/tags/${UBOOT_TAG}" | xz -T0 -6 >"$STAGE/uboot/u-boot-${UBOOT_TAG}.tar.xz"
add_made "u-boot" "$UBOOT_TAG" "GPL-2.0-or-later" "MLO, u-boot.img" "uboot/u-boot-${UBOOT_TAG}.tar.xz" \
    "https://source.denx.de/u-boot/u-boot.git" "none:"
echo "$UBOOT_COMMIT" >"$STAGE/uboot/u-boot-${UBOOT_TAG}.COMMIT"
add_made "u-boot-commit" "$UBOOT_TAG" "-" "the git commit of the tag" "uboot/u-boot-${UBOOT_TAG}.COMMIT" \
    "https://source.denx.de/u-boot/u-boot.git" "content:uboot/build.sh:UBOOT_COMMIT="

info "ScummVM $SCUMMVM_COMMIT (this is the largest member)"
git -C "$SCUMMVM_SRC" archive --format=tar --prefix="scummvm-${SCUMMVM_COMMIT}/" "$SCUMMVM_COMMIT" | xz -T0 -6 >"$STAGE/scummvm/scummvm-${SCUMMVM_COMMIT}.tar.xz"
add_made "scummvm" "$SCUMMVM_COMMIT" "GPL-3.0-or-later" "scummvm, scummremastered.zip, gui-icons.dat" \
    "scummvm/scummvm-${SCUMMVM_COMMIT}.tar.xz" "https://github.com/scummvm/scummvm.git" "none:"
echo "$SCUMMVM_COMMIT" >"$STAGE/scummvm/scummvm.COMMIT"
add_made "scummvm-commit" "$SCUMMVM_COMMIT" "-" "the git commit of the tree" "scummvm/scummvm.COMMIT" \
    "https://github.com/scummvm/scummvm.git" "content:scummvm-arca/upstream-commit:"
# The ScummVM backend and configure.patch are in the repo archive (scummvm-arca/backend-files).

info "dependency tarballs"
V="$REPO/vnc_client/deps/src"
add "libvncserver" "0.9.14" "GPL-2.0-or-later" "vnc_client" "vnc_client/LibVNCServer-0.9.14.tar.gz" \
    "https://github.com/LibVNC/libvncserver/archive/refs/tags/LibVNCServer-0.9.14.tar.gz" "$V/LibVNCServer-0.9.14.tar.gz" \
    "grep:vnc_client/build-deps.sh:LibVNCServer-0.9.14.tar.gz)"
add "libjpeg-turbo" "2.1.5.1" "IJG;BSD-3-Clause;Zlib" "vnc_client" "vnc_client/libjpeg-turbo-2.1.5.1.tar.gz" \
    "https://github.com/libjpeg-turbo/libjpeg-turbo/releases/download/2.1.5.1/libjpeg-turbo-2.1.5.1.tar.gz" "$V/libjpeg-turbo-2.1.5.1.tar.gz" \
    "grep:vnc_client/build-deps.sh:libjpeg-turbo-2.1.5.1.tar.gz)"
add "zlib-vnc" "1.2.13" "Zlib" "vnc_client" "vnc_client/zlib-1.2.13.tar.gz" \
    "https://github.com/madler/zlib/releases/download/v1.2.13/zlib-1.2.13.tar.gz" "$V/zlib-1.2.13.tar.gz" \
    "grep:vnc_client/build-deps.sh:zlib-1.2.13.tar.gz)"
B="$REPO/bluetooth/arm-deps-softfp/src"
add "bluez" "5.66" "GPL-2.0-or-later;LGPL-2.1-or-later" "bluetoothd, bluetoothctl, btmon, libbluetooth" "bluetooth/bluez-5.66.tar.xz" \
    "https://www.kernel.org/pub/linux/bluetooth/bluez-5.66.tar.xz" "$B/bluez-5.66.tar.xz" "grep:bluetooth/build-bluez.sh:BLUEZ_SHA256="
add "sbc" "2.0" "LGPL-2.1-or-later" "bluealsa (linked statically)" "bluetooth/sbc-2.0.tar.xz" \
    "https://www.kernel.org/pub/linux/bluetooth/sbc-2.0.tar.xz" "$B/sbc-2.0.tar.xz" "grep:bluetooth/build-bluealsa.sh:SBC_SHA256="
add "bluez-alsa" "4.3.1" "MIT" "bluealsa" "bluetooth/bluez-alsa-4.3.1.tar.gz" \
    "https://github.com/arkq/bluez-alsa/archive/refs/tags/v4.3.1.tar.gz" "$B/bluez-alsa-4.3.1.tar.gz" "grep:bluetooth/build-bluealsa.sh:BA_SHA256="
N="$REPO/native_apps/arm-deps-softfp/src"
add "alsa-lib-build" "1.2.1.2" "LGPL-2.1-or-later" "link-time headers only; the runtime library is Buildroot's" "native_apps/alsa-lib-1.2.1.2.tar.bz2" \
    "https://www.alsa-project.org/files/pub/lib/alsa-lib-1.2.1.2.tar.bz2" "$N/alsa-lib-1.2.1.2.tar.bz2" "grep:native_apps/build-alsa-lib.sh:ALSA_SHA256="
add "libvterm" "0.3.3" "MIT" "terminal app (static)" "native_apps/libvterm-0.3.3.tar.gz" \
    "https://launchpad.net/libvterm/trunk/v0.3/+download/libvterm-0.3.3.tar.gz" "$N/libvterm-0.3.3.tar.gz" "grep:native_apps/build-libvterm.sh:VTERM_SHA256="

# ScummVM's zlib and libpng are fetched by build_arm_deps into a temporary directory and
# deleted, so they are downloaded again here and checked against the pins in that script.
mkdir -p "$CACHE"
fetch() { # <url> <file>
    if [ ! -f "$CACHE/$2" ]; then
        curl -fsSL -o "$CACHE/$2.part" "$1" || die "download of $1 failed"
        mv "$CACHE/$2.part" "$CACHE/$2"
    fi
}
fetch "https://github.com/madler/zlib/releases/download/v1.3.1/zlib-1.3.1.tar.gz" zlib-1.3.1.tar.gz
fetch "https://github.com/pnggroup/libpng/archive/refs/tags/v1.6.43.tar.gz" libpng-1.6.43.tar.gz
add "zlib-scummvm" "1.3.1" "Zlib" "scummvm (static)" "scummvm-deps/zlib-1.3.1.tar.gz" \
    "https://github.com/madler/zlib/releases/download/v1.3.1/zlib-1.3.1.tar.gz" "$CACHE/zlib-1.3.1.tar.gz" \
    "grep:scummvm-arca/build-and-deploy.sh:zlib-1.3.1.tar.gz"
add "libpng" "1.6.43" "libpng-2.0" "scummvm (static)" "scummvm-deps/libpng-1.6.43.tar.gz" \
    "https://github.com/pnggroup/libpng/archive/refs/tags/v1.6.43.tar.gz" "$CACHE/libpng-1.6.43.tar.gz" \
    "grep:scummvm-arca/build-and-deploy.sh:libpng-1.6.43.tar.gz"

info "Ubuntu glibc ${UBUNTU_GLIBC_VERSION}"
fetch "${UBUNTU_GLIBC_BASE}/glibc_${UBUNTU_GLIBC_VERSION}.dsc" "glibc_${UBUNTU_GLIBC_VERSION}.dsc"
echo "${UBUNTU_GLIBC_DSC_SHA256}  ${CACHE}/glibc_${UBUNTU_GLIBC_VERSION}.dsc" | sha256sum -c --quiet - \
    || die "the Ubuntu glibc .dsc does not match UBUNTU_GLIBC_DSC_SHA256"
for f in glibc_2.31.orig.tar.xz "glibc_${UBUNTU_GLIBC_VERSION}.debian.tar.xz"; do
    fetch "${UBUNTU_GLIBC_BASE}/$f" "$f"
    # the dsc is the pin: its Checksums-Sha256 names this file
    want="$(grep -E "^ [0-9a-f]{64} [0-9]+ $f\$" "$CACHE/glibc_${UBUNTU_GLIBC_VERSION}.dsc" | cut -d' ' -f2)"
    [ -n "$want" ] || die "$f is not in the .dsc"
    echo "$want  $CACHE/$f" | sha256sum -c --quiet - || die "$f does not match the .dsc"
done
GL="LGPL-2.1-or-later"
GB="vnc_client, devmem_write (static libc.a of the host cross-toolchain)"
add "glibc-host" "$UBUNTU_GLIBC_VERSION" "$GL" "$GB" "host-libc/glibc_2.31.orig.tar.xz" "${UBUNTU_GLIBC_BASE}/glibc_2.31.orig.tar.xz" \
    "$CACHE/glibc_2.31.orig.tar.xz" "dsc:host-libc/glibc_${UBUNTU_GLIBC_VERSION}.dsc:glibc_2.31.orig.tar.xz"
add "glibc-host-debian" "$UBUNTU_GLIBC_VERSION" "$GL" "$GB" "host-libc/glibc_${UBUNTU_GLIBC_VERSION}.debian.tar.xz" \
    "${UBUNTU_GLIBC_BASE}/glibc_${UBUNTU_GLIBC_VERSION}.debian.tar.xz" "$CACHE/glibc_${UBUNTU_GLIBC_VERSION}.debian.tar.xz" \
    "dsc:host-libc/glibc_${UBUNTU_GLIBC_VERSION}.dsc:glibc_${UBUNTU_GLIBC_VERSION}.debian.tar.xz"
add "glibc-host-dsc" "$UBUNTU_GLIBC_VERSION" "$GL" "$GB" "host-libc/glibc_${UBUNTU_GLIBC_VERSION}.dsc" \
    "${UBUNTU_GLIBC_BASE}/glibc_${UBUNTU_GLIBC_VERSION}.dsc" "$CACHE/glibc_${UBUNTU_GLIBC_VERSION}.dsc" \
    "grep:rootfs/make-source-offer.sh:UBUNTU_GLIBC_DSC_SHA256="

info "licence texts"
# Notices that must travel with binaries whose source we are not obliged to offer.
cp "$REPO/kernel/3rdparty/realtek/bluetooth/LICENCE.rtlwifi_firmware.txt" "$STAGE/licenses/LICENCE.rtlwifi_firmware.txt"
lic() { # <tarball> <member in tarball> <name here>
    tar -xOf "$1" "$2" >"$STAGE/licenses/$3" 2>/dev/null || die "no $2 in $1"
}
lic "$STAGE/vnc_client/libjpeg-turbo-2.1.5.1.tar.gz" libjpeg-turbo-2.1.5.1/LICENSE.md libjpeg-turbo-LICENSE.md
lic "$STAGE/vnc_client/libjpeg-turbo-2.1.5.1.tar.gz" libjpeg-turbo-2.1.5.1/README.ijg libjpeg-turbo-README.ijg
lic "$STAGE/vnc_client/zlib-1.2.13.tar.gz" zlib-1.2.13/LICENSE zlib-LICENSE
lic "$STAGE/bluetooth/bluez-alsa-4.3.1.tar.gz" bluez-alsa-4.3.1/LICENSE bluez-alsa-LICENSE
lic "$STAGE/native_apps/libvterm-0.3.3.tar.gz" libvterm-0.3.3/LICENSE libvterm-LICENSE
lic "$STAGE/scummvm-deps/libpng-1.6.43.tar.gz" libpng-1.6.43/LICENSE libpng-LICENSE
cp "$LEGAL/README" "$STAGE/buildroot/legal-info/README" 2>/dev/null || true

info "image-md5.txt, BUILD-ENV.txt, README-SOURCE.txt"
{
    echo "# md5 of the image files this source corresponds to"
    if [ "${#IMAGES[@]}" -eq 0 ]; then echo "# (none given: pass --image <file> for each)"; fi
    for f in "${IMAGES[@]}"; do
        [ -f "$f" ] || die "--image $f is not a file"
        printf '%s  %s\n' "$(md5sum "$f" | cut -d' ' -f1)" "$(basename "$f")"
    done
} >"$STAGE/image-md5.txt"
{
    echo "# Build environment recorded at assembly time, not at release time."
    echo "product:      $PRODUCT_NAME"
    echo "tag:          $TAG"
    echo "repo commit:  $REV$([ "$DIRTY" -eq 0 ] || echo ' (plus uncommitted changes: development build)')"
    echo "buildroot:    $BR_VERSION"
    echo "host:         $(lsb_release -ds 2>/dev/null || sed -n 's/^PRETTY_NAME=//p' /etc/os-release)"
    echo "kernel (WSL): $(uname -r)"
    echo
    echo "# cross-toolchain packages installed on this host"
    dpkg-query -W -f='${Package} ${Version}\n' 2>/dev/null \
        | grep -E '^(gcc|g\+\+|binutils|libc6-dev|libc6|cpp|linux-libc-dev|u-boot-tools|device-tree-compiler|bison|flex|bc|libssl-dev|make)[a-z0-9+-]*(arm|cross|gnueabi)|^(u-boot-tools|device-tree-compiler|make|bc|bison|flex) ' \
        | sort || true
    for c in arm-linux-gnueabi-gcc arm-linux-gnueabihf-gcc arm-linux-gnueabihf-gcc-10; do
        command -v "$c" >/dev/null 2>&1 && printf '%s: %s\n' "$c" "$("$c" --version | head -n 1)"
    done
} >"$STAGE/BUILD-ENV.txt"

cat >"$STAGE/README-SOURCE.txt" <<README
${PRODUCT_NAME}: corresponding source for the card image tagged ${TAG}
====================================================================

This tar is the complete corresponding source of the GPL- and LGPL-licensed programs on
the ${PRODUCT_NAME} card image published under the same release tag, offered under
section 3(a) of the GNU General Public License version 2 (and the matching terms of the
LGPL 2.1 and GPL 3). The image-md5.txt file names the image files it belongs to.
README
if [ "$DIRTY" -ne 0 ]; then
    echo "DEVELOPMENT BUILD: made from a working tree with uncommitted changes. It matches no release." >>"$STAGE/README-SOURCE.txt"
fi
cat >>"$STAGE/README-SOURCE.txt" <<'README'

Finding the source of a program on the card
-------------------------------------------
 SOURCES.csv   one row per member: component, version, licence, the binaries it produces,
               the member path inside this tar, the upstream URL, the sha256 of the member,
               and where the build script pins that hash.
 buildroot/    the Buildroot build system; buildroot/legal-info/manifest.csv lists the
               packages it compiled (glibc, busybox, bash, util-linux, alsa, ...), with
               their source archives in sources/ and licence texts in licenses/.
               buildroot/packages-file-list.txt says which package installed which file.
 kernel/       Linux 4.14.52 as released. Our patches, device tree, driver sources and
               configuration are in repo/ (kernel/patches, kernel/dts, kernel/drivers,
               kernel/config-changes and usb_host/device_config); the kernel build script
               kernel/build-image.sh applies them and produces uImage-system and the .ko files.
 uboot/        U-Boot, the tagged commit; our patch is uboot/rw20-board.patch in repo/.
 scummvm/      ScummVM at the pinned commit; our backend and configure.patch are in repo/
               (scummvm-arca/backend-files).
 vnc_client/ bluetooth/ native_apps/ scummvm-deps/   the libraries those programs link.
 host-libc/    the Ubuntu glibc source: the host cross-toolchain links its static libc.a into
               vnc_client and devmem_write, which are therefore LGPL-covered binaries.
 repo/         our own code (native apps, launcher, build and deploy scripts, rootfs/ external
               tree, patches) at the release commit.
 licenses/     licence notices for components whose source is not required, and the Realtek
               firmware licence (the Bluetooth firmware blobs are redistributed unmodified).
 BUILD-ENV.txt the toolchain versions this was assembled with.

Rebuilding
----------
Each component builds with the script that sits beside it in repo/ (see DEVELOPING.md in
repo/ for the order): rootfs/build-rootfs.sh, kernel/build-image.sh, uboot/build.sh,
native_apps/build-and-deploy.sh, vnc_client, scummvm-arca, bluetooth, usb_host. The
scripts fetch the same tarballs that are in this tar and check them against the hashes
named in SOURCES.csv. A binary rebuilt with a different toolchain is not byte-identical.

For the LGPL libraries (glibc, alsa-lib, libdaemon, glib, kmod, libxcrypt, sbc, the
static libc) the object files and build scripts needed to relink are the scripts above
plus the sources here; replace the library and rebuild.

Not offered, deliberately: the compilers and build tools themselves (not part of the
image), and the Realtek Bluetooth firmware (no source exists; licence text in licenses/).
README

# ── grade it before it is written ───────────────────────────────────────────
info "grading the offer"
check_source_offer "$STAGE" || die "the offer failed its own check; nothing was written (stage left at $STAGE)"

info "writing $NAME.tar"
tar -cf "$OUT_ABS/$NAME.tar" -C "$OUT_ABS" "$NAME"
( cd "$OUT_ABS" && sha256sum "$NAME.tar" >"$NAME.tar.sha256" )
rm -rf "$STAGE" "$OUT_ABS/legal-info.log"
ls -l "$OUT_ABS/$NAME.tar" "$OUT_ABS/$NAME.tar.sha256"
