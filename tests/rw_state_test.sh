#!/bin/bash
#
# rw_state_test.sh — regression for lib/rw-state.sh
#
# Host-only, no device, no root. Run it:
#
#   wsl.exe -e bash -lc "cd /mnt/c/work/roomwizard && ./tests/rw_state_test.sh"
#
# WHAT IT COVERS:
#
#   A  Paths that would escape the extraction root (absolute, '..') are flagged by
#      rw_state_unsafe_paths; ordinary ones are not.
#   B  The generated lister, run under dash against a synthetic tree, lists what
#      exists and nothing that does not.
#   C  rw_state_backup_foreign: what restore.sh may write is the BACKUP set only.
#   D  rw_state_ssh_keys_deny: a card image may carry no SSH key material.
#
# The count at the end includes a floor: a file that silently ran zero cases
# reports success just as loudly as one that ran all of them.

# shellcheck source-path=SCRIPTDIR
set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

# shellcheck source=../lib/rw-state.sh
. "$REPO_DIR/lib/rw-state.sh"

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

# The real `tar -tf` of a state archive captured from .188 (2026-10), in archive order.
cat > "$TMP/real.lst" <<'EOF'
etc/touch_calibration.conf
etc/touch_calibration.conf.bak1
etc/touch_calibration.conf.bak2
etc/touch_calibration.conf.bak3
etc/touch_calibration.conf.bak4
etc/touch_calibration.conf.bak5
etc/touch_calibration.conf.bak6
etc/touch_calibration.conf.bak7
etc/touch_calibration.conf.bak8
etc/touch_calibration.conf.bak9
opt/games/rw_config.conf
opt/games/scummvm.ini
opt/vnc_client/vnc_client.conf
var/lib/alsa/asound.state
var/lib/bluetooth/
var/lib/bluetooth/A0:AD:9F:70:DD:CA/
var/lib/bluetooth/A0:AD:9F:70:DD:CA/cache/
var/lib/bluetooth/A0:AD:9F:70:DD:CA/cache/5C:D3:3D:83:A3:B6
var/lib/bluetooth/A0:AD:9F:70:DD:CA/cache/94:45:60:89:F6:48
var/lib/bluetooth/A0:AD:9F:70:DD:CA/cache/E4:17:D8:13:49:41
var/lib/bluetooth/A0:AD:9F:70:DD:CA/cache/08:C8:C2:3C:67:17
var/lib/bluetooth/A0:AD:9F:70:DD:CA/cache/E4:17:D8:42:BD:FF
var/lib/bluetooth/A0:AD:9F:70:DD:CA/cache/08:EB:ED:FE:C1:3D
var/lib/bluetooth/A0:AD:9F:70:DD:CA/cache/90:7A:58:B9:C6:F5
var/lib/bluetooth/A0:AD:9F:70:DD:CA/cache/AC:0B:FB:14:87:62
var/lib/bluetooth/A0:AD:9F:70:DD:CA/cache/E6:7A:00:00:20:9F
var/lib/bluetooth/A0:AD:9F:70:DD:CA/cache/E4:17:D8:40:EB:AE
var/lib/bluetooth/A0:AD:9F:70:DD:CA/cache/34:88:5D:55:62:C5
var/lib/bluetooth/A0:AD:9F:70:DD:CA/cache/00:0C:BF:40:02:9B
var/lib/bluetooth/A0:AD:9F:70:DD:CA/08:EB:ED:FE:C1:3D/
var/lib/bluetooth/A0:AD:9F:70:DD:CA/08:EB:ED:FE:C1:3D/info
var/lib/bluetooth/A0:AD:9F:70:DD:CA/08:EB:ED:FE:C1:3D/attributes
var/lib/bluetooth/A0:AD:9F:70:DD:CA/90:7A:58:B9:C6:F5/
var/lib/bluetooth/A0:AD:9F:70:DD:CA/90:7A:58:B9:C6:F5/info
var/lib/bluetooth/A0:AD:9F:70:DD:CA/90:7A:58:B9:C6:F5/attributes
var/lib/bluetooth/A0:AD:9F:70:DD:CA/E6:7A:00:00:20:9F/
var/lib/bluetooth/A0:AD:9F:70:DD:CA/E6:7A:00:00:20:9F/info
var/lib/bluetooth/A0:AD:9F:70:DD:CA/E6:7A:00:00:20:9F/attributes
var/lib/bluetooth/A0:AD:9F:70:DD:CA/E4:17:D8:40:EB:AE/
var/lib/bluetooth/A0:AD:9F:70:DD:CA/E4:17:D8:40:EB:AE/info
var/lib/bluetooth/A0:AD:9F:70:DD:CA/E4:17:D8:40:EB:AE/attributes
var/lib/bluetooth/A0:AD:9F:70:DD:CA/34:88:5D:55:62:C5/
var/lib/bluetooth/A0:AD:9F:70:DD:CA/34:88:5D:55:62:C5/info
var/lib/bluetooth/A0:AD:9F:70:DD:CA/34:88:5D:55:62:C5/attributes
var/lib/bluetooth/A0:AD:9F:70:DD:CA/settings
var/lib/bluealsa/
var/lib/bluealsa/08:C8:C2:3C:67:17
var/lib/bluealsa/08:EB:ED:FE:C1:3D
var/lib/bluealsa/90:7A:58:B9:C6:F5
etc/hostname
etc/hosts
etc/timezone
etc/localtime
home/root/.ssh/
home/root/.ssh/authorized_keys
opt/roomwizard/default-app
home/root/kq2.000
home/root/wget.err
home/root/wget.out
EOF

: > "$TMP/empty"

echo ""
echo "A  paths that escape the root"
printf '%s\n' /etc/passwd ../etc/x etc/../../x home/root/.. > "$TMP/unsafe.lst"
printf '%s\n' etc/hostname ./etc/timezone 'var/lib/bluetooth/..x' > "$TMP/safe.lst"
rw_state_unsafe_paths < "$TMP/unsafe.lst" > "$TMP/a1.out"; rc=$?
expect_out "$TMP/unsafe.lst" "$TMP/a1.out" "A1 rw_state_unsafe_paths flags all four"
expect_rc 1 "$rc" "A2 and exits 1"
rw_state_unsafe_paths < "$TMP/safe.lst" > "$TMP/a3.out"; rc=$?
expect_out "$TMP/empty" "$TMP/a3.out" "A3 ordinary paths, including a name starting '..', pass"
expect_rc 0 "$rc" "A4 and exit 0"

echo ""
echo "B  the generated lister against a synthetic tree"
R="$TMP/root"
mkdir -p "$R/etc" "$R/opt/games/scumm" "$R/opt/vnc_client" "$R/opt/roomwizard" "$R/var/lib/bluetooth/AA" \
    "$R/home/root/.ssh" "$R/home/root/data/ssh" "$R/home/root/subdir"
for f in etc/hostname etc/hosts etc/timezone etc/touch_calibration.conf etc/touch_calibration.conf.bak1 \
         etc/input_config.conf opt/games/rw_config.conf opt/games/scummvm.ini opt/games/scumm/x.la0 \
         opt/vnc_client/vnc_client.conf opt/roomwizard/default-app var/lib/bluetooth/AA/info \
         home/root/.ssh/authorized_keys home/root/data/snake.hig home/root/data/ssh/ssh_host_rsa_key \
         home/root/kq2.000 home/root/uImage-test home/root/subdir/x ft.s01; do
    : > "$R/$f"
done
ln -s /usr/share/zoneinfo/UTC "$R/etc/localtime"
ln -s /nowhere "$R/home/root/dangling"
SH=$(command -v dash || command -v sh)
rw_state_lister backup > "$TMP/lister.backup"
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
opt/games/rw_config.conf
opt/roomwizard/default-app
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
rw_state_lister image > /dev/null 2>&1; rc=$?
expect_rc 2 "$rc" "B2 the retired image set is refused, exit 2"

echo ""
echo "C  what restore.sh will write: the BACKUP set and nothing else"
tar -C "$R" -cf - -T "$TMP/listed.backup" | tar -tf - > "$TMP/c1.lst"
rw_state_backup_foreign < "$TMP/c1.lst" > "$TMP/c1.out"; rc=$?
expect_out "$TMP/empty" "$TMP/c1.out" "C1 a tar of the backup set is all members"
expect_rc 0 "$rc" "C2 and exits 0"
rw_state_backup_foreign < "$TMP/real.lst" > "$TMP/c3.out"
expect_out "$TMP/empty" "$TMP/c3.out" "C3 the real .188 listing is all backup members"
printf '%s\n' etc/shadow etc/ssh/sshd_config usr/bin/x home/root/subdir/x home/root/subdir/ \
    /etc/hostname ../x opt/games/snake > "$TMP/c4.lst"
rw_state_backup_foreign < "$TMP/c4.lst" > "$TMP/c4.out"; rc=$?
expect_out "$TMP/c4.lst" "$TMP/c4.out" "C4 system files, a nested /home/root file, unsafe paths and a binary are foreign"
expect_rc 1 "$rc" "C5 and exits 1"

echo ""
echo "D  rw_state_ssh_keys_deny: a card image carries no SSH key material"
printf '%s\n' ./ ./etc/ssh/sshd_config ./home/root/.ssh/ ./home/root/.ssh/authorized_keys ./usr/bin/ssh > "$TMP/g1.lst"
rw_state_ssh_keys_deny < "$TMP/g1.lst" > "$TMP/g1.out"; rc=$?
printf '%s\n' ./home/root/.ssh/authorized_keys > "$TMP/g1.want"
expect_out "$TMP/g1.want" "$TMP/g1.out" "D1 an authorized_keys under home/root/.ssh is refused, the .ssh directory entry and sshd_config are not"
expect_rc 1 "$rc" "D2 and exits 1"
printf '%s\n' home/root/.ssh/id_ed25519 home/root/.ssh/known_hosts home/pi/.ssh/authorized_keys2 \
    etc/skel/authorized_keys > "$TMP/g3.lst"
rw_state_ssh_keys_deny < "$TMP/g3.lst" > "$TMP/g3.out"
expect_out "$TMP/g3.lst" "$TMP/g3.out" "D3 any file inside a .ssh directory, in any home, and any authorized_keys* is refused"
printf '%s\n' home/root/.ssh home/root/.ssh/id_rsa home/root/.ssh/id_ecdsa home/root/.ssh/id_ed25519 \
    home/root/.ssh/id_dsa etc/ssh/ssh_host_rsa_key etc/ssh/ssh_host_ed25519_key > "$TMP/g3b.lst"
rw_state_ssh_keys_deny < "$TMP/g3b.lst" > "$TMP/g3b.out"; rc=$?
expect_out "$TMP/g3b.lst" "$TMP/g3b.out" "D3b SSH host private keys (ssh_host_*_key) and user keys (id_*) anywhere are refused"
expect_rc 1 "$rc" "D3c and exits 1"
printf '%s\n' ./ ./etc/ ./etc/ssh/ ./etc/ssh/sshd_config ./etc/ssh/ssh_host_rsa_key.pub ./home/root/ ./home/root/.ssh/ \
    ./home/root/.sshrc.d/x ./usr/share/doc/ssh-keys.txt ./etc/ssh/ssh_host_ed25519_key.pub > "$TMP/g4.lst"
rw_state_ssh_keys_deny < "$TMP/g4.lst" > "$TMP/g4.out"; rc=$?
expect_out "$TMP/empty" "$TMP/g4.out" "D4 control: a rootfs listing with sshd_config, .ssh directory, .pub files and look-alike names passes"
expect_rc 0 "$rc" "D5 and exits 0"
printf '%s\n' ./etc/ssh/sshd_config ./etc/ssh/ssh_host_ed25519_key.pub home/root/.ssh/ > "$TMP/g6.lst"
rw_state_ssh_keys_deny < "$TMP/g6.lst" > "$TMP/g6.out"; rc=$?
expect_out "$TMP/empty" "$TMP/g6.out" "D6 a legitimate sshd_config, public key, and empty .ssh directory pass"
expect_rc 0 "$rc" "D7 and exits 0"

echo ""
TOTAL=$((PASS + FAIL))
echo "  $PASS passed, $FAIL failed"
# groups A..D: 4 + 4 + 5 + 9 = 22 (group B has the zero-numbered B0 twice, group D has 3b/3c)
MIN_CASES=22
if [ "$TOTAL" -lt "$MIN_CASES" ]; then
    echo -e "  ${RED}HARNESS ERROR${NC}: only $TOTAL cases ran, expected at least $MIN_CASES."
    exit 2
fi
[ "$FAIL" -eq 0 ] || exit 1
echo -e "  ${GREEN}all good${NC}"
