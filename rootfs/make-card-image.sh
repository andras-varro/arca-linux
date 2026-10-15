#!/bin/bash
# make-card-image.sh --mlo <MLO> --uboot <u-boot.img> --kernel <uImage> [--uenv <file>] [--modules <dir>]
#                    [--bundle <b>] <rootfs.tar> <out.img>
#
# Builds a whole SD-card IMAGE FILE for an Arca, from nothing but our own parts: one kind of
# image, generic and publishable, with no file of the vendor's on it and nothing of any unit's.
# Inputs: our boot chain (MLO and u-boot.img from uboot/build.sh, a uImage kernel from
# kernel/build-image.sh), a Buildroot rootfs.tar, and a directory of kernel modules (*.ko, built by
# kernel/build-modules.sh --out <dir>; default ~/rw-kmods of the invoking user, i.e. $SUDO_USER under
# sudo; cy8ctmg120_ts.ko must be there).
#
# Run as ROOT in WSL (wsl.exe -u root -e bash -lc ...), on the native WSL fs:
# DrvFs (/mnt/c) keeps no symlinks, modes or device nodes, so the image is
# refused there and copied to Windows afterwards. The only block device this
# script touches is the loop device it creates itself on the image file; it
# never opens /dev/mmcblk* or any other disk, and any argument that is a
# block device is refused.
#
# The partition table is written here by sfdisk from ARCA_LAYOUT in lib/arca-identify.sh (the one
# copy of it, which that library also identifies a card by): MBR, three primary partitions and
# nothing else. p3 is the last, so a bigger card can extend it (and resize2fs its filesystem). The
# image ends where p3 ends, 3293184 sectors (1.57 GiB), and fits a 2 GB card.
#
# p1 is formatted FAT32 here and holds exactly: MLO (written first, into the root directory, where
# the OMAP3 boot ROM looks), u-boot.img, uImage-system, uImage-fallback and uEnv.txt. The fallback is
# a second copy of the same kernel: the boot script tries it when the first fails to load, so it
# only guards against a damaged file until the operator stages a different kernel as uImage-system.
# p2 (the log partition, 1 GiB, mounted on /home/root/log) is made fresh and EMPTY; p3 gets the
# rootfs and the kernel modules. There is no data partition and no swap: /home/root/data is a plain
# directory on p3.
#
# --uboot is refused unless its compiled-in root= names p3 (lib/arca-identify.sh
# arca_uboot_root_ok), and so is an uncommented bootargs= in --uenv whose root= is not p3: a card
# with its root on p3 under a U-Boot that boots another partition panics with no log. Both
# refusals come before the root check, so they need no root.
#
# p3 also gets the licence documents under /usr/share/doc/arca-licenses/, so a unit carries the
# licence texts of what it runs: LICENSE.md (this repository's per-file licence record), Buildroot's
# legal-info manifest.csv and licenses/ (from --legal-info), and SOURCE-OFFER.txt, the written offer
# that names the source file published under the same release tag (rootfs/make-source-offer.sh).
#
# No SSH key material of any kind may be in the rootfs tar, on p3, or from the bundle (refused, never
# filtered), and the image has no host name of its own: the unit names itself and makes its SSH host
# key on the first boot.
#
# With --bundle <dir|tar> (a release.sh --stage-only bundle) it then mounts p3 and p2
# as <base>/{root,log} and runs rootfs/commission-offline.sh
# over them: the boot scripts, their links and every component's files go in, and that
# script's own verify pass checks them. It asks nothing and cleans nothing: the tree is
# ours, with no vendor stack. p1 is not mounted by that step.
set -u

die() { echo "FAIL: $*" >&2; exit 1; }

usage() {
    cat <<'USAGE'
Usage: make-card-image.sh --mlo <MLO> --uboot <u-boot.img> --kernel <uImage> [--uenv <file>]
                          [--modules <dir>] [--bundle <dir|tar>] [--legal-info <dir>]
                          [--source-tag <tag>] [--source-url <url>] <rootfs.tar> <out.img>

  --mlo <file>     the SPL (MLO) from uboot/build.sh; becomes p1/MLO, written first
  --uboot <file>   u-boot.img from uboot/build.sh (a legacy uImage); becomes p1/u-boot.img. Refused
                   unless it boots root=/dev/mmcblk0p3 (rebuild it with uboot/build.sh)
  --kernel <file>  our kernel, a uImage from kernel/build-image.sh; becomes p1/uImage-system and,
                   as a second copy, p1/uImage-fallback
  --uenv <file>    p1/uEnv.txt (default: uboot/uEnv.txt, the all-commented template)
  --modules <dir>  directory of kernel modules (*.ko) copied to p3
                   /lib/modules/4.14.52/extra/ (default: ~/rw-kmods of the
                   invoking user, $SUDO_USER under sudo). cy8ctmg120_ts.ko is required.
  --bundle <b>     a release.sh --stage-only bundle (directory or .tar.gz),
                   installed with rootfs/commission-offline.sh (no prompt),
                   so the card boots with every component
                   and no provision.sh or deploy-all.sh afterwards. Needs
                   arm-linux-gnueabihf-objdump (that script's ARM check).
  --legal-info <d> Buildroot's legal-info tree (make legal-info): its manifest.csv and licenses/ are
                   copied to p3 /usr/share/doc/arca-licenses/ (default: br-rw/legal-info of the
                   invoking user, when it is there). Without one the image carries no package
                   licence texts and the summary says it must not be published.
  --source-tag <t> the release tag the source file (<product>-source-<tag>.tar, from
                   rootfs/make-source-offer.sh) is published under; named in SOURCE-OFFER.txt on
                   p3. Default: the tag of --bundle, when it has one.
  --source-url <u> the release page URL, written into SOURCE-OFFER.txt as well (optional)
  -h, --help       this text

The image is the same for every unit: no vendor file, no SSH key, no host name (the unit picks
one and generates its host key on first boot). It is 3293184 sectors (1.57 GiB) and fits a 2 GB card.
USAGE
}
MODDIR=""
BUNDLE=""
MLO=""
UBOOT=""
KERNEL=""
UENV=""
LEGAL=""
SOURCE_TAG=""
SOURCE_URL=""
need_val() { [ "$1" -ge 2 ] || { usage >&2; exit 2; }; }
while [ $# -gt 0 ]; do
    case "$1" in
        -h|--help) usage; exit 0 ;;
        --mlo) need_val $#; MLO="$2"; shift 2 ;;
        --mlo=*) MLO="${1#--mlo=}"; shift ;;
        --uboot) need_val $#; UBOOT="$2"; shift 2 ;;
        --uboot=*) UBOOT="${1#--uboot=}"; shift ;;
        --kernel) need_val $#; KERNEL="$2"; shift 2 ;;
        --kernel=*) KERNEL="${1#--kernel=}"; shift ;;
        --uenv) need_val $#; UENV="$2"; shift 2 ;;
        --uenv=*) UENV="${1#--uenv=}"; shift ;;
        --modules) need_val $#; MODDIR="$2"; shift 2 ;;
        --modules=*) MODDIR="${1#--modules=}"; shift ;;
        --bundle) need_val $#; BUNDLE="$2"; shift 2 ;;
        --bundle=*) BUNDLE="${1#--bundle=}"; shift ;;
        --legal-info) need_val $#; LEGAL="$2"; shift 2 ;;
        --legal-info=*) LEGAL="${1#--legal-info=}"; shift ;;
        --source-tag) need_val $#; SOURCE_TAG="$2"; shift 2 ;;
        --source-tag=*) SOURCE_TAG="${1#--source-tag=}"; shift ;;
        --source-url) need_val $#; SOURCE_URL="$2"; shift 2 ;;
        --source-url=*) SOURCE_URL="${1#--source-url=}"; shift ;;
        --) shift; break ;;
        -*) usage >&2; exit 2 ;;
        *) break ;;
    esac
done
[ $# -eq 2 ] || { usage >&2; exit 2; }
if [ -z "$MLO" ] || [ -z "$UBOOT" ] || [ -z "$KERNEL" ]; then
    echo "--mlo, --uboot and --kernel are all required" >&2; usage >&2; exit 2
fi
ROOTTAR="$1"; OUT="$2"
SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
[ -n "$UENV" ] || UENV="$SCRIPT_DIR/../uboot/uEnv.txt"
if [ -z "$MODDIR" ]; then
    UHOME=$HOME
    if [ -n "${SUDO_USER:-}" ]; then UHOME=$(getent passwd "$SUDO_USER" | cut -d: -f6); fi
    MODDIR="$UHOME/rw-kmods"
fi
# The licence arguments are checked here, ahead of the root check, so a refusal does not depend on
# who runs the script. An explicit --legal-info that is not a legal-info tree is refused; the
# default is used only when it is there, and its absence is reported in the summary.
LEGAL_GIVEN=0
if [ -n "$LEGAL" ]; then
    LEGAL_GIVEN=1
else
    UH="${UHOME:-$HOME}"
    if [ -n "${SUDO_USER:-}" ]; then UH=$(getent passwd "$SUDO_USER" | cut -d: -f6); fi
    [ ! -f "$UH/br-rw/legal-info/manifest.csv" ] || LEGAL="$UH/br-rw/legal-info"
fi
if [ "$LEGAL_GIVEN" -eq 1 ] && { [ ! -f "$LEGAL/manifest.csv" ] || [ ! -d "$LEGAL/licenses" ]; }; then
    die "--legal-info $LEGAL has no manifest.csv and licenses/ (run 'make legal-info' in the Buildroot build tree)"
fi
case "$SOURCE_TAG" in *[!A-Za-z0-9._-]*) die "--source-tag may contain only letters, digits, . _ -" ;; esac

# The boot chain must agree with the layout: the root partition number is compiled into U-Boot
# (and may be forced by an uncommented bootargs= in uEnv.txt). Checked here, ahead of the
# block-device loop and the root check, so a refusal does not depend on who runs the script.
# shellcheck disable=SC1090,SC1091
. "$SCRIPT_DIR/../lib/arca-identify.sh" || die "cannot source arca-identify.sh"
arca_uboot_root_ok "$UBOOT" || die "--uboot $UBOOT does not boot the root partition of this card layout (above); no image was written"
if [ -f "$UENV" ] && grep -Eq '^[[:space:]]*bootargs=' "$UENV"; then
    ROOT_PART=""
    for r in $ARCA_PART_ROLES; do [ "${r#*:}" = "root" ] && ROOT_PART="${r%%:*}"; done
    UENV_ROOT=$(grep -E '^[[:space:]]*bootargs=' "$UENV" | grep -o 'root=/dev/mmcblk0p[0-9]*' | head -1)
    if [ -z "$ROOT_PART" ] || [ "$UENV_ROOT" != "root=/dev/mmcblk0p$ROOT_PART" ]; then
        die "--uenv $UENV has an uncommented bootargs= whose root= is [${UENV_ROOT:-none}], not root=/dev/mmcblk0p${ROOT_PART:-?}; comment the line out or fix it (no image was written)"
    fi
fi

for a in "$ROOTTAR" "$OUT" "$MLO" "$UBOOT" "$KERNEL" "$UENV"; do
    [ ! -b "$a" ] || die "$a is a block device; this script only writes an image file"
done
[ "$(id -u)" -eq 0 ] || die "run as root (wsl.exe -u root ...)"
OUTABS=$(realpath -m -- "$OUT")
case "$OUTABS" in
    /mnt/*) die "$OUTABS is on DrvFs (no symlinks, modes or device nodes); build on the native WSL fs and copy the finished image out" ;;
    /dev/*) die "$OUTABS is under /dev" ;;
esac
[ ! -e "$OUTABS" ] || [ -f "$OUTABS" ] || die "$OUTABS exists and is not a regular file"
[ -f "$ROOTTAR" ] || die "no rootfs tar $ROOTTAR"
[ -f "$MODDIR/cy8ctmg120_ts.ko" ] || die "no $MODDIR/cy8ctmg120_ts.ko (build with kernel/build-modules.sh --out <dir>, or pass --modules)"
[ -s "$MLO" ] || die "no MLO $MLO"
[ -f "$UENV" ] || die "no uEnv.txt $UENV"
# u-boot.img and the kernel are legacy uImages: magic 0x27051956 in the first four bytes.
for f in "$UBOOT" "$KERNEL"; do
    [ -s "$f" ] || die "no $f"
    [ "$(head -c 4 "$f" | od -An -tx1 | tr -d ' \n')" = "27051956" ] || die "$f is not a uImage (bad magic)"
done
for t in sfdisk losetup mke2fs e2fsck fsck.vfat mkfs.vfat truncate; do
    command -v "$t" > /dev/null || die "$t not installed"
done
BUNDLE_TAG=""
if [ -n "$BUNDLE" ]; then
    # Absolute, because commission-offline.sh runs from the repo root. Checked here,
    # before any image exists, rather than after p3 is filled.
    [ -e "$BUNDLE" ] || die "no bundle $BUNDLE"
    BUNDLE=$(realpath -- "$BUNDLE") || die "realpath $BUNDLE"
    command -v "${OBJDUMP:-arm-linux-gnueabihf-objdump}" > /dev/null \
        || die "${OBJDUMP:-arm-linux-gnueabihf-objdump} not installed; commission-offline.sh refuses unverified ARM binaries"
    if [ -d "$BUNDLE" ]; then
        INFO=$(cat "$BUNDLE/manifest.d/bundle.info" 2>/dev/null) || INFO=""
    else
        INFO=$(tar -xzOf "$BUNDLE" --wildcards '*manifest.d/bundle.info' 2>/dev/null) || INFO=""
    fi
    BUNDLE_TAG=$(printf '%s\n' "$INFO" | awk -F= '$1=="tag"{t=$2} $1=="commit"{c=$2} $1=="built"{b=$2}
        END{ if (t != "") printf "%s (commit %s, built %s)", t, c, b }')
    [ -n "$BUNDLE_TAG" ] || BUNDLE_TAG="untagged (no manifest.d/bundle.info)"
    # The image and its source file are published under one tag: the bundle's tag is the default.
    if [ -z "$SOURCE_TAG" ]; then
        SOURCE_TAG=$(printf '%s\n' "$INFO" | awk -F= '$1=="tag"{print $2}')
        case "$SOURCE_TAG" in untagged|*[!A-Za-z0-9._-]*) SOURCE_TAG="" ;; esac
    fi
fi

# shellcheck disable=SC1090,SC1091
. "$SCRIPT_DIR/../lib/product.sh" || die "cannot source product.sh"
# shellcheck disable=SC1090,SC1091
. "$SCRIPT_DIR/../lib/arca-state.sh" || die "cannot source arca-state.sh"

# The partition table is ARCA_LAYOUT (lib/arca-identify.sh, sourced above): <n>:<start>:<size>:<type>,
# 512-byte sectors, three primaries, identical on every card. Nothing is restated here. The image
# ends where the last partition (p3, the growable one) ends. Sectors 1..62 stay zero.
DISK_SECT=$(arca_layout_end)
case "$DISK_SECT" in ''|*[!0-9]*|0) die "arca_layout_end gave [$DISK_SECT]" ;; esac
# p2 and p3 start on 4 MiB (8192-sector) boundaries, for the card's erase blocks; p1 stays at 63,
# where the boot ROM expects it.
for e in $ARCA_LAYOUT; do
    IFS=: read -r n s _ _ <<< "$e"
    [ "$n" = 1 ] || [ $((s % 8192)) -eq 0 ] || die "ARCA_LAYOUT: p$n starts at $s, not on an 8192-sector boundary"
done

# ssh_scan <dir>... — the refusal over what lands on the card: the rootfs tar is listed below
# before any image exists, this runs on the mounted trees (the bundle can carry files the tar does
# not). Never crosses a mount.
ssh_scan() {
    local d hits
    for d in "$@"; do
        if ! hits=$(cd "$d" && find . -xdev ! -type d ! -type l | arca_state_ssh_keys_deny); then
            echo "FAIL: SSH key material on the card under $d:" >&2
            printf '%s\n' "$hits" | sed 's/^/  /' >&2
            return 1
        fi
    done
    return 0
}

# The image gate, before any image exists. Symlinks are left out: they carry no key bytes, and the
# image deliberately links etc/ssh/ssh_host_*_key into /home/root/data (on p3), where each unit makes its own.
tar -tf "$ROOTTAR" > /dev/null || die "$ROOTTAR does not list"
# A stale tar from before an overlay fix passes every check below and ships the old behaviour, so
# the layout check runs here too, not only after a Buildroot build.
bash "$SCRIPT_DIR/check-rootfs.sh" "$ROOTTAR" > /dev/null || die "$ROOTTAR fails rootfs/check-rootfs.sh (run it for the list)"
ROOT_LIST=$(tar -tvf "$ROOTTAR" | awk '$1 !~ /^l/ { $1 = $2 = $3 = $4 = $5 = ""; sub(/^ +/, ""); print }')
if ! KEYS_DENIED=$(printf '%s\n' "$ROOT_LIST" | arca_state_ssh_keys_deny); then
    echo "FAIL: $ROOTTAR carries SSH key material a card image may not hold:" >&2
    printf '%s\n' "$KEYS_DENIED" | sed 's/^/  /' >&2
    echo "No image was written. Rebuild the rootfs without it." >&2
    exit 1
fi

# table_of <image> — "<n>:<start>:<size>:<type>" per partition from sfdisk -d, plus " boot" on p1 if active.
table_of() {
    sfdisk -d "$1" | awk '
        $0 ~ /^[^ ]+[0-9] *:/ {
            n = $1; sub(/^.*[^0-9]/, "", n)
            match($0, /start= *[0-9]+/); s = substr($0, RSTART, RLENGTH); gsub(/[^0-9]/, "", s)
            match($0, /size= *[0-9]+/);  z = substr($0, RSTART, RLENGTH); gsub(/[^0-9]/, "", z)
            match($0, /type=[0-9a-f]+/); t = substr($0, RSTART + 5, RLENGTH - 5)
            printf "%s:%s:%s:%s%s\n", n, s, z, t, ($0 ~ /bootable/ ? " boot" : "")
        }' | tr '\n' ' ' | sed 's/ $//'
}

LOOP=""
MNT=$(mktemp -d /tmp/arca-card-mnt.XXXXXX) || die "mktemp"
cleanup() {
    local rc=$?
    set +e
    if mountpoint -q "$MNT/p"; then umount "$MNT/p"; fi
    for r in root log; do
        if mountpoint -q "$MNT/base/$r"; then umount "$MNT/base/$r"; fi
    done
    if [ -n "$LOOP" ]; then losetup -d "$LOOP"; fi
    rm -rf "$MNT"
    exit $rc
}
trap cleanup EXIT INT TERM

echo "== creating sparse $OUTABS ($DISK_SECT sectors)"
mkdir -p "$(dirname "$OUTABS")" || die "mkdir"
rm -f "$OUTABS"
truncate -s $((DISK_SECT * 512)) "$OUTABS" || die "truncate"

echo "== partition table"
arca_layout_sfdisk_script > "$MNT/table.sfdisk" || die "arca_layout_sfdisk_script"
sfdisk --no-reread --no-tell-kernel "$OUTABS" < "$MNT/table.sfdisk" > "$MNT/sfdisk.log" 2>&1 \
    || { cat "$MNT/sfdisk.log" >&2; die "sfdisk"; }

echo "== partition table of the image"
sfdisk -d "$OUTABS"
WANT_TABLE=""
for e in $ARCA_LAYOUT; do WANT_TABLE="$WANT_TABLE${WANT_TABLE:+ }${e}$([ "${e%%:*}" = 1 ] && echo ' boot')"; done
GOT_TABLE=$(table_of "$OUTABS")
[ "$GOT_TABLE" = "$WANT_TABLE" ] || die "partition table differs: want [$WANT_TABLE], got [$GOT_TABLE]"
arca_is_card_disk "$OUTABS" || die "arca_is_card_disk says the image does not carry the Arca layout"
echo "arca_is_card_disk: yes; all three partitions match ARCA_LAYOUT"
# Sectors 1..62 (between the MBR and p1) are zero on every unit.
[ "$(dd if="$OUTABS" bs=512 skip=1 count=62 status=none | tr -d '\000' | wc -c)" -eq 0 ] || die "sectors 1..62 are not zero"

LOOP=$(losetup -P --show -f "$OUTABS") || die "losetup"
echo "== loop device $LOOP"
for n in 1 2 3; do
    PD=$(arca_part_dev "$LOOP" "$n")
    for _ in 1 2 3 4 5 6 7 8 9 10; do [ -b "$PD" ] && break; sleep 0.5; done
    [ -b "$PD" ] || die "$PD did not appear"
done

echo "== mkfs"
# ext4 features are limited to what kernel 4.14 mounts and what an older
# e2fsck understands: no metadata_csum, no 64bit. Labels are informational;
# nothing identifies a partition by label or UUID.
# -b 4096 is explicit: mke2fs picks 1 KiB blocks for a filesystem under 512 MiB, and a 4 KiB block
# size is what p3 keeps when it is grown to a bigger card.
mkext() { mke2fs -F -q -t "$1" -b 4096 -O "$2" -L "$3" -m 1 "$4" || die "mke2fs $4"; }
mkext ext4 ^metadata_csum,^64bit arca-log "$(arca_part_dev "$LOOP" 2)"
mkext ext4 ^metadata_csum,^64bit arca-root "$(arca_part_dev "$LOOP" 3)"

echo "== p1: FAT32, our boot files only"
P1DEV=$(arca_part_dev "$LOOP" 1)
# A fixed volume id keeps two builds of the same inputs identical on this partition.
mkfs.vfat -F 32 -n ARCABOOT -i 41524331 "$P1DEV" > /dev/null || die "mkfs.vfat $P1DEV"
mkdir "$MNT/p" || die "mkdir mnt"
mount -t vfat "$P1DEV" "$MNT/p" || die "mount p1"
# MLO first: the boot ROM reads it from the root directory of the first FAT partition.
P1_WANT="MLO u-boot.img uImage-system uImage-fallback uEnv.txt"
cp "$MLO" "$MNT/p/MLO" || die "copy MLO"
cp "$UBOOT" "$MNT/p/u-boot.img" || die "copy u-boot.img"
cp "$KERNEL" "$MNT/p/uImage-system" || die "copy uImage-system"
cp "$KERNEL" "$MNT/p/uImage-fallback" || die "copy uImage-fallback"
cp "$UENV" "$MNT/p/uEnv.txt" || die "copy uEnv.txt"
sync
echo "p1 final listing:"
ls -la "$MNT/p"
P1_HAVE=$(find "$MNT/p" -mindepth 1 -maxdepth 1 -printf '%f\n' | LC_ALL=C sort | tr '\n' ' ' | sed 's/ $//')
P1_EXPECT=$(echo "$P1_WANT" | tr ' ' '\n' | LC_ALL=C sort | tr '\n' ' ' | sed 's/ $//')
[ "$P1_HAVE" = "$P1_EXPECT" ] || die "p1 holds [$P1_HAVE], expected [$P1_EXPECT]"
umount "$MNT/p" || die "umount p1"

# licence_docs_install <p3 root> <repo dir> <legal-info dir or ""> <source tag or ""> <release url or "">
# Writes <p3 root>/usr/share/doc/arca-licenses/: LICENSE.md, SOURCE-OFFER.txt and, with a legal-info
# tree, manifest.csv and licenses/. The source file named is the one rootfs/make-source-offer.sh
# writes: <product stem>-source-<tag>.tar, with a .sha256 beside it. Without a tag the offer says the
# image was made for local use and must not be redistributed.
licence_docs_install() {
    local root="$1" repo="$2" legal="$3" tag="$4" url="$5" d asset
    d="$root/usr/share/doc/arca-licenses"
    asset="${PRODUCT_STEM}-source-${tag}.tar"
    mkdir -p "$d" || return 1
    [ -f "$repo/LICENSE.md" ] || { echo "no $repo/LICENSE.md" >&2; return 1; }
    install -m 0644 "$repo/LICENSE.md" "$d/LICENSE.md" || return 1
    if [ -n "$legal" ]; then
        if [ ! -f "$legal/manifest.csv" ] || [ ! -d "$legal/licenses" ]; then
            echo "$legal is not a legal-info tree" >&2; return 1
        fi
        install -m 0644 "$legal/manifest.csv" "$d/manifest.csv" || return 1
        cp -r --no-preserve=mode,ownership "$legal/licenses" "$d/licenses" || return 1
        chmod -R u=rwX,go=rX "$d/licenses" || return 1
    fi
    {
        echo "${PRODUCT_NAME}: written offer for source code"
        echo "============================================="
        echo
        if [ -n "$tag" ]; then
            echo "The programs on this card include free software under the GNU General Public License"
            echo "and the GNU Lesser General Public License (the Linux kernel, U-Boot, BusyBox, bash,"
            echo "glibc and others). The complete corresponding source of all of them is the file"
            echo
            echo "    $asset"
            echo
            echo "with its checksum file $asset.sha256, attached to the release tagged"
            echo "$tag of the project this image was downloaded from."
            [ -z "$url" ] || echo "Release page: $url"
            echo
            echo "This is a written offer under section 3(a) of the GNU General Public License, version 2,"
            echo "honoured for the lifetime of that release. The file holds the upstream sources at the"
            echo "versions built, this project's patches and build scripts, and a README-SOURCE.txt that"
            echo "says where the source of each program on the card is."
        else
            echo "This image was made without a release tag, so no source file is named. It was built"
            echo "for local use and must not be passed on without the source of the programs on it."
        fi
        echo
        echo "Beside this file:"
        echo "  LICENSE.md     the project's per-file licence record"
        if [ -n "$legal" ]; then
            echo "  manifest.csv   every package the root filesystem was built from, with its licence"
            echo "  licenses/      the licence texts of those packages"
        else
            echo "(this image carries no package licence texts: it was made without --legal-info.)"
        fi
    } > "$d/SOURCE-OFFER.txt" || return 1
}

fill() {   # fill <partnum> <rootfs tar>
    local n="$1"
    mount "$(arca_part_dev "$LOOP" "$n")" "$MNT/p" || die "mount p$n"
    # The rootfs keeps its owners (dbus, avahi and the like are real users there).
    tar --numeric-owner -xpf "$2" -C "$MNT/p" || die "extract $2 into p$n"
    if [ "$n" = 3 ]; then
        # p2 (log) mounts on home/root/log, so that stays an empty mount point; home/root/data is a plain
        # directory on this filesystem and must exist in the tree.
        mkdir -p "$MNT/p/home/root/data" "$MNT/p/home/root/log"
        [ ! -d "$MNT/p/home/root/.ssh" ] || chmod 700 "$MNT/p/home/root/.ssh"
        # The image keeps the build-time placeholder host name: the first boot replaces it.
        EXTRA="$MNT/p/lib/modules/4.14.52/extra"
        mkdir -p "$EXTRA" || die "mkdir $EXTRA"
        NKO=0
        for ko in "$MODDIR"/*.ko; do
            [ -f "$ko" ] || continue
            install -m 0644 -o 0 -g 0 "$ko" "$EXTRA/" || die "install $ko"
            NKO=$((NKO + 1))
        done
        [ -f "$EXTRA/cy8ctmg120_ts.ko" ] || die "cy8ctmg120_ts.ko did not land in p3"
        if ! licence_docs_install "$MNT/p" "$SCRIPT_DIR/.." "$LEGAL" "$SOURCE_TAG" "$SOURCE_URL"; then
            umount "$MNT/p"; rm -f "$OUTABS"
            die "could not install the licence documents; the image is deleted"
        fi
        echo "p3 holds $(find "$MNT/p" -xdev | wc -l) entries; $NKO kernel modules in /lib/modules/4.14.52/extra"
        if ! ssh_scan "$MNT/p"; then
            umount "$MNT/p"; rm -f "$OUTABS"
            die "SSH key material on the image; the image is deleted"
        fi
    fi
    sync
    umount "$MNT/p" || die "umount p$n"
}
echo "== filling p3 (p2, the log partition, stays empty)"
fill 3 "$ROOTTAR"

if [ -n "$BUNDLE" ]; then
    echo "== installing bundle $BUNDLE_TAG"
    # The layout arca_check_card_mounts and arca_offline_path expect: p3 as root, p2 as log,
    # mounted, so a bundle path under /home/root/log lands on p2 and not in p3's empty mount point;
    # a path under /home/root/data is under root. p1 is not mounted.
    for rp in root:3 log:2; do
        mkdir -p "$MNT/base/${rp%%:*}" || die "mkdir base"
        mount "$(arca_part_dev "$LOOP" "${rp#*:}")" "$MNT/base/${rp%%:*}" || die "mount p${rp#*:} as ${rp%%:*}"
    done
    if ! bash "$SCRIPT_DIR/commission-offline.sh" --bundle "$BUNDLE" \
            --base "$MNT/base" < /dev/null; then
        rm -f "$OUTABS"
        die "commission-offline.sh failed (above); the image is deleted, write nothing"
    fi
    sync
    if ! ssh_scan "$MNT/base/root" "$MNT/base/log"; then
        rm -f "$OUTABS"
        die "the bundle put SSH key material on the image; the image is deleted"
    fi
    for r in root log; do umount "$MNT/base/$r" || die "umount base/$r"; done
fi

echo "== fsck"
FSCK_BAD=0
for n in 2 3; do
    if e2fsck -fn "$(arca_part_dev "$LOOP" "$n")" > "$MNT/fsck.$n" 2>&1; then
        echo "p$n: clean ($(tail -n 1 "$MNT/fsck.$n"))"
    else
        echo "p$n: e2fsck reported problems"; cat "$MNT/fsck.$n"; FSCK_BAD=1
    fi
done
if fsck.vfat -n "$P1DEV" > "$MNT/fsck.1" 2>&1; then
    echo "p1: clean ($(tail -n 1 "$MNT/fsck.1"))"
else
    echo "p1: fsck.vfat reported problems"; cat "$MNT/fsck.1"; FSCK_BAD=1
fi

losetup -d "$LOOP" && LOOP=""
sync

echo "== table check after the fill"
[ "$(table_of "$OUTABS")" = "$WANT_TABLE" ] || die "partition table changed while filling"

echo
echo "==================== SUMMARY ===================="
echo "image:           $OUTABS"
echo "size:            $(stat -c %s "$OUTABS") bytes apparent ($DISK_SECT sectors), $(du -h "$OUTABS" | awk '{print $1}') allocated"
echo "rootfs tar:      $ROOTTAR ($(stat -c %y "$ROOTTAR"))"
echo "p1:              $P1_WANT"
echo "fsck:            $([ "$FSCK_BAD" -eq 0 ] && echo 'p1, p2, p3 clean' || echo 'PROBLEMS, see above')"
echo "bundle:          ${BUNDLE_TAG:-none (no --bundle: run provision.sh and deploy-all.sh after the first boot)}"
echo "licences:        /usr/share/doc/arca-licenses: LICENSE.md, SOURCE-OFFER.txt$([ -n "$LEGAL" ] && echo ", manifest.csv, licenses/")"
if [ -z "$LEGAL" ]; then
    echo "WARNING:         no --legal-info: the package licence texts are NOT on this image. Do not publish it."
fi
if [ -n "$SOURCE_TAG" ]; then
    echo "source offer:    names ${PRODUCT_STEM}-source-${SOURCE_TAG}.tar: publish that file under release $SOURCE_TAG with this image"
else
    echo "WARNING:         no source tag (--source-tag, or a tagged --bundle): the offer names no source file. Do not publish it."
fi
echo
echo "Next, on Windows: copy the image out of WSL, e.g."
printf '  copy \\\\wsl$\\<distro>%s C:\\work\\rw-scratch\\card.img\n' "$OUTABS"
echo "(outside the repo), write it with Rufus, balenaEtcher or Win32 Disk Imager to a"
echo "card of 2 GB or more, then put it in the unit. The image holds no key and root has no"
echo "password: SSH is opened from the unit's Control Panel, SSH page (README.md, 'Getting SSH"
echo "access to a generic card')."
[ "$FSCK_BAD" -eq 0 ]
