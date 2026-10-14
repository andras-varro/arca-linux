#!/bin/bash
#
# arca_kernel_tarball_test.sh — regression for lib/arca-kernel-src.sh and for the call sites
# that must verify usb_host/linux-4.14.52.tar.xz before `tar xf`.
#
# Host-only, no device, no root, no network, no kernel build. Run it:
#
#   wsl.exe -e bash -lc "cd /mnt/c/work/roomwizard && ./tests/arca_kernel_tarball_test.sh"
#
# The function is driven over a tiny fixture tarball and a fixture pin file built in a temp dir
# OUTSIDE the repo. LIB=<path> sources another copy of the library instead, which is how a
# sabotaged copy is graded (tests/CLAUDE.md, negative controls).
#
#   A  the function: a good pin passes; one changed byte fails; a missing pin line, an
#      ambiguous pin, a malformed pin, a missing file and a missing argument each fail
#   B  the real pin file carries the linux-4.14.52.tar.xz line, and the real tarball verifies
#      against it when it is present (skipped, and said so, when it is absent)
#   C  neither call site contains a 64-hex literal: the pin is read, never copied
#   D  each call site sources the library and calls arca_check_kernel_tarball within the six
#      lines above every `tar xf`; build-kernel-modules.sh also calls it after its wget
#
# CAN NOT SEE: whether the real tarball's digest is the pin's, unless it is present on disk, and
# whether the scripts behave when run: they are read as text, never executed.

# shellcheck source-path=SCRIPTDIR
set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
LIB="${LIB:-$REPO_DIR/lib/arca-kernel-src.sh}"
REAL_PIN="$REPO_DIR/rootfs/package-hashes/linux-headers.hash"
REAL_TARBALL="$REPO_DIR/usb_host/linux-4.14.52.tar.xz"
BUILD_IMAGE="$REPO_DIR/kernel/build-image.sh"
BUILD_MODULES="$REPO_DIR/usb_host/build-kernel-modules.sh"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'
PASS=0
FAIL=0
SKIP=0
ok()   { PASS=$((PASS + 1)); echo -e "  ${GREEN}pass${NC}  $1"; }
bad()  { FAIL=$((FAIL + 1)); echo -e "  ${RED}FAIL${NC}  $1"; }
skip() { SKIP=$((SKIP + 1)); echo -e "  ${YELLOW}skip${NC}  $1"; }
# expect_rc <description> <want> <command...>: ok when the command's exit status equals <want>
expect_rc() {
    local d="$1" want="$2"; shift 2
    local rc=0
    "$@" >/dev/null 2>&1 || rc=$?
    if [ "$rc" -eq "$want" ]; then ok "$d"; else bad "$d (exit $rc, wanted $want)"; fi
}

TMP=$(mktemp -d /tmp/arca-kt-test.XXXXXX)
trap 'rm -rf "$TMP"' EXIT INT TERM

# shellcheck source=../lib/arca-kernel-src.sh
. "$LIB"

# ── fixtures: a tiny tarball and a pin file for it, built outside the repo ──────────
mkdir -p "$TMP/fx/src"
printf 'fixture kernel source\n' > "$TMP/fx/src/readme.txt"
tar -cf "$TMP/fx/linux-fixture.tar.xz" -C "$TMP/fx/src" readme.txt
FIX_TB="$TMP/fx/linux-fixture.tar.xz"
FIX_HEX="$(sha256sum "$FIX_TB" | cut -d' ' -f1)"
printf 'sha256  %s  linux-fixture.tar.xz\n' "$FIX_HEX" > "$TMP/fx/pins.hash"
printf 'sha256  %s  COPYING\n' "$FIX_HEX" > "$TMP/fx/other.hash"

# A copy of the tarball with its first byte changed (the tar header's name byte, so the file
# stays a tarball-shaped file and only the digest can tell).
mkdir -p "$TMP/flip"
cp "$FIX_TB" "$TMP/flip/linux-fixture.tar.xz"
printf 'Z' | dd of="$TMP/flip/linux-fixture.tar.xz" bs=1 seek=0 conv=notrunc 2>/dev/null

# A pin file whose line for the fixture is malformed: a 63-character hex.
printf 'sha256  %s  linux-fixture.tar.xz\n' "${FIX_HEX%?}" > "$TMP/fx/short.hash"
# Two pin lines for the same name.
{ cat "$TMP/fx/pins.hash"; printf 'sha256  %s  linux-fixture.tar.xz\n' "$FIX_HEX"; } > "$TMP/fx/twice.hash"
# A pin line with no hex at all.
printf 'sha256  linux-fixture.tar.xz\n' > "$TMP/fx/empty.hash"
: > "$TMP/fx/zero.hash"

echo "== A  arca_check_kernel_tarball over a fixture"
expect_rc "A1 a good pin passes" 0 arca_check_kernel_tarball "$FIX_TB" "$TMP/fx/pins.hash"
expect_rc "A2 one changed byte fails" 1 arca_check_kernel_tarball "$TMP/flip/linux-fixture.tar.xz" "$TMP/fx/pins.hash"
expect_rc "A3 a missing pin line fails" 1 arca_check_kernel_tarball "$FIX_TB" "$TMP/fx/other.hash"
expect_rc "A4 an empty pin file fails" 1 arca_check_kernel_tarball "$FIX_TB" "$TMP/fx/zero.hash"
expect_rc "A5 a missing file fails" 1 arca_check_kernel_tarball "$TMP/fx/absent.tar.xz" "$TMP/fx/pins.hash"
expect_rc "A6 a malformed (63-hex) pin fails" 1 arca_check_kernel_tarball "$FIX_TB" "$TMP/fx/short.hash"
expect_rc "A7 two pins for one name fail" 1 arca_check_kernel_tarball "$FIX_TB" "$TMP/fx/twice.hash"
expect_rc "A8 a pin line with no hex fails" 1 arca_check_kernel_tarball "$FIX_TB" "$TMP/fx/empty.hash"
expect_rc "A9 no tarball argument fails" 1 arca_check_kernel_tarball ""
# A message on mismatch must carry both digests and the re-download advice.
msg="$(arca_check_kernel_tarball "$TMP/flip/linux-fixture.tar.xz" "$TMP/fx/pins.hash" 2>&1 >/dev/null)"
case "$msg" in
    *"expected: $FIX_HEX"*"got:      "*"re-download"*) ok "A10 a mismatch names expected, got and the re-download advice" ;;
    *) bad "A10 a mismatch names expected, got and the re-download advice (message: $msg)" ;;
esac

echo "== B  the real pin file and the real tarball"
if grep -Eq '^sha256  [0-9a-f]{64}  linux-4\.14\.52\.tar\.xz\r?$' "$REAL_PIN"; then
    ok "B1 linux-headers.hash has exactly the linux-4.14.52.tar.xz sha256 line"
else
    bad "B1 linux-headers.hash has exactly the linux-4.14.52.tar.xz sha256 line"
fi
if [ -f "$REAL_TARBALL" ]; then
    # No hash-file argument: exercises the default path the library computes from its own location.
    expect_rc "B2 the real usb_host/linux-4.14.52.tar.xz verifies against the pin" 0 arca_check_kernel_tarball "$REAL_TARBALL"
else
    skip "B2 usb_host/linux-4.14.52.tar.xz is absent; not verified here"
fi

echo "== C  no 64-hex literal in the call sites"
# The pattern must match a 64-hex string, or the absence below proves nothing.
if printf 'x %s y\n' "$FIX_HEX" | grep -Eq '[0-9a-f]{64}'; then
    for f in "$BUILD_IMAGE" "$BUILD_MODULES"; do
        if grep -Eq '[0-9a-f]{64}' "$f"; then
            bad "C $(basename "$f") contains a 64-hex literal (the pin was copied in)"
        else
            ok "C $(basename "$f") contains no 64-hex literal"
        fi
    done
else
    bad "C the 64-hex pattern does not match a 64-hex string (the check is blind)"
fi

# call_site_ok <script> <needs-wget 0|1>: sources the library, and every `tar xf` line has a
# call to arca_check_kernel_tarball within the six lines above it; with <needs-wget> 1 there is
# also such a call after the first wget line.
call_site_ok() {
    local f="$1" wget_needed="$2" tarlines n
    grep -Fq 'lib/arca-kernel-src.sh' "$f" || return 1
    grep -Fq 'arca_check_kernel_tarball "' "$f" || return 1
    tarlines="$(grep -n '^[[:space:]]*tar xf' "$f" | cut -d: -f1)"
    [ -n "$tarlines" ] || return 1
    for n in $tarlines; do
        awk -v n="$n" 'NR < n && NR >= n - 6 && /^[ \t]*arca_check_kernel_tarball "/ { hit = 1 } END { exit !hit }' "$f" || return 1
    done
    if [ "$wget_needed" -eq 1 ]; then
        local w
        w="$(grep -n '^[[:space:]]*wget ' "$f" | head -1 | cut -d: -f1)"
        [ -n "$w" ] || return 1
        awk -v w="$w" 'NR > w && /^[ \t]*arca_check_kernel_tarball "/ { hit = 1 } END { exit !hit }' "$f" || return 1
    fi
    return 0
}

echo "== D  the call sites"
expect_rc "D1 kernel/build-image.sh sources the library and checks before tar xf" 0 call_site_ok "$BUILD_IMAGE" 0
expect_rc "D2 usb_host/build-kernel-modules.sh checks before tar xf and after wget" 0 call_site_ok "$BUILD_MODULES" 1
# Negative control: a call placed AFTER the tar xf must be refused by the same test.
mkdir -p "$TMP/sab"
cat > "$TMP/sab/late.sh" <<'LATE'
#!/bin/bash
. "$DIR/lib/arca-kernel-src.sh"
tar xf "$KERNEL_TARBALL"
arca_check_kernel_tarball "$KERNEL_TARBALL"
LATE
expect_rc "D3 negative control: a check after tar xf is refused" 1 call_site_ok "$TMP/sab/late.sh" 0

echo
echo "  $PASS passed, $FAIL failed, $SKIP skipped"
[ "$FAIL" -eq 0 ]
