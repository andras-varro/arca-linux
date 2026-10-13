#!/bin/bash
#
# arca-ssh.sh — the one SSH reachability gate, and the key bootstrap behind it
#
# Sourced, never executed.
#
# ── What this replaces ─────────────────────────────────────────────────────
#
# Eight copies of
#
#     ssh -o ConnectTimeout=5 -o BatchMode=yes "$DEVICE" true 2>/dev/null \
#         || err "Cannot reach $DEVICE — check IP and SSH key"
#
# which had already drifted in wording ("check IP and SSH key" / "check the IP and
# the SSH key" / "Check: network connectivity, SSH key auth, device is powered on")
# and which share one defect: `BatchMode=yes` disables password authentication, so
# an operator with no key installed is told to check a key nothing ever offered to
# create or install. The old vendor device accepted a password (it forced
# `PasswordAuthentication yes`) — and the host had no way to use it.
#
# ⚠️ BatchMode STAYS. Only the probes set it; the ssh/scp calls behind them do not,
# so dropping it "works" and then prompts for the password once per call, and
# native_apps/build-and-deploy.sh makes dozens. The goal is not a password-driven
# deploy. The goal is to get a key installed, once, and then never ask again.
#
# ── Down and refused are different answers ────────────────────────────────
#
# They were one error message, which is why the remedy could not be offered: you
# cannot suggest ssh-copy-id to someone whose device is unplugged. Every string
# below was MEASURED against OpenSSH_8.2p1 on this host, not read out of a manual:
#
#   ssh: connect to host 127.0.0.1 port 1: Connection refused
#   ssh: connect to host 192.0.2.1 port 22: Connection timed out
#   ssh: Could not resolve hostname arca-no-such-host.invalid: Name or service not known
#   <user>@127.0.0.1: Permission denied (publickey,keyboard-interactive).
#
# The last came from a real sshd started on a high port with an empty
# AuthorizedKeysFile, which is the only way to produce a genuine auth denial from
# this host with no device present (tests/arca_ssh_test.sh does the same).
#
# ⚠️ Note what that measurement refutes: the parenthetical method list is
# SERVER-dependent. A vendor-root unit said `(publickey,password)` because it
# set PasswordAuthentication yes; that local sshd said
# `(publickey,keyboard-interactive)`. So the classifier matches `Permission denied`
# and never the parenthetical — keying on `(publickey,password)` would pass against
# a device and misclassify every other server as "down", which is the direction that
# suppresses the offer instead of making a spurious one.
#
# An unrecognised error classifies as `down`, deliberately: `down` offers nothing,
# and inventing a key remedy for an unknown failure is the more expensive mistake.

# The gate's own timeout. Overridable so a test suite need not wait 5 s per case;
# every caller uses the default.
ARCA_SSH_CONNECT_TIMEOUT="${ARCA_SSH_CONNECT_TIMEOUT:-5}"

# ── The last probe's result, as globals ────────────────────────────────────
#
# ⚠️ arca_ssh_probe reports its answer TWICE, on purpose. It prints the state on
# stdout, which is what a caller wanting one word in a `$(...)` should use, and it
# also sets these two.
#
# The globals exist because the gate needs BOTH the state and the stderr text, and
# `state="$(arca_ssh_probe ...)"` runs the probe in a SUBSHELL — so the assignment to
# ARCA_SSH_LAST_STDERR is discarded and the gate's diagnosis prints an empty line where
# ssh's actual complaint should be. That was the first version of this file, and it
# passed every message-shape assertion in tests/arca_ssh_test.sh because they grepped
# for the surrounding wording rather than for what ssh said. So: the gate calls the
# probe with stdout redirected and reads these, and that suite's cases B6/C3a/C5a
# assert the real text reaches the operator.
ARCA_SSH_LAST_STDERR=""
ARCA_SSH_LAST_STATE=""

# Everything this file prints is operator-facing narration on an error path, so it
# goes to stderr — stdout belongs to arca_ssh_probe's one-word state.
arca_ssh_say() { echo "$*" >&2; }

# ── arca_ssh_classify <ssh-stderr-text> → ok | auth | hostkey | down ──────────
#
# Pure: no network, no side effects. That is what makes the one decision this file
# turns on testable without a device.
arca_ssh_classify() {
    local text="$1"

    # Host-key trouble first. It is neither "down" nor fixable with a key, and its
    # remedy is a different command — offering ssh-copy-id here would fail too.
    case "$text" in
        *"Host key verification failed"*|*"REMOTE HOST IDENTIFICATION HAS CHANGED"*)
            echo hostkey; return 0 ;;
    esac

    # Up and answering, and refusing us. The case where a key can be installed.
    case "$text" in
        *"Permission denied"*|*"Too many authentication failures"*|\
        *"No supported authentication methods available"*)
            echo auth; return 0 ;;
    esac

    echo down
}

# ── arca_ssh_probe <target> [extra ssh options...] ────────────────────────────
#
# Prints the state, returns 0 only when the target is reachable AND authenticated.
# Also sets ARCA_SSH_LAST_STATE and ARCA_SSH_LAST_STDERR — see the note above them.
# Safe under `set -e` in the caller: the assignment's status is consumed here.
arca_ssh_probe() {
    local target="$1"; shift
    local rc=0

    ARCA_SSH_LAST_STDERR=$(ssh -o ConnectTimeout="$ARCA_SSH_CONNECT_TIMEOUT" \
        -o BatchMode=yes "$@" "$target" true 2>&1 >/dev/null) || rc=$?

    if [ "$rc" -eq 0 ]; then
        # Cleared on success, so ARCA_SSH_LAST_STDERR means "the error behind the
        # current state" and nothing else. ssh writes to stderr on a SUCCESSFUL
        # connection too — the `Permanently added ... to the list of known hosts`
        # warning is the common one — and a caller that printed that as a diagnosis
        # would be reporting a warning as a fault.
        ARCA_SSH_LAST_STDERR=""
        ARCA_SSH_LAST_STATE=ok
    else
        ARCA_SSH_LAST_STATE=$(arca_ssh_classify "$ARCA_SSH_LAST_STDERR")
    fi
    echo "$ARCA_SSH_LAST_STATE"
    [ "$ARCA_SSH_LAST_STATE" = ok ]
}

# ── The invoking operator's home, which is not always $HOME ─────────────────
#
# This script sudo's each individual write rather than requiring root, so run
# standalone it has the operator's own $HOME and the key is where they expect.
# But an offline flow that runs as root (the old card prep did) sees $HOME as
# /root under sudo — where no operator's
# SSH key lives. The key was therefore NEVER found in the offline flow: the prompt
# fell through to "enter a path" on a host where a perfectly good
# ~/.ssh/id_rsa.pub existed.
#
# $SUDO_USER is the only place the invoking identity survives, and getent is asked
# for the home directory rather than assuming /home/<user> — that is wrong for a
# root-owned account and on any host with a non-default home layout. If getent is
# absent or the name resolves to nothing, this falls back to $HOME, which is the
# pre-existing behaviour rather than a new failure.
#
# It lives here rather than in a caller because the key GENERATION below needs
# the same answer, and two copies of "whose home" is how the original bug got in.
arca_ssh_operator_home() {
    local h
    if [ -n "${SUDO_USER:-}" ] && [ "$SUDO_USER" != "root" ]; then
        h=$(getent passwd "$SUDO_USER" 2>/dev/null | cut -d: -f6)
        if [ -n "$h" ] && [ -d "$h" ]; then
            printf '%s\n' "$h"
            return 0
        fi
    fi
    printf '%s\n' "$HOME"
}

# ── arca_ssh_pubkey → path of the operator's public key, or nothing ───────────
#
# Both key types, because "never found" is the same operator-facing failure
# whichever algorithm they generated. ed25519 first: on a host that has both, it is
# the newer one and the one ssh offers first.
arca_ssh_pubkey() {
    local home k
    home="$(arca_ssh_operator_home)"
    for k in id_ed25519.pub id_rsa.pub; do
        if [ -f "$home/.ssh/$k" ]; then
            printf '%s\n' "$home/.ssh/$k"
            return 0
        fi
    done
    return 1
}

# ── arca_ssh_key_owner <euid> → "user:group" to chown to, or nothing ──────────
#
# ⚠️ THE trap in generating a key from a script that may be running under sudo. The
# key belongs in the OPERATOR's ~/.ssh (arca_ssh_operator_home), but ssh-keygen run as
# root writes it root-owned — inside their home, unreadable to them, and ssh would
# need sudo forever afterwards. So a generated key is chowned back.
#
# The euid is an ARGUMENT rather than read from $EUID inside, so both branches are
# reachable from a test running as an ordinary user. The two cases that must NOT
# chown: not root (the files are already ours), and SUDO_USER unset or root (there
# is no other operator to hand them to).
arca_ssh_key_owner() {
    local euid="$1" grp
    [ "$euid" = "0" ] || return 0
    [ -n "${SUDO_USER:-}" ] || return 0
    [ "$SUDO_USER" != "root" ] || return 0
    grp=$(id -gn "$SUDO_USER" 2>/dev/null) || return 0
    [ -n "$grp" ] || return 0
    printf '%s:%s\n' "$SUDO_USER" "$grp"
}

# ── arca_ssh_keygen → generate an ed25519 key, print its .pub path ────────────
#
# `-N ''` (no passphrase) because this key's job is unattended deploy over a LAN to
# a device with no secrets on it, and a passphrase would put an agent in the
# dependency list of every build script. No password is stored anywhere, and that is
# the same rule `release.sh` enforces when it refuses to publish a config: one shipped
# file already carries a plaintext password, and this key must not add a second.
arca_ssh_keygen() {
    local home ssh_dir key owner
    home="$(arca_ssh_operator_home)"
    ssh_dir="$home/.ssh"
    key="$ssh_dir/id_ed25519"

    command -v ssh-keygen >/dev/null 2>&1 || {
        arca_ssh_say "arca_ssh_keygen: ssh-keygen not found — install openssh-client"
        return 1
    }
    # Refuse rather than overwrite: a key here is an identity the operator may use
    # for things that have nothing to do with this repo.
    if [ -e "$key" ] || [ -e "$key.pub" ]; then
        arca_ssh_say "arca_ssh_keygen: $key already exists — refusing to overwrite it"
        return 1
    fi

    mkdir -p "$ssh_dir" || return 1
    chmod 700 "$ssh_dir" || return 1
    ssh-keygen -t ed25519 -N '' -C "arca-$(id -un)@$(hostname 2>/dev/null || echo host)" \
        -f "$key" >/dev/null 2>&1 || {
        arca_ssh_say "arca_ssh_keygen: ssh-keygen failed"
        return 1
    }

    owner="$(arca_ssh_key_owner "$(id -u)")"
    if [ -n "$owner" ]; then
        chown "$owner" "$ssh_dir" "$key" "$key.pub" 2>/dev/null \
            || arca_ssh_say "  warning: could not chown the new key to $owner"
    fi

    printf '%s\n' "$key.pub"
}

# ── arca_ssh_can_prompt — is there a human on the other end? ──────────────────
#
# TTY-gated because release.sh and deploy-all.sh drive component scripts as a
# batch; a blocking `read` in one of those would hang the whole run with a prompt
# nobody is watching. A non-TTY caller gets the diagnosis and the exact command
# instead, which is strictly more than the eight old gates gave anyone.
arca_ssh_can_prompt() { [ -t 0 ]; }

# arca_ssh_ask <prompt> — y/n, default no. Reads stdin; only ever called behind
# arca_ssh_can_prompt.
arca_ssh_ask() {
    local reply=""
    printf '  %s (y/n): ' "$1" >&2
    read -r reply || return 1
    case "$reply" in [Yy]*) return 0 ;; *) return 1 ;; esac
}

# ── arca_ssh_gate <target> [extra ssh options...] ─────────────────────────────
#
# The whole gate. Returns 0 when the target is usable — including after installing
# a key during the call — and nonzero otherwise, having said what is wrong and what
# to run. It does NOT exit: callers have their own error function and their own exit
# conventions, and a library that exits cannot be tested.
arca_ssh_gate() {
    local target="$1"; shift

    # ⚠️ NOT `state="$(arca_ssh_probe ...)"`. That runs the probe in a subshell and
    # throws away ARCA_SSH_LAST_STDERR, so every message below would show a blank line
    # where ssh's own complaint belongs.
    arca_ssh_probe "$target" "$@" >/dev/null && return 0

    case "$ARCA_SSH_LAST_STATE" in
        hostkey)
            arca_ssh_say ""
            arca_ssh_say "  $target answered, but its host key does not match the one on record."
            arca_ssh_say "  $ARCA_SSH_LAST_STDERR"
            arca_ssh_say ""
            arca_ssh_say "  If this device was re-commissioned, that is expected — drop the old key:"
            arca_ssh_say "      ssh-keygen -R ${target#*@}"
            arca_ssh_say ""
            return 1
            ;;
        auth)
            arca_ssh_offer_key "$target" "$@"
            return $?
            ;;
        *)
            arca_ssh_say ""
            arca_ssh_say "  Cannot reach $target — nothing is answering on port 22."
            arca_ssh_say "  $ARCA_SSH_LAST_STDERR"
            arca_ssh_say ""
            arca_ssh_say "  Check the IP, that the unit is powered on, and that it is on this network."
            arca_ssh_say "  A unit whose card says net.mode=manual sends no DHCP request and appears"
            arca_ssh_say "  in no router lease list."
            arca_ssh_say ""
            return 1
            ;;
    esac
}

# ── arca_ssh_offer_key <target> [extra ssh options...] ───────────────────────
#
# Reached only when the target is up and refused us, which is the one moment a
# password is legitimately worth asking for — and ssh-copy-id asks for it itself,
# once, and stores nothing.
arca_ssh_offer_key() {
    local target="$1"; shift
    local key=""

    arca_ssh_say ""
    arca_ssh_say "  $target is up and answering, but refused this host's credentials:"
    arca_ssh_say "      $ARCA_SSH_LAST_STDERR"
    arca_ssh_say ""

    key="$(arca_ssh_pubkey)" || key=""

    if [ -z "$key" ]; then
        arca_ssh_say "  No SSH public key found in $(arca_ssh_operator_home)/.ssh."
        if arca_ssh_can_prompt; then
            if arca_ssh_ask "Generate one now (ed25519, no passphrase)?"; then
                key="$(arca_ssh_keygen)" || return 1
                arca_ssh_say "  Generated $key"
            else
                arca_ssh_say ""
                arca_ssh_say "  Then this is the pair of commands:"
                arca_ssh_say "      ssh-keygen -t ed25519 -N ''"
                arca_ssh_say "      ssh-copy-id $target"
                arca_ssh_say ""
                return 1
            fi
        else
            arca_ssh_say ""
            arca_ssh_say "  Not a terminal, so nothing is being prompted for. Run:"
            arca_ssh_say "      ssh-keygen -t ed25519 -N ''"
            arca_ssh_say "      ssh-copy-id $target"
            arca_ssh_say ""
            return 1
        fi
    fi

    if ! command -v ssh-copy-id >/dev/null 2>&1; then
        arca_ssh_say "  ssh-copy-id is not installed (it ships with openssh-client). Install it, then:"
        arca_ssh_say "      ssh-copy-id -i $key $target"
        arca_ssh_say ""
        return 1
    fi

    if ! arca_ssh_can_prompt; then
        arca_ssh_say "  Not a terminal, so nothing is being prompted for. Install the key with:"
        arca_ssh_say "      ssh-copy-id -i $key $target"
        arca_ssh_say ""
        arca_ssh_say "  It asks for the device's root password once and stores nothing."
        arca_ssh_say ""
        return 1
    fi

    arca_ssh_say "  ssh-copy-id can install $key now."
    arca_ssh_say "  It will ask for the DEVICE's root password once, and store nothing."
    if ! arca_ssh_ask "Install it on $target?"; then
        arca_ssh_say ""
        arca_ssh_say "  Nothing was changed. When you want to, run:"
        arca_ssh_say "      ssh-copy-id -i $key $target"
        arca_ssh_say ""
        return 1
    fi

    arca_ssh_say ""
    # Not silenced: its password prompt and its own diagnostics are the point.
    # BatchMode is deliberately NOT passed here — this is the one call that must be
    # allowed to ask.
    ssh-copy-id -i "$key" "$@" "$target" >&2 || {
        arca_ssh_say ""
        arca_ssh_say "  ssh-copy-id failed. Nothing else has been changed."
        arca_ssh_say ""
        return 1
    }

    # Re-probe rather than assume. ssh-copy-id can succeed and the key still not
    # work — a wrong home directory, a mode sshd refuses, StrictModes.
    arca_ssh_say ""
    arca_ssh_probe "$target" "$@" >/dev/null && {
        arca_ssh_say "  Key installed and verified — $target is usable now."
        arca_ssh_say ""
        return 0
    }
    arca_ssh_say "  The key was copied, but $target still refuses it ($ARCA_SSH_LAST_STATE):"
    arca_ssh_say "      $ARCA_SSH_LAST_STDERR"
    arca_ssh_say ""
    return 1
}
