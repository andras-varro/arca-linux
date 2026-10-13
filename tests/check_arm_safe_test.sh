#!/bin/bash
# Regression for native_apps/check-arm-safe.sh — the SIGILL gate.
#
# Host-only: no device, no card, no root. Needs the ARM cross toolchain, which is
# measured present in this WSL. A missing toolchain is a
# REFUSAL, not a pass — a gate test that skips itself reports "0 failed" and is
# indistinguishable from a suite that cannot detect the breakage.
#
# The property under test: the gate is sound only on a binary that still has its
# symbol table. These binaries are Thumb-2 and objdump needs the symbol table to
# know that; stripped, it falls back to 32-bit ARM and re-reads the same bytes as
# ARM words, manufacturing sdiv/udiv out of ordinary Thumb code. So a hit in a
# stripped binary must never be counted as a failure — and must never be silently
# passed either, which is the same bug from the other side.
#
# Run:  bash tests/check_arm_safe_test.sh
set -u

REPO="$(cd "$(dirname "$0")/.." && pwd)"
# ARCA_GATE lets measure_arm_gate_sabotage.sh aim this same suite at a deliberately
# broken copy of the gate. The assertions must never be restated over there — a
# harness that re-implements what it is checking can repair the sabotage it was
# written to catch (a harness that restates the check can repair the sabotage).
GATE="${ARCA_GATE:-$REPO/native_apps/check-arm-safe.sh}"

CC=arm-linux-gnueabihf-gcc
STRIP=arm-linux-gnueabihf-strip
OBJDUMP=arm-linux-gnueabihf-objdump

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'
passed=0; failed=0

for t in "$CC" "$STRIP" "$OBJDUMP"; do
    command -v "$t" >/dev/null 2>&1 || {
        echo -e "${RED}REFUSING TO RUN: $t is not installed.${NC}" >&2
        echo "  This suite compiles its own ARM fixtures; without the cross" >&2
        echo "  toolchain it would assert nothing and report success." >&2
        echo "  Install: sudo apt install gcc-arm-linux-gnueabihf" >&2
        exit 1
    }
done
[[ -x "$GATE" ]] || { echo -e "${RED}REFUSING TO RUN: $GATE is not executable${NC}" >&2; exit 1; }

WORK="$(mktemp -d /tmp/arca-armgate.XXXXXX)"
trap 'rm -rf "$WORK"' EXIT

ok()   { passed=$((passed + 1)); echo -e "  ${GREEN}pass${NC}  $1"; }
bad()  { failed=$((failed + 1)); echo -e "  ${RED}FAIL${NC}  $1"; }

# Every assertion is made against one captured run, so a case cannot pass by
# re-running the gate with different arguments than it reported on.
run_gate() { GATE_OUT="$("$GATE" "$@" 2>&1)"; GATE_RC=$?; }

expect_rc()       { if [[ "$GATE_RC" == "$1" ]]; then ok "$2 (exit $1)"; else bad "$2 — expected exit $1, got $GATE_RC"; fi; }
expect_match()    { if grep -qE "$1" <<<"$GATE_OUT"; then ok "$2"; else bad "$2 — no line matched /$1/"; fi; }
expect_no_match() { if grep -qE "$1" <<<"$GATE_OUT"; then bad "$2 — a line matched /$1/ and must not"; else ok "$2"; fi; }

dump_on_fail() { [[ "${VERBOSE:-0}" == 1 ]] && printf '%s\n' "$GATE_OUT" | sed 's/^/        /'; return 0; }

# ── fixtures ────────────────────────────────────────────────────────────────
# clean.c divides, on purpose: on a target with no hardware divide the compiler
# emits a CALL to __aeabi_uidiv, whose NAME contains the substring "udiv". That is
# the ~45-hit false positive the gate's mnemonic-field match exists to avoid, so
# the clean fixture has to contain it or that property goes untested.
cat > "$WORK/clean.c" <<'EOF'
#include <stdio.h>
unsigned volatile a = 4000, b = 7;
int main(void) { printf("%u\n", a / b); return 0; }
EOF

echo ""
echo "  compiling fixtures..."
# armv7-a has NO integer divide: a/b becomes a call to the software helper.
"$CC" -O2 -static -mthumb -march=armv7-a -o "$WORK/clean" "$WORK/clean.c" 2>/dev/null \
    || { echo -e "${RED}fixture build failed (clean)${NC}"; exit 1; }
# cortex-a15 HAS it: the same source emits a real udiv instruction.
"$CC" -O2 -static -mthumb -mcpu=cortex-a15 -o "$WORK/idiv" "$WORK/clean.c" 2>/dev/null \
    || { echo -e "${RED}fixture build failed (idiv)${NC}"; exit 1; }
cp "$WORK/clean" "$WORK/clean.stripped" && "$STRIP" "$WORK/clean.stripped"
cp "$WORK/idiv"  "$WORK/idiv.stripped"  && "$STRIP" "$WORK/idiv.stripped"
gcc -O2 -o "$WORK/host" "$WORK/clean.c" 2>/dev/null \
    || { echo -e "${RED}fixture build failed (host gcc)${NC}"; exit 1; }

# The fixtures are only meaningful if they are what they claim to be. Assert it
# here rather than trusting the compiler flags: a gate test whose "clean" binary
# silently carried a udiv, or whose "idiv" one did not, would invert every result.
# ⚠️ The mnemonic test must go through awk, not `grep -E`. GNU grep -E does not
# interpret \t as a tab — it reads it as a literal "t" — so `grep -qE '\t(sdiv|udiv)\t'`
# matches nothing on any input and both of these assertions would pass regardless of
# what the compiler emitted. That is how 0b first "passed" against a fixture that had
# no udiv in it. awk does interpret \t, which is why the gate itself uses awk.
has_divide() { "$OBJDUMP" -d "$1" | awk '/\t(sdiv|udiv)(\.w)?\t/ { found = 1 } END { exit !found }'; }

echo ""
echo "0. the fixtures are what they claim to be"
if has_divide "$WORK/clean"; then
    bad "0a clean fixture has no hardware divide"
else
    ok "0a clean fixture has no hardware divide"
fi
if has_divide "$WORK/idiv"; then
    ok "0b idiv fixture really does carry one"
else
    bad "0b idiv fixture really does carry one — -mcpu=cortex-a15 did not emit udiv"
fi
if "$OBJDUMP" -d "$WORK/clean" | grep -q '__aeabi_uidiv\|__udivsi3'; then
    ok "0c clean fixture calls the software helper (the substring false positive)"
else
    bad "0c clean fixture calls the software helper (the substring false positive)"
fi
if "$OBJDUMP" -t "$WORK/clean.stripped" 2>/dev/null | grep -q '\.text'; then
    bad "0d stripped fixture really has no symbol table"
else
    ok "0d stripped fixture really has no symbol table"
fi

# ── 1. the sound cases: symbol table present ──────────────────────────────
echo ""
echo "1. with a symbol table the gate is sound, and still fires"
run_gate "$WORK/clean"
expect_rc 0 "1a an unstripped clean binary passes"
expect_match 'no hardware divide in 1 binaries' "1b ...and says so"
expect_no_match 'would SIGILL' "1c ...with no failure line"

run_gate "$WORK/idiv"
expect_rc 1 "1d an unstripped binary with a real udiv is refused"
expect_match 'would SIGILL' "1e ...naming the SIGILL risk"
expect_match '(sdiv|udiv)' "1f ...and printing the instruction"

# ── 2. the unsound case: no symbol table ──────────────────────────────────
# objdump reads Thumb-2 as ARM and manufactures a divide out of
# ordinary code, so the verdict is worthless in BOTH directions.
echo ""
echo "2. without a symbol table the gate must not pretend to a verdict"
run_gate "$WORK/clean.stripped"
expect_rc 2 "2a a stripped binary is unverifiable, not a failure"
expect_no_match 'would SIGILL' "2b ...so it is not reported as a SIGILL risk"
expect_match 'not verified|could not be verified|unverifi' "2c ...and says it could not verify it"
expect_no_match '✓ ARM-safe' "2d ...and does not claim it is ARM-safe"
dump_on_fail

# The accepted residue, asserted so it stays deliberate: a stripped binary that
# genuinely does carry a udiv also comes back unverifiable. The gate cannot see it
# — only the build-time run on the unstripped artifact can. What it must never do
# is call it clean.
run_gate "$WORK/idiv.stripped"
expect_rc 2 "2e a stripped binary that IS bad is also unverifiable (the known limit)"
expect_no_match '✓ ARM-safe' "2f ...and is still not called ARM-safe"

# ── 3. mixed input: the bundle's actual shape ──────────────────────────────
# 18 unstripped native_apps binaries plus a stripped scummvm and vnc_client. The
# installer's whole failure was this case, so the counts must be separable.
echo ""
echo "3. mixed input reports both counts separately"
run_gate "$WORK/clean" "$WORK/clean.stripped"
expect_rc 2 "3a one verified + one unverifiable exits 2"
expect_match 'no hardware divide in 1 binaries' "3b ...counts the verified one as verified"
expect_match 'ARM-SUMMARY checked=1 unverified=1 bad=0' "3c ...and emits a machine-readable summary"
dump_on_fail

run_gate "$WORK/idiv" "$WORK/clean.stripped"
expect_rc 1 "3d a real hit outranks an unverifiable one"
expect_match 'ARM-SUMMARY checked=1 unverified=1 bad=1' "3e ...and the summary carries all three counts"

# ── 4. behaviour the fix must not regress ──────────────────────────────────
echo ""
echo "4. pre-existing guarantees still hold"
run_gate "$WORK/host"
expect_rc 0 "4a a non-ARM binary is skipped, not failed"
expect_match 'skipped 1 non-ARM' "4b ...and the skip is reported, not silent"
expect_no_match 'no hardware divide in 1 binaries' "4c ...and is not counted as checked"

run_gate "$WORK/no_such_file"
expect_rc 1 "4d a missing file is a failure, not a pass"

echo ""
echo "════════════════════════════════════════"
if [[ "$failed" -eq 0 ]]; then
    echo -e "  ${GREEN}$passed passed, 0 failed${NC}"
    exit 0
fi
echo -e "  ${RED}$passed passed, $failed failed${NC}"
echo -e "  ${YELLOW}re-run with VERBOSE=1 to see the gate's output${NC}"
exit 1
