#!/bin/bash
#
# measure_bundle_ssh_sabotage.sh — measure tests/arca_bundle_ssh_test.sh against
# deliberately broken copies of arca_bundle_install_ssh. The counts in that file's
# header come from here.
#
#   wsl.exe -e bash -lc "cd /mnt/c/work/roomwizard && bash tests/measure_bundle_ssh_sabotage.sh"
#
# ⚠️ Every sabotage ASSERTS THAT IT APPLIED before the suite is run. Without that,
# a pattern that no longer matches reports "0 failed" — which reads exactly like a
# suite that cannot detect the breakage. That is not hypothetical: the first run of
# these four all reported 22/0 because none of the patterns had matched.

set -u
cd "$(dirname "$0")/.." || exit 1

BAK=/tmp/arca-bundle.orig.$$
HARNESS_ERR=0
cp lib/arca-bundle.sh "$BAK"
trap 'cp "$BAK" lib/arca-bundle.sh; rm -f "$BAK"' EXIT INT TERM

run() { ./tests/arca_bundle_ssh_test.sh 2>&1 | sed 's/\x1b\[[0-9;]*m//g' | grep -oE '[0-9]+ passed, [0-9]+ failed'; }

# sab <label> <sed-expression>
sab() {
    local label="$1" expr="$2" before after
    cp "$BAK" lib/arca-bundle.sh
    before=$(md5sum lib/arca-bundle.sh | cut -d' ' -f1)
    sed -i "$expr" lib/arca-bundle.sh
    after=$(md5sum lib/arca-bundle.sh | cut -d' ' -f1)
    if [ "$before" = "$after" ]; then
        printf '  %-34s DID NOT APPLY — the count below would be a lie\n' "$label"
        HARNESS_ERR=1; return
    fi
    if ! bash -n lib/arca-bundle.sh 2>/dev/null; then
        printf '  %-34s broke the syntax — not a usable sabotage\n' "$label"
        HARNESS_ERR=1; return
    fi
    printf '  %-34s %s\n' "$label" "$(run)"
}

echo ""
cp "$BAK" lib/arca-bundle.sh
printf '  %-34s %s\n' "baseline (nothing broken)" "$(run)"

# The +x measurement, which is the only check that can see a missing chmod on a
# real filesystem — and the one that cannot be demonstrated on /mnt/c at all.
sab "the +x check removed" 's|^    if \[ -n "\$xlist" \]; then$|    if false; then|'

# The md5 comparison, which is what catches a truncated transfer.
sab "md5 mismatch no longer reported" 's|^            elif \[ "\$got" != "\$want" \]; then$|            elif false; then|'

# The both-directions structural check, run BEFORE anything is written.
sab "arca_bundle_check skipped" 's|^    if ! arca_bundle_check "\$dir"; then$|    if false; then|'

# Modes taken from the transfer instead of the manifest. THE bug the declared-mode
# rule exists for: the staging tree lives on /mnt/c, where every file reads 0777 and
# chmod is discarded, so a tar that preserved modes would carry a number nobody
# measured.
sab "chmod loop dropped (trust the tar)" 's|^    done \| \$SSHC "\$target" "sh -s" > "\$dir/.chmod.out" 2>&1 \|\| bad=1$|    done >/dev/null; : > "$dir/.chmod.out"|'

# The owner-execute digit read from the END instead of by position: 0700 is
# executable by its owner and by nobody else, so this calls a correct install broken.
sab "owner digit read from the end" 's|o = (length(m) == 4) ? substr(m, 2, 1) : substr(m, 1, 1)|o = substr(m, length(m), 1)|'

# A failed transfer that proceeds to verification instead of stopping. The `return 1`
# is the behaviour, not the echo — an earlier version of this sabotage appended
# `; true` to the echo and changed nothing, then reported 0 failed.
sab "failed transfer not fatal" 's|^        return 1$|        :|'

cp "$BAK" lib/arca-bundle.sh
echo ""
printf '  %-34s %s\n' "restored" "$(run)"
echo ""
# A stanza that edited nothing is a harness error, not a verdict: fail the run.
[ "$HARNESS_ERR" -eq 0 ] || { echo "HARNESS ERROR: a sabotage matched nothing"; exit 1; }
