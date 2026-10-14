#!/bin/bash
#
# card_layout_test.sh — the card layout rootfs/make-card-image.sh builds is the layout
# lib/arca-identify.sh recognises, and a U-Boot built for another layout is refused.
#
# Host-only, no device, no SD card, no root. Run it:
#
#   wsl.exe -e bash -lc "cd /mnt/c/work/roomwizard && ./tests/card_layout_test.sh"
#
# The layout (one table, lib/arca-identify.sh ARCA_LAYOUT): MBR, three primaries.
#   p1  FAT boot, start 63, 144522 sectors, type c, bootable
#   p2  log, start 147456, 2097152 sectors (1 GiB), type 83
#   p3  root (+ /home/root/data as a plain directory), start 2244608, 1048576 sectors,
#       type 83, the LAST partition and the growable one
#
# GROUPS
#   A  arithmetic and text, no tool needed (the non-skippable floor):
#        the numbers above as HARD-CODED literals compared with the lib, so a wrong lib
#        constant cannot agree with itself; 8192-sector alignment of p2/p3; adjacency,
#        no overlap; ARCA_PART_ROLES, fstab and the U-Boot source agree on p2 and p3;
#        and TEXT gates over make-card-image.sh (it defines no table of its own, it calls
#        the lib's two functions, no EBR / swap / p5 code is left, the U-Boot refusal sits
#        ahead of the root check). A text gate shows what the script says, not what it does.
#   B  the arca_uboot_root_ok function over printf fixtures, and the real u-boot.img from
#        uboot/build.sh when ~/rw-uboot-out has one (a real-artifact control: whatever it
#        carries is graded against a grep of the file, so a stale p6 build must be refused).
#   C  make-card-image.sh DRIVEN as a program: a --uboot (or --uenv) for the wrong root
#        partition is refused, exit 1, a message naming mmcblk0p, and no image file is
#        created. The refusal precedes the root check, so this needs no root; the p3 fixture
#        is the control for the p6 one (it gets past the refusal and dies at the NEXT stage:
#        "run as root", or, as root, "no rootfs tar" -- the tar never exists, so this suite
#        cannot start a build even when run as root).
#   D  sfdisk on SPARSE FILES (needs sfdisk; skipped, labelled, when it is absent): the
#        table arca_layout_sfdisk_script prints, written by sfdisk, reads back as the
#        literal oracle rows and is accepted by arca_is_card_disk; negative controls: the
#        old seven-partition table and a vendor 8 GB table are rejected, p3 grown to 3x is
#        accepted, p2 start +2 is rejected, a table truncated to two entries is rejected.
#
# WHAT IT CANNOT SEE: the loop-device half of make-card-image.sh (losetup, mke2fs, the fill,
# the bundle install, fsck) needs root and is checked only by building a real image.
#
# Environment knobs (both for the sabotage sweep): MCI=<path> grades another copy of
# make-card-image.sh (its lib/ is found beside it: <dir>/../lib), ARCA_IDENTIFY_LIB=<path>
# another arca-identify.sh for the in-process cases. CARD_LAYOUT_NO_SFDISK=1 behaves as if
# sfdisk were absent (to measure the lower floor).

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
MCI="${MCI:-$REPO_DIR/rootfs/make-card-image.sh}"
LIB="${ARCA_IDENTIFY_LIB:-$REPO_DIR/lib/arca-identify.sh}"

# shellcheck disable=SC1090
. "$LIB" || { echo "cannot source $LIB"; exit 2; }

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'
PASS=0; FAIL=0; SKIP=0
ok()      { PASS=$((PASS + 1)); echo -e "  ${GREEN}pass${NC}  $1"; }
bad()     { FAIL=$((FAIL + 1)); echo -e "  ${RED}FAIL${NC}  $1"; }
skipped() { SKIP=$((SKIP + 1)); echo -e "  ${YELLOW}skip${NC}  $1 — $2"; }
expect_eq() { if [ "$1" = "$2" ]; then ok "$3"; else bad "$3 (expected '$1', got '$2')"; fi; }

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT INT TERM

if [ -f "$MCI" ]; then :; else echo "no $MCI"; exit 2; fi

# ── A. arithmetic and text ──────────────────────────────────────────────────

echo ""
echo "A. the layout, as literals"

# The oracle: typed here from the design, never derived from the lib.
ORACLE_LAYOUT="1:63:144522:c 2:147456:2097152:83 3:2244608:1048576:83"
expect_eq "$ORACLE_LAYOUT" "$ARCA_LAYOUT" "ARCA_LAYOUT is the three-partition table"
expect_eq "3293184" "$(arca_layout_end)" "the image ends at sector 3293184"
expect_eq "3" "${ARCA_LAYOUT_GROW:-}" "p3 is the growable partition"
expect_eq "3" "$(echo "$ARCA_LAYOUT" | wc -w | tr -d ' ')" "exactly three entries (no extended container, no p4)"
expect_eq "" "$(echo "$ARCA_LAYOUT" | tr ' ' '\n' | awk -F: '$4=="5"||$4=="f"||$4=="82"')" "no extended partition and no swap in the table"

P2START=$(echo "$ARCA_LAYOUT" | tr ' ' '\n' | awk -F: '$1==2{print $2}')
P2SIZE=$(echo "$ARCA_LAYOUT" | tr ' ' '\n' | awk -F: '$1==2{print $3}')
P3START=$(echo "$ARCA_LAYOUT" | tr ' ' '\n' | awk -F: '$1==3{print $2}')
P3SIZE=$(echo "$ARCA_LAYOUT" | tr ' ' '\n' | awk -F: '$1==3{print $3}')
P1END=$((63 + 144522))
if [ $((P2START % 8192)) -eq 0 ] && [ $((P3START % 8192)) -eq 0 ]; then ok "p2 and p3 start on 4 MiB (8192-sector) boundaries"; else bad "p2 start $P2START / p3 start $P3START not multiples of 8192"; fi
if [ $((P2SIZE % 8192)) -eq 0 ]; then ok "p2 size is a multiple of 8192 (so p3 is aligned too)"; else bad "p2 size $P2SIZE not a multiple of 8192"; fi
if [ "$P1END" -le "$P2START" ]; then ok "p2 starts after p1 ends (sector $P1END)"; else bad "p2 overlaps p1"; fi
expect_eq "$P3START" "$((P2START + P2SIZE))" "p3 starts exactly where p2 ends (no overlap, no gap)"
expect_eq "$(arca_layout_end)" "$((P3START + P3SIZE))" "p3 is the last partition: it ends at the image end"
expect_eq "3 2" "$(echo "$ARCA_PART_ROLES" | tr ' ' '\n' | awk -F: '{printf "%s%s", (NR>1?" ":""), $1}')" "ARCA_PART_ROLES numbers are 3 (root) and 2 (log)"
expect_eq "3:root 2:log" "$ARCA_PART_ROLES" "ARCA_PART_ROLES names root on p3 and log on p2"
if echo "$ARCA_PART_ROLES" | tr ' ' '\n' | awk -F: '$1==1{f=1} END{exit f?0:1}'; then bad "p1 must not be in ARCA_PART_ROLES"; else ok "p1 is not in ARCA_PART_ROLES"; fi

# fstab, role table and the U-Boot source name the same two partitions.
FSTAB="$REPO_DIR/rootfs/board/arca/overlay/etc/fstab"
if [ ! -f "$FSTAB" ]; then
    bad "no $FSTAB"
else
    expect_eq "/dev/mmcblk0p2" "$(awk '$1!~/^#/ && $2=="/home/root/log"{print $1}' "$FSTAB")" "fstab mounts p2 on /home/root/log"
    expect_eq "" "$(awk '$1!~/^#/ && $2=="/home/root/data"{print $1}' "$FSTAB")" "fstab has no /home/root/data mount (data is a directory on root)"
    expect_eq "/dev/mmcblk0p2" "$(awk '$1~/^\/dev\/mmcblk/{print $1}' "$FSTAB" | sort -u | tr '\n' ' ' | sed 's/ $//')" "fstab names no partition but p2 (root is the kernel's root=)"
fi
UB_PATCH="$REPO_DIR/uboot/rw20-board.patch"
if [ -f "$UB_PATCH" ]; then
    expect_eq "sysroot=/dev/mmcblk0p3 rw" "$(grep -a -o 'sysroot=/dev/mmcblk0p[0-9]* rw' "$UB_PATCH" | sort -u | tr '\n' ';' | sed 's/;$//')" "uboot/rw20-board.patch boots root=/dev/mmcblk0p3 (the root of ARCA_PART_ROLES)"
else
    bad "no $UB_PATCH"
fi
if [ -f "$REPO_DIR/uboot/uEnv.txt" ]; then
    expect_eq "" "$(grep -o 'root=/dev/mmcblk0p[0-9]*' "$REPO_DIR/uboot/uEnv.txt" | grep -v 'p3$')" "uboot/uEnv.txt names no root= but p3"
else
    bad "no uboot/uEnv.txt"
fi

echo ""
echo "A. make-card-image.sh, text gates"

if grep -q '^LAYOUT=' "$MCI"; then bad "make-card-image.sh defines a LAYOUT= of its own (the table has one home)"; else ok "make-card-image.sh defines no LAYOUT= of its own"; fi
if grep -q 'arca_layout_sfdisk_script' "$MCI"; then ok "make-card-image.sh writes the table with arca_layout_sfdisk_script"; else bad "make-card-image.sh does not call arca_layout_sfdisk_script"; fi
if grep -qF "DISK_SECT=\$(arca_layout_end)" "$MCI"; then ok "make-card-image.sh takes its size from arca_layout_end"; else bad "make-card-image.sh does not size the image with arca_layout_end"; fi
for dead in write_ebr EXT_START mkswap arca-spare 'lay 4' 'lay 7'; do
    if grep -qF -- "$dead" "$MCI"; then bad "make-card-image.sh still carries '$dead' (extended-partition, swap or p5 code)"; else ok "make-card-image.sh carries no '$dead'"; fi
done
if grep -qF "mke2fs -F -q -t \"\$1\" -b 4096" "$MCI"; then ok "mkext passes -b 4096 (the default flips to 1 KiB below 512 MiB)"; else bad "mkext does not pass -b 4096"; fi
if grep -qF 'root:3 log:2' "$MCI" && ! grep -qF 'data:2' "$MCI"; then ok "the bundle mounts are root:3 and log:2, with no data mount"; else bad "the bundle mounts are not exactly root:3 log:2"; fi

UB_CALL=$(grep -n '^arca_uboot_root_ok ' "$MCI" | head -1 | cut -d: -f1)
ID_LINE=$(grep -nF 'id -u)" -eq 0' "$MCI" | head -1 | cut -d: -f1)
BLK_LINE=$(grep -n 'is a block device' "$MCI" | head -1 | cut -d: -f1)
if [ -n "$UB_CALL" ] && [ -n "$ID_LINE" ] && [ "$UB_CALL" -lt "$ID_LINE" ]; then
    ok "the U-Boot refusal (line $UB_CALL) precedes the root check (line $ID_LINE), so it fires without root"
else
    bad "the U-Boot refusal does not precede the root check (refusal ${UB_CALL:-absent}, id -u ${ID_LINE:-absent})"
fi
if [ -n "$UB_CALL" ] && [ -n "$BLK_LINE" ] && [ "$UB_CALL" -lt "$BLK_LINE" ]; then
    ok "the U-Boot refusal precedes the block-device loop"
else
    bad "the U-Boot refusal does not precede the block-device loop"
fi

# ── B. arca_uboot_root_ok ───────────────────────────────────────────────────

echo ""
echo "B. arca_uboot_root_ok over fixtures"

UB="$TMP/ub"; mkdir -p "$UB"
printf 'junk\0sysroot=/dev/mmcblk0p3 rw\0sysrootfstype=ext4 rootwait\0' > "$UB/p3.bin"
printf 'junk\0sysroot=/dev/mmcblk0p6 rw\0sysrootfstype=ext4 rootwait\0' > "$UB/p6.bin"
printf 'junk\0sysroot=/dev/mmcblk0p3 rw\0more\0sysroot=/dev/mmcblk0p6 rw\0' > "$UB/p3-then-p6.bin"
printf 'junk with no root string at all\0' > "$UB/none.bin"
: > "$UB/empty.bin"

if arca_uboot_root_ok "$UB/p3.bin" 2> "$TMP/e"; then ok "a u-boot.img booting mmcblk0p3 is accepted"; else bad "p3 fixture refused: $(cat "$TMP/e")"; fi
if arca_uboot_root_ok "$UB/p3-then-p6.bin" 2> "$TMP/e"; then ok "p3 first with a stray later p6 string: accepted (the first match decides)"; else bad "p3-then-p6 refused: $(cat "$TMP/e")"; fi
if arca_uboot_root_ok "$UB/p6.bin" 2> "$TMP/e"; then
    bad "a u-boot.img booting mmcblk0p6 was accepted"
elif grep -q 'mmcblk0p6' "$TMP/e" && grep -q 'p3' "$TMP/e"; then
    ok "a u-boot.img booting mmcblk0p6 is refused, and the message names p6 and p3"
else
    bad "p6 fixture refused with the wrong message: $(cat "$TMP/e")"
fi
if arca_uboot_root_ok "$UB/none.bin" 2> "$TMP/e"; then bad "a file with no sysroot= string was accepted"; elif grep -q "no 'sysroot" "$TMP/e"; then ok "no sysroot= string: refused, with its own message"; else bad "no-sysroot refused with the wrong message: $(cat "$TMP/e")"; fi
if arca_uboot_root_ok "$UB/empty.bin" 2> "$TMP/e"; then bad "an empty file was accepted"; else ok "an empty file is refused"; fi
if arca_uboot_root_ok "$UB/missing.bin" 2> "$TMP/e"; then bad "a missing file was accepted"; elif grep -q 'not found' "$TMP/e"; then ok "a missing file is refused as not found"; else bad "missing refused with the wrong message: $(cat "$TMP/e")"; fi

# The real artefact, when it is there: graded against a grep of the file itself.
UBOUT="${ARCA_UBOOT_OUT:-$HOME/rw-uboot-out}/u-boot.img"
if [ ! -f "$UBOUT" ]; then
    skipped "real u-boot.img" "$UBOUT not present"
elif grep -aq 'sysroot=/dev/mmcblk0p3 rw' "$UBOUT"; then
    if arca_uboot_root_ok "$UBOUT" 2> "$TMP/e"; then ok "the real u-boot.img carries p3 and is accepted"; else bad "the real u-boot.img carries p3 but was refused: $(cat "$TMP/e")"; fi
elif grep -aq 'sysroot=/dev/mmcblk0p6 rw' "$UBOUT"; then
    if arca_uboot_root_ok "$UBOUT" 2> "$TMP/e"; then bad "the real u-boot.img carries p6 (not rebuilt) and was ACCEPTED"; else ok "the real u-boot.img still carries p6 and is refused (rebuild it with uboot/build.sh)"; fi
else
    skipped "real u-boot.img" "$UBOUT carries no sysroot= string either way"
fi
# The kept copy of the pre-layout build, when it is there: a real p6 artefact, so the refusal is
# seen on bytes uboot/build.sh made and not only on a printf fixture.
UBOLD="${ARCA_UBOOT_OUT:-$HOME/rw-uboot-out}/u-boot-p6-old.img"
if [ ! -f "$UBOLD" ]; then
    skipped "old p6 u-boot.img" "$UBOLD not present"
elif ! grep -aq 'sysroot=/dev/mmcblk0p6 rw' "$UBOLD"; then
    skipped "old p6 u-boot.img" "$UBOLD does not carry p6, so it is no control"
elif arca_uboot_root_ok "$UBOLD" 2> "$TMP/e"; then
    bad "the real p6 u-boot.img was ACCEPTED"
else
    ok "the real p6 u-boot.img is refused"
fi

# ── C. make-card-image.sh as a program ──────────────────────────────────────

echo ""
echo "C. make-card-image.sh refuses a boot chain for another layout"

NEVER="$TMP/never.img"
# drive <args...>: stdout/stderr to files, status in DRC; the tar never exists.
drive() { rm -f "$NEVER"; bash "$MCI" "$@" > "$TMP/out" 2> "$TMP/err" < /dev/null; DRC=$?; }
# passes_refusal: the run got PAST the layout refusals (and died at a later, unrelated stage).
passes_refusal() { [ "$DRC" -eq 1 ] && ! grep -q 'card layout has its root\|bootargs' "$TMP/err" && grep -q 'run as root\|no rootfs tar' "$TMP/err" && [ ! -e "$NEVER" ]; }

printf '#bootargs=console=ttyO1 root=/dev/mmcblk0p6 rw rootwait\n# comment only\n' > "$TMP/uenv-comment-p6.txt"
printf 'kernel=uImage-system\nbootargs=console=ttyO1 root=/dev/mmcblk0p6 rw rootwait\n' > "$TMP/uenv-live-p6.txt"
printf 'kernel=uImage-system\nbootargs=console=ttyO1 root=/dev/mmcblk0p3 rw rootwait\n' > "$TMP/uenv-live-p3.txt"
printf 'kernel=uImage-system\nbootargs=console=ttyO1 rw rootwait\n' > "$TMP/uenv-live-noroot.txt"

drive --mlo m --uboot "$UB/p6.bin" --kernel k a.tar "$NEVER"
if [ "$DRC" -eq 1 ] && grep -q 'mmcblk0p' "$TMP/err" && [ ! -e "$NEVER" ]; then ok "--uboot booting p6: exit 1, stderr names mmcblk0p, no image created"; else bad "p6 --uboot: rc $DRC, image exists: $([ -e "$NEVER" ] && echo yes || echo no): $(cat "$TMP/err")"; fi
drive --mlo m --uboot "$UB/p3.bin" --kernel k a.tar "$NEVER"
if passes_refusal; then ok "--uboot booting p3 (the control) gets past the refusal: $(tail -n 1 "$TMP/err" | cut -c1-60)"; else bad "p3 --uboot did not reach the later stage: rc $DRC: $(cat "$TMP/err")"; fi
drive --mlo m --uboot "$UB/none.bin" --kernel k a.tar "$NEVER"
if [ "$DRC" -eq 1 ] && grep -q "sysroot" "$TMP/err" && [ ! -e "$NEVER" ]; then ok "--uboot with no sysroot= string is refused"; else bad "no-sysroot --uboot: rc $DRC: $(cat "$TMP/err")"; fi
drive --mlo m --uboot "$UB/missing.bin" --kernel k a.tar "$NEVER"
if [ "$DRC" -eq 1 ] && grep -q 'not found' "$TMP/err" && [ ! -e "$NEVER" ]; then ok "a missing --uboot file is refused"; else bad "missing --uboot: rc $DRC: $(cat "$TMP/err")"; fi

drive --mlo m --uboot "$UB/p3.bin" --kernel k --uenv "$TMP/uenv-live-p6.txt" a.tar "$NEVER"
if [ "$DRC" -eq 1 ] && grep -q 'mmcblk0p6' "$TMP/err" && grep -q 'bootargs' "$TMP/err" && [ ! -e "$NEVER" ]; then ok "an uncommented bootargs= with root=p6 in --uenv is refused"; else bad "live p6 uenv: rc $DRC: $(cat "$TMP/err")"; fi
drive --mlo m --uboot "$UB/p3.bin" --kernel k --uenv "$TMP/uenv-live-noroot.txt" a.tar "$NEVER"
if [ "$DRC" -eq 1 ] && grep -q 'bootargs' "$TMP/err" && [ ! -e "$NEVER" ]; then ok "an uncommented bootargs= with no root= is refused"; else bad "live no-root uenv: rc $DRC: $(cat "$TMP/err")"; fi
drive --mlo m --uboot "$UB/p3.bin" --kernel k --uenv "$TMP/uenv-live-p3.txt" a.tar "$NEVER"
if passes_refusal; then ok "an uncommented bootargs= with root=p3 passes (control)"; else bad "live p3 uenv: rc $DRC: $(cat "$TMP/err")"; fi
drive --mlo m --uboot "$UB/p3.bin" --kernel k --uenv "$TMP/uenv-comment-p6.txt" a.tar "$NEVER"
if passes_refusal; then ok "a COMMENTED bootargs= naming p6 passes (control: only live lines count)"; else bad "commented p6 uenv: rc $DRC: $(cat "$TMP/err")"; fi
if [ -f "$REPO_DIR/uboot/uEnv.txt" ]; then
    drive --mlo m --uboot "$UB/p3.bin" --kernel k --uenv "$REPO_DIR/uboot/uEnv.txt" a.tar "$NEVER"
    if passes_refusal; then ok "the shipped uboot/uEnv.txt passes the refusal"; else bad "shipped uEnv.txt: rc $DRC: $(cat "$TMP/err")"; fi
fi

# ── D. sfdisk on sparse files ───────────────────────────────────────────────

echo ""
echo "D. the partition table, written by sfdisk into sparse files"

if [ "${CARD_LAYOUT_NO_SFDISK:-0}" = 1 ] || ! command -v sfdisk > /dev/null 2>&1; then
    HAVE_SFDISK=no
    skipped "sparse-file partition tables" "sfdisk not installed (or CARD_LAYOUT_NO_SFDISK=1)"
else
    HAVE_SFDISK=yes
fi

if [ "$HAVE_SFDISK" = yes ]; then
    # table_of <image>: "<n>:<start>:<size>:<type>" per entry, " boot" on the active one.
    table_of() {
        sfdisk -d "$1" 2> /dev/null | awk '
            $0 ~ /^[^ ]+[0-9] *:/ {
                n = $1; sub(/^.*[^0-9]/, "", n)
                match($0, /start= *[0-9]+/); s = substr($0, RSTART, RLENGTH); gsub(/[^0-9]/, "", s)
                match($0, /size= *[0-9]+/);  z = substr($0, RSTART, RLENGTH); gsub(/[^0-9]/, "", z)
                match($0, /type=[0-9a-f]+/); t = substr($0, RSTART + 5, RLENGTH - 5)
                printf "%s:%s:%s:%s%s\n", n, s, z, t, ($0 ~ /bootable/ ? " boot" : "")
            }' | tr '\n' ' ' | sed 's/ $//'
    }
    # build_table <image> <sfdisk script> [size]: a sparse file, then sfdisk. No root, no loop device.
    build_table() {
        local img="$1" table="$2" size="${3:-}"
        rm -f "$img"
        if [ -n "$size" ]; then truncate -s "$size" "$img"; else truncate -s $(($(arca_layout_end) * 512)) "$img"; fi
        printf '%s\n' "$table" | sfdisk --no-reread --no-tell-kernel "$img" > /dev/null 2>&1
    }
    expect_disk() {
        local want="$1" dev="$2" desc="$3" got
        if arca_is_card_disk "$dev"; then got=yes; else got=no; fi
        if [ "$got" = "$want" ]; then ok "$desc"; else bad "$desc (expected $want, got $got)"; fi
    }

    # table_case <yes|no> <image> <table> <size or ""> <description>
    table_case() {
        if build_table "$2" "$3" "$4"; then
            expect_disk "$1" "$2" "$5"
        else
            bad "$5 (sfdisk could not write the table)"
        fi
    }

    ORACLE_ROWS="1:63:144522:c boot 2:147456:2097152:83 3:2244608:1048576:83"
    IMG="$TMP/layout.img"
    if build_table "$IMG" "$(arca_layout_sfdisk_script)"; then
        expect_eq "$ORACLE_ROWS" "$(table_of "$IMG")" "the table arca_layout_sfdisk_script prints reads back as the literal rows (three entries, p1 bootable)"
        expect_disk yes "$IMG" "arca_is_card_disk accepts the table the builder writes"
        expect_eq "0" "$(dd if="$IMG" bs=512 skip=1 count=62 status=none | tr -d '\000' | wc -c | tr -d ' ')" "sectors 1..62 are zero"
        expect_eq "$(( $(arca_layout_end) * 512 ))" "$(stat -c %s "$IMG")" "the image file is exactly arca_layout_end sectors"
        ALL_ROLES_OK=yes
        for role in $ARCA_PART_ROLES; do
            table_of "$IMG" | tr ' ' '\n' | grep -q "^${role%%:*}:" || ALL_ROLES_OK=no
        done
        expect_eq "yes" "$ALL_ROLES_OK" "every partition number in ARCA_PART_ROLES exists in the table"
        expect_eq "2244608:1048576" "$(table_of "$IMG" | tr ' ' '\n' | awk -F: '$1==3{print $2 ":" $3}')" "p3 reads back start 2244608, size 1048576"
    else
        bad "sfdisk could not write arca_layout_sfdisk_script's table"
    fi

    ARCA_TABLE='label: dos
1 : start=63, size=144522, type=c, bootable
2 : start=147456, size=2097152, type=83
3 : start=2244608, size=1048576, type=83'
    if build_table "$TMP/lit.img" "$ARCA_TABLE"; then
        expect_disk yes "$TMP/lit.img" "the literal three-partition table is accepted"
    else
        bad "sfdisk could not write the literal table"
    fi

    # Negative controls: each differs from the accepted table in one respect.
    GROWN="${ARCA_TABLE/size=1048576/size=3145728}"
    table_case yes "$TMP/grown.img" "$GROWN" "4G" "p3 grown to 3x (same start) is accepted"
    P2MOVED="${ARCA_TABLE/start=147456/start=147458}"
    table_case no "$TMP/p2moved.img" "$P2MOVED" "" "p2 start +2 sectors is rejected"
    P3MOVED="${ARCA_TABLE/start=2244608/start=2244610}"
    table_case no "$TMP/p3moved.img" "$P3MOVED" "" "p3 start +2 sectors is rejected (its start is pinned)"
    TWO='label: dos
1 : start=63, size=144522, type=c, bootable
2 : start=147456, size=2097152, type=83'
    table_case no "$TMP/two.img" "$TWO" "" "a table truncated to two entries is rejected"
    OLD7='label: dos
1 : start=63, size=144522, type=c, bootable
2 : start=144585, size=514080, type=83
3 : start=658665, size=498015, type=83
4 : start=1156680, size=5462100, type=5
5 : start=1156743, size=2939832, type=83
6 : start=4096638, size=2008062, type=83
7 : start=6104763, size=514017, type=82'
    table_case no "$TMP/old7.img" "$OLD7" "4G" "the old seven-partition table is rejected"
    VENDOR8='label: dos
1 : start=63, size=144522, type=c, bootable
2 : start=144585, size=514080, type=83
3 : start=658665, size=498015, type=83
4 : start=1156680, size=12000000, type=5
5 : start=1156743, size=2939832, type=83
6 : start=4096638, size=2008062, type=83
7 : start=6104763, size=6000000, type=82'
    table_case no "$TMP/vendor8.img" "$VENDOR8" "8G" "a vendor-style 8 GB table is rejected"
fi

# ── result, including a check on the harness ────────────────────────────────

echo ""
TOTAL=$((PASS + FAIL))
echo "  $PASS passed, $FAIL failed, $SKIP skipped"

# A harness that runs nothing reports success. Counted by RUNNING the file (the real u-boot.img
# case is skippable and not counted): MIN_FULL with sfdisk, MIN_NOSFDISK without it, because
# otherwise fixing a missing tool just trades a skip for a red harness error.
MIN_FULL=59
MIN_NOSFDISK=46
MIN_CASES=$MIN_FULL
[ "$HAVE_SFDISK" = no ] && MIN_CASES=$MIN_NOSFDISK
if [ "$TOTAL" -lt "$MIN_CASES" ]; then
    echo -e "  ${RED}HARNESS ERROR${NC}: only $TOTAL cases ran, expected at least $MIN_CASES."
    echo "  Cases were skipped that cannot be skipped, or the file was truncated."
    exit 2
fi

if [ "$FAIL" -gt 0 ]; then
    exit 1
fi
echo -e "  ${GREEN}all good${NC}"
