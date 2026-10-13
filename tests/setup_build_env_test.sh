#!/bin/bash
# Regression for ../setup-build-env.sh — the one home for host build prerequisites.
#
# Host-only: no device, no card, no root, and NOTHING IS EVER INSTALLED. The one
# destructive thing the script under test can do is `sudo apt-get install`, so this suite
# shims `sudo` onto PATH and asserts on the argv it recorded. That is deliberately a
# tripwire rather than an exit-status check: it makes the dangerous input safe to run for
# real, so the "print the exact command before running it" promise is measured against what
# the process was actually handed, not against what the script printed.
#
# ⚠️ What this suite CANNOT see, both of which under-report:
#   - File modes. /mnt/c is DrvFs 9p and reports every file executable, so a missing +x on
#     setup-build-env.sh can neither fire nor be demonstrated from this host.
#   - A real `apt-get install`. Whether the package NAMES resolve on a live archive is not
#     checked here; the suite checks that the right names reach the right command line.
#
# Run:  bash tests/setup_build_env_test.sh
set -u

REPO="$(cd "$(dirname "$0")/.." && pwd)"
# ARCA_SETUP lets a sabotage harness aim this same suite at a deliberately broken copy of the
# script. The assertions must never be restated over there — a harness that re-implements
# what it is checking can repair the sabotage it was written to catch.
SETUP="${ARCA_SETUP:-$REPO/setup-build-env.sh}"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'
passed=0; failed=0

[ -f "$SETUP" ] || { echo -e "${RED}REFUSING TO RUN: $SETUP does not exist${NC}" >&2; exit 1; }

# ⚠️ A suite that skips itself reports "0 failed" and is indistinguishable from one that
# cannot detect the breakage. This suite needs a Linux shell because the script under test
# refuses to run anywhere else — so an absent Linux is a REFUSAL, not a pass.
[ "$(uname -s)" = "Linux" ] || {
    echo -e "${RED}REFUSING TO RUN: uname says $(uname -s), not Linux.${NC}" >&2
    echo "  setup-build-env.sh refuses outside Linux, so every case here would assert" >&2
    echo "  only that refusal. Run this from WSL." >&2
    exit 1
}

WORK="$(mktemp -d /tmp/arca-setupenv.XXXXXX)"
trap 'rm -rf "$WORK"' EXIT

ok()  { passed=$((passed + 1)); echo -e "  ${GREEN}pass${NC}  $1"; }
bad() { failed=$((failed + 1)); echo -e "  ${RED}FAIL${NC}  $1"; }

# Every assertion is made against one captured run, so a case cannot pass by re-running
# the script with different arguments than it reported on. Environment assignments come
# first, then `--`, then the script's own arguments — without that separator `env` would
# try to parse `--group` as one of its own options.
#
# ⚠️ The shimmed PATH goes on EVERY run, not just the cases that assert on sudo. A sabotage
# that makes the script install unbidden would otherwise reach the real `sudo apt-get` from
# whichever case forgot the shim — measured while writing the sweep: forcing DO_INSTALL=1
# sent four unshimmed cases at a live apt. A caller-supplied PATH still wins, because env
# applies assignments left to right.
run() {
    local envs=()
    while [ "$#" -gt 0 ] && [ "$1" != "--" ]; do envs+=("$1"); shift; done
    [ "${1:-}" = "--" ] && shift
    OUT="$(env PATH="$WORK/bin:$PATH" "${envs[@]}" "$SETUP" "$@" < /dev/null 2>&1)"; RC=$?
}

expect_rc()       { if [ "$RC" = "$1" ]; then ok "$2 (exit $1)"; else bad "$2 — expected exit $1, got $RC"; fi; }
expect_match()    { if grep -qE "$1" <<<"$OUT"; then ok "$2"; else bad "$2 — no line matched /$1/"; fi; }
expect_no_match() { if grep -qE "$1" <<<"$OUT"; then bad "$2 — a line matched /$1/ and must not"; else ok "$2"; fi; }
dump_on_fail()    { [ "${VERBOSE:-0}" = 1 ] && printf '%s\n' "$OUT" | sed 's/^/        /'; return 0; }

# ── fixtures ────────────────────────────────────────────────────────────────
# Tables rather than the real package set, so the suite asserts on the LOOP and not on
# whatever happens to be installed in this WSL. A green run against the real table would
# only mean "this host is provisioned" — which is a silenced instrument, not a test.

# Everything present. `sh` and `/etc/hostname` exist on any Linux.
cat > "$WORK/all-present" <<'EOF'
core|cmd|sh|arca-fake-sh
core|file|/etc/hostname|arca-fake-hostname
EOF

# One absent subject per probe KIND. Each kind has its own way of being wrong, and a
# single-kind fixture would leave three of the four branches unexercised.
cat > "$WORK/all-absent" <<'EOF'
core|cmd|arca-absent-tool|arca-pkg-cmd
decode|run|arca-absent-tool --version|arca-pkg-run
decode|py|arca_absent_module|arca-pkg-py
kmod|file|/usr/include/arca-absent-header.h|arca-pkg-file
EOF

# A mix, to check the counts are not just "all" or "none".
cat > "$WORK/mixed" <<'EOF'
core|cmd|sh|arca-fake-sh
core|cmd|arca-absent-tool|arca-pkg-cmd
EOF

# The tripwire. `sudo` records its argv and installs nothing, so --install-deps is safe to
# run for real. It exits 0 so the script proceeds exactly as it would on success.
mkdir -p "$WORK/bin"
{
    echo '#!/bin/sh'
    echo "echo \"SUDO-ARGV: \$*\" >> $WORK/argv"
    echo 'exit 0'
} > "$WORK/bin/sudo"
chmod +x "$WORK/bin/sudo"

# A PATH with no apt-get at all, for the non-Debian refusal. Built by shimming apt-get to
# nothing rather than by emptying PATH, which would take the shell's own builtins with it.
mkdir -p "$WORK/noapt"
for t in sh grep sed awk cat printf uname env; do
    p="$(command -v "$t" 2>/dev/null)" && ln -sf "$p" "$WORK/noapt/$t"
done

echo ""
echo "1. the probe loop reports what is there and what is not"
run PACKAGES_FILE="$WORK/all-present" --
expect_rc 0 "1a everything present exits 0"
expect_match '2 of 2 present, 0 missing' "1b ...and says so with both counts"

run PACKAGES_FILE="$WORK/all-absent" --
expect_rc 1 "1c something missing exits 1"
expect_match '0 of 4 present, 4 missing' "1d ...with all four counted"
dump_on_fail

run PACKAGES_FILE="$WORK/mixed" --
expect_match '1 of 2 present, 1 missing' "1e a mixed table is not rounded to all-or-nothing"

echo ""
echo "2. each probe kind can actually detect an absence"
# Without this, three of the four branches could be dead and every case above still pass.
run PACKAGES_FILE="$WORK/all-absent" --
expect_match 'arca-absent-tool +core — arca-pkg-cmd' "2a cmd: a missing program"
expect_match 'arca-absent-tool --version +decode — arca-pkg-run' "2b run: a program that cannot execute"
expect_match 'arca_absent_module +decode — arca-pkg-py' "2c py: a missing python module"
expect_match 'arca-absent-header.h +kmod — arca-pkg-file' "2d file: a missing -dev header"

echo ""
echo "3. zero subjects probed is a FAILURE, not a pass"
# ⚠️ This case is why the guard exists. The first version of setup-build-env.sh named its
# array GROUPS, which bash owns and silently refuses to assign — so the selection was the
# invoking user's numeric group IDs, nothing matched the table, and the script reported
# "0 missing" and exited 0 on a host it had never looked at. Any future rename into another
# bash special name fails here rather than shipping a green gate over no measurement.
run PACKAGES_FILE="$WORK/all-present" -- --group nosuchgroup
expect_rc 2 "3a a group matching nothing refuses"
expect_match 'no prerequisites were probed at all' "3b ...and says the count was zero, not that all was well"
expect_no_match '0 missing' "3c ...and never reports success"

echo ""
echo "4. nothing is installed unless it was asked for"
rm -f "$WORK/argv"
run PACKAGES_FILE="$WORK/all-absent" PATH="$WORK/bin:$PATH" --
expect_rc 1 "4a non-TTY without --install-deps exits 1"
expect_match 'Not a TTY and --install-deps not given' "4b ...and says why it stopped"
if [ -f "$WORK/argv" ]; then
    bad "4c ...and sudo was NOT called — but the tripwire fired: $(cat "$WORK/argv")"
else
    ok "4c ...and sudo was never called"
fi

echo ""
echo "5. --install-deps hands apt exactly the command it printed"
rm -f "$WORK/argv"
OUT="$(env PACKAGES_FILE="$WORK/all-absent" PATH="$WORK/bin:$PATH" \
        "$SETUP" --install-deps < /dev/null 2>&1)"; RC=$?
expect_rc 0 "5a a successful install exits 0"
if [ ! -f "$WORK/argv" ]; then
    bad "5b the tripwire did NOT fire — sudo was never reached, so nothing here is measured"
else
    ARGV="$(cat "$WORK/argv")"
    if [ "$ARGV" = "SUDO-ARGV: apt-get install -y arca-pkg-cmd arca-pkg-run arca-pkg-py arca-pkg-file" ]; then
        ok "5b sudo received exactly the four packages, in table order"
    else
        bad "5b sudo argv wrong — got: $ARGV"
    fi
    # ⚠️ The promise is that the operator SEES what runs. Comparing the printed line to the
    # recorded argv is what makes that a measurement rather than a claim: a script could
    # print one package set and install another, and every other case here would pass.
    printed="$(grep -oE 'sudo apt-get install -y .*' <<<"$OUT" | head -1)"
    if [ "SUDO-ARGV: ${printed#sudo }" = "$ARGV" ]; then
        ok "5c ...and the printed command is the command that ran"
    else
        bad "5c printed and executed differ — printed '$printed', ran '$ARGV'"
    fi
fi

echo ""
echo "6. a non-Debian host is refused, not guessed at"
OUT="$(env PACKAGES_FILE="$WORK/all-absent" PATH="$WORK/noapt" \
        "$SETUP" --install-deps < /dev/null 2>&1)"; RC=$?
expect_rc 2 "6a no apt-get exits 2"
expect_match 'Not a Debian/Ubuntu host' "6b ...naming the reason"
expect_match 'arca-pkg-cmd' "6c ...and still listing what to install by hand"

echo ""
echo "7. argument handling"
run PACKAGES_FILE="$WORK/all-present" -- --bogus
expect_rc 2 "7a an unknown argument is refused"
run PACKAGES_FILE="$WORK/all-present" -- --group
expect_rc 2 "7b --group with no value is refused"
OUT="$("$SETUP" --help 2>&1)"; RC=$?
expect_rc 0 "7c --help exits 0"
# ⚠️ An unquoted heredoc would run backticks and $() in the help text: one half prints
# command-not-found, the other silently eats the word. Running --help is the only way to see it.
expect_match 'setup-build-env.sh' "7d ...and prints a usage line"
expect_no_match 'command not found' "7e ...with nothing executed out of the heredoc"

echo ""
echo "════════════════════════════════════════"
if [ "$failed" -eq 0 ]; then
    echo -e "  ${GREEN}$passed passed, 0 failed${NC}"
    exit 0
fi
echo -e "  ${RED}$passed passed, $failed failed${NC}"
echo -e "  ${YELLOW}re-run with VERBOSE=1 to see the script output${NC}"
exit 1
