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
#   B  The generated lister, run under dash against a synthetic tree, lists what
#      exists and nothing that does not.
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
echo "B  the generated lister against a synthetic tree"
R="$TMP/root"
mkdir -p "$R/etc" "$R/opt/games/scumm" "$R/opt/vnc_client" "$R/opt/arca" "$R/var/lib/bluetooth/AA" \
    "$R/home/root/.ssh" "$R/home/root/data/ssh" "$R/home/root/subdir"
for f in etc/hostname etc/hosts etc/timezone etc/touch_calibration.conf etc/touch_calibration.conf.bak1 \
         etc/input_config.conf opt/games/arca_config.conf opt/games/scummvm.ini opt/games/scumm/x.la0 \
         opt/vnc_client/vnc_client.conf opt/arca/default-app var/lib/bluetooth/AA/info \
         home/root/.ssh/authorized_keys home/root/data/snake.hig home/root/data/ssh/ssh_host_rsa_key \
         home/root/kq2.000 home/root/uImage-test home/root/subdir/x ft.s01; do
    : > "$R/$f"
done
ln -s /usr/share/zoneinfo/UTC "$R/etc/localtime"
ln -s /nowhere "$R/home/root/dangling"
SH=$(command -v dash || command -v sh)
arca_state_lister backup > "$TMP/lister.backup"
if $SH -n "$TMP/lister.backup"; then ok "B0 backup lister parses under $SH"; else bad "B0 backup lister does not parse under $SH"; fi
# Re-root it: the shipped script's first line is `cd / || exit 1`.
sed "1s|^cd / |cd \"$R\" |" "$TMP/lister.backup" > "$TMP/lister.backup.rooted"
if ! cmp -s "$TMP/lister.backup" "$TMP/lister.backup.rooted"; then
    ok "B0 backup lister re-rooted (its first line is the cd)"
else
    bad "B0 backup lister: first line is not 'cd / ...', cannot re-root"
fi
$SH "$TMP/lister.backup.rooted" > "$TMP/listed.backup"
cat > "$TMP/want.backup" <<'EOF'
etc/hostname
etc/timezone
etc/localtime
etc/touch_calibration.conf
etc/input_config.conf
opt/games/arca_config.conf
opt/arca/default-app
var/lib/bluetooth
home/root/.ssh
etc/hosts
etc/touch_calibration.conf.bak1
opt/vnc_client/vnc_client.conf
opt/games/scummvm.ini
home/root/data/snake.hig
home/root/data/ssh
home/root/kq2.000
ft.s01
EOF
expect_out "$TMP/want.backup" "$TMP/listed.backup" "B1 backup lister: what exists, a symlink as itself, the private files, p2, and loose saves (not uImage-*, subdirs, dangling links)"
arca_state_lister image > /dev/null 2>&1; rc=$?
expect_rc 2 "$rc" "B2 the retired image set is refused, exit 2"

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
# groups A, B, D: 4 + 4 + 9 = 17 (group B has the zero-numbered B0 twice, group D has 3b/3c)
MIN_CASES=17
if [ "$TOTAL" -lt "$MIN_CASES" ]; then
    echo -e "  ${RED}HARNESS ERROR${NC}: only $TOTAL cases ran, expected at least $MIN_CASES."
    exit 2
fi
[ "$FAIL" -eq 0 ] || exit 1
echo -e "  ${GREEN}all good${NC}"
