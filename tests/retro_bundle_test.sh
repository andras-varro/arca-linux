#!/bin/bash
#
# retro_bundle_test.sh — an image/release bundle never carries the three retro cores
#                        that embed ROMs without a redistribution grant (Enterprise,
#                        Spectrum, C64/VICE), and does carry every other core.
#
# Subject: `retro/build-and-deploy.sh --bundle <dir> --no-build`, run against a fake
# out dir of placeholder *_libretro.so files (RETRO_OUT), so no cross-build is needed.
# Checked: the bundle's manifest and its root/ tree name no excluded core, no .sys whose
# core= is excluded and nothing under an excluded core's system-dir subfolder; and each
# shipped core and .sys IS there.
#
# ⚠️ NEGATIVE CONTROL: the same check is run on a sabotaged copy of the script with the
# `vice` entry removed from IMAGE_EXCLUDE. It must report a leak; a checker that has
# only ever said "clean" proves nothing.
#
# The list under test is read from the script itself (IMAGE_EXCLUDE), so the test
# cannot drift from it; the expected shipped set is read from retro/systems/*.sys.
#
# Needs bash only. Scratch is a mktemp dir, removed on exit.

set -u
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
SUBJECT="$REPO_DIR/retro/build-and-deploy.sh"

RED='\033[0;31m'; GREEN='\033[0;32m'; NC='\033[0m'
PASS=0; FAIL=0
ok()  { PASS=$((PASS + 1)); echo -e "  ${GREEN}pass${NC}  $1"; }
bad() { FAIL=$((FAIL + 1)); echo -e "  ${RED}FAIL${NC}  $1"; }
# t <description> <command...>: pass when the command succeeds.
t()   { local d="$1"; shift; if "$@"; then ok "$d"; else bad "$d"; fi; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# The excluded list, as the script declares it: one "id:core file:system subfolder" per line.
excl_lines() { sed -n '/^IMAGE_EXCLUDE=(/,/^)/p' "$1" | sed -n 's/^ *"\(.*\)"$/\1/p'; }
EXCL="$(excl_lines "$SUBJECT")"
t "A1 IMAGE_EXCLUDE is readable from the script" test -n "$EXCL"
# The three, by core file: the operator's decision, written once more here on purpose so
# that emptying the list in the script cannot make this test agree with it.
for so in ep128emu_core_libretro.so fuse_libretro.so vice_x64_libretro.so; do
    t "A2 IMAGE_EXCLUDE names $so" grep -q ":$so:" <<<"$EXCL"
done

# Fake out dir: one placeholder per core the .sys files name, plus the unlisted snes9x2005.
OUT="$TMP/out"; mkdir -p "$OUT"
for sys in "$REPO_DIR"/retro/systems/*.sys; do
    c=$(sed -n 's/^core=//p' "$sys" | tr -d '\r'); printf 'x\n' > "$OUT/$c"
done
printf 'x\n' > "$OUT/snes9x2005_libretro.so"

# stage <script> <bundle dir>: run the script's --bundle against the fake out dir.
stage() { RETRO_OUT="$OUT" bash "$1" --bundle "$2" --no-build >"$2.log" 2>&1; }

# leaks <bundle dir>: print one line per excluded thing found in the bundle.
leaks() {
    local b="$1" so sys c
    for so in ep128emu_core_libretro.so fuse_libretro.so vice_x64_libretro.so; do
        grep -q "/cores/$so\$" "$b"/manifest.d/retro.list 2>/dev/null && echo "manifest names $so"
        [ -e "$b/root/home/root/data/retro/cores/$so" ] && echo "tree holds $so"
    done
    for sys in "$REPO_DIR"/retro/systems/*.sys; do
        c=$(sed -n 's/^core=//p' "$sys" | tr -d '\r')
        case "$c" in ep128emu_core_libretro.so|fuse_libretro.so|vice_x64_libretro.so)
            [ -e "$b/root/home/root/data/retro/systems/$(basename "$sys")" ] && echo "tree holds $(basename "$sys")" ;;
        esac
    done
    [ -d "$b/root/home/root/data/retro/system/ep128emu" ] && echo "tree holds system/ep128emu"
    return 0
}

echo "B. the real script"
stage "$SUBJECT" "$TMP/b1"; b1rc=$?
t "B1 --bundle --no-build exits 0" test "$b1rc" -eq 0
[ "$b1rc" -eq 0 ] || cat "$TMP/b1.log"
L="$(leaks "$TMP/b1")"
t "B2 no excluded core, .sys or system file in the bundle (leaks: $L)" test -z "$L"

missing=""
for sys in "$REPO_DIR"/retro/systems/*.sys; do
    c=$(sed -n 's/^core=//p' "$sys" | tr -d '\r')
    printf '%s\n' "$EXCL" | grep -q ":$c:" && continue
    [ -e "$TMP/b1/root/home/root/data/retro/cores/$c" ] || missing="$missing $c"
    [ -e "$TMP/b1/root/home/root/data/retro/systems/$(basename "$sys")" ] || missing="$missing $(basename "$sys")"
done
t "B3 every other core and its .sys is in the bundle (missing:$missing)" test -z "$missing"
t "B4 retro.list and retro.md5 agree" test "$(wc -l < "$TMP/b1/manifest.d/retro.list")" -eq "$(wc -l < "$TMP/b1/manifest.d/retro.md5")"
t "B5 cores are declared 755" grep -q "^755 .*/cores/" "$TMP/b1/manifest.d/retro.list"

echo "C. negative control — vice removed from IMAGE_EXCLUDE"
SAB="$TMP/sab"; mkdir -p "$SAB/retro" "$SAB/lib"
cp "$REPO_DIR"/lib/*.sh "$SAB/lib/"
cp -r "$REPO_DIR/retro/systems" "$REPO_DIR/retro/system" "$SAB/retro/"
sed '/"vice:vice_x64_libretro.so:"/d' "$SUBJECT" > "$SAB/retro/build-and-deploy.sh"
if cmp -s "$SUBJECT" "$SAB/retro/build-and-deploy.sh"; then
    bad "C1 the sabotage changed nothing — the control is vacuous"
else
    ok "C1 the sabotaged copy differs from the script"
fi
stage "$SAB/retro/build-and-deploy.sh" "$TMP/b2" || true
L2="$(leaks "$TMP/b2")"
case "$L2" in *vice_x64*) ok "C2 the checker reports the leaked VICE core ($(printf '%s' "$L2" | tr '\n' ';'))" ;;
    *) bad "C2 the checker did not report the leaked VICE core: '$L2'" ;; esac

echo ""
echo "  $PASS passed, $FAIL failed, $((PASS + FAIL)) total"
if [ "$((PASS + FAIL))" -lt 10 ]; then echo "  only $((PASS + FAIL)) cases ran — the harness is broken"; exit 1; fi
[ "$FAIL" -eq 0 ] || exit 1
exit 0
