#!/bin/bash
#
# measure_provision_sabotage.sh — re-measure tests/arca_provision_test.sh group F
# against deliberately broken copies of lib/arca-provision.sh.
#
#   wsl.exe -e bash -lc "cd /mnt/c/work/roomwizard && bash tests/measure_provision_sabotage.sh"
#
# Host-only: no device, no card, no root.
#
# ── Why this file exists ────────────────────────────────────────────────────
#
# A one-of-eight install defect shipped past 94 passing cases in arca_provision_test.sh:
# `arca_provision_online_script` installed the first file of the eight and silently
# dropped the rest. Group F is the answer,
# and a group that has only ever been seen passing is not evidence that it can
# fail — which is exactly the property that defect disproved about the suite it was added
# to. So every case below breaks one thing and names which assertions notice.
#
# ⚠️ It stages ONE FILE, via $ARCA_PROVISION_LIB. `cp -a` of a directory out of
# /mnt/c into WSL /tmp can exceed a 300 s budget and looks indistinguishable from a
# hung suite (that is measured, not feared). Sabotage 5 is the exception: it patches
# the SUITE, so it stages a copy of it under tests/ — the suite resolves REPO_DIR
# from its own location, so a copy in /tmp would measure nothing.
#
# ⚠️ Every sabotage is asserted APPLIED before its count is believed, in two
# independent ways: the staged file must differ from the shipped one, AND the
# intended text must be present. A sed that stops matching leaves the file correct,
# the suite passes, and "0 failed" reads exactly like "this sabotage is
# undetectable". Restoring is a `cp` from the shipped file — never `git checkout`,
# which would revert the uncommitted fix this harness is measuring.
#
# ── What each sabotage is ───────────────────────────────────────────────────
#
#   1  the plan read on stdin, not fd 3      THE DEFECT ITSELF. The first ssh consumes the
#                                            rest of the plan and one file is copied
#   2  the missing-source check deleted      an install whose device-files/ source
#                                            was renamed silently copies nothing and
#                                            the executor blames the device
#   3  the mkdir -p dropped                  an install whose target directory the
#                                            image lacks fails at scp instead of
#                                            creating it
#   4  the summary truncated to three types  the defect's own header: "35 action(s) — 8
#                                            install, 9 link, 10 unlink" accounted
#                                            for 27, hiding four verbs. It drops
#                                            link-opt AND the unknown-type fallback
#   5  the suite's stub ssh stops reading     a control on the HARNESS, not the lib:
#      its stdin                              if the stub is unfaithful, F1 cannot
#                                             reproduce the defect and all of F is vacuous
#
# ── Measured 2026-10-09, ~4 min for the whole run ────────────────────────────
#
# Counts are minimums, not equalities — a case added later may raise one. Which
# assertions fail matters more than how many, so they are named.
#
#   baseline                                    112 passed,  0 failed
#   1 the plan on stdin, not fd 3               105 passed,  7 failed
#       F2 F3 F4 F5 F7 F8 F12 — 1 of 8 copied, so the function's own got-vs-want
#                     guard returns 1, as F2 asserts, and every downstream count is short
#   2 the missing-source refusal deleted        111 passed,  1 failed
#       F10         — ⚠️ ONE case, and not the obvious one. "It returned non-zero"
#                     still passes, because scp fails on a missing source anyway;
#                     what changes is that a mkdir ran on the device first
#   3 the mkdir -p dropped                      105 passed,  7 failed
#       F2 F3 F4 F5 F7 F8 F12 — the same set as sabotage 1. Indistinguishable by
#                     count AND by name, which is honest: both mean "the copy step
#                     does not copy". F7/F8 are what separate the causes, by naming
#                     the directories, and they are in both sets
#   4 the summary truncated to 3 types          111 passed,  1 failed
#       F15         — ⚠️ NOT F14. F14 adds up the shipped plan, which compiles
#                     base+usb and holds no link-opt (the one link-opt rule is in
#                     group mdns), so F14 passed this sabotage and the sweep went
#                     red. F15 summarises a synthetic plan with every type in it
#   5 control: the stub ssh stops reading stdin 111 passed,  1 failed
#       F1 ONLY     — required to be exactly F1. An unfaithful stub must break the
#                     one case whose job is to prove the stub, and nothing else
#
# F6 passes under sabotages 1 and 3, deliberately: it compares the bytes of the
# files that DID arrive, and one correct file is still one correct file. F5 is the
# case that counts the absent ones.
#
set -u

REPO="$(cd "$(dirname "$0")/.." && pwd)"
SUITE="$REPO/tests/arca_provision_test.sh"
LIB_REL="lib/arca-provision.sh"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'

[ -f "$SUITE" ] || { echo "no suite at $SUITE"; exit 1; }
[ -f "$REPO/$LIB_REL" ] || { echo "no library at $REPO/$LIB_REL"; exit 1; }

WORK="$(mktemp -d /tmp/arca-provsab.XXXXXX)"
# ⚠️ The staged copy lives in $REPO/lib/, not in /tmp. arca_provision_validate derives
# the repo root from its own BASH_SOURCE/.. in order to check that every install
# source exists — so a library staged under /tmp reports the whole rules file
# invalid and the baseline fails for a reason no sabotage produced. Dot-prefixed and
# gitignored, and removed on every exit path.
STAGED="$REPO/lib/.arca-provision.sab.sh"
SAB_SUITE=""
cleanup() { rm -rf "$WORK"; rm -f "$STAGED"; [ -n "$SAB_SUITE" ] && rm -f "$SAB_SUITE"; }
trap cleanup EXIT INT TERM

fail=0

# ⚠️ cp from the SHIPPED file, never `git checkout` — the fix under measurement is
# uncommitted, and a checkout here would silently restore the defect.
stage() { cp "$REPO/$LIB_REL" "$STAGED"; }

run_suite() { ARCA_PROVISION_LIB="$STAGED" timeout 240 bash "$SUITE" 2>&1; }
run_suite_file() { ARCA_PROVISION_LIB="$STAGED" timeout 240 bash "$1" 2>&1; }
counts()   { grep -oE '[0-9]+ passed, [0-9]+ failed' <<<"$1" | tail -1; }
failed_n() { local n; n="$(grep -oE '[0-9]+ failed' <<<"$1" | tail -1 | grep -oE '[0-9]+')"; echo "${n:-0}"; }
names()    { grep -oE 'FAIL[^ ]*  [A-Z][0-9]+' <<<"$1" | grep -oE '[A-Z][0-9]+' | tr '\n' ' '; }

# ═══════════════════════════════════════════════════════════════════════════
echo ""
echo "  baseline — the shipped library, staged as one file"
stage
base_out="$(run_suite)"
if [ "$(failed_n "$base_out")" = 0 ]; then
    echo -e "  ${GREEN}pass${NC}  $(counts "$base_out")"
else
    echo -e "  ${RED}FAIL${NC}  the shipped library does not pass its own suite — fix that first"
    printf '%s\n' "$base_out" | grep -E 'FAIL' | head -8 | sed 's/^/        /'
    exit 1
fi

# measure <label> <file> <applied-grep> <want-min> <output>
#
# <applied-grep> may be prefixed with "!" to mean "this text must be GONE" — a
# sabotage that deletes a line has no new text to look for.
measure() {
    local label="$1" f="$2" applied="$3" want="$4" out="$5" n
    if cmp -s "$REPO/$LIB_REL" "$f"; then
        echo -e "  ${RED}NOT APPLIED${NC}  $label — the staged file is byte-identical to the shipped one"
        fail=1; return
    fi
    case "$applied" in
        '!'*)
            if grep -qE "${applied#!}" "$f"; then
                echo -e "  ${RED}NOT APPLIED${NC}  $label — /${applied#!}/ is still in it"
                fail=1; return
            fi ;;
        *)
            if ! grep -qE "$applied" "$f"; then
                echo -e "  ${RED}NOT APPLIED${NC}  $label — changed, but /$applied/ is not in it"
                fail=1; return
            fi ;;
    esac
    if ! bash -n "$f" 2>/dev/null; then
        echo -e "  ${RED}NOT APPLIED${NC}  $label — the sabotage broke the parse, so it measures nothing"
        fail=1; return
    fi
    n="$(failed_n "$out")"
    if [ "$n" -ge "$want" ]; then
        echo -e "  ${GREEN}pass${NC}  $label — $(counts "$out")"
        echo -e "        caught by: $(names "$out")"
    else
        echo -e "  ${RED}FAIL${NC}  $label — $(counts "$out"), wanted >= $want failure(s)"
        echo -e "        caught by: $(names "$out")"
        fail=1
    fi
}

# ── 1. the plan back on stdin — the defect itself ───────────────────────────
echo ""
echo "  1. the plan read on stdin instead of fd 3"
stage
sed -i 's|read -r kind mode tgt src <&3|read -r kind mode tgt src|; s|done 3< "\$plan"|done < "$plan"|' "$STAGED"
measure "1 plan on stdin" "$STAGED" 'done < "\$plan"' 6 "$(run_suite)"

# ── 2. the missing-source check deleted ─────────────────────────────────────
#
# ⚠️ This one is only detectable because F10 asserts nothing ran on the DEVICE. The
# obvious assertion — "it returned non-zero" — passes on the sabotaged library,
# because the stub scp fails on a missing source anyway. That is why F10 counts the
# ssh calls too.
echo ""
echo "  2. the missing-source refusal deleted"
stage
sed -i '/missing \$repo\/\$src/d' "$STAGED"
measure "2 no missing-source check" "$STAGED" '!missing \$repo/\$src' 1 "$(run_suite)"

# ── 3. the mkdir -p dropped ─────────────────────────────────────────────────
#
# Patched INSIDE the remote command string, so the `|| { … }` continuation and the
# ssh call itself both survive: a sabotage that only breaks the parse measures
# nothing, and replacing the whole line would orphan the next one.
echo ""
echo "  3. the mkdir -p dropped (an install target dir the image lacks)"
stage
sed -i "s|\"mkdir -p '\$dir'\"|\"true # mkdir dropped\"|" "$STAGED"
measure "3 no mkdir -p" "$STAGED" 'true # mkdir dropped' 3 "$(run_suite)"

# ── 4. the summary truncated to three types ─────────────────────────────────
echo ""
echo "  4. the plan summary truncated to install/link/unlink (the defect's own header)"
stage
sed -i 's|split("unlink install link link-opt", o, " ")|split("unlink install link", o, " ")|' "$STAGED"
sed -i '/if (!(k in seen)) out = out/d' "$STAGED"
measure "4 truncated summary" "$STAGED" 'split\("unlink install link"' 1 "$(run_suite)"

# ── 5. control on the HARNESS: the stub ssh stops reading its stdin ──────────
echo ""
echo "  5. control — the suite's stub ssh stops reading its stdin"
#
# If the stub is unfaithful, F1 stops reproducing the defect and the whole group becomes a
# vacuous pass. So F1 must FAIL here, and it is the only one that may.
stage
SAB_SUITE="$REPO/tests/.sab-provision-suite.sh"
sed 's|^cat > /dev/null  |: # NOT reading stdin |' "$SUITE" > "$SAB_SUITE"
if cmp -s "$SUITE" "$SAB_SUITE"; then
    echo -e "  ${RED}NOT APPLIED${NC}  5 stub control — the sed matched nothing in the suite"
    fail=1
else
    out5="$(run_suite_file "$SAB_SUITE")"
    n5="$(failed_n "$out5")"; names5="$(names "$out5")"
    if [ "$n5" -ge 1 ] && [ "${names5% }" = "F1" ]; then
        echo -e "  ${GREEN}pass${NC}  5 stub control — $(counts "$out5"), and only F1: $names5"
    else
        echo -e "  ${RED}FAIL${NC}  5 stub control — $(counts "$out5"), caught by: $names5"
        echo -e "        ${YELLOW}want exactly F1${NC}: an unfaithful stub must break the case that proves the stub"
        fail=1
    fi
fi
rm -f "$SAB_SUITE"; SAB_SUITE=""

echo ""
if [ "$fail" = 0 ]; then
    echo -e "  ${GREEN}✓ every sabotage was applied and caught${NC}"
    echo ""
    exit 0
fi
echo -e "  ${RED}✗ at least one sabotage was not applied or not caught${NC}"
echo ""
exit 1
