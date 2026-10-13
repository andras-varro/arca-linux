#!/bin/bash
#
# measure_ssh_sabotage.sh — measure tests/arca_ssh_test.sh against deliberately broken
# copies of lib/arca-ssh.sh and of the call sites. The counts in that file's header come
# from here.
#
#   wsl.exe -e bash -lc "cd /mnt/c/work/roomwizard && bash tests/measure_ssh_sabotage.sh"
#
# ⚠️ Every sabotage ASSERTS THAT IT APPLIED before the suite is run. Without that, a
# pattern that no longer matches reports "0 failed" — which reads exactly like a suite
# that cannot detect the breakage. tests/measure_bundle_ssh_sabotage.sh records four
# sabotages that all reported 22/0 for that reason.
#
# The first case is not a sabotage but the PRE-FIX TREE: the eight call sites as they
# were before F16, restored from git. That is the "write the failing version first"
# measurement — a wiring check that has only ever been seen passing is not evidence it
# can fail.

set -u
cd "$(dirname "$0")/.." || exit 1

LIB=lib/arca-ssh.sh
BAK=/tmp/arca-ssh.orig.$$
cp "$LIB" "$BAK"
HARNESS_ERR=0

# The seven files whose gate F16 rewired. Restoring them all from HEAD~ is how the
# pre-fix measurement is taken without unstaging anything.
SITES="commissioning/provision.sh deploy-all.sh arca.sh
       native_apps/build-and-deploy.sh vnc_client/build-and-deploy.sh
       usb_host/build-and-deploy.sh scummvm-arca/build-and-deploy.sh"

SITEBAK=/tmp/arca-ssh-sites.$$
mkdir -p "$SITEBAK"
for f in $SITES; do
    mkdir -p "$SITEBAK/$(dirname "$f")"
    cp "$f" "$SITEBAK/$f"
done

restore() {
    cp "$BAK" "$LIB"
    for f in $SITES; do cp "$SITEBAK/$f" "$f"; done
}
trap 'restore; rm -rf "$BAK" "$SITEBAK"' EXIT INT TERM

run() {
    bash ./tests/arca_ssh_test.sh 2>&1 | sed 's/\x1b\[[0-9;]*m//g' \
        | grep -oE '[0-9]+ passed, [0-9]+ failed' | tail -1
}

echo ""
printf '  %-44s %s\n' "baseline (nothing broken)" "$(run)"

# ── the pre-fix tree ───────────────────────────────────────────────────────
#
# Not a sed patch: the actual eight gates as they shipped before F16. `git show` is
# read from Git Bash's index but runs fine here because it filters nothing
# (CLAUDE.md → Working from this host: git log/show succeed under WSL, git status and
# git diff do not).
PREFIX_OK=1
for f in $SITES; do
    git show "622c4dc^:$f" > "$f" 2>/dev/null || PREFIX_OK=0
done
if [ "$PREFIX_OK" -eq 1 ]; then
    printf '  %-44s %s\n' "the pre-fix call sites, from 622c4dc^" "$(run)"
else
    printf '  %-44s %s\n' "the pre-fix call sites" "COULD NOT RESTORE — count would be a lie"; HARNESS_ERR=1
fi
restore

# sab <label> <sed-expression>
sab() {
    local label="$1" expr="$2" before after
    cp "$BAK" "$LIB"
    before=$(md5sum "$LIB" | cut -d' ' -f1)
    sed -i "$expr" "$LIB"
    after=$(md5sum "$LIB" | cut -d' ' -f1)
    if [ "$before" = "$after" ]; then
        printf '  %-44s DID NOT APPLY — the count below would be a lie\n' "$label"
        HARNESS_ERR=1; return
    fi
    if ! bash -n "$LIB" 2>/dev/null; then
        printf '  %-44s broke the syntax — not a usable sabotage\n' "$label"
        HARNESS_ERR=1; return
    fi
    printf '  %-44s %s\n' "$label" "$(run)"
}

# ⚠️ THE sabotage this file exists for: the classifier keyed on the parenthetical
# method list, which is the signature this repo had written down. It passes against a
# vendor-root unit (PasswordAuthentication yes → "publickey,password") and calls every other
# server "down", so the offer is SUPPRESSED and nothing looks broken.
sab "classify: auth keyed on (publickey,password)" \
    's|\*"Permission denied"\*|*"Permission denied (publickey,password)"*|'

# The other direction: an unknown error treated as an auth failure, so the operator of
# an unplugged device is offered a key they do not need.
sab "classify: unknown text falls through to auth" \
    's|^    echo down$|    echo auth|'

# The TTY guard removed. This is the one that hangs deploy-all.sh, and the reason
# every case in group C runs under `timeout`.
sab "the non-TTY guard removed (can_prompt true)" \
    's|^arca_ssh_can_prompt() { \[ -t 0 \]; }$|arca_ssh_can_prompt() { true; }|'

# The re-probe after ssh-copy-id dropped, i.e. trusting its exit status. Only
# reachable because the sshd in group D is real.
sab "the re-probe after ssh-copy-id dropped" \
    's|^    arca_ssh_probe "\$target" "\$@" >/dev/null \&\& {$|    true \&\& {|'

# A passphrase on the generated key: it would then need an agent in the dependency
# list of every build script.
sab "keygen writes a passphrase" \
    "s|-N '' -C |-N 'x' -C |"

# The chown decision inverted at the euid guard: a key handed to $SUDO_USER when we are NOT root is a
# chown that cannot work, and the guard being right in both directions is what F1/F5 assert.
#
# ⚠️ `#` as the sed delimiter, not `|`. The line contains `||`, which closes a `|`-delimited s///
# early — the first version of this sabotage reported DID NOT APPLY, which is the whole reason this
# script asserts application before printing a count.
sab "key_owner chowns when not root" \
    's#^    \[ "\$euid" = "0" \] || return 0$#    [ "$euid" != "0" ] || return 0#'

# The subshell defect the first version of this library had: the gate reads
# ARCA_SSH_LAST_STDERR set inside a `$(...)`, so ssh's own complaint is replaced by a
# blank line. Every wording assertion still passes; only C3a/C5a see it.
sab "the gate reads the stderr from a subshell" \
    's|^    arca_ssh_probe "\$target" "\$@" >/dev/null \&\& return 0$|    [ "$(arca_ssh_probe "$target" "$@")" = ok ] \&\& return 0|'

restore
echo ""
printf '  %-44s %s\n' "restored" "$(run)"
echo ""
# A stanza that edited nothing is a harness error, not a verdict: fail the run.
[ "$HARNESS_ERR" -eq 0 ] || { echo "HARNESS ERROR: a sabotage matched nothing"; exit 1; }
