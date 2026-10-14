#!/bin/bash
#
# arca_growroot_test.sh — regression for rootfs/board/arca/overlay/etc/init.d/growroot, the first-boot
# grow of the root (p3) and its ext4 filesystem to the end of a bigger card.
#
#   A  a card with a tail: the kernel is told, the MBR size field holds the new size (four bytes,
#      little-endian), the filesystem is grown, two console lines; a second run does nothing at all
#   B  idempotence and recovery: a full card is silent; a table already grown with the filesystem
#      still small only grows the filesystem; a tail under 1 MiB is left; a size over what the
#      32-bit field holds is clamped
#   C  refusals, each one stderr line, exit 0, nothing written and nothing run: root is not p3, a
#      partition after p3, an MBR entry that disagrees with sysfs, no 55AA signature, no p3, an
#      unreadable superblock; and a failing resizepart leaves the table unchanged
#
# Every group has a negative control: the same assertion run against a decoy script that breaks the
# rule must FAIL, so a green run cannot be a vacuous one.
#
# Host-only, no device, no root. Run it:
#   wsl.exe -e bash -lc "cd /mnt/c/work/roomwizard && ./tests/arca_growroot_test.sh"
# What it cannot see: BLKPG on a mounted partition and an online resize2fs, which only a unit does.

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
GROW="$REPO_DIR/rootfs/board/arca/overlay/etc/init.d/growroot"

RED='\033[0;31m'; GREEN='\033[0;32m'; NC='\033[0m'
PASS=0
FAIL=0
ok()  { PASS=$((PASS + 1)); echo -e "  ${GREEN}pass${NC}  $1"; }
bad() { FAIL=$((FAIL + 1)); echo -e "  ${RED}FAIL${NC}  $1"; }
check() { local d="$1"; shift; if "$@"; then ok "$d"; else bad "$d"; fi; }
control() { local d="$1"; shift; if "$@"; then bad "$d (the assertion passed against a decoy)"; else ok "$d"; fi; }

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT INT TERM

# The layout of rootfs/make-card-image.sh: p1 63+144522, p2 147456+2097152, p3 2244608+1048576.
P3START=2244608
P3SIZE=1048576
CARD_FULL=$((P3START + P3SIZE))

# put32 <file> <offset> <value>: four little-endian bytes
put32() {
    local v=$3 b
    b=$(printf '\\%03o\\%03o\\%03o\\%03o' $((v & 255)) $(((v >> 8) & 255)) $(((v >> 16) & 255)) $(((v >> 24) & 255)))
    # shellcheck disable=SC2059  # the escapes are the format
    printf "$b" | dd of="$1" bs=1 seek="$2" conv=notrunc 2> /dev/null
}
# get32 <file> <offset>
get32() {
    # shellcheck disable=SC2046  # the od fields are meant to split
    set -- $(dd if="$1" bs=1 skip="$2" count=4 2> /dev/null | od -An -tu1)
    echo $(($1 + ($2 << 8) + ($3 << 16) + ($4 << 24)))
}

# mkfix <dir> <disk-sectors>: disk image with an MBR, p3 with an ext superblock, a sysfs tree,
# cmdline, and stubs that record their arguments (and, like the real tools, change what the next run sees)
mkfix() {
    local d="$1" disk="$2"
    rm -rf "$d"; mkdir -p "$d/dev" "$d/sys/mmcblk0/mmcblk0p1" "$d/sys/mmcblk0/mmcblk0p2" "$d/sys/mmcblk0/mmcblk0p3"
    : > "$d/dev/mmcblk0"
    dd if=/dev/zero of="$d/dev/mmcblk0" bs=512 count=1 2> /dev/null
    put32 "$d/dev/mmcblk0" 454 63;       put32 "$d/dev/mmcblk0" 458 144522
    put32 "$d/dev/mmcblk0" 470 147456;   put32 "$d/dev/mmcblk0" 474 2097152
    put32 "$d/dev/mmcblk0" 486 $P3START; put32 "$d/dev/mmcblk0" 490 $P3SIZE
    printf '\125\252' | dd of="$d/dev/mmcblk0" bs=1 seek=510 conv=notrunc 2> /dev/null
    dd if=/dev/zero of="$d/dev/mmcblk0p3" bs=1024 count=4 2> /dev/null
    put32 "$d/dev/mmcblk0p3" 1028 131072    # s_blocks_count: 512 MiB of 4 KiB blocks
    put32 "$d/dev/mmcblk0p3" 1048 2         # s_log_block_size: 1 KiB << 2
    echo "$disk" > "$d/sys/mmcblk0/size"
    echo 63 > "$d/sys/mmcblk0/mmcblk0p1/start";      echo 144522 > "$d/sys/mmcblk0/mmcblk0p1/size"
    echo 147456 > "$d/sys/mmcblk0/mmcblk0p2/start";  echo 2097152 > "$d/sys/mmcblk0/mmcblk0p2/size"
    echo $P3START > "$d/sys/mmcblk0/mmcblk0p3/start"; echo $P3SIZE > "$d/sys/mmcblk0/mmcblk0p3/size"
    echo "console=ttyO2,115200n8 root=/dev/mmcblk0p3 rw" > "$d/cmdline"
    cat > "$d/resizepart" << 'EOF'
#!/bin/sh
echo "$@" >> "$0.calls"
[ -e "$0.fail" ] && exit 1
echo "$3" > "$(dirname "$0")/sys/mmcblk0/mmcblk0p3/size"
EOF
    cat > "$d/resize2fs" << 'EOF'
#!/bin/sh
echo "$@" >> "$0.calls"
# the real tool fills the partition: record that in the superblock (blocks of 4 KiB = 8 sectors)
sz=$(cat "$(dirname "$0")/sys/mmcblk0/mmcblk0p3/size")
b=$((sz / 8))
printf "$(printf '\\%03o\\%03o\\%03o\\%03o' $((b & 255)) $(((b >> 8) & 255)) $(((b >> 16) & 255)) $(((b >> 24) & 255)))" |
    dd of="$1" bs=1 seek=1028 conv=notrunc 2> /dev/null
EOF
    chmod +x "$d/resizepart" "$d/resize2fs"
}
# run <dir> [script]: start, stdout to $d/out, stderr to $d/err, status to $d/rc
run() {
    local d="$1" s="${2:-$GROW}"
    ARCA_SYS="$d/sys" ARCA_DEVDIR="$d/dev" ARCA_CMDLINE="$d/cmdline" \
        ARCA_RESIZEPART="$d/resizepart" ARCA_RESIZE2FS="$d/resize2fs" \
        dash "$s" start > "$d/out" 2> "$d/err"
    echo $? > "$d/rc"
}
calls() { cat "$1/$2.calls" 2> /dev/null; }
nlines() { wc -l < "$1" | tr -d ' '; }

BIG=15597568   # the 8 GB card of unit .188
NEWSZ=$((BIG - P3START))

echo "A  a card with a tail"
mkfix "$TMP/a" $BIG
cp "$TMP/a/dev/mmcblk0" "$TMP/a/mbr.before"
run "$TMP/a"
a1() { [ "$(cat "$TMP/a/rc")" = 0 ] && [ "$(get32 "$TMP/a/dev/mmcblk0" 490)" = "$NEWSZ" ]; }
check "A1 exit 0 and the MBR size field holds the new size (disk end minus p3 start)" a1
a2() {
    cmp -s <(head -c 490 "$TMP/a/mbr.before") <(head -c 490 "$TMP/a/dev/mmcblk0") \
        && cmp -s <(tail -c +495 "$TMP/a/mbr.before") <(tail -c +495 "$TMP/a/dev/mmcblk0")
}
check "A2 only the four size bytes changed: every other byte of the MBR is as it was" a2
check "A3 the kernel was told: resizepart <disk> 3 <new size>" \
    [ "$(calls "$TMP/a" resizepart)" = "$TMP/a/dev/mmcblk0 3 $NEWSZ" ]
check "A4 the filesystem was grown on p3" [ "$(calls "$TMP/a" resize2fs)" = "$TMP/a/dev/mmcblk0p3" ]
check "A5 two console lines, no stderr" bash -c "[ $(nlines "$TMP/a/out") -eq 2 ] && [ ! -s '$TMP/a/err' ]"
cp "$TMP/a/dev/mmcblk0" "$TMP/a/mbr.after"
run "$TMP/a"
a6() {
    [ ! -s "$TMP/a/out" ] && [ ! -s "$TMP/a/err" ] && cmp -s "$TMP/a/mbr.after" "$TMP/a/dev/mmcblk0" \
        && [ "$(nlines "$TMP/a/resizepart.calls")" -eq 1 ] && [ "$(nlines "$TMP/a/resize2fs.calls")" -eq 1 ]
}
check "A6 a second run prints nothing, writes nothing and runs nothing" a6
nogrow="$TMP/nogrow.sh"; printf '#!/bin/sh\nexit 0\n' > "$nogrow"
mkfix "$TMP/ac" $BIG; run "$TMP/ac" "$nogrow"
a7() { [ "$(cat "$TMP/ac/rc")" = 0 ] && [ "$(get32 "$TMP/ac/dev/mmcblk0" 490)" = "$NEWSZ" ]; }
control "A7 control: a script that never grows fails assertion A1" a7

echo "B  idempotence, recovery, limits"
mkfix "$TMP/b1" $CARD_FULL; cp "$TMP/b1/dev/mmcblk0" "$TMP/b1/mbr.before"; run "$TMP/b1"
b1() {
    [ ! -s "$TMP/b1/out" ] && [ ! -s "$TMP/b1/err" ] && [ ! -e "$TMP/b1/resizepart.calls" ] \
        && [ ! -e "$TMP/b1/resize2fs.calls" ] && cmp -s "$TMP/b1/mbr.before" "$TMP/b1/dev/mmcblk0"
}
check "B1 a card with no tail: silent, nothing written, nothing run" b1
# the table was grown on an earlier boot, the filesystem was not
mkfix "$TMP/b2" $BIG
put32 "$TMP/b2/dev/mmcblk0" 490 $NEWSZ; echo $NEWSZ > "$TMP/b2/sys/mmcblk0/mmcblk0p3/size"
run "$TMP/b2"
b2() { [ ! -e "$TMP/b2/resizepart.calls" ] && [ "$(calls "$TMP/b2" resize2fs)" = "$TMP/b2/dev/mmcblk0p3" ] && [ "$(cat "$TMP/b2/rc")" = 0 ]; }
check "B2 table already grown, filesystem small: only the filesystem is grown" b2
mkfix "$TMP/b3" $((CARD_FULL + 1000)); run "$TMP/b3"
b3() { [ ! -s "$TMP/b3/out" ] && [ ! -e "$TMP/b3/resizepart.calls" ] && [ "$(get32 "$TMP/b3/dev/mmcblk0" 490)" = $P3SIZE ]; }
check "B3 a tail under 1 MiB is left alone" b3
HUGE=20000000000   # sectors, beyond what a 32-bit field holds
mkfix "$TMP/b4" $HUGE; run "$TMP/b4"
want=$((4294967295 - P3START))
check "B4 a size over 32 bits is clamped to what the field holds" [ "$(get32 "$TMP/b4/dev/mmcblk0" 490)" = "$want" ]
mkfix "$TMP/b5" $((CARD_FULL + 1000)); sed '/^MIN_GAP=/s/2048/2/' "$GROW" > "$TMP/b5.sh"
run "$TMP/b5" "$TMP/b5.sh"
b5() { [ ! -s "$TMP/b5/out" ] && [ ! -e "$TMP/b5/resizepart.calls" ]; }
control "B5 control: with the 1 MiB threshold lowered, the small tail is no longer left alone" b5

echo "C  refusals"
# refused <dir> <reason-words>: exit 0, exactly one stderr line, no stdout, MBR unchanged, nothing run
refused() {
    [ "$(cat "$1/rc")" = 0 ] && [ "$(nlines "$1/err")" -eq 1 ] && [ ! -s "$1/out" ] \
        && cmp -s "$1/mbr.before" "$1/dev/mmcblk0" && [ ! -e "$1/resize2fs.calls" ] \
        && { [ ! -e "$1/resizepart.calls" ] || [ -e "$1/resizepart.fail" ]; }
}
setup_c() { mkfix "$TMP/$1" $BIG; cp "$TMP/$1/dev/mmcblk0" "$TMP/$1/mbr.before"; }
setup_c c1; echo "console=ttyO2 root=/dev/mmcblk0p5 rw" > "$TMP/c1/cmdline"; run "$TMP/c1"
check "C1 root is not p3: refused" refused "$TMP/c1"
setup_c c2; mkdir "$TMP/c2/sys/mmcblk0/mmcblk0p4"; echo $((P3START + P3SIZE)) > "$TMP/c2/sys/mmcblk0/mmcblk0p4/start"; run "$TMP/c2"
check "C2 a partition after p3: refused" refused "$TMP/c2"
setup_c c3; put32 "$TMP/c3/dev/mmcblk0" 490 12345; cp "$TMP/c3/dev/mmcblk0" "$TMP/c3/mbr.before"; run "$TMP/c3"
check "C3 the MBR entry disagrees with sysfs: refused" refused "$TMP/c3"
setup_c c4; printf '\000\000' | dd of="$TMP/c4/dev/mmcblk0" bs=1 seek=510 conv=notrunc 2> /dev/null; cp "$TMP/c4/dev/mmcblk0" "$TMP/c4/mbr.before"; run "$TMP/c4"
check "C4 no 55AA signature: refused" refused "$TMP/c4"
setup_c c5; rm -rf "$TMP/c5/sys/mmcblk0/mmcblk0p3"; run "$TMP/c5"
check "C5 no p3 in sysfs: refused" refused "$TMP/c5"
setup_c c6; : > "$TMP/c6/resizepart.fail"; run "$TMP/c6"
c6() {   # it announced the grow (stdout) before resizepart failed; the rest is a refusal
    [ "$(cat "$TMP/c6/rc")" = 0 ] && [ "$(nlines "$TMP/c6/err")" -eq 1 ] && cmp -s "$TMP/c6/mbr.before" "$TMP/c6/dev/mmcblk0" \
        && [ ! -e "$TMP/c6/resize2fs.calls" ]
}
check "C6 resizepart fails: the table is unchanged, one stderr line, the filesystem is not touched" c6
mkfix "$TMP/c7" $CARD_FULL; put32 "$TMP/c7/dev/mmcblk0p3" 1048 30; echo $((P3SIZE + 100000)) > "$TMP/c7/sys/mmcblk0/mmcblk0p3/size"
cp "$TMP/c7/dev/mmcblk0" "$TMP/c7/mbr.before"; run "$TMP/c7"
check "C7 an unreadable superblock (impossible block size): the filesystem is not touched, one line" refused "$TMP/c7"
# decoy: the same script with the root= check removed must trip C1
sed 's/^        if ! grep -qE .*; then$/        if false; then/' "$GROW" > "$TMP/decoy-noroot.sh"
check "C8 the decoy really lost the root= check" bash -c "! diff -q '$GROW' '$TMP/decoy-noroot.sh' > /dev/null"
setup_c c9; echo "console=ttyO2 root=/dev/mmcblk0p5 rw" > "$TMP/c9/cmdline"; run "$TMP/c9" "$TMP/decoy-noroot.sh"
control "C9 control: without the root= check, assertion C1 fails (it grows a card it should refuse)" refused "$TMP/c9"
# decoy: a script that dies on stdout garbage must fail the one-line-stderr rule
chatty="$TMP/chatty.sh"; printf '#!/bin/sh\necho hello\nexit 0\n' > "$chatty"
setup_c c10; run "$TMP/c10" "$chatty"
control "C10 control: a script that prints to stdout and stderr nothing fails the refusal assertion" refused "$TMP/c10"

echo ""
TOTAL=$((PASS + FAIL))
echo "  $PASS passed, $FAIL failed"
MIN_CASES=22
if [ "$TOTAL" -lt "$MIN_CASES" ]; then
    echo -e "  ${RED}HARNESS ERROR${NC}: only $TOTAL cases ran, expected at least $MIN_CASES."
    exit 2
fi
[ "$FAIL" -eq 0 ] || exit 1
echo -e "  ${GREEN}all good${NC}"
