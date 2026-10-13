#!/bin/bash
#
# arca_ssh_test.sh — regression for lib/arca-ssh.sh, the shared SSH gate
#
# Host-only, no device, no SD card, no root. Run it:
#
#   wsl.exe -e bash -lc "cd /mnt/c/work/roomwizard && ./tests/arca_ssh_test.sh"
#
# ── What each group is for ─────────────────────────────────────────────────
#
#   A  arca_ssh_classify, the one decision this file turns on: is the target DOWN or
#      is it UP AND REFUSING US? They were one error message across eight call
#      sites, which is why the remedy could never be offered. Pure function, so
#      every case is a recorded stderr string in, one word out.
#   B  arca_ssh_probe against real targets. Three unreachable ones are genuinely
#      unreachable (port 1 on loopback, TEST-NET-1, an .invalid name), and the auth
#      case is a REAL sshd — see below.
#   C  the non-TTY path. ⚠️ This is the group that matters most for regressions,
#      because a gate that blocks here hangs deploy-all.sh with a prompt nobody is
#      watching, and there is no CI to notice.
#   D  the TTY path end to end, under a real pty: no key at all → offer to generate
#      → generate → offer to install → install → RE-PROBE → usable. Only the
#      credential step is faked; the server is real, so "the key was copied and the
#      server still refuses it" is a state this can actually reach.
#   E  every call site is wired to the shared gate, and no raw probe or
#      drifted message survives. ⚠️ THE group that fails against the pre-fix tree —
#      see the count in the header below.
#   F  key generation and, separately, the ownership decision behind it.
#
# ── Why a real sshd, and why it can run here ───────────────────────────────
#
# A genuine `Permission denied` cannot be produced by a stub without writing the
# string the code under test is supposed to recognise — which tests the string
# against itself. So group B starts /usr/sbin/sshd on a high loopback port with its
# own host key and an EMPTY AuthorizedKeysFile. That needs no root: sshd logs
# `setgroups() failed: Operation not permitted` and authenticates the invoking user
# to itself anyway.
#
# ⚠️ It refutes the string this repo had written down. The recorded signature was
# `Permission denied (publickey,password)`; that local sshd says
# `Permission denied (publickey,keyboard-interactive)`, because the parenthetical is
# the SERVER's method list. A classifier keyed on it would pass against a device
# (the pre-hardening vendor sshd said (publickey,password)) and call every other server
# "down" — suppressing the offer rather than making a spurious one, so nothing would
# look broken. Case A6 is that negative control.
#
# ⚠️ ssh resolves default identity paths from the passwd entry, NOT from $HOME, so a
# key generated into a fixture $HOME is not offered automatically. Groups B and D
# therefore pass an explicit IdentityFile through the gate's extra-options
# passthrough. That is a fact about this fixture, not about the library: in
# production the operator's home IS their passwd home.
#
# ── Measured against the pre-fix tree and against sabotage ────────────────
#
# tests/measure_ssh_sabotage.sh, which asserts each patch APPLIED before reporting a
# count. Baseline 69; measured 2026-08-07:
#
#   the eight pre-fix call sites, restored from git            17 fail
#   classify: auth keyed on "(publickey,password)"             13 fail
#   the gate reads the stderr from a subshell                  12 fail
#   classify: unknown text falls through to auth               11 fail
#   keygen writes a passphrase                                  3 fail
#   key_owner chowns when not root                               2 fail
#   the re-probe after ssh-copy-id dropped                       2 fail
#   the non-TTY guard removed (arca_ssh_can_prompt always true)     1 fail
#
# ⚠️ Two things that measurement says about this file, both worth keeping visible.
#
# The `key_owner` sabotage FIRST REPORTED "DID NOT APPLY" — its sed used `|` as the
# delimiter and the target line contains `||`, which closes the s/// early. That is
# the exact failure the assert-before-counting rule exists for: with no assert it
# would have printed "0 failed" and read as a suite that cannot see the breakage.
#
# ⚠️ The non-TTY sabotage fails only ONE case, and that is a real limit rather than a
# tight result. With `arca_ssh_can_prompt` forced true, `read` at EOF still fails
# immediately, so the gate falls through the "declined" branch and C4 (returns
# nonzero, does not hang) still passes — only C5's "Not a terminal" wording fails. So
# group C proves the gate does not BLOCK on a closed stdin; it does NOT prove it
# cannot block on a stdin that is open but never written, which is what a CI runner or
# a `yes | ...`-less pipeline actually looks like. The `timeout` wrapper is what would
# catch that, and it has never fired.
#
# What is NOT proven here, and needs root, like tests/commission_offline_test.sh:
# that arca_ssh_keygen's chown actually hands the key to $SUDO_USER. Group F tests the
# DECISION (arca_ssh_key_owner, which takes the euid as an argument so both branches
# are reachable) and that the call site passes the real euid to it.

# Source-path directive: resolves the source= hints below against this script's directory.
# shellcheck source-path=SCRIPTDIR
set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

# shellcheck source=../lib/arca-ssh.sh
. "$REPO_DIR/lib/arca-ssh.sh"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'
PASS=0; FAIL=0; SKIP=0
ok()      { PASS=$((PASS + 1)); echo -e "  ${GREEN}pass${NC}  $1"; }
bad()     { FAIL=$((FAIL + 1)); echo -e "  ${RED}FAIL${NC}  $1"; }
skipped() { SKIP=$((SKIP + 1)); echo -e "  ${YELLOW}skip${NC}  $1 — $2"; }
assert_eq() { if [ "$1" = "$2" ]; then ok "$3"; else bad "$3 (want '$1', got '$2')"; fi; }

# ⚠️ Under $(mktemp -d), i.e. WSL's own filesystem. /mnt/c discards chmod, and sshd
# refuses a host key it considers world-readable — on DrvFs every file is 0777, so
# the whole of groups B and D would be unreachable there.
TMP=$(mktemp -d)
SSHD_PID_FILE="$TMP/sshd.pid"
cleanup() {
    [ -f "$SSHD_PID_FILE" ] && kill "$(cat "$SSHD_PID_FILE")" 2>/dev/null
    rm -rf "$TMP"
}
trap cleanup EXIT INT TERM

# 2 rather than the shipped 5: one case is a real connect timeout and the suite
# should not spend 5 s on it. Every caller uses the default.
ARCA_SSH_CONNECT_TIMEOUT=2

# ═══════════════════════════════════════════════════════════════════════════
echo ""
echo "A. arca_ssh_classify — down and refused are different answers"
# ═══════════════════════════════════════════════════════════════════════════

# Every string here was captured from OpenSSH_8.2p1 on this host, not written from
# memory. The three network ones came from `ssh -o BatchMode=yes` against port 1 on
# loopback, TEST-NET-1 and an .invalid name; the auth one from the sshd group B
# starts.
assert_eq down "$(arca_ssh_classify 'ssh: connect to host 127.0.0.1 port 1: Connection refused')" \
    "A1 Connection refused is down"
assert_eq down "$(arca_ssh_classify 'ssh: connect to host 192.0.2.1 port 22: Connection timed out')" \
    "A2 Connection timed out is down"
assert_eq down "$(arca_ssh_classify 'ssh: Could not resolve hostname rw.invalid: Name or service not known')" \
    "A3 an unresolvable name is down"
assert_eq down "$(arca_ssh_classify 'ssh: connect to host 10.0.0.1 port 22: No route to host')" \
    "A4 No route to host is down"

assert_eq auth "$(arca_ssh_classify 'root@192.168.50.73: Permission denied (publickey,password).')" \
    "A5 the DEVICE's denial is auth"

# ⚠️ The negative control for the string F16 wrote down. A classifier keyed on
# "(publickey,password)" passes A5 and fails this, and the failure direction is
# silent: the offer is suppressed and the operator sees the old "cannot reach".
assert_eq auth "$(arca_ssh_classify 'z@127.0.0.1: Permission denied (publickey,keyboard-interactive).')" \
    "A6 a denial with a DIFFERENT method list is still auth"
assert_eq auth "$(arca_ssh_classify 'Received disconnect from 10.0.0.5 port 22:2: Too many authentication failures')" \
    "A7 Too many authentication failures is auth"

# A host key change is neither, and ssh-copy-id would fail against it too.
assert_eq hostkey "$(arca_ssh_classify 'Host key verification failed.')" \
    "A8 Host key verification failed is hostkey"
assert_eq hostkey "$(arca_ssh_classify 'WARNING: REMOTE HOST IDENTIFICATION HAS CHANGED!')" \
    "A9 a changed host identification is hostkey"

# ⚠️ An unrecognised error must NOT be auth: inventing a key remedy for an unknown
# failure is worse than saying "cannot reach". This is the direction sabotage 3
# breaks.
assert_eq down "$(arca_ssh_classify 'ssh: something nobody has seen before')" \
    "A10 an unrecognised error is down, not auth"
assert_eq down "$(arca_ssh_classify '')" "A11 empty stderr is down"

# ═══════════════════════════════════════════════════════════════════════════
echo ""
echo "B. arca_ssh_probe — against real targets"
# ═══════════════════════════════════════════════════════════════════════════

assert_eq down "$(arca_ssh_probe root@127.0.0.1 -p 1 2>/dev/null)" \
    "B1 a closed port on loopback probes down"
assert_eq down "$(arca_ssh_probe root@arca-no-such-host-$$.invalid 2>/dev/null)" \
    "B2 an unresolvable name probes down"
assert_eq down "$(arca_ssh_probe root@192.0.2.1 2>/dev/null)" \
    "B3 an unroutable address probes down (real ConnectTimeout)"

if arca_ssh_probe root@127.0.0.1 -p 1 >/dev/null 2>&1; then
    bad "B4 arca_ssh_probe returns nonzero for an unreachable target"
else
    ok "B4 arca_ssh_probe returns nonzero for an unreachable target"
fi

# ── the real sshd ───────────────────────────────────────────────────────────
SSHD_BIN=""
for c in /usr/sbin/sshd /usr/local/sbin/sshd; do
    [ -x "$c" ] && { SSHD_BIN="$c"; break; }
done

SSHD_UP=0
SSHD_PORT=0
AUTH_KEYS="$TMP/authorized_keys"
FIXTURE_KEY="$TMP/fixture/.ssh/id_ed25519"

if [ -z "$SSHD_BIN" ]; then
    skipped "B5-B7 probe against a real sshd" "no sshd binary on this host"
elif ! command -v ssh-keygen >/dev/null 2>&1; then
    skipped "B5-B7 probe against a real sshd" "ssh-keygen absent"
else
    mkdir -p "$TMP/fixture/.ssh"
    chmod 700 "$TMP/fixture/.ssh"
    ssh-keygen -q -t ed25519 -N '' -f "$TMP/hostkey"
    ssh-keygen -q -t ed25519 -N '' -f "$FIXTURE_KEY"
    : > "$AUTH_KEYS"
    chmod 600 "$AUTH_KEYS"

    # A port nobody else is on. Tried rather than assumed: a busy port makes sshd
    # exit and every case below would probe `down` and look like a code defect.
    for p in 22345 22346 22347 22348 22349; do
        cat > "$TMP/sshd_config" <<EOF
Port $p
ListenAddress 127.0.0.1
HostKey $TMP/hostkey
PidFile $SSHD_PID_FILE
AuthorizedKeysFile $AUTH_KEYS
PasswordAuthentication no
PubkeyAuthentication yes
UsePAM no
StrictModes no
EOF
        if "$SSHD_BIN" -f "$TMP/sshd_config" -E "$TMP/sshd.log" 2>/dev/null; then
            # sshd daemonises; wait for the pid file rather than sleeping blind.
            for _ in 1 2 3 4 5 6 7 8 9 10; do
                [ -s "$SSHD_PID_FILE" ] && break
                sleep 0.2
            done
            [ -s "$SSHD_PID_FILE" ] && { SSHD_UP=1; SSHD_PORT=$p; break; }
        fi
    done
fi

# The extra options every case against this sshd needs. IdentitiesOnly + an explicit
# IdentityFile because ssh takes default key paths from passwd, not $HOME; -F
# /dev/null so this host's own ~/.ssh/config cannot change the outcome.
SSHD_TARGET="$(id -un)@127.0.0.1"
sshd_opts() {
    printf '%s\n' "-p" "$SSHD_PORT" \
        "-o" "StrictHostKeyChecking=no" \
        "-o" "UserKnownHostsFile=/dev/null" \
        "-o" "IdentitiesOnly=yes" \
        "-o" "IdentityFile=$FIXTURE_KEY" \
        "-F" "/dev/null"
}
mapfile -t SSHD_OPTS < <(sshd_opts)

# An agent key would be offered ahead of ours and can exhaust MaxAuthTries.
unset SSH_AUTH_SOCK 2>/dev/null || true

if [ "$SSHD_UP" -eq 1 ]; then
    # ⚠️ Called WITHOUT a subshell, because that is the contract arca_ssh_gate relies
    # on: the gate needs the state AND the stderr text, and `$(arca_ssh_probe ...)`
    # discards the globals. The first version of the library did exactly that, and
    # its gate printed a blank line where ssh's complaint belongs.
    arca_ssh_probe "$SSHD_TARGET" "${SSHD_OPTS[@]}" >/dev/null 2>&1
    assert_eq auth "$ARCA_SSH_LAST_STATE" \
        "B5 a real sshd with an empty authorized_keys probes auth"

    # Not "the string contains what I expect" — the string is the INPUT to classify.
    # This asserts the real server produced a denial classify recognises, which is
    # the wiring A5/A6 cannot check.
    case "$ARCA_SSH_LAST_STDERR" in
        *"Permission denied"*) ok "B6 the real denial is what classify keys on" ;;
        *) bad "B6 the real denial is what classify keys on (got: $ARCA_SSH_LAST_STDERR)" ;;
    esac

    cat "$FIXTURE_KEY.pub" > "$AUTH_KEYS"
    arca_ssh_probe "$SSHD_TARGET" "${SSHD_OPTS[@]}" >/dev/null 2>&1
    assert_eq ok "$ARCA_SSH_LAST_STATE" \
        "B7 the same server with the key installed probes ok"
    assert_eq "" "$ARCA_SSH_LAST_STDERR" "B8 a successful probe leaves no error text behind"

    # The printed word and the global must agree — two reports of one answer is a
    # place they can disagree.
    : > "$AUTH_KEYS"
    assert_eq "$(arca_ssh_probe "$SSHD_TARGET" "${SSHD_OPTS[@]}" 2>/dev/null)" auth \
        "B9 the state it PRINTS matches the state it records"
else
    [ -n "$SSHD_BIN" ] && skipped "B5-B9 probe against a real sshd" \
        "sshd would not start on any candidate port"
fi

# ═══════════════════════════════════════════════════════════════════════════
echo ""
echo "C. arca_ssh_gate — the non-TTY path must never block"
# ═══════════════════════════════════════════════════════════════════════════
#
# ⚠️ Each case runs with stdin from /dev/null AND under `timeout`. Without the
# timeout a gate that blocks would hang this suite instead of failing it, which on a
# host with no CI means nobody ever learns.

# gate_nontty <outfile> <extra opts...> — returns the gate's status
gate_nontty() {
    local out="$1"; shift
    local child="$TMP/child.sh"
    {
        echo '. "$1"; shift'
        echo 'arca_ssh_gate "$@"'
    } > "$child"
    timeout 25 env HOME="$TMP/nokey" bash "$child" "$REPO_DIR/lib/arca-ssh.sh" "$@" \
        > "$out" 2>&1 < /dev/null
}

mkdir -p "$TMP/nokey"

gate_nontty "$TMP/c_down.out" root@127.0.0.1 -p 1
C1=$?
if [ "$C1" -eq 124 ]; then
    bad "C1 an unreachable target does not block (it TIMED OUT — the gate blocks)"
elif [ "$C1" -ne 0 ]; then
    ok "C1 an unreachable target returns nonzero without blocking"
else
    bad "C1 an unreachable target returns nonzero without blocking (returned 0)"
fi
if grep -qF "Cannot reach" "$TMP/c_down.out"; then
    ok "C2 it says the target cannot be reached"
else
    bad "C2 it says the target cannot be reached"
fi
if grep -qF "ssh-copy-id" "$TMP/c_down.out"; then
    bad "C3 a DOWN target is not offered a key fix"
else
    ok "C3 a DOWN target is not offered a key fix"
fi
# ⚠️ ssh's OWN complaint must reach the operator, not a blank line. This is the case
# that catches the subshell defect: with `state="$(arca_ssh_probe ...)"` the gate's
# ARCA_SSH_LAST_STDERR is empty and every other assertion in this group still passes.
if grep -qF "Connection refused" "$TMP/c_down.out"; then
    ok "C3a the gate quotes what ssh actually said"
else
    bad "C3a the gate quotes what ssh actually said (blank where ssh's error belongs)"
fi

if [ "$SSHD_UP" -eq 1 ]; then
    # No key in the fixture home, so the gate must name BOTH commands and stop.
    gate_nontty "$TMP/c_auth.out" "$SSHD_TARGET" "${SSHD_OPTS[@]}"
    C4=$?
    if [ "$C4" -eq 124 ]; then
        bad "C4 auth failure does not block when stdin is not a TTY (it TIMED OUT)"
    elif [ "$C4" -ne 0 ]; then
        ok "C4 auth failure returns nonzero without blocking"
    else
        bad "C4 auth failure returns nonzero without blocking (returned 0)"
    fi
    if grep -qF "Not a terminal" "$TMP/c_auth.out"; then
        ok "C5 it says why it is not prompting"
    else
        bad "C5 it says why it is not prompting"
    fi
    # The same negative control as C3a, on the branch an operator is far more likely
    # to hit: the refusal ssh reported must be visible, not summarised away.
    if grep -qF "Permission denied" "$TMP/c_auth.out"; then
        ok "C5a the refusal ssh reported is shown verbatim"
    else
        bad "C5a the refusal ssh reported is shown verbatim (blank where it belongs)"
    fi
    if grep -qF "ssh-keygen -t ed25519" "$TMP/c_auth.out"; then
        ok "C6 with no key at all, it names ssh-keygen"
    else
        bad "C6 with no key at all, it names ssh-keygen"
    fi
    if grep -qF "ssh-copy-id" "$TMP/c_auth.out"; then
        ok "C7 with no key at all, it names ssh-copy-id"
    else
        bad "C7 with no key at all, it names ssh-copy-id"
    fi
    # ⚠️ The gate must not have GENERATED anything without being asked.
    if [ -e "$TMP/nokey/.ssh/id_ed25519" ]; then
        bad "C8 the non-TTY path generates no key behind the operator's back"
    else
        ok "C8 the non-TTY path generates no key behind the operator's back"
    fi
else
    skipped "C4-C8 the non-TTY auth path" "no sshd fixture"
fi

# ═══════════════════════════════════════════════════════════════════════════
echo ""
echo "D. arca_ssh_gate — the TTY path, end to end under a real pty"
# ═══════════════════════════════════════════════════════════════════════════

if [ "$SSHD_UP" -ne 1 ]; then
    skipped "D1-D6 the interactive path" "no sshd fixture"
elif ! command -v script >/dev/null 2>&1; then
    skipped "D1-D6 the interactive path" "util-linux 'script' absent, cannot fake a TTY"
else
    # The stub is ssh-copy-id ONLY. It is the one step that needs a credential this
    # host does not have; everything else — the probe, the classify, the keygen, the
    # re-probe — is the real thing against the real server.
    mkdir -p "$TMP/bin"
    cat > "$TMP/bin/ssh-copy-id" <<'STUB'
#!/bin/sh
# Records that it ran, and installs the -i key into the fixture's authorized_keys.
: > "$ARCA_TEST_COPYID_RAN"
key=""
while [ $# -gt 0 ]; do
    case "$1" in
        -i) key="$2"; shift 2 ;;
        *)  shift ;;
    esac
done
[ -n "$key" ] || { echo "stub ssh-copy-id: no -i key given" >&2; exit 1; }
cat "$key" >> "$ARCA_TEST_AUTH_KEYS" || exit 1
echo "stub ssh-copy-id: installed $key"
STUB
    chmod +x "$TMP/bin/ssh-copy-id"

    # tty_gate <answers> <outfile> <home> — run the gate under a pty
    #
    # `script -e` returns the child's status. The child sources the SHIPPED library;
    # nothing about the gate is restated here.
    tty_gate() {
        local answers="$1" out="$2" home="$3"
        cat > "$TMP/tty_child.sh" <<CHILD
#!/bin/bash
PATH="$TMP/bin:\$PATH"
export ARCA_TEST_COPYID_RAN="$TMP/copyid.ran"
export ARCA_TEST_AUTH_KEYS="$AUTH_KEYS"
export HOME="$home"
unset SUDO_USER
ARCA_SSH_CONNECT_TIMEOUT=2
. "$REPO_DIR/lib/arca-ssh.sh"
arca_ssh_gate "$SSHD_TARGET" ${SSHD_OPTS[*]} -o IdentityFile="$home/.ssh/id_ed25519"
echo "GATE_RC=\$?"
CHILD
        chmod +x "$TMP/tty_child.sh"
        printf '%s' "$answers" | timeout 40 script -q -e \
            -c "bash $TMP/tty_child.sh" /dev/null > "$out" 2>&1
        tr -d '\r' < "$out" > "$out.clean" && mv "$out.clean" "$out"
    }

    # ── D1-D4: yes to generate, yes to install → usable ────────────────────
    rm -f "$TMP/copyid.ran"
    : > "$AUTH_KEYS"
    D_HOME="$TMP/tty_home_yes"
    mkdir -p "$D_HOME"
    tty_gate 'y
y
' "$TMP/d_yes.out" "$D_HOME"

    if grep -qF "Generate one now" "$TMP/d_yes.out"; then
        ok "D1 with no key, the TTY path OFFERS to generate one"
    else
        bad "D1 with no key, the TTY path OFFERS to generate one"
    fi
    if [ -f "$D_HOME/.ssh/id_ed25519" ] && [ -f "$D_HOME/.ssh/id_ed25519.pub" ]; then
        ok "D2 answering yes generates the key in the operator's home"
    else
        bad "D2 answering yes generates the key in the operator's home"
    fi
    if [ -f "$TMP/copyid.ran" ]; then
        ok "D3 answering yes runs ssh-copy-id"
    else
        bad "D3 answering yes runs ssh-copy-id"
    fi
    # THE case: the gate returns 0 because it RE-PROBED the real server and the key
    # now works. A gate that assumed success would also print this, which is why D5
    # exists.
    if grep -qF "GATE_RC=0" "$TMP/d_yes.out"; then
        ok "D4 the gate returns 0 after the key is installed and verified"
    else
        bad "D4 the gate returns 0 after the key is installed and verified"
    fi
    if grep -qF "installed and verified" "$TMP/d_yes.out"; then
        ok "D5 it says the key was verified, not merely copied"
    else
        bad "D5 it says the key was verified, not merely copied"
    fi

    # ── D6-D8: yes to generate, NO to install → nothing happens ───────────
    rm -f "$TMP/copyid.ran"
    : > "$AUTH_KEYS"
    D_HOME2="$TMP/tty_home_no"
    mkdir -p "$D_HOME2"
    tty_gate 'y
n
' "$TMP/d_no.out" "$D_HOME2"

    if [ -f "$TMP/copyid.ran" ]; then
        bad "D6 answering no does NOT run ssh-copy-id"
    else
        ok "D6 answering no does NOT run ssh-copy-id"
    fi
    if grep -qF "GATE_RC=0" "$TMP/d_no.out"; then
        bad "D7 declining leaves the gate failing (it returned 0)"
    else
        ok "D7 declining leaves the gate failing"
    fi
    if grep -qF "Nothing was changed" "$TMP/d_no.out"; then
        ok "D8 declining says nothing was changed and names the command"
    else
        bad "D8 declining says nothing was changed and names the command"
    fi

    # ── D9: the key was copied and the server STILL refuses it ────────────
    #
    # Reachable only because the server is real: the stub is told to install into a
    # file the server does not read, so ssh-copy-id "succeeds" and the re-probe
    # fails. A gate that trusted ssh-copy-id's exit status returns 0 here.
    rm -f "$TMP/copyid.ran"
    : > "$AUTH_KEYS"
    D_HOME3="$TMP/tty_home_liar"
    mkdir -p "$D_HOME3"
    SAVED_KEYS="$AUTH_KEYS"
    AUTH_KEYS="$TMP/not_read_by_sshd"
    : > "$AUTH_KEYS"
    tty_gate 'y
y
' "$TMP/d_liar.out" "$D_HOME3"
    AUTH_KEYS="$SAVED_KEYS"
    if grep -qF "GATE_RC=0" "$TMP/d_liar.out"; then
        bad "D9 a copy that did not take is reported as a failure (returned 0)"
    else
        ok "D9 a copy that did not take is reported as a failure"
    fi
    if grep -qF "still refuses it" "$TMP/d_liar.out"; then
        ok "D10 it says the key was copied but is still refused"
    else
        bad "D10 it says the key was copied but is still refused"
    fi
fi

# ═══════════════════════════════════════════════════════════════════════════
echo ""
echo "E. every call site is wired to the shared gate"
# ═══════════════════════════════════════════════════════════════════════════
#
# ⚠️ THE group that must fail against the pre-fix tree. Eight raw probes, already
# drifted in wording three ways, are what F16 is about; a library nobody calls fixes
# nothing.

GATE_USERS="
commissioning/provision.sh
deploy-all.sh
arca.sh
native_apps/build-and-deploy.sh
vnc_client/build-and-deploy.sh
usb_host/build-and-deploy.sh
bluetooth/build-and-deploy.sh
scummvm-arca/build-and-deploy.sh
"

for f in $GATE_USERS; do
    p="$REPO_DIR/$f"
    if [ ! -f "$p" ]; then
        bad "E: $f is missing"
        continue
    fi
    if grep -q 'lib/arca-ssh.sh' "$p"; then
        ok "E $f sources lib/arca-ssh.sh"
    else
        bad "E $f sources lib/arca-ssh.sh"
    fi
    if grep -qE 'arca_ssh_(gate|probe)' "$p"; then
        ok "E $f calls the shared gate"
    else
        bad "E $f calls the shared gate"
    fi
done

# scummvm has TWO gates (deploy and set-default), and only one of them being wired
# is the shape a per-file grep cannot see.
SCUMM_HITS=$(grep -cE 'arca_ssh_(gate|probe)' "$REPO_DIR/scummvm-arca/build-and-deploy.sh" || true)
if [ "$SCUMM_HITS" -ge 2 ]; then
    ok "E scummvm-arca wires BOTH of its gates ($SCUMM_HITS calls)"
else
    bad "E scummvm-arca wires BOTH of its gates (found $SCUMM_HITS, want >= 2)"
fi

# ⚠️ Search patterns in a FILE, not in argv: a /proc scanner in this repo once
# counted its own grep argv, and this grep runs over a tree that contains this test.
cat > "$TMP/raw_probe.re" <<'RE'
BatchMode=yes
RE
RAW=$(grep -rlFf "$TMP/raw_probe.re" \
        --include='*.sh' "$REPO_DIR" 2>/dev/null \
      | sed "s|^$REPO_DIR/||" \
      | grep -v '^lib/arca-ssh\.sh$' \
      | grep -v '^tests/' || true)
if [ -z "$RAW" ]; then
    ok "E no raw BatchMode probe survives outside lib/arca-ssh.sh"
else
    bad "E no raw BatchMode probe survives outside lib/arca-ssh.sh"
    echo "$RAW" | sed 's/^/          still raw: /'
fi

# The drift itself: three wordings of one message. Their absence is the evidence that
# the message now has one home.
#
# ⚠️ COMMENT LINES ARE STRIPPED FIRST. The drift that matters is a string a script
# PRINTS; several of the rewritten call sites quote the old wording in a comment
# explaining what they replaced, and a check that counted those would fire on the fix
# itself — which is how a gate teaches people to delete the documentation.
cat > "$TMP/drift.re" <<'RE'
check IP and SSH key
check the IP and the SSH key
Check: network connectivity, SSH key auth
RE
DRIFT=""
while IFS= read -r f; do
    sed 's/^[[:space:]]*#.*$//' "$f" | grep -qFf "$TMP/drift.re" \
        && DRIFT="$DRIFT${DRIFT:+$'\n'}${f#"$REPO_DIR"/}"
done < <(find "$REPO_DIR" -name '*.sh' -not -path '*/tests/*' -not -name 'arca-ssh.sh')
if [ -z "$DRIFT" ]; then
    ok "E none of the three drifted messages survives"
else
    bad "E none of the three drifted messages survives"
    echo "$DRIFT" | sed 's/^/          still drifted: /'
fi

# ═══════════════════════════════════════════════════════════════════════════
echo ""
echo "F. key generation, and the ownership decision behind it"
# ═══════════════════════════════════════════════════════════════════════════

# arca_ssh_key_owner takes the euid as an ARGUMENT, which is what makes the root
# branch reachable from a test running as an ordinary user.
SAVED_SUDO_USER="${SUDO_USER:-}"

unset SUDO_USER
assert_eq "" "$(arca_ssh_key_owner 1000)" "F1 not root: nothing to chown"
assert_eq "" "$(arca_ssh_key_owner 0)"    "F2 root with no SUDO_USER: nothing to chown"
SUDO_USER=root
assert_eq "" "$(arca_ssh_key_owner 0)"    "F3 root via a root login: nothing to chown"

# ⚠️ The case the chown exists for. Root, invoked by a real operator: the key must be
# handed back or it lands root-owned inside their home and ssh needs sudo forever.
if command -v id >/dev/null 2>&1 && [ "$(id -un)" != "root" ]; then
    ME="$(id -un)"; MYGRP="$(id -gn "$ME")"
    SUDO_USER="$ME"
    assert_eq "$ME:$MYGRP" "$(arca_ssh_key_owner 0)" \
        "F4 root under sudo: chown back to \$SUDO_USER"
    # ⚠️ Not root, but SUDO_USER set — the files are already ours. Chowning here is
    # harmless but the guard being right in both directions is the point.
    assert_eq "" "$(arca_ssh_key_owner 1000)" \
        "F5 SUDO_USER set but not root: still nothing to chown"
else
    skipped "F4-F5 the chown decision under sudo" "running as root, or no id"
fi

if [ -n "$SAVED_SUDO_USER" ]; then SUDO_USER="$SAVED_SUDO_USER"; else unset SUDO_USER; fi

# The call site must pass the REAL euid. A function that is right and a call site
# that hardcodes 0 is the shape F4/F5 cannot see — the same hole
# a call site that ignores arca_ssh_operator_home (F13-F16 test the function alone).
if grep -qF 'arca_ssh_key_owner "$(id -u)"' "$REPO_DIR/lib/arca-ssh.sh"; then
    ok "F6 arca_ssh_keygen passes the real euid to arca_ssh_key_owner"
else
    bad "F6 arca_ssh_keygen passes the real euid to arca_ssh_key_owner"
fi

# ── generation itself ──────────────────────────────────────────────────────
if ! command -v ssh-keygen >/dev/null 2>&1; then
    skipped "F7-F11 key generation" "ssh-keygen absent"
else
    GEN_HOME="$TMP/genhome"
    mkdir -p "$GEN_HOME"
    GEN_OUT=$(HOME="$GEN_HOME" SUDO_USER="" arca_ssh_keygen 2>&1) && GEN_RC=0 || GEN_RC=1

    assert_eq 0 "$GEN_RC" "F7 arca_ssh_keygen succeeds in an empty home"
    assert_eq "$GEN_HOME/.ssh/id_ed25519.pub" "$GEN_OUT" \
        "F8 it prints the path of the public key it made"
    if grep -q '^ssh-ed25519 ' "$GEN_HOME/.ssh/id_ed25519.pub" 2>/dev/null; then
        ok "F9 the key is ed25519"
    else
        bad "F9 the key is ed25519"
    fi
    # ⚠️ No passphrase, tested by USING it: `ssh-keygen -y` reads the private key and
    # would prompt (and, with stdin closed, fail) if one were set. Grepping the file
    # for "ENCRYPTED" would pass against a key with an empty-string passphrase.
    if ssh-keygen -y -P '' -f "$GEN_HOME/.ssh/id_ed25519" >/dev/null 2>&1 < /dev/null; then
        ok "F10 the key has no passphrase, so no agent is needed to use it"
    else
        bad "F10 the key has no passphrase, so no agent is needed to use it"
    fi
    # ⚠️ Refuse rather than overwrite: a key here is an identity the operator may use
    # for things that have nothing to do with this repo.
    if HOME="$GEN_HOME" SUDO_USER="" arca_ssh_keygen >/dev/null 2>&1; then
        bad "F11 a second call refuses rather than overwriting the key"
    else
        ok "F11 a second call refuses rather than overwriting the key"
    fi
fi

# ── arca_ssh_operator_home: whose ~/.ssh the key is looked for in ────────────
# Under sudo $HOME is /root, so the lookup must follow $SUDO_USER. Run in THIS
# shell with HOME/SUDO_USER restored after, so failures are counted.
G_REAL_HOME="$HOME"
G_REAL_SUDO="${SUDO_USER:-}"

unset SUDO_USER
HOME=/home/nobody-in-particular
assert_eq "/home/nobody-in-particular" "$(arca_ssh_operator_home)" \
    "F13 operator_home: no SUDO_USER uses \$HOME"
SUDO_USER=root
assert_eq "/home/nobody-in-particular" "$(arca_ssh_operator_home)" \
    "F14 operator_home: SUDO_USER=root falls back to \$HOME"
SUDO_USER="arca-no-such-user-$$"
assert_eq "/home/nobody-in-particular" "$(arca_ssh_operator_home)" \
    "F15 operator_home: unresolvable SUDO_USER falls back to \$HOME"
# The case the bug was: a real invoking user while $HOME points at root's.
G_ME="$(id -un 2>/dev/null)"
G_MY_HOME="$(getent passwd "$G_ME" 2>/dev/null | cut -d: -f6)"
if [ -n "$G_MY_HOME" ] && [ -d "$G_MY_HOME" ] && [ "$G_ME" != "root" ]; then
    SUDO_USER="$G_ME"; HOME=/root
    assert_eq "$G_MY_HOME" "$(arca_ssh_operator_home)" \
        "F16 operator_home: under sudo, SUDO_USER's home wins over /root"
else
    skipped "F16 operator_home under sudo" "no non-root user with a resolvable home, or getent absent"
fi
HOME="$G_REAL_HOME"
if [ -n "$G_REAL_SUDO" ]; then SUDO_USER="$G_REAL_SUDO"; else unset SUDO_USER; fi

# ── the library parses, and under the right shell ─────────────────────────
if bash -n "$REPO_DIR/lib/arca-ssh.sh" 2>/dev/null; then
    ok "F12 lib/arca-ssh.sh parses under bash"
else
    bad "F12 lib/arca-ssh.sh parses under bash"
fi

echo ""
TOTAL=$((PASS + FAIL))
echo "  $PASS passed, $FAIL failed, $SKIP skipped"

# A harness that runs nothing reports success. Non-skippable: A (11) + B1-B4 (4)
# + C1-C3a (4) + E (7 files x 2 + 3) + F1-F3 (3) + F6 (1) + F12 (1) + F13-F15 (3) = 48.
# Skippable and deliberately uncounted: B5-B9 and C4-C8 and D1-D10 (need sshd,
# and D needs `script`), F4-F5 and F16 (need a non-root user), F7-F11 (need ssh-keygen).
MIN_CASES=48
if [ "$TOTAL" -lt "$MIN_CASES" ]; then
    echo -e "  ${RED}HARNESS ERROR${NC}: only $TOTAL cases ran, expected at least $MIN_CASES."
    echo "  Cases were skipped that cannot be skipped, or the file was truncated."
    exit 2
fi

if [ "$FAIL" -gt 0 ]; then
    exit 1
fi
echo -e "  ${GREEN}all good${NC}"
