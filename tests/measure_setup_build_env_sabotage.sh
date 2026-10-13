#!/bin/bash
# Sabotage sweep for tests/setup_build_env_test.sh.
#
# There is no CI, so a suite that has only ever been seen passing is not evidence that it
# can fail. This breaks setup-build-env.sh in five specific ways and asserts that the suite
# notices each one, naming the cases that must go red. A sabotage that produces "0 failed"
# is a hole in the suite, not a clean bill.
#
# ⚠️ Nothing is restored with git. The working tree may hold an uncommitted fix that a
# `git checkout` would destroy; every sabotage here is applied to a COPY under /tmp and the
# original is never written.
#
# ⚠️ The sed patterns live in this file rather than in argv, because a pattern containing
# `\t` does not survive `wsl.exe -e bash -lc` quoting and one containing `||` silently
# matches nothing. Each sabotage verifies its own patch APPLIED before drawing a conclusion
# — a sed that matched nothing reports "not caught" for the wrong reason, which is
# indistinguishable from a suite that cannot detect the breakage.
#
# Run:  bash tests/measure_setup_build_env_sabotage.sh
set -u

REPO="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$REPO/setup-build-env.sh"
SUITE="$REPO/tests/setup_build_env_test.sh"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'

[ -f "$SRC" ]   || { echo "no $SRC" >&2; exit 1; }
[ -f "$SUITE" ] || { echo "no $SUITE" >&2; exit 1; }
[ "$(uname -s)" = "Linux" ] || { echo "REFUSING: needs Linux (WSL)" >&2; exit 1; }

WORK="$(mktemp -d /tmp/arca-setupenv-sab.XXXXXX)"
trap 'rm -rf "$WORK"' EXIT

# ── the baseline ────────────────────────────────────────────────────────────
# ⚠️ A pre-fix-tree control is vacuous if the suite dies for an unrelated reason, so the
# unmodified copy must be measured green FIRST. If this is red, every "caught" below is
# meaningless.
echo ""
echo "════════════════════════════════════════"
echo " baseline: the unmodified script"
echo "════════════════════════════════════════"
cp "$SRC" "$WORK/baseline.sh"; chmod +x "$WORK/baseline.sh"
base_out="$(ARCA_SETUP="$WORK/baseline.sh" bash "$SUITE" 2>&1)"; base_rc=$?
base_n="$(grep -cE '^  .{0,20}FAIL' <<<"$base_out")"
if [ "$base_rc" -eq 0 ] && [ "$base_n" -eq 0 ]; then
    echo -e "  ${GREEN}green${NC} — $(grep -oE '[0-9]+ passed' <<<"$base_out" | tail -1), 0 failed"
else
    echo -e "  ${RED}BASELINE IS RED ($base_n failed, exit $base_rc) — every result below is vacuous${NC}"
    printf '%s\n' "$base_out" | sed 's/^/    /'
    exit 1
fi

total_sab=0; caught=0; missed=0

# $1 label   $2 sed expression   $3 cases that must fail
sabotage() {
    local label="$1" expr="$2" expect="$3"
    total_sab=$((total_sab + 1))
    local f="$WORK/sab.sh"
    cp "$SRC" "$f"
    sed -i "$expr" "$f"
    chmod +x "$f"

    echo ""
    echo -e "${YELLOW}$total_sab. $label${NC}"

    # Did the patch actually change anything? A rotted pattern is the failure mode this
    # check exists for.
    if cmp -s "$SRC" "$f"; then
        echo -e "  ${RED}SED DID NOT APPLY — pattern has rotted, this measures nothing${NC}"
        echo "    $expr"
        missed=$((missed + 1))
        return
    fi
    # A sabotage that breaks the syntax is not a sabotage; the suite would fail for a
    # reason unrelated to the property under test.
    if ! bash -n "$f" 2>/dev/null; then
        echo -e "  ${RED}SABOTAGED COPY DOES NOT PARSE — measures nothing${NC}"
        missed=$((missed + 1))
        return
    fi

    local out n
    out="$(ARCA_SETUP="$f" bash "$SUITE" 2>&1)"
    n="$(grep -cE '^  .{0,20}FAIL' <<<"$out")"
    if [ "$n" -gt 0 ]; then
        echo -e "  ${GREEN}caught${NC} — $n case(s) failed; expected $expect"
        grep -E '^  .{0,20}FAIL' <<<"$out" | sed 's/^/    /'
        caught=$((caught + 1))
    else
        echo -e "  ${RED}NOT CAUGHT${NC} — the suite stayed green. Expected $expect to fail."
        missed=$((missed + 1))
    fi
}

# ── 1. the bug this suite was written around ────────────────────────────────
# Rename the selection array back to GROUPS, which bash owns: the assignment is silently
# refused and the selection becomes the invoking user's numeric group IDs. Nothing matches
# the table. This is the real defect the first draft shipped with.
sabotage "SEL_GROUPS renamed back to GROUPS (a bash special variable)" \
    's/\bSEL_GROUPS\b/GROUPS/g' \
    "1a-1e (the probe loop selects nothing)"

# ── 2. the zero-subjects guard ──────────────────────────────────────────────
# Without it, a selection matching nothing prints "0 of 0 present, 0 missing" and exits 0 —
# a green gate over a host it never looked at. This is what made sabotage 1 survivable.
sabotage "the zero-subjects guard removed" \
    's/^if \[ "\$seen" -eq 0 \]; then$/if [ "$seen" -eq -1 ]; then/' \
    "3a-3c (a bogus group reports success)"

# ── 3. installing without being asked ───────────────────────────────────────
# The TTY test is what keeps a blocking prompt out of release.sh and deploy-all.sh.
# ⚠️ Forcing it true does NOT reach sudo: `read` against /dev/null returns empty, which
# falls into the decline branch, so the exit status and the tripwire both stay correct and
# only the explanatory line goes missing. That is why sabotage 6 exists — this one alone
# would leave case 4c, the tripwire, unswept.
sabotage "the non-TTY branch takes the interactive path" \
    's/^    elif \[ -t 0 \]; then$/    elif true; then/' \
    "4b only (the decline branch still refuses, so 4a and 4c stay green)"

# ── 4. printing one command and running another ─────────────────────────────
# The promise is that the operator SEES what runs. Drop a package from the printed line
# only: every other case still passes, and 5c is the one that notices.
sabotage "the printed command no longer matches the one executed" \
    's|^    echo "  sudo apt-get install -y \${MISSING\[\*\]}"$|    echo "  sudo apt-get install -y ${MISSING[0]}"|' \
    "5c (printed and executed differ)"

# ── 5. guessing at a non-Debian host ────────────────────────────────────────
# apt-only with a clean refusal is deliberate: a wrong package manager that half-succeeds
# is worse than a refusal naming the set.
sabotage "the apt-get check removed, so a non-Debian host is guessed at" \
    's/^    if ! command -v apt-get >\/dev\/null 2>&1; then$/    if false; then/' \
    "6a-6b (no refusal on a host without apt-get)"

# ── 6. the tripwire's own control ───────────────────────────────────────────
# ⚠️ Case 4c asserts sudo was NEVER called. An assertion that something did not happen
# passes for free in any run where the code path is simply unreachable — so unless some
# sabotage actually reaches sudo unbidden, 4c is a vacuous pass. Defaulting DO_INSTALL to 1
# is that sabotage: it installs with no TTY and no flag, and 4c is what notices.
sabotage "DO_INSTALL defaults to 1, so every run installs unbidden" \
    's/^DO_INSTALL=0$/DO_INSTALL=1/' \
    "4a and 4c (sudo reached with neither a TTY nor --install-deps)"

# ── 7. argument handling ────────────────────────────────────────────────────
# An unknown argument must be refused rather than ignored, or a typo runs a different job
# than the one asked for.
sabotage "an unknown argument is ignored instead of refused" \
    's|^        \*)              echo "Unknown argument: \$1" >&2; echo "" >&2; usage >&2; exit 2 ;;$|        *)              : ;;|' \
    "7a (a bogus flag no longer refuses)"

sabotage "--group stops checking that it was given a value" \
    's/^                        \[ "\$#" -gt 0 \] || { echo "--group needs a value" >&2; exit 2; }$/                        : ;/' \
    "7b (--group with no value no longer refuses)"

# ── verdict ─────────────────────────────────────────────────────────────────
echo ""
echo "════════════════════════════════════════"
if [ "$missed" -eq 0 ]; then
    echo -e "  ${GREEN}$caught of $total_sab sabotages caught${NC}"
    exit 0
fi
echo -e "  ${RED}$caught of $total_sab caught, $missed NOT caught${NC}"
echo -e "  ${YELLOW}an uncaught sabotage is a hole in the suite, not a clean bill${NC}"
exit 1
