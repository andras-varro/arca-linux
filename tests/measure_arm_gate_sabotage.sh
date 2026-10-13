#!/bin/bash
# measure_arm_gate_sabotage.sh — re-measure tests/check_arm_safe_test.sh against
# deliberately broken copies of the gate, and print the counts its header claims.
#
# There is no CI, so a suite that has only ever been seen passing is not evidence
# that it can fail. This drives the REAL suite (via ARCA_GATE) against each sabotage;
# it never restates an assertion, because a harness that re-implements what it
# checks can repair the very defect it was written to catch.
#
# ⚠️ Every sabotage is asserted APPLIED before its count is believed. A sed pattern
# that fails to match leaves the file correct, the suite passes, and "0 failed"
# reads exactly like "this sabotage is undetectable".
#
# Host-only: no device, no card, no root. Needs the ARM cross toolchain.
set -u

REPO="$(cd "$(dirname "$0")/.." && pwd)"
GATE_REL="native_apps/check-arm-safe.sh"
SUITE="$REPO/tests/check_arm_safe_test.sh"

# The commit that carries the PRE-FIX gate: docs only, gate untouched. Restoring
# from git rather than sed-ing the current file is the point — a hand-written
# "pre-fix" copy is a guess about what the old code was.
PREFIX_REV="f958534"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'

WORK="$(mktemp -d /tmp/arca-armsab.XXXXXX)"
trap 'rm -rf "$WORK"' EXIT

fail=0

# Baseline: the shipped gate must pass, or every number below is meaningless.
echo ""
echo "  baseline — the shipped gate"
if out="$(bash "$SUITE" 2>&1)"; then
    echo -e "  ${GREEN}pass${NC}  $(grep -oE '[0-9]+ passed, [0-9]+ failed' <<<"$out" | tail -1)"
else
    echo -e "  ${RED}FAIL${NC}  the shipped gate does not pass its own suite — fix that first"
    printf '%s\n' "$out" | tail -5
    exit 1
fi

# measure <label> <applied-grep> <expected-min-failures> -- builds $WORK/gate.sh first
measure() {
    local label="$1" applied="$2" want="$3" copy="$WORK/gate.sh"

    if ! grep -qE "$applied" "$copy"; then
        echo -e "  ${RED}NOT APPLIED${NC}  $label — the sabotage did not change the file; count discarded"
        fail=$((fail + 1))
        return
    fi
    chmod +x "$copy"

    local out rc got
    out="$(ARCA_GATE="$copy" bash "$SUITE" 2>&1)"; rc=$?
    got="$(grep -oE '[0-9]+ failed' <<<"$out" | tail -1 | grep -oE '[0-9]+')"
    got="${got:-0}"

    if [[ "$rc" -ne 0 && "$got" -ge "$want" ]]; then
        echo -e "  ${GREEN}caught${NC}  $label — $got failed (expected >= $want)"
    else
        echo -e "  ${RED}MISSED${NC}  $label — exit $rc, $got failed (expected >= $want)"
        fail=$((fail + 1))
    fi
}

echo ""
echo "  sabotages"

# 1. The actual pre-fix gate: judges stripped binaries and reports the phantoms.
git -C "$REPO" show "$PREFIX_REV:$GATE_REL" > "$WORK/gate.sh" 2>/dev/null || {
    echo -e "  ${RED}NOT APPLIED${NC}  pre-fix gate — could not read $PREFIX_REV:$GATE_REL"
    fail=$((fail + 1))
}
measure "pre-fix gate restored from $PREFIX_REV" 'stripped — verify any hit' 8

# 2. False negative: a stripped binary is skipped silently, claimed clean.
#    This is the other half of the bug and the tempting "simple" fix.
sed 's/^        unverified=\$((unverified + 1))/        :/' \
    "$REPO/$GATE_REL" > "$WORK/gate.sh"
measure "stripped binaries skipped without being counted" '^        :$' 3

# 3. Counts them, but exits 0 — the caller then cannot tell it was partial.
sed 's/^\[ "\$unverified" -eq 0 \] || exit 2/[ "$unverified" -eq 0 ] || exit 0/' \
    "$REPO/$GATE_REL" > "$WORK/gate.sh"
measure "unverified reported but exit status says all-clear" '\|\| exit 0' 3

# 4. Drops the machine-readable line the installer parses. Without it
#    commission-offline.sh cannot name the count in its own warning.
sed '/^ARM-SUMMARY\|printf .ARM-SUMMARY/d' "$REPO/$GATE_REL" > "$WORK/gate.sh"
if grep -q 'ARM-SUMMARY' "$WORK/gate.sh"; then
    # the printf spans two lines; drop its continuation too
    sed '/ARM-SUMMARY/,+1d' "$REPO/$GATE_REL" > "$WORK/gate.sh"
fi
measure "ARM-SUMMARY line removed" '^summary_line\(\) \{' 2

echo ""
echo "════════════════════════════════════════"
if [[ "$fail" -eq 0 ]]; then
    echo -e "  ${GREEN}every sabotage applied and was caught${NC}"
    exit 0
fi
echo -e "  ${RED}$fail sabotage(s) not applied or not caught${NC}"
echo -e "  ${YELLOW}an unapplied sabotage measures nothing — treat it as a harness bug${NC}"
exit 1
