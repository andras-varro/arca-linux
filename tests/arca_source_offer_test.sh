#!/bin/bash
#
# arca_source_offer_test.sh — regression for rootfs/check-source-offer.sh and the refusals of
# rootfs/make-source-offer.sh
#
# Host-only, no device, no root, no network. Run it:
#
#   wsl.exe -e bash -lc "cd /mnt/c/work/roomwizard && ./tests/arca_source_offer_test.sh"
#
# The checker is driven over a MINIATURE offer built here (a few bytes per member, a real
# xz repo archive holding the pin files), never over a real 0.7 GB one. CSO=<path> runs the
# same cases against another copy of the checker, which is how a sabotaged copy is graded.
#
#   A  a faithful miniature passes, and pins were actually checked
#   B  a member that is corrupt, missing, pinned differently by its build script, pinned by
#      an unknown kind, or an offer that pins nothing at all, fails (the last is exit 2:
#      "could not judge")
#   C  Buildroot's legal-info: a package without source, a source whose hash no longer
#      verifies, and a pointer that leads nowhere each fail
#   D  the image: programs mapped through the package list and the source table pass; a stray
#      ELF, a removed row, an unmapped kernel module and an empty tree each fail
#   E  make-source-offer.sh: --help has no side effect; it refuses with nothing written when
#      --out is inside the repo, when the tree is dirty, and when HEAD is on no remote branch
#   F  make-card-image.sh: the licence documents it puts on p6 (LICENSE.md, manifest.csv,
#      licenses/, SOURCE-OFFER.txt naming the source file), and its refusals
#      (MCI=<path> grades a sabotaged copy; tests/measure_source_offer_sabotage.sh does)
#
# CAN NOT SEE: the real caches, the downloads, or a real card image. The map table is graded
# against an invented image here; against the real one it was run by hand. Group F cannot see
# the loop-mounted fill itself (root): F12 reads the call as text, and the step was seen to work
# on a real image built with --bundle.

# shellcheck source-path=SCRIPTDIR
set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
CSO="${CSO:-$REPO_DIR/rootfs/check-source-offer.sh}"
MSO="$REPO_DIR/rootfs/make-source-offer.sh"
# MCI=<path> runs the card-image cases (group F) against another copy of make-card-image.sh.
MCI="${MCI:-$REPO_DIR/rootfs/make-card-image.sh}"
T="$(mktemp -d)"
trap 'rm -rf "$T"' EXIT

RED='\033[0;31m'; GREEN='\033[0;32m'; NC='\033[0m'
pass=0; fail=0
ok()  { pass=$((pass+1)); echo -e "  ${GREEN}PASS${NC}  $*"; }
bad() { fail=$((fail+1)); echo -e "  ${RED}FAIL${NC}  $*"; }
sha() { sha256sum "$1" | cut -d' ' -f1; }

H40="0123456789abcdef0123456789abcdef01234567"

# build_offer <dir> : a miniature offer that should pass
build_offer() {
    local o="$1" r="$1.repo/arca-t" s
    mkdir -p "$o"/{buildroot/legal-info/sources/foo-1.0,buildroot/legal-info/sources/linux-headers-4.14.52,buildroot/legal-info/licenses/foo-1.0,kernel,bluetooth,uboot,host-libc,licenses,repo} \
             "$r/rootfs" "$r/build" "$r/rootfs/package-hashes"
    echo readme >"$o/README-SOURCE.txt"; echo env >"$o/BUILD-ENV.txt"; echo md5 >"$o/image-md5.txt"
    echo rtl >"$o/licenses/LICENCE.rtlwifi_firmware.txt"
    echo "foo-src" >"$o/buildroot/legal-info/sources/foo-1.0/foo-1.0.tar.gz"
    echo "COPYING" >"$o/buildroot/legal-info/licenses/foo-1.0/COPYING"
    echo "see ../../../../kernel/linux.tar.xz" >"$o/buildroot/legal-info/sources/linux-headers-4.14.52/POINTER.txt"
    printf '%s\n' '"PACKAGE","VERSION","LICENSE","LICENSE FILES","SOURCE ARCHIVE","SOURCE SITE","DEPENDENCIES"' \
        '"foo","1.0","GPL-2.0","COPYING","foo-1.0.tar.gz","http://x",""' \
        '"linux-headers","4.14.52","GPL-2.0","COPYING","linux.tar.xz","http://x",""' >"$o/buildroot/legal-info/manifest.csv"
    ( cd "$o/buildroot/legal-info" && {
        sha256sum sources/foo-1.0/foo-1.0.tar.gz licenses/foo-1.0/COPYING
        echo "$(printf '%064d' 0)  host-sources/gcc/gcc.tar.xz"
        echo "$(printf '%064d' 1)  sources/linux-headers-4.14.52/linux.tar.xz"
      } >legal-info.sha256 )
    printf '%s\n' 'foo,./usr/bin/foo' 'toolchain,./lib/ld.so' >"$o/buildroot/packages-file-list.txt"
    # members
    echo bluez-src >"$o/bluetooth/bluez.tar.xz"
    echo linux-src >"$o/kernel/linux.tar.xz"
    echo "$H40" >"$o/uboot/u-boot.COMMIT"
    echo glibc-src >"$o/host-libc/g.tar.xz"
    printf 'Checksums-Sha256:\n %s 9 g.tar.xz\n' "$(sha "$o/host-libc/g.tar.xz")" >"$o/host-libc/g.dsc"
    # the repo archive: the build scripts' pins
    printf '%s\n' "BLUEZ_SHA256=$(sha "$o/bluetooth/bluez.tar.xz")" "UBOOT_COMMIT=\"$H40\"" \
        "DSC_SHA256=$(sha "$o/host-libc/g.dsc")" >"$r/build/pins.sh"
    printf 'sha256  %s  linux.tar.xz\n' "$(sha "$o/kernel/linux.tar.xz")" >"$r/rootfs/package-hashes/l.hash"
    printf '%s\t%s\n' '*/bluetoothd' bluez '*.ko' linux 'p1/MLO' u-boot >"$r/rootfs/source-map.tsv"
    ( cd "$1.repo" && tar -cf - arca-t | xz -6 >"$o/repo/arca-t.tar.xz" )
    s="$o/SOURCES.csv"
    echo "component,version,licence,binaries,member,url,sha256,pin" >"$s"
    row() { printf '%s,1,GPL,bins,%s,http://x,%s,%s\n' "$1" "$2" "$(sha "$o/$2")" "$3" >>"$s"; }
    row arca repo/arca-t.tar.xz none:
    row bluez bluetooth/bluez.tar.xz grep:build/pins.sh:BLUEZ_SHA256=
    row linux kernel/linux.tar.xz grep:rootfs/package-hashes/l.hash:linux.tar.xz
    row u-boot uboot/u-boot.COMMIT content:build/pins.sh:UBOOT_COMMIT=
    row glibc-host host-libc/g.tar.xz dsc:host-libc/g.dsc:g.tar.xz
    row glibc-host-dsc host-libc/g.dsc grep:build/pins.sh:DSC_SHA256=
    row buildroot repo/arca-t.tar.xz none:
}

# build_image <dir> : a miniature image tree, every program mapped
build_image() {
    mkdir -p "$1"/{usr/bin,usr/sbin,lib/modules/x,etc} "$1.p1"
    printf '\177ELF....' >"$1/usr/bin/foo"
    printf '\177ELF....' >"$1/usr/sbin/bluetoothd"
    printf 'ko' >"$1/lib/modules/x/y.ko"
    echo text >"$1/etc/motd"
    echo mlo >"$1.p1/MLO"
}

# run <offer> [args]: prints "<rc>" and leaves output in $T/out
run() { "$CSO" "$@" >"$T/out" 2>&1; echo $?; }
expect() { # <label> <want rc> <got rc> [grep pattern in output]
    if [ "$3" = "$2" ] && { [ -z "${4:-}" ] || grep -q -- "$4" "$T/out"; }; then ok "$1"
    else bad "$1 (rc $3, wanted $2${4:+, pattern [$4]}): $(tail -n 3 "$T/out" | tr '\n' '|')"; fi
}
fresh() { rm -rf "$T/o" "$T/o.repo" "$T/img" "$T/img.p1"; build_offer "$T/o"; build_image "$T/img"; }

# ── A ───────────────────────────────────────────────────────────────────────
fresh
expect "A1 the miniature offer passes" 0 "$(run "$T/o")" "pins checked: [1-9]"
expect "A2 it passes with a tree and a p1 directory" 0 "$(run "$T/o" --tree "$T/img" --p1-dir "$T/img.p1")" "image: 4 programs inspected, 0 unmapped"
tar -cf "$T/o.tar" -C "$T" o
expect "A3 a tar of the offer is accepted too" 0 "$(run "$T/o.tar")" "PASS"

# ── B ───────────────────────────────────────────────────────────────────────
fresh; echo x >>"$T/o/bluetooth/bluez.tar.xz"
expect "B1 a member changed after the row was written fails" 1 "$(run "$T/o")" "bluetooth/bluez.tar.xz has sha256"
fresh; rm "$T/o/kernel/linux.tar.xz"
expect "B2 a missing member fails" 1 "$(run "$T/o")" "member kernel/linux.tar.xz is missing"
fresh
sed -i 's/^BLUEZ_SHA256=.*/BLUEZ_SHA256=1111111111111111111111111111111111111111111111111111111111111111/' "$T/o.repo/arca-t/build/pins.sh"
( cd "$T/o.repo" && tar -cf - arca-t | xz -6 >"$T/o/repo/arca-t.tar.xz" )
sed -i "s|^\(arca,1,GPL,bins,repo/arca-t.tar.xz,http://x,\)[0-9a-f]*|\1$(sha "$T/o/repo/arca-t.tar.xz")|; s|^\(buildroot,1,GPL,bins,repo/arca-t.tar.xz,http://x,\)[0-9a-f]*|\1$(sha "$T/o/repo/arca-t.tar.xz")|" "$T/o/SOURCES.csv"
expect "B3 a hash the build script pins differently fails (the members themselves are intact)" 1 "$(run "$T/o")" "bluez: .* is [0-9a-f]*, build/pins.sh pins [0-9a-f]*"
fresh; sed -i 's|grep:build/pins.sh:BLUEZ_SHA256=|bogus:x:y|' "$T/o/SOURCES.csv"
expect "B4 an unknown pin kind fails" 1 "$(run "$T/o")" "unknown pin kind"
fresh; sed -i 's/,\(grep\|dsc\|content\):.*$/,none:/' "$T/o/SOURCES.csv"
expect "B5 an offer that pins nothing is exit 2, could not judge" 2 "$(run "$T/o")" "inspected nothing"
fresh; echo 0000000000000000000000000000000000000000 >"$T/o/uboot/u-boot.COMMIT"
sed -i "s|^\(u-boot,1,GPL,bins,uboot/u-boot.COMMIT,http://x,\)[0-9a-f]*|\1$(sha "$T/o/uboot/u-boot.COMMIT")|" "$T/o/SOURCES.csv"
expect "B6 a commit file that differs from the pinned commit fails" 1 "$(run "$T/o")" "u-boot: uboot/u-boot.COMMIT holds 0000000000000000000000000000000000000000, build/pins.sh pins"

# ── C ───────────────────────────────────────────────────────────────────────
fresh; rm -rf "$T/o/buildroot/legal-info/sources/foo-1.0"; mkdir "$T/o/buildroot/legal-info/sources/foo-1.0"
expect "C1 a package with no source fails" 1 "$(run "$T/o")" "no source for foo 1.0"
fresh; echo tampered >>"$T/o/buildroot/legal-info/sources/foo-1.0/foo-1.0.tar.gz"
expect "C2 a source that no longer verifies fails" 1 "$(run "$T/o")" "does not verify"
fresh; rm "$T/o/kernel/linux.tar.xz"; sed -i '/^linux,/d' "$T/o/SOURCES.csv"
expect "C3 a pointer that leads nowhere fails" 1 "$(run "$T/o")" "pointer for linux-headers"
fresh; echo "$(printf '%064d' 2)  licenses/missing/COPYING" >>"$T/o/buildroot/legal-info/legal-info.sha256"
expect "C4 a checksum line for a file that is not in the offer fails" 1 "$(run "$T/o")" "licenses/missing/COPYING"

# ── D ───────────────────────────────────────────────────────────────────────
fresh; printf '\177ELF....' >"$T/img/usr/bin/stray"
expect "D1 a stray ELF on the image fails (negative control for the map)" 1 "$(run "$T/o" --tree "$T/img" --p1-dir "$T/img.p1")" "no source row for usr/bin/stray"
fresh; sed -i '/^bluez,/d' "$T/o/SOURCES.csv"
expect "D2 dropping the BlueZ row leaves bluetoothd unmapped" 1 "$(run "$T/o" --tree "$T/img" --p1-dir "$T/img.p1")" "no source row for usr/sbin/bluetoothd"
fresh; sed -i '/^linux,/d' "$T/o/SOURCES.csv"
expect "D3 a kernel module with no kernel row fails" 1 "$(run "$T/o" --tree "$T/img" --p1-dir "$T/img.p1")" "no source row for lib/modules/x/y.ko"
fresh; echo junk >"$T/img.p1/uImage-system"
expect "D4 an unlisted boot file on p1 fails" 1 "$(run "$T/o" --tree "$T/img" --p1-dir "$T/img.p1")" "no source row for p1/uImage-system"
fresh; rm -rf "$T/empty"; mkdir "$T/empty"
expect "D5 an image holding no program is a failure, not a pass" 1 "$(run "$T/o" --tree "$T/empty")" "nothing inspected"
fresh; ( cd "$T/img" && tar -cf "$T/img.tar" . )
expect "D6 a rootfs tar is unpacked and graded" 0 "$(run "$T/o" --rootfs-tar "$T/img.tar")" "3 programs inspected, 0 unmapped"

# ── E ───────────────────────────────────────────────────────────────────────
before="$(find "$REPO_DIR" -maxdepth 1 | wc -l)"
"$MSO" --help >"$T/out" 2>&1; rc=$?
after="$(find "$REPO_DIR" -maxdepth 1 | wc -l)"
if [ "$rc" = 0 ] && grep -q '^Usage' "$T/out" && [ "$before" = "$after" ]; then ok "E1 make-source-offer --help prints usage and writes nothing"; else bad "E1 rc $rc"; fi
"$CSO" --help >"$T/out" 2>&1; rc=$?
if [ "$rc" = 0 ] && grep -q '^Usage' "$T/out"; then ok "E2 check-source-offer --help prints usage"; else bad "E2 rc $rc"; fi
"$MSO" --out "$T/x" >"$T/out" 2>&1; rc=$?
if [ "$rc" = 1 ] && grep -q -- '--tag is required' "$T/out" && [ ! -e "$T/x" ]; then ok "E3 no --tag is refused and nothing is created"; else bad "E3 rc $rc, $(cat "$T/out")"; fi
"$MSO" --tag t --out "$REPO_DIR/zz-offer-test" >"$T/out" 2>&1; rc=$?
if [ "$rc" = 1 ] && grep -q 'inside the repository' "$T/out" && [ ! -e "$REPO_DIR/zz-offer-test" ]; then ok "E4 --out inside the repo is refused and not created"; else bad "E4 rc $rc, $(cat "$T/out")"; rm -rf "$REPO_DIR/zz-offer-test"; fi
# a throwaway repo around copies of the script, so its own git state is the subject
mkdir -p "$T/g/rootfs" "$T/g/lib" "$T/g/uboot" "$T/g/scummvm-arca" "$T/g/rootfs/board/arca"
cp "$MSO" "$CSO" "$T/g/rootfs/"; cp "$REPO_DIR/lib/product.sh" "$T/g/lib/"; cp "$REPO_DIR/rootfs/board/arca/os-release" "$T/g/rootfs/board/arca/"
( cd "$T/g" && git init -q . && git -c user.name=t -c user.email=t@t add -A && git -c user.name=t -c user.email=t@t commit -q -m c )
"$T/g/rootfs/make-source-offer.sh" --tag t --out "$T/gout" >"$T/out" 2>&1; rc=$?
if [ "$rc" = 1 ] && grep -q 'not on any remote branch' "$T/out" && [ ! -e "$T/gout" ]; then ok "E5 a commit on no remote branch is refused before anything is written"; else bad "E5 rc $rc, $(cat "$T/out")"; fi
echo "# touched" >>"$T/g/rootfs/make-source-offer.sh"
"$T/g/rootfs/make-source-offer.sh" --tag t --out "$T/gout" >"$T/out" 2>&1; rc=$?
if [ "$rc" = 1 ] && grep -q 'uncommitted changes' "$T/out" && [ ! -e "$T/gout" ]; then ok "E6 a dirty tree is refused before anything is written"; else bad "E6 rc $rc, $(cat "$T/out")"; fi

# ── F ───────────────────────────────────────────────────────────────────────
# make-card-image.sh needs root and a loop device, so the licence step is extracted from the
# shipped file (MCI=<path> points at another copy) and run on fixture directories; the
# script's own refusals are driven as a program, because they come before the root check.
# PRODUCT_NAME / PRODUCT_STEM come from lib/product.sh, as in the script.
# shellcheck source=../lib/product.sh
. "$REPO_DIR/lib/product.sh"
awk '/^licence_docs_install\(\) \{$/{p=1} p{print} p&&/^\}$/{exit}' "$MCI" >"$T/ldi.sh"
if [ -s "$T/ldi.sh" ]; then ok "F0 licence_docs_install is in make-card-image.sh"; else bad "F0 make-card-image.sh has no licence_docs_install"; : >"$T/ldi.sh"; fi
# shellcheck source=/dev/null
. "$T/ldi.sh"
ldi_run() { rm -rf "$T/p6"; mkdir -p "$T/p6"; licence_docs_install "$T/p6" "$@" >"$T/out" 2>&1; echo $?; }
mkdir -p "$T/fx/repo" "$T/fx/legal/licenses/foo-1.0"
echo "licence record" >"$T/fx/repo/LICENSE.md"; chmod 0755 "$T/fx/repo/LICENSE.md"
printf '"PACKAGE","VERSION"\n"foo","1.0"\n' >"$T/fx/legal/manifest.csv"
echo "GPL text" >"$T/fx/legal/licenses/foo-1.0/COPYING"; chmod 0755 "$T/fx/legal/licenses/foo-1.0/COPYING"
D="$T/p6/usr/share/doc/arca-licenses"
if [ "$(declare -F licence_docs_install >/dev/null && ldi_run "$T/fx/repo" "$T/fx/legal" v1.0 https://example.invalid/rel/v1.0)" = 0 ] \
   && [ -f "$D/LICENSE.md" ] && [ -f "$D/manifest.csv" ] && [ "$(cat "$D/licenses/foo-1.0/COPYING" 2>/dev/null)" = "GPL text" ]; then
    ok "F1 LICENSE.md, manifest.csv and licenses/ land under /usr/share/doc/arca-licenses"
else bad "F1 the licence documents did not land: $(find "$T/p6" 2>&1 | tr '\n' ' ')"; fi
if [ "$(stat -c %a "$D/LICENSE.md" 2>/dev/null)" = 644 ] && [ "$(stat -c %a "$D/licenses/foo-1.0/COPYING" 2>/dev/null)" = 644 ]; then
    ok "F2 they are 0644 whatever the source file's mode was"; else bad "F2 modes: $(stat -c %a "$D/LICENSE.md" "$D/licenses/foo-1.0/COPYING" 2>&1 | tr '\n' ' ')"; fi
if grep -qxF "    ${PRODUCT_STEM}-source-v1.0.tar" "$D/SOURCE-OFFER.txt" && grep -qF "${PRODUCT_STEM}-source-v1.0.tar.sha256" "$D/SOURCE-OFFER.txt" \
   && grep -qF "release tagged" "$D/SOURCE-OFFER.txt" && grep -qF "https://example.invalid/rel/v1.0" "$D/SOURCE-OFFER.txt" && grep -qF "section 3(a)" "$D/SOURCE-OFFER.txt"; then
    ok "F3 SOURCE-OFFER.txt names the source file, its checksum, the tag and the release page"
else bad "F3 SOURCE-OFFER.txt: $(tr '\n' '|' < "$D/SOURCE-OFFER.txt" 2>&1)"; fi
if grep -qF "$PRODUCT_NAME" "$D/SOURCE-OFFER.txt"; then ok "F4 and carries the product name from lib/product.sh"; else bad "F4 no product name in SOURCE-OFFER.txt"; fi
rc="$(ldi_run "$T/fx/repo" "$T/fx/legal" "" "")"
if [ "$rc" = 0 ] && grep -qF "without a release tag" "$D/SOURCE-OFFER.txt" && ! grep -qF -e "-source-" "$D/SOURCE-OFFER.txt"; then
    ok "F5 without a tag the offer names no file and says the image is for local use"
else bad "F5 rc $rc: $(tr '\n' '|' < "$D/SOURCE-OFFER.txt" 2>&1)"; fi
rc="$(ldi_run "$T/fx/repo" "" v1.0 "")"
if [ "$rc" = 0 ] && [ ! -e "$D/manifest.csv" ] && [ ! -e "$D/licenses" ] && grep -qF "without --legal-info" "$D/SOURCE-OFFER.txt"; then
    ok "F6 without a legal-info tree no package texts are claimed, and the offer says so"
else bad "F6 rc $rc: $(find "$T/p6" 2>&1 | tr '\n' ' ')"; fi
mkdir -p "$T/fx/bad"; echo x >"$T/fx/bad/manifest.csv"
rc="$(ldi_run "$T/fx/repo" "$T/fx/bad" v1.0 "")"
if [ "$rc" != 0 ] && grep -qF "not a legal-info tree" "$T/out"; then ok "F7 a legal-info tree without licenses/ is refused"; else bad "F7 rc $rc: $(cat "$T/out")"; fi
rc="$(ldi_run "$T/fx/nowhere" "$T/fx/legal" v1.0 "")"
if [ "$rc" != 0 ] && grep -qF "LICENSE.md" "$T/out"; then ok "F8 a missing LICENSE.md is refused"; else bad "F8 rc $rc: $(cat "$T/out")"; fi
# the script's own arguments, as a program (no root needed: these refusals precede the root check)
bash "$MCI" --mlo m --uboot u --kernel k --legal-info "$T/fx/nowhere" a.tar "$T/never.img" >"$T/out" 2>&1; rc=$?
if [ "$rc" = 1 ] && grep -q -- '--legal-info .* has no manifest.csv' "$T/out" && [ ! -e "$T/never.img" ]; then ok "F9 an explicit --legal-info that is not a legal-info tree is refused"; else bad "F9 rc $rc: $(cat "$T/out")"; fi
bash "$MCI" --mlo m --uboot u --kernel k --legal-info "$T/fx/legal" --source-tag 'a/b' a.tar "$T/never.img" >"$T/out" 2>&1; rc=$?
if [ "$rc" = 1 ] && grep -q -- '--source-tag may contain only' "$T/out"; then ok "F10 a --source-tag that is not a plain tag is refused"; else bad "F10 rc $rc: $(cat "$T/out")"; fi
bash "$MCI" --help >"$T/out" 2>&1; rc=$?
if [ "$rc" = 0 ] && grep -q -- '--legal-info' "$T/out" && grep -q -- '--source-tag' "$T/out" && grep -q -- '--source-url' "$T/out"; then ok "F11 --help names the three new options"; else bad "F11 rc $rc"; fi
# the wiring: the p6 fill step calls it with the three values it was given. Text, not behaviour:
# the call is inside a loop-mounted fill that needs root; a real image was built to see it work.
# shellcheck disable=SC2016  # the literal text of the call is the thing being looked for
if grep -qF 'licence_docs_install "$MNT/p" "$SCRIPT_DIR/.." "$LEGAL" "$SOURCE_TAG" "$SOURCE_URL"' "$MCI"; then
    ok "F12 the p6 fill calls it with the legal-info tree, the tag and the URL"; else bad "F12 fill does not call licence_docs_install with its arguments"; fi

total=$((pass+fail))
echo ""
echo "  $pass passed, $fail failed, $total total"
[ "$total" -ge 25 ] || { echo "  FLOOR: only $total cases ran"; exit 1; }
[ "$fail" -eq 0 ]
