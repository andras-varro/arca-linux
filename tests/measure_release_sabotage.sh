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
SAB=/tmp/arca-release.sab.$$
HARNESS_ERR=0
cp release.sh "$BAK"
mkdir -p "$SAB"
restore() { cp "$BAK" release.sh; }
trap 'restore; rm -rf "$BAK" "$SAB"' EXIT INT TERM

run() {
    if [ "${1:-}" = sab ] && cmp -s release.sh "$BAK"; then
        echo "SABOTAGE DID NOT APPLY - release.sh is byte-identical to the shipped one"; HARNESS_ERR=1; return; fi
    ./tests/arca_release_test.sh 2>&1 | sed 's/\x1b\[[0-9;]*m//g' | grep -oE '[0-9]+ passed, [0-9]+ failed'; }

echo ""
echo -n "  baseline (nothing broken)             "; run

# ── 1. the whole pre-fix file ───────────────────────────────────────────────
# ARCA_SAB_COPIES_ONLY=1 skips stanzas 1-8, which edit release.sh in place (and restore it
# from a copy), and runs only the stanzas that work on copies.
if [ -z "${ARCA_SAB_COPIES_ONLY:-}" ]; then
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
fi

# ── 9+. the source-offer cases (group E), on COPIES ─────────────────────────
# The suite's RELEASE_SH points it at a copy, so release.sh itself is never touched by these
# stanzas. Each prints the count and the cases that failed: every one must name at least one
# group E case, and the baseline above must stay at 0 failed.
#
# Case 9 is the real pre-change file (`git show d79b40c:release.sh`, the commit before
# --source-offer existed; pinned, because HEAD stops being pre-change at the next commit).
runcopy() {
    if cmp -s "$SAB/cur.sh" release.sh; then
        echo "SABOTAGE DID NOT APPLY - the copy is byte-identical to the shipped release.sh"; HARNESS_ERR=1; return; fi
    local out
    out="$(RELEASE_SH="$SAB/cur.sh" ./tests/arca_release_test.sh 2>&1 | sed 's/\x1b\[[0-9;]*m//g')"
    echo "$(echo "$out" | grep -oE '[0-9]+ passed, [0-9]+ failed')   $(echo "$out" | grep '^  FAIL' | awk '{print $2}' | head -14 | tr '\n' ' ')"
}
start_copy() { cp release.sh "$SAB/cur.sh"; }   # each case then edits the copy with perl
report() { printf '  %-44s' "$1"; runcopy; }
printf '  %-44s' "9. the pre-change release.sh entire"
git show d79b40c:release.sh > "$SAB/cur.sh"; runcopy
start_copy; perl -0pi -e 's/bash "\$SCRIPT_DIR\/rootfs\/make-source-offer.sh" "\$\{SOURCE_ARGS\[\@\]\}" \x5c\n/true \x5c\n/' "$SAB/cur.sh"; report "10. the offer script is never run"
start_copy; perl -0pi -e 's/\[\[ \$STAGE_ONLY -eq 0 \]\] \|\| SOURCE_ARGS\+=\(--allow-dev\)/:/' "$SAB/cur.sh"; report "11. --allow-dev dropped from --stage-only"
start_copy; perl -0pi -e 's/ASSETS\+=\("\$SOURCE_OUT\/\$SOURCE_FILE" "\$SOURCE_OUT\/\$SOURCE_FILE.sha256"\)/:/' "$SAB/cur.sh"; report "12. source file not attached to the release"
start_copy; perl -0pi -e 's/ASSETS\+=\("\$\{IMAGES\[\@\]\}"\)/:/' "$SAB/cur.sh"; report "13. images not attached to the release"
start_copy; perl -0pi -e 's/if \[\[ \$SOURCE_OFFER -eq 1 \]\]; then\ncat <<\x27NOTICE\x27\nCorresponding/if false; then\ncat <<\x27NOTICE\x27\nCorresponding/' "$SAB/cur.sh"; report "14. NOTICE never names the source file"
start_copy; perl -0pi -e 's/if \[\[ \$\{#IMAGES\[\@\]\} -gt 0 && \$SOURCE_OFFER -eq 0 \]\]; then/if false; then/' "$SAB/cur.sh"; report "15. --image allowed without --source-offer"
start_copy; perl -0pi -e 's/\[\[ -s "\$SOURCE_OUT\/\$SOURCE_FILE" && -s "\$SOURCE_OUT\/\$SOURCE_FILE.sha256" \]\] \x5c\n[^\n]*\n/:\n/' "$SAB/cur.sh"; report "16. the written-file check dropped"
start_copy; perl -0pi -e 's/"\$\(pwd -P\)"\/\*\) err/"\/no-such-root-ever"\/*) err/' "$SAB/cur.sh"; report "17. the inside-the-repo refusal dropped"
start_copy; perl -0pi -e 's/SOURCE_ARGS=\(--tag "\$RELEASE_TAG"/SOURCE_ARGS=(--tag "\${TAG:-untagged}"/' "$SAB/cur.sh"; report "18. the offer gets a different tag"
start_copy; perl -0pi -e 's/\$\{GIT_DIRTY:\+-dev\}//' "$SAB/cur.sh"; report "19. no -dev in the name release.sh expects"
start_copy; perl -0pi -e '($b)=/(# ── the source offer.*?)(?=for comp in)/s; s/\Q$b\E//; s/(# ── bundle metadata)/$b$1/' "$SAB/cur.sh"; report "20. the offer moved after the components"
start_copy; perl -0pi -e '($b)=/(# ── the source offer.*?)(?=for comp in)/s; s/\Q$b\E//; s/(# ── the host gate)/$b$1/' "$SAB/cur.sh"; report "21. the offer moved before the host gate"
start_copy; perl -0pi -e 's/ && -f "\$2" \]\] \|\| \{ echo "no such image/ ]] || { echo "no such image/' "$SAB/cur.sh"; report "22. the --image existence check dropped"
start_copy; perl -0pi -e 's/err "the parent of --source-out/true "the parent of --source-out/' "$SAB/cur.sh"; report "23. the --source-out parent check dropped"
start_copy; perl -0pi -e 's/\[\[ \$STAGE_ONLY -eq 0 \]\] \|\| SOURCE_ARGS\+=\(--allow-dev\)/SOURCE_ARGS+=(--allow-dev)/' "$SAB/cur.sh"; report "24. --allow-dev also when publishing"
start_copy; perl -0pi -e 's/echo "Corresponding source \(GPLv2[^\n]*\n/:\n/' "$SAB/cur.sh"; report "25. release notes do not name the source"
start_copy; perl -0pi -e 's/apps-\$\{RELEASE_TAG\}/apps-\${TAG:-x}/' "$SAB/cur.sh"; report "26. bundle tarball tagged differently"
start_copy; perl -0pi -e 's/if \[\[ \$SOURCE_OFFER -eq 1 \]\]; then\n        ASSETS\+=/if true; then\n        ASSETS+=/' "$SAB/cur.sh"; report "27. source attached even without the flag"
start_copy; perl -0pi -e 's/if \[\[ \$SOURCE_OFFER -eq 1 \]\]; then\n    echo ""\n    echo "─/if true; then\n    echo ""\n    echo "─/' "$SAB/cur.sh"; report "28. the offer is run without the flag"
start_copy; perl -0pi -e 's/if \[\[ \$SOURCE_OFFER -eq 1 \]\]; then\ncat <<\x27NOTICE\x27\nCorresponding/if true; then\ncat <<\x27NOTICE\x27\nCorresponding/' "$SAB/cur.sh"; report "29. NOTICE always names the source file"

restore
echo ""
echo "  (no case here can exercise a real four-component build: release.sh has no"
echo "   --no-build flag, and the suite stubs the component script. That hole is"
echo "   named in tests/arca_release_test.sh's header and in tests/CLAUDE.md.)"
echo ""
# A stanza that edited nothing is a harness error, not a verdict: fail the run.
[ "$HARNESS_ERR" -eq 0 ] || { echo "HARNESS ERROR: a sabotage matched nothing"; exit 1; }
