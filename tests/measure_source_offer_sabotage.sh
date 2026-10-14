#!/bin/bash
#
# Measure group F of tests/arca_source_offer_test.sh (the licence documents make-card-image.sh
# puts on p6) against deliberately broken COPIES of rootfs/make-card-image.sh, and print, per
# sabotage, the counts and the cases that failed.
#
#   wsl.exe -e bash -lc "cd /mnt/c/work/roomwizard && bash tests/measure_source_offer_sabotage.sh"
#
# The suite's MCI variable points it at the copy, so the shipped file is never edited and there
# is nothing to restore (never `git checkout` a sabotage: it would destroy the fix under test).
# A sabotage that changed nothing is a harness error and fails the run, because "0 failed" from
# an edit that did not apply reads exactly like a suite that cannot see the breakage.
#
# Case 1 is the real pre-change file (`git show d79b40c:rootfs/make-card-image.sh`, pinned: HEAD
# stops being pre-change at the next commit). Every other case must name at least one F case.

set -u
cd "$(dirname "$0")/.." || exit 1

SAB="$(mktemp -d /tmp/arca-mci-sab.XXXXXX)"
trap 'rm -rf "$SAB"' EXIT INT TERM
HARNESS_ERR=0

run() { # <label>: grade $SAB/cur.sh
    local out
    if cmp -s "$SAB/cur.sh" rootfs/make-card-image.sh; then
        printf '  %-48s SABOTAGE DID NOT APPLY\n' "$1"; HARNESS_ERR=1; return
    fi
    out="$(MCI="$SAB/cur.sh" ./tests/arca_source_offer_test.sh 2>&1 | sed 's/\x1b\[[0-9;]*m//g')"
    printf '  %-48s %s   %s\n' "$1" "$(echo "$out" | grep -oE '[0-9]+ passed, [0-9]+ failed')" \
        "$(echo "$out" | grep '^  FAIL' | awk '{print $2}' | head -14 | tr '\n' ' ')"
}
start_copy() { cp rootfs/make-card-image.sh "$SAB/cur.sh"; }   # each case then edits the copy with perl
report() { run "$1"; }

printf '  %-48s ' "baseline (nothing broken)"
MCI="$PWD/rootfs/make-card-image.sh" ./tests/arca_source_offer_test.sh 2>&1 | sed 's/\x1b\[[0-9;]*m//g' | grep -oE '[0-9]+ passed, [0-9]+ failed'

git show d79b40c:rootfs/make-card-image.sh > "$SAB/cur.sh"; run "1. the pre-change make-card-image.sh entire"
start_copy; perl -0pi -e 's/    install -m 0644 "\$repo\/LICENSE.md"[^\n]*\n//' "$SAB/cur.sh"; report "2. LICENSE.md not copied"
start_copy; perl -0pi -e 's/        install -m 0644 "\$legal\/manifest.csv"[^\n]*\n//' "$SAB/cur.sh"; report "3. manifest.csv not copied"
start_copy; perl -0pi -e 's/        cp -r --no-preserve=mode,ownership[^\n]*\n//' "$SAB/cur.sh"; report "4. licenses/ not copied"
start_copy; perl -0pi -e 's/cp -r --no-preserve=mode,ownership/cp -r/; s/        chmod -R u=rwX,go=rX[^\n]*\n//' "$SAB/cur.sh"; report "5. licenses/ copied with its source modes"
start_copy; perl -0pi -e 's/echo "    \$asset"/:/' "$SAB/cur.sh"; report "6. the offer never names the source file"
start_copy; perl -0pi -e 's/if \[ -n "\$tag" \]; then/if false; then/' "$SAB/cur.sh"; report "7. the offer ignores the tag"
start_copy; perl -0pi -e 's/\[ -z "\$url" \] \|\| echo "Release page: \$url"/:/' "$SAB/cur.sh"; report "8. the release page URL dropped"
start_copy; perl -0pi -e 's/if \[ ! -f "\$legal\/manifest.csv" \] \|\| \[ ! -d "\$legal\/licenses" \]; then/if false; then/' "$SAB/cur.sh"; report "9. a bad legal-info tree accepted"
start_copy; perl -0pi -e 's/            echo "\(this image carries no package[^\n]*\n/            :\n/' "$SAB/cur.sh"; report "10. no text when there is no legal-info"
start_copy; perl -0pi -e 's/if \[ "\$LEGAL_GIVEN" -eq 1 \] && \{[^\n]*; then\n/if false; then\n/' "$SAB/cur.sh"; report "11. an explicit --legal-info not checked"
start_copy; perl -0pi -e 's/die "--source-tag may contain only[^\n]*;; esac/:  ;; esac/' "$SAB/cur.sh"; report "12. --source-tag not validated"
start_copy; perl -0pi -e 's/licence_docs_install "\$MNT\/p"/true "\$MNT\/p"/' "$SAB/cur.sh"; report "13. the p6 fill does not call it"

echo ""
[ "$HARNESS_ERR" -eq 0 ] || { echo "HARNESS ERROR: a sabotage matched nothing"; exit 1; }
