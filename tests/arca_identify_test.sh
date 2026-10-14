#!/bin/bash
#
# arca_identify_test.sh — regression for lib/arca-identify.sh
#
# Host-only, no device, no SD card, no root. Run it:
#
#   wsl.exe -e bash -lc "cd /mnt/c/work/roomwizard && ./tests/arca_identify_test.sh"
#
# WHAT IT COVERS, and why each case is here rather than being obvious:
#
#   arca_is_rootfs      Only our own root is a rootfs: the marker is required. The case
#                     that matters: a tree with every vendor file (/opt/pv02,
#                     /opt/arca, the RW20 banner) and no marker is rejected,
#                     because nothing prepares a vendor root any more. Plus the
#                     negative control that matters: a tree that has all four
#                     required files and no marker, i.e. an ordinary Linux host's
#                     own root. The detector the UUID lookup replaced could not
#                     select the dev host; a content scan could, and case 4 forbids it.
#
#   arca_is_card_disk   Partition tables built with sfdisk on sparse files, so the
#                     positive AND the negative control are synthetic and need
#                     no card. If the two real card images happen to be present
#                     (they are gitignored) they are checked too — one of them
#                     is the unit that made the UUID approach fail. The expected
#                     numbers (start/size of the three partitions, the image end)
#                     are HARD-CODED here, not derived from the lib, so a wrong
#                     lib constant cannot agree with itself.
#
#   arca_layout_end / arca_layout_sfdisk_script
#                     The one table the card-image builder writes: exact literal
#                     output, and the output built into a sparse file is accepted.
#
#   arca_uboot_root_ok  U-Boot's compiled-in root= must be the partition this layout
#                     puts the root on. Fixtures are made with printf; no U-Boot build.
#
#   arca_part_dev       The two partition-naming schemes. /dev/sdf + 3 is
#   arca_card_partitions  /dev/sdf3 but /dev/mmcblk0 + 3 is /dev/mmcblk0p3, and
#                     getting that wrong means mounting nothing at all. Plus the
#                     assertion that matters most: p1 is NOT in the role table,
#                     so no caller can reach it through these functions.
#
#   arca_host_root_disk Resolved, not guessed. Checked against findmnt independently
#                     of the implementation, and the veto is checked in both
#                     directions — a veto that always says "no" is invisible.
#
#   arca_check_card_mounts
#                     Both halves, including the one that earns its keep: two
#                     mounts in the WRONG ORDER must be rejected. A rootfs where
#                     the log tree was expected is the mistake that makes every
#                     later path resolve under the wrong tree.
#
# The count at the end includes a check on the harness itself: a test file that
# silently ran zero cases reports success just as loudly as one that ran all of
# them.

# Source-path directive: resolves the source= hints below against this script's directory.
# shellcheck source-path=SCRIPTDIR
set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

# shellcheck source=../lib/arca-identify.sh
. "$REPO_DIR/lib/arca-identify.sh"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'

PASS=0
FAIL=0
SKIP=0

ok()      { PASS=$((PASS + 1)); echo -e "  ${GREEN}pass${NC}  $1"; }
bad()     { FAIL=$((FAIL + 1)); echo -e "  ${RED}FAIL${NC}  $1"; }
skipped() { SKIP=$((SKIP + 1)); echo -e "  ${YELLOW}skip${NC}  $1 — $2"; }

# expect_rootfs <yes|no> <dir> <description>
expect_rootfs() {
    local want="$1" dir="$2" desc="$3" got
    if arca_is_rootfs "$dir"; then got=yes; else got=no; fi
    if [ "$got" = "$want" ]; then
        ok "$desc"
    else
        bad "$desc (expected $want, got $got)"
    fi
}

# expect_disk <yes|no> <device-or-image> <description>
expect_disk() {
    local want="$1" dev="$2" desc="$3" got
    if arca_is_card_disk "$dev"; then got=yes; else got=no; fi
    if [ "$got" = "$want" ]; then
        ok "$desc"
    else
        bad "$desc (expected $want, got $got)"
    fi
}

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT INT TERM

# ── synthetic rootfs trees ──────────────────────────────────────────────────

# The four files a caller edits. Present in every case below, including the
# negative controls, so that no case passes or fails for the wrong reason.
make_required() {
    mkdir -p "$1/etc/ssh" "$1/etc/network"
    : > "$1/etc/shadow"
    : > "$1/etc/hosts"
    : > "$1/etc/ssh/sshd_config"
    : > "$1/etc/network/interfaces"
}

echo ""
echo "arca_is_rootfs"

# 1. Our own root filesystem (rootfs/): the marker post-build.sh writes is what
#    identifies it, and nothing of the vendor's is here.
O="$TMP/ours"
make_required "$O"
echo 'Welcome to Buildroot' > "$O/etc/issue"
: > "$O/etc/arca-rootfs"
expect_rootfs yes "$O" "our own rootfs identified by etc/arca-rootfs"

# 2. THE case this file gained when vendor detection was deleted: a tree with all
#    four required files and EVERY vendor marker (/opt/pv02, /opt/arca, the
#    RW20 banner, the watchdog script) but no marker of ours. Nothing prepares a
#    vendor root any more, so accepting one would install beside a running vendor stack.
V="$TMP/vendor"
make_required "$V"
mkdir -p "$V/opt/pv02" "$V/opt/arca" "$V/opt/sbin/watchdog"
: > "$V/opt/sbin/watchdog/watchdog.sh"
echo 'SteelCase RW20 Embedded Platform (Yocto) 3.1.4 \n \l' > "$V/etc/issue"
expect_rootfs no "$V" "a tree without our marker is not a rootfs even with vendor files present"

# 3. The banner alone, the weakest vendor marker the old detector had.
B="$TMP/banner"
make_required "$B"
echo 'SteelCase RW20 Embedded Platform (Yocto) 3.1.4 \n \l' > "$B/etc/issue"
expect_rootfs no "$B" "the vendor login banner alone is not a rootfs"

# 4. THE negative control: an ordinary Linux root. Everything the required list
#    names is present; nothing identifies it as an Arca. Selecting this would
#    mean rewriting the dev host's own /etc/shadow.
H="$TMP/host"
make_required "$H"
mkdir -p "$H/opt"
echo 'Ubuntu 20.04.6 LTS \n \l' > "$H/etc/issue"
expect_rootfs no "$H" "ordinary Linux root is rejected"

# 5. An Arca non-root partition (p2/p3): no /etc at all.
D="$TMP/datapart"
mkdir -p "$D/cron" "$D/websign"
expect_rootfs no "$D" "Arca data partition is rejected"

# 6. Our marker present but a required file missing: a half-built tree.
I="$TMP/incomplete"
make_required "$I"
rm -f "$I/etc/shadow"
: > "$I/etc/arca-rootfs"
expect_rootfs no "$I" "our tree missing /etc/shadow is rejected"

# 7. Empty directory — an unmounted mount point.
mkdir -p "$TMP/empty"
expect_rootfs no "$TMP/empty" "empty directory is rejected"

# ── synthetic partition tables ──────────────────────────────────────────────

echo ""
echo "arca_is_card_disk"

if command -v sfdisk >/dev/null 2>&1; then HAVE_SFDISK=yes; else HAVE_SFDISK=no; fi

if [ "$HAVE_SFDISK" = no ]; then
    skipped "synthetic partition tables" "sfdisk not installed"
else
    # Sparse file (4 GB unless told otherwise). sfdisk on a regular file needs no
    # root and touches no block device, so these cases are safe to run anywhere.
    build_table() {
        local img="$1" table="$2" size="${3:-4G}"
        rm -f "$img"
        truncate -s "$size" "$img"
        sfdisk --no-reread --no-tell-kernel "$img" >/dev/null 2>&1 <<EOT
$table
EOT
    }

    # The Arca table, as literals: p1 FAT boot, p2 log, p3 root (last, growable).
    # Deliberately NOT built from the lib's constants, so a wrong lib constant
    # cannot agree with itself.
    ARCA_TABLE='label: dos
1 : start=63, size=144522, type=c, bootable
2 : start=147456, size=2097152, type=83
3 : start=2244608, size=1048576, type=83'

    if build_table "$TMP/good.img" "$ARCA_TABLE"; then
        expect_disk yes "$TMP/good.img" "Arca layout (p1 boot, p2 log, p3 root)"
    else
        skipped "Arca layout" "sfdisk could not write the table"
    fi

    # p3 grown to 3x on a bigger card, same start: ACCEPTED. p3's size is the one
    # field the layout does not pin, because growing it is the point.
    GROWN_TABLE="${ARCA_TABLE/3 : start=2244608, size=1048576/3 : start=2244608, size=3145728}"
    if build_table "$TMP/grown.img" "$GROWN_TABLE"; then
        expect_disk yes "$TMP/grown.img" "p3 grown to 3x size (same start) is accepted"
    else
        skipped "p3 grown" "sfdisk could not write the table"
    fi

    # Moved starts. Everything else identical, so each fails on exactly one field --
    # if the check were a no-op returning 0, these cases are what catch it.
    P3MOVED_TABLE="${ARCA_TABLE/3 : start=2244608/3 : start=2244610}"
    if build_table "$TMP/p3moved.img" "$P3MOVED_TABLE"; then
        expect_disk no "$TMP/p3moved.img" "p3 start moved by 2 sectors is rejected"
    else
        skipped "p3 moved" "sfdisk could not write the table"
    fi

    MOVED_TABLE=$(echo "$ARCA_TABLE" | sed 's/^2 : start=147456/2 : start=147458/')
    if build_table "$TMP/moved.img" "$MOVED_TABLE"; then
        expect_disk no "$TMP/moved.img" "p2 start moved by 2 sectors is rejected"
    else
        skipped "p2 moved" "sfdisk could not write the table"
    fi

    # p2 is fixed-size: a different size is a different layout.
    P2SIZE_TABLE="${ARCA_TABLE/2 : start=147456, size=2097152/2 : start=147456, size=2097000}"
    if build_table "$TMP/p2size.img" "$P2SIZE_TABLE"; then
        expect_disk no "$TMP/p2size.img" "p2 size changed is rejected"
    else
        skipped "p2 size" "sfdisk could not write the table"
    fi

    # The previous seven-partition layout (p4 extended, p5, p6 root, p7 swap) and
    # the vendor's own 8 GB table (p4/p7 absorb the card size) are not this layout.
    OLD_TABLE='label: dos
1 : start=63, size=144522, type=c, bootable
2 : start=144585, size=514080, type=83
3 : start=658665, size=498015, type=83
4 : start=1156680, size=7000000, type=5
5 : start=1156743, size=2939832, type=83
6 : start=4096638, size=2008062, type=83
7 : start=6104763, size=400000, type=82'
    if build_table "$TMP/old7.img" "$OLD_TABLE"; then
        expect_disk no "$TMP/old7.img" "the old seven-partition table is rejected"
    else
        skipped "old seven-partition table" "sfdisk could not write the table"
    fi

    VENDOR8_TABLE='label: dos
1 : start=63, size=144522, type=c, bootable
2 : start=144585, size=514080, type=83
3 : start=658665, size=498015, type=83
4 : start=1156680, size=12000000, type=5
5 : start=1156743, size=2939832, type=83
6 : start=4096638, size=2008062, type=83
7 : start=6104763, size=6000000, type=82'
    if build_table "$TMP/vendor8.img" "$VENDOR8_TABLE" 8G; then
        expect_disk no "$TMP/vendor8.img" "the vendor 8 GB table is rejected"
    else
        skipped "vendor 8 GB table" "sfdisk could not write the table"
    fi

    if build_table "$TMP/plain.img" 'label: dos
1 : start=2048, size=8000000, type=83'; then
        expect_disk no "$TMP/plain.img" "ordinary single-partition disk is rejected"
    else
        skipped "single-partition disk" "sfdisk could not write the table"
    fi

    # The builder's own script, written into a sparse file, is accepted: the table
    # make-card-image.sh writes is the table arca_is_card_disk recognises.
    if build_table "$TMP/script.img" "$(arca_layout_sfdisk_script)"; then
        expect_disk yes "$TMP/script.img" "the table arca_layout_sfdisk_script prints is accepted"
    else
        skipped "arca_layout_sfdisk_script table" "sfdisk could not write the table"
    fi

    expect_disk no "$TMP/empty" "a directory is rejected"
fi

# ── the four mounts, by position ────────────────────────────────────────────

# expect_eq <want> <got> <description>
expect_eq() {
    if [ "$1" = "$2" ]; then
        ok "$3"
    else
        bad "$3 (expected '$1', got '$2')"
    fi
}

echo ""
echo "arca_layout_end / arca_layout_sfdisk_script"

# Literals, not derived from the lib: p3 starts at 2244608 and is 1048576 long.
expect_eq "3293184" "$(arca_layout_end)" "the image ends at sector 3293184 (p3 start + size)"
expect_eq "3" "${ARCA_LAYOUT_GROW:-}" "p3 is the growable partition"

EXPECT_SCRIPT='label: dos
label-id: 0x00000000
unit: sectors

x1 : start=63, size=144522, type=c, bootable
x2 : start=147456, size=2097152, type=83
x3 : start=2244608, size=1048576, type=83'
expect_eq "$EXPECT_SCRIPT" "$(arca_layout_sfdisk_script)" "the sfdisk script is exactly the three-partition table, p1 bootable"
expect_eq "3" "$(arca_layout_sfdisk_script | grep -c '^x[0-9] :')" "the sfdisk script has exactly three partition lines"

echo ""
echo "arca_uboot_root_ok"

# Fixtures: the string U-Boot carries in its built-in environment, between NULs.
# stderr is captured separately so a case can assert WHICH refusal fired -- a
# function that always refuses would otherwise pass every refusal case.
UB="$TMP/uboot"
mkdir -p "$UB"
printf 'junk\0sysroot=/dev/mmcblk0p3 rw\0sysrootfstype=ext4 rootwait\0' > "$UB/p3.bin"
printf 'junk\0sysroot=/dev/mmcblk0p6 rw\0sysrootfstype=ext4 rootwait\0' > "$UB/p6.bin"
printf 'junk\0nothing to see\0' > "$UB/nosys.bin"
: > "$UB/empty.bin"
# First match wins: a stray p6 string AFTER the real p3 one is not looked at.
printf 'junk\0sysroot=/dev/mmcblk0p3 rw\0more\0sysroot=/dev/mmcblk0p6 rw\0' > "$UB/p3then6.bin"

# expect_uboot <yes|no> <file> <stderr-pattern or -> <description>
expect_uboot() {
    local want="$1" file="$2" pat="$3" desc="$4" got err
    if err=$(arca_uboot_root_ok "$file" 2>&1 >/dev/null); then got=yes; else got=no; fi
    if [ "$got" != "$want" ]; then
        bad "$desc (expected $want, got $got)"
    elif [ "$pat" != "-" ] && ! echo "$err" | grep -q -- "$pat"; then
        bad "$desc (refused, but the message lacks '$pat': $err)"
    else
        ok "$desc"
    fi
}
expect_uboot yes "$UB/p3.bin"       -                     "a U-Boot booting root=p3 is accepted"
expect_uboot no  "$UB/p6.bin"       "mmcblk0p6.*root on p3" "a U-Boot booting root=p6 is refused, naming p6 and p3"
expect_uboot no  "$UB/nosys.bin"    "sysroot"             "a file with no sysroot= string is refused"
expect_uboot no  "$UB/empty.bin"    "sysroot"             "an empty file is refused"
expect_uboot no  "$UB/does-not-exist" "does-not-exist: not found" "a missing file is refused"
expect_uboot yes "$UB/p3then6.bin"  -                     "p3 string plus a later stray p6 string: first match wins, accepted"

echo ""
echo "arca_part_dev"

# The scheme split. /dev/sdf3 vs /dev/mmcblk0p3: appending the number naively
# gives /dev/mmcblk03, which does not exist, so an offline tool would find no
# partitions on the device's OWN naming scheme and report "not an Arca card".
expect_eq "/dev/sdf3"        "$(arca_part_dev /dev/sdf 3)"        "sd disk: /dev/sdf + 3"
expect_eq "/dev/mmcblk0p3"   "$(arca_part_dev /dev/mmcblk0 3)"    "mmcblk: a 'p' is inserted"
expect_eq "/dev/loop3p2"     "$(arca_part_dev /dev/loop3 2)"      "loop device: a 'p' is inserted"
expect_eq "/dev/nvme0n1p5"   "$(arca_part_dev /dev/nvme0n1 5)"    "nvme: a 'p' is inserted"

echo ""
echo "arca_card_partitions"

PARTS=$(arca_card_partitions /dev/mmcblk0)
expect_eq "root /dev/mmcblk0p3" "$(echo "$PARTS" | grep '^root ')"   "p3 is root"
expect_eq "log /dev/mmcblk0p2"  "$(echo "$PARTS" | grep '^log ')"    "p2 is log"
expect_eq "2" "$(echo "$PARTS" | grep -c .)"                         "exactly two roles"
expect_eq "0" "$(echo "$PARTS" | grep -c '^data ')"                  "there is no data role (data is a plain directory on root)"

# ⚠️ THE assertion in this section. p1 carries mlo, u-boot.bin, ctrlblock.bin and
# uImage-system; an untouched p1 is what keeps a power cycle a free undo. If it
# ever appears in the role table, every consumer gains the ability to write to it
# — so the table is checked for its absence rather than the consumers for their
# restraint. The old p4-p7 numbers likewise have nothing to mount (and do not exist).
for forbidden in 1 4 5 6 7; do
    if echo "$PARTS" | grep -q "p${forbidden}\$"; then
        bad "p$forbidden must NOT be in the role table"
    else
        ok "p$forbidden is absent from the role table"
    fi
done

echo ""
echo "arca_role_device_path"
expect_eq "/"                    "$(arca_role_device_path root)"   "root  → /"
expect_eq "/home/root/log"       "$(arca_role_device_path log)"    "log   → /home/root/log"
if arca_role_device_path data >/dev/null 2>&1; then
    bad "data is not a role any more and must not resolve to a path"
else
    ok "data does not resolve as a role"
fi
if arca_role_device_path boot >/dev/null 2>&1 || arca_role_device_path backup >/dev/null 2>&1; then
    bad "an unknown role (boot, and the retired backup) must not resolve to a path"
else
    ok "an unknown role does not resolve"
fi

echo ""
echo "arca_offline_path / arca_offline_base_ok"
expect_eq "/mnt/x/root/etc/rc5.d"      "$(arca_offline_path /mnt/x /etc/rc5.d)"                "/etc → p3"
expect_eq "/mnt/x/root/home/root/data/websign" "$(arca_offline_path /mnt/x /home/root/data/websign)" "data is a plain directory on root (p3), not a mount"
expect_eq "/mnt/x/log/Xorg.0.log"      "$(arca_offline_path /mnt/x /home/root/log/Xorg.0.log)" "log → p2"
expect_eq "/mnt/x/root/home/root/backup/factory" "$(arca_offline_path /mnt/x /home/root/backup/factory)" "backup is not a role: under p3"
# Longest prefix wins: root's device path "/" prefixes everything, and
# /home/root itself is NOT one of the mount points.
expect_eq "/mnt/x/root/home/root/.ssh" "$(arca_offline_path /mnt/x /home/root/.ssh)"           "/home/root/.ssh → p3, not p2"
expect_eq "/mnt/x/root/home/rootless"  "$(arca_offline_path /mnt/x /home/rootless)"            "a partial-component prefix does not count"
expect_eq "/mnt/x/log"                 "$(arca_offline_path /mnt/x /home/root/log)"            "the mount point itself maps to the mount"
if arca_offline_path /mnt/x etc/rc5.d >/dev/null 2>&1; then
    bad "a relative device path must not resolve"
else
    ok "a relative device path does not resolve"
fi

# The guard every offline writer calls first: unprefixed, the rules resolve to
# this host's own /etc, /opt and /usr/lib.
mkdir -p "$TMP/offline-base"
for b in "" "/" "/." "//" "$TMP/offline-base/../offline-base" "$TMP/does-not-exist"; do
    if arca_offline_base_ok "$b" 2>/dev/null; then
        bad "base '$b' must be refused"
    else
        ok "base '$b' is refused"
    fi
done
# The positive control: a guard that refuses everything is invisible.
if arca_offline_base_ok "$TMP/offline-base" 2>/dev/null; then
    ok "a real directory is accepted as a base"
else
    bad "a real directory must be accepted as a base"
fi

echo ""
echo "arca_host_root_disk / arca_is_host_root_disk"

HOST_ROOT_DISK=$(arca_host_root_disk 2>/dev/null || true)
if [ -z "$HOST_ROOT_DISK" ]; then
    skipped "host root disk" "lsblk/findmnt could not resolve /"
    skipped "root-disk veto (positive)" "no root disk resolved"
    skipped "root-disk veto (negative)" "no root disk resolved"
else
    # Cross-checked against findmnt directly rather than against the function's
    # own output, so this is a second opinion and not a tautology.
    ROOT_SRC=$(findmnt -no SOURCE --target / 2>/dev/null)
    case "$ROOT_SRC" in
        *"$HOST_ROOT_DISK"*) ok "root disk '$HOST_ROOT_DISK' is a prefix of '$ROOT_SRC'" ;;
        *) bad "root disk '$HOST_ROOT_DISK' does not appear in '$ROOT_SRC'" ;;
    esac

    # Both directions. A veto stuck at "yes" blocks every card; one stuck at "no"
    # is worse — it silently permits writing to this host's own disk, which is
    # the one mistake here with no undo.
    if arca_is_host_root_disk "/dev/$HOST_ROOT_DISK"; then
        ok "the host root disk is vetoed"
    else
        bad "the host root disk is NOT vetoed"
    fi
    if arca_is_host_root_disk "/dev/arca-no-such-disk-$$"; then
        bad "a nonexistent device must not be vetoed as the root disk"
    else
        ok "a nonexistent device is not vetoed"
    fi
fi

echo ""
echo "arca_check_card_mounts"

# A correctly mounted card: rootfs at root/, the log tree beside it. The log tree
# is built from what that partition holds (syslog output), so the negative half
# of the check is exercised against realistic input rather than an empty directory.
# /home/root/data is a plain directory inside root, not a mount.
GOOD="$TMP/mnt-good"
mkdir -p "$GOOD"
cp -a "$O" "$GOOD/root"
mkdir -p "$GOOD/log/arca"
: > "$GOOD/log/messages"
if OUT=$(arca_check_card_mounts "$GOOD"); then
    ok "a correctly mounted card passes"
else
    bad "a correctly mounted card was rejected: $OUT"
fi

# ⚠️ The case this function exists for: the root tree mounted where the log tree was expected. Two
# rootfs trees, so `root/` still looks right and only the SECOND half of the check
# can catch it. Without that half the clean runs against the wrong tree and
# reports having deleted nothing.
SWAPPED="$TMP/mnt-swapped"
mkdir -p "$SWAPPED"
cp -a "$O" "$SWAPPED/root"
cp -a "$O" "$SWAPPED/log"
if arca_check_card_mounts "$SWAPPED" >/dev/null; then
    bad "a rootfs mounted as 'log' must be rejected"
else
    ok "a rootfs mounted as 'log' is rejected (wrong partition order)"
fi

# root/ is not a rootfs at all — e.g. the log partition mounted alone, which is what happens if
# the partition numbers are read off a differently-partitioned card.
NOROOT="$TMP/mnt-noroot"
mkdir -p "$NOROOT/root/cron" "$NOROOT/log"
if arca_check_card_mounts "$NOROOT" >/dev/null; then
    bad "a non-rootfs mounted as 'root' must be rejected"
else
    ok "a non-rootfs mounted as 'root' is rejected"
fi

# ── the real card images, when they are here ────────────────────────────────
#
# Both are gitignored, so this section is opportunistic. It is the only part
# that exercises a real vendor partition table (a free negative control: both images
# carry the old seven-partition layout), and roomvizard_new.img is the
# card that shares no filesystem UUID with the reference unit — the failure
# this whole file exists because of.

echo ""
echo "real card images (gitignored; skipped when absent)"
for img in arca.img roomvizard_new.img; do
    if [ ! -f "$REPO_DIR/$img" ]; then
        skipped "$img" "not present in $REPO_DIR"
    elif [ "$HAVE_SFDISK" = no ]; then
        skipped "$img" "sfdisk not installed, so no table can be read"
    else
        expect_disk no "$REPO_DIR/$img" "$img carries the OLD (vendor-derived) table, not the three-partition layout"
    fi
done

# ── result, including a check on the harness ────────────────────────────────

echo ""
TOTAL=$((PASS + FAIL))
echo "  $PASS passed, $FAIL failed, $SKIP skipped"

# A harness that runs nothing reports success.  The non-skippable cases are
# (counted by RUNNING the file, per section, not by hand):
#   7  arca_is_rootfs
#  10  arca_is_card_disk (synthetic tables, incl. the directory) -- these need sfdisk
#   4  arca_layout_end / arca_layout_sfdisk_script
#   6  arca_uboot_root_ok
#   4  arca_part_dev
#   9  arca_card_partitions (4 role lines + no-data) + 5 forbidden-partition assertions
#   4  arca_role_device_path
#  15  arca_offline_path (8) + arca_offline_base_ok (7)
#   3  arca_check_card_mounts
# = 62.   arca_host_root_disk's 3 are skippable (they need a working lsblk), and the
# real card images are gitignored, so neither is counted.  The 10 synthetic
# arca_is_card_disk cases need sfdisk, so on a host without it the floor drops to
# 52 -- otherwise fixing the skip above just trades a red FAIL for a red harness
# error, which is the same defect wearing a different label.
MIN_CASES=62
[ "$HAVE_SFDISK" = no ] && MIN_CASES=52
if [ "$TOTAL" -lt "$MIN_CASES" ]; then
    echo -e "  ${RED}HARNESS ERROR${NC}: only $TOTAL cases ran, expected at least $MIN_CASES."
    echo "  Cases were skipped that cannot be skipped, or the file was truncated."
    exit 2
fi

if [ "$FAIL" -gt 0 ]; then
    exit 1
fi
echo -e "  ${GREEN}all good${NC}"
