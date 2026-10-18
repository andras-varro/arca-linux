#!/bin/bash
#
# arca_state_test.sh — regression for lib/arca-state.sh
#
# Host-only, no device, no root. Run it:
#
#   wsl.exe -e bash -lc "cd /mnt/c/work/roomwizard && ./tests/arca_state_test.sh"
#
# WHAT IT COVERS:
#
#   A  Paths that would escape the extraction root (absolute, '..') are flagged by
#      arca_state_unsafe_paths; ordinary ones are not.
#   D  arca_state_ssh_keys_deny: a card image may carry no SSH key material.
#
# The count at the end includes a floor: a file that silently ran zero cases
# reports success just as loudly as one that ran all of them.

# shellcheck source-path=SCRIPTDIR
set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

# shellcheck source=../lib/arca-state.sh
. "$REPO_DIR/lib/arca-state.sh"

RED='\033[0;31m'; GREEN='\033[0;32m'; NC='\033[0m'
PASS=0
FAIL=0
ok()  { PASS=$((PASS + 1)); echo -e "  ${GREEN}pass${NC}  $1"; }
bad() { FAIL=$((FAIL + 1)); echo -e "  ${RED}FAIL${NC}  $1"; }

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT INT TERM

# expect_out <expected-file> <actual-file> <description>
expect_out() {
    if diff <(sort "$1") <(sort "$2") > "$TMP/diff" 2>&1; then
        ok "$3"
    else
        bad "$3"; sed 's/^/        /' "$TMP/diff"
    fi
}
# expect_rc <want> <got> <description>
expect_rc() {
    if [ "$1" = "$2" ]; then ok "$3"; else bad "$3 (exit $2, expected $1)"; fi
}

: > "$TMP/empty"

echo ""
echo "A  paths that escape the root"
printf '%s\n' /etc/passwd ../etc/x etc/../../x home/root/.. > "$TMP/unsafe.lst"
printf '%s\n' etc/hostname ./etc/timezone 'var/lib/bluetooth/..x' > "$TMP/safe.lst"
arca_state_unsafe_paths < "$TMP/unsafe.lst" > "$TMP/a1.out"; rc=$?
expect_out "$TMP/unsafe.lst" "$TMP/a1.out" "A1 arca_state_unsafe_paths flags all four"
expect_rc 1 "$rc" "A2 and exits 1"
arca_state_unsafe_paths < "$TMP/safe.lst" > "$TMP/a3.out"; rc=$?
expect_out "$TMP/empty" "$TMP/a3.out" "A3 ordinary paths, including a name starting '..', pass"
expect_rc 0 "$rc" "A4 and exit 0"

echo ""
echo "D  arca_state_ssh_keys_deny: a card image carries no SSH key material"
printf '%s\n' ./ ./etc/ssh/sshd_config ./home/root/.ssh/ ./home/root/.ssh/authorized_keys ./usr/bin/ssh > "$TMP/g1.lst"
arca_state_ssh_keys_deny < "$TMP/g1.lst" > "$TMP/g1.out"; rc=$?
printf '%s\n' ./home/root/.ssh/authorized_keys > "$TMP/g1.want"
expect_out "$TMP/g1.want" "$TMP/g1.out" "D1 an authorized_keys under home/root/.ssh is refused, the .ssh directory entry and sshd_config are not"
expect_rc 1 "$rc" "D2 and exits 1"
printf '%s\n' home/root/.ssh/id_ed25519 home/root/.ssh/known_hosts home/pi/.ssh/authorized_keys2 \
    etc/skel/authorized_keys > "$TMP/g3.lst"
arca_state_ssh_keys_deny < "$TMP/g3.lst" > "$TMP/g3.out"
expect_out "$TMP/g3.lst" "$TMP/g3.out" "D3 any file inside a .ssh directory, in any home, and any authorized_keys* is refused"
printf '%s\n' home/root/.ssh home/root/.ssh/id_rsa home/root/.ssh/id_ecdsa home/root/.ssh/id_ed25519 \
    home/root/.ssh/id_dsa etc/ssh/ssh_host_rsa_key etc/ssh/ssh_host_ed25519_key > "$TMP/g3b.lst"
arca_state_ssh_keys_deny < "$TMP/g3b.lst" > "$TMP/g3b.out"; rc=$?
expect_out "$TMP/g3b.lst" "$TMP/g3b.out" "D3b SSH host private keys (ssh_host_*_key) and user keys (id_*) anywhere are refused"
expect_rc 1 "$rc" "D3c and exits 1"
printf '%s\n' ./ ./etc/ ./etc/ssh/ ./etc/ssh/sshd_config ./etc/ssh/ssh_host_rsa_key.pub ./home/root/ ./home/root/.ssh/ \
    ./home/root/.sshrc.d/x ./usr/share/doc/ssh-keys.txt ./etc/ssh/ssh_host_ed25519_key.pub > "$TMP/g4.lst"
arca_state_ssh_keys_deny < "$TMP/g4.lst" > "$TMP/g4.out"; rc=$?
expect_out "$TMP/empty" "$TMP/g4.out" "D4 control: a rootfs listing with sshd_config, .ssh directory, .pub files and look-alike names passes"
expect_rc 0 "$rc" "D5 and exits 0"
printf '%s\n' ./etc/ssh/sshd_config ./etc/ssh/ssh_host_ed25519_key.pub home/root/.ssh/ > "$TMP/g6.lst"
arca_state_ssh_keys_deny < "$TMP/g6.lst" > "$TMP/g6.out"; rc=$?
expect_out "$TMP/empty" "$TMP/g6.out" "D6 a legitimate sshd_config, public key, and empty .ssh directory pass"
expect_rc 0 "$rc" "D7 and exits 0"

echo ""
TOTAL=$((PASS + FAIL))
echo "  $PASS passed, $FAIL failed"
# groups A, D: 4 + 9 = 13 (group D has 3b/3c)
MIN_CASES=13
if [ "$TOTAL" -lt "$MIN_CASES" ]; then
    echo -e "  ${RED}HARNESS ERROR${NC}: only $TOTAL cases ran, expected at least $MIN_CASES."
    exit 2
fi
[ "$FAIL" -eq 0 ] || exit 1
echo -e "  ${GREEN}all good${NC}"
