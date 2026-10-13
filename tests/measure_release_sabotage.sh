#!/bin/bash
#
# Measure tests/arca_release_test.sh against deliberately broken copies of
# release.sh, and print the counts that suite's header rests on.
#
#   wsl.exe -e bash -lc "cd /mnt/c/work/roomwizard && bash tests/measure_release_sabotage.sh"
#
# In a FILE and not in argv, because a pattern
# containing `$`, `[` or a tab does not survive being quoted through
# `wsl.exe -e bash -lc "..."`, and a sabotage that fails to apply reports "0
# failed" — which reads exactly like a suite that cannot detect the breakage.
#
# ⚠️ Restore is a `cp` from a copy taken up front, never `git checkout`: the fix
# being measured is uncommitted while this runs, and a git restore would destroy it.
#
# ⚠️ Case 1 is the real pre-fix tree (`git show ba7eded^:release.sh`) and its count
# includes failures that are NOT about the guard — that revision predates the
# hoisted publish preflight and the provenance stamp, so group B's structural
# cases fail too. The targeted cases below are what attribute the count.
#
# Safe to run: the suite replaces `rm` with a tripwire for every guard case, so a
# sabotaged guard that accepts `--out /` logs the path and dies instead of running.

set -u
cd "$(dirname "$0")/.." || exit 1

BAK=/tmp/arca-release.orig.$$
HARNESS_ERR=0
cp release.sh "$BAK"
restore() { cp "$BAK" release.sh; }
trap 'restore; rm -f "$BAK"' EXIT INT TERM

run() {
    if [ "${1:-}" = sab ] && cmp -s release.sh "$BAK"; then
        echo "SABOTAGE DID NOT APPLY - release.sh is byte-identical to the shipped one"; HARNESS_ERR=1; return; fi
    ./tests/arca_release_test.sh 2>&1 | sed 's/\x1b\[[0-9;]*m//g' | grep -oE '[0-9]+ passed, [0-9]+ failed'; }

echo ""
echo -n "  baseline (nothing broken)             "; run

# ── 1. the whole pre-fix file ───────────────────────────────────────────────
# The guard as it stood before 2026-09-07: four checks, no normalisation, no
# ownership check, and a QUOTED "/*" case pattern that is a literal.
restore
git show ba7eded^:release.sh > release.sh
echo -n "  the pre-fix release.sh entire        "; run sab

# ── 2. the normalisation dropped ────────────────────────────────────────────
# OUT_NORM is what makes "//", "///" and "/." unable to spell root past a string
# test. Without it only the exact string "/" is caught.
restore
perl -0pi -e 's/^OUT_NORM=.*$/OUT_NORM="\$OUT"/m' release.sh
echo -n "  --out normalisation dropped          "; run sab

# ── 3. the ownership check dropped ──────────────────────────────────────────
# The substitute for del()'s containment stage. Without it any non-empty
# directory — /usr, or an operator's own tree — goes into the rm -rf.
restore
perl -0pi -e 's/if \[\[ -d "\$OUT" && -n "\$\(ls -A "\$OUT" 2>\/dev\/null\)" \]\]; then/if false; then/' release.sh
echo -n "  the ownership check dropped          "; run sab

# ── 4. the quoted-glob regression, on its own ───────────────────────────────
# The exact defect: the guard reduced to its two cheap string refusals, with the
# "/*" pattern that matches only the literal two characters.
restore
perl -0pi -e 's/^OUT_NORM=.*\n(case "\$OUT_NORM" in\n)    ""\|"\/"\) err[^\n]*\n/$1    ""|"\/"|"\/*") err "refusing to stage into \x27\$OUT\x27" ;;\n/m' release.sh
perl -0pi -e 's/if \[\[ -d "\$OUT" && -n "\$\(ls -A "\$OUT" 2>\/dev\/null\)" \]\]; then/if false; then/' release.sh
echo -n "  the quoted-glob guard restored       "; run sab

# ── 5. the staged-changes half of the dirty check ───────────────────────────
# `git diff --quiet` alone. C3 is the only case that can see this, and C1/C2/C4
# must still pass — that is what makes C3 an attribution rather than a wobble.
restore
perl -0pi -e 's/^   \|\| ! git -C "\$SCRIPT_DIR" diff --cached --quiet 2>\/dev\/null; then$/   ; then/m' release.sh
echo -n "  the --cached dirty check dropped     "; run sab

# ── 6. the post-build re-check's --cached half ──────────────────────────────
# C5 alone must fail: it is the only case that can reach a check --stage-only
# never runs.
restore
perl -0pi -e 's/^       \|\| ! git -C "\$SCRIPT_DIR" diff --cached --quiet 2>\/dev\/null; then$/       ; then/m' release.sh
echo -n "  the post-build --cached half dropped "; run sab

# ── 7. a control on the HARNESS, not on the code ────────────────────────────
# A release.sh that refuses everything. Every group A case exits non-zero, so a
# suite asserting only "it failed" would go green here. The whole of B and C must
# fail instead — if this line reports a small number, group A is passing on exit
# status alone and the tripwire is not being read.
restore
printf '#!/bin/bash\nexit 1\n' > release.sh
echo -n "  release.sh replaced by 'exit 1'      "; run sab

# ── 8. the tripwire defeated ────────────────────────────────────────────────
# The other harness control: a script that deletes and then succeeds. Group A
# must fail wholesale — its refusals are asserted on the rm never being reached,
# not on the exit status.
restore
printf '#!/bin/bash\nexit 0\n' > release.sh
echo -n "  release.sh replaced by 'exit 0'      "; run sab

restore
echo ""
echo "  (no case here can exercise a real four-component build: release.sh has no"
echo "   --no-build flag, and the suite stubs the component script. That hole is"
echo "   named in tests/arca_release_test.sh's header and in tests/CLAUDE.md.)"
echo ""
# A stanza that edited nothing is a harness error, not a verdict: fail the run.
[ "$HARNESS_ERR" -eq 0 ] || { echo "HARNESS ERROR: a sabotage matched nothing"; exit 1; }
