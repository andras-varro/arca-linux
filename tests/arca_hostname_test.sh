#!/bin/bash
#
# arca_hostname_test.sh — regression for the host-name tools of the generic image:
#
#   A  rootfs/board/arca/overlay/usr/sbin/set-hostname (installed on the unit as /usr/sbin/set-hostname):
#      a valid name lands in /etc/hostname and /etc/hosts; every invalid name exits
#      non-zero with ONE stderr line and leaves both files untouched.
#   B  rootfs/board/arca/overlay/etc/init.d/firstboot-hostname: the placeholder
#      or an absent name becomes arca-<4 hex> (valid by A's rules), one console line;
#      a name somebody set, and every later boot, print nothing and change nothing;
#      no random bytes keeps the name and breaks nothing.
#   C  init.d/sshd with the /etc/ssh/sshd_off marker present: one quiet line, the
#      daemon is never run, exit 0.
#
# Every group has a negative control: the same assertion run against a decoy that
# breaks the rule must FAIL, so a green run cannot be a vacuous one.
#
# Host-only, no device, no root. Run it:
#   wsl.exe -e bash -lc "cd /mnt/c/work/roomwizard && ./tests/arca_hostname_test.sh"

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
SETTER="$REPO_DIR/rootfs/board/arca/overlay/usr/sbin/set-hostname"
FIRST="$REPO_DIR/rootfs/board/arca/overlay/etc/init.d/firstboot-hostname"
SSHD="$REPO_DIR/rootfs/board/arca/overlay/etc/init.d/sshd"

RED='\033[0;31m'; GREEN='\033[0;32m'; NC='\033[0m'
PASS=0
FAIL=0
ok()  { PASS=$((PASS + 1)); echo -e "  ${GREEN}pass${NC}  $1"; }
bad() { FAIL=$((FAIL + 1)); echo -e "  ${RED}FAIL${NC}  $1"; }
# check <description> <command...>: ok when the command succeeds
check() { local d="$1"; shift; if "$@"; then ok "$d"; else bad "$d"; fi; }
# control <description> <command...>: ok when the command FAILS (the assertion can fail)
control() { local d="$1"; shift; if "$@"; then bad "$d (the assertion passed against a decoy)"; else ok "$d"; fi; }

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT INT TERM

# mkroot <dir> [hostname]: a rootfs fixture; no hostname argument means no /etc/hostname.
mkroot() {
    mkdir -p "$1/etc"
    printf '127.0.0.1 localhost\n127.0.1.1 arca\n' > "$1/etc/hosts"
    rm -f "$1/etc/hostname"
    if [ $# -ge 2 ]; then printf '%s\n' "$2" > "$1/etc/hostname"; fi
}

echo "A  set-hostname.sh"
mkroot "$TMP/a1" arca
sh "$SETTER" arca-0a1f "$TMP/a1" > /dev/null 2> "$TMP/a1.err"; rc=$?
check "A1 a valid name exits 0" [ "$rc" -eq 0 ]
check "A2 /etc/hostname holds it" [ "$(cat "$TMP/a1/etc/hostname")" = arca-0a1f ]
a3() {
    grep -q '^127.0.0.1 arca-0a1f$' "$TMP/a1/etc/hosts" && grep -q 'localhost' "$TMP/a1/etc/hosts" \
        && ! grep -qE '(^|[[:space:]])arca([[:space:]]|$)' "$TMP/a1/etc/hosts"
}
check "A3 /etc/hosts maps it to loopback, localhost kept, the old name gone" a3

# rejects <setter> <name>: 0 when the setter refuses the name with exactly one stderr line and
# leaves hostname and hosts as they were.
RN=0
rejects() {
    local s="$1" n="$2" d rc
    RN=$((RN + 1)); d="$TMP/r$RN"
    mkroot "$d" arca
    cp "$d/etc/hosts" "$d/hosts.before"
    sh "$s" "$n" "$d" > /dev/null 2> "$d/err"; rc=$?
    [ "$rc" -ne 0 ] || return 1
    [ "$(wc -l < "$d/err")" -eq 1 ] || return 1
    [ "$(cat "$d/etc/hostname")" = arca ] || return 1
    cmp -s "$d/hosts.before" "$d/etc/hosts"
}
i=0
long=$(printf 'a%.0s' $(seq 1 64))
newline_name=$(printf 'ok\nx')
for bad_name in "rw09.local" "-lead" "trail-" "has space" "under_score" "$long" "" "ünï" "$newline_name"; do
    i=$((i + 1))
    check "A4.$i invalid name (${#bad_name} chars) is refused: non-zero, one line, files untouched" rejects "$SETTER" "$bad_name"
done
printf '#!/bin/sh\nexit 0\n' > "$TMP/accept-all.sh"
control "A5 control: a setter that accepts everything fails the refusal assertion" rejects "$TMP/accept-all.sh" "rw09.local"

echo "B  firstboot-hostname"
URAND_OK="$TMP/urandom.ok"
head -c 4096 /dev/urandom > "$URAND_OK"
# fb <root> [urandom] [setter]: run start, stdout to $TMP/fb.out, stderr to $TMP/fb.err
fb() {
    ARCA_ROOT="$1" ARCA_URANDOM="${2:-$URAND_OK}" ARCA_SET_HOSTNAME="${3:-$SETTER}" \
        dash "$FIRST" start > "$TMP/fb.out" 2> "$TMP/fb.err"
}
named() { # <root>: hostname is arca-xxxx, hosts maps it, console said so in one line
    local n; n=$(cat "$1/etc/hostname" 2> /dev/null)
    printf '%s' "$n" | grep -qE '^arca-[0-9a-f]{4}$' || return 1
    grep -q "^127.0.0.1 $n\$" "$1/etc/hosts" || return 1
    [ "$(wc -l < "$TMP/fb.out")" -eq 1 ] && grep -qF "$n" "$TMP/fb.out"
}
silent() { [ ! -s "$TMP/fb.out" ] && [ ! -s "$TMP/fb.err" ]; }
mkroot "$TMP/b1" arca
fb "$TMP/b1"; rc=$?
check "B1 the placeholder name becomes arca-<4 hex>, mapped in hosts, one console line" named "$TMP/b1"
check "B2 exit 0, no stderr" bash -c "[ $rc -eq 0 ] && [ ! -s '$TMP/fb.err' ]"
first=$(cat "$TMP/b1/etc/hostname")
fb "$TMP/b1"
b3() { silent && [ "$(cat "$TMP/b1/etc/hostname")" = "$first" ]; }
check "B3 idempotent: a second boot prints nothing and keeps the name" b3
mkroot "$TMP/b2" mybox
fb "$TMP/b2"
b4() { silent && [ "$(cat "$TMP/b2/etc/hostname")" = mybox ]; }
check "B4 a name somebody set is left alone, silently" b4
mkroot "$TMP/b3"
fb "$TMP/b3"
check "B5 an absent /etc/hostname is named too" named "$TMP/b3"
: > "$TMP/urandom.empty"
mkroot "$TMP/b4" arca
fb "$TMP/b4" "$TMP/urandom.empty"; rc=$?
b6() {
    [ "$rc" -eq 0 ] && [ "$(cat "$TMP/b4/etc/hostname")" = arca ] && [ "$(wc -l < "$TMP/fb.err")" -eq 1 ]
}
check "B6 no random bytes: exit 0, name kept, one stderr line" b6
allgood=1
for _ in $(seq 1 25); do
    mkroot "$TMP/b5" arca
    fb "$TMP/b5" /dev/urandom || allgood=0
    named "$TMP/b5" || allgood=0
done
check "B7 25 draws from the real /dev/urandom all match arca-<4 hex> and are accepted by set-hostname" [ "$allgood" -eq 1 ]
printf '#!/bin/sh\nexit 0\n' > "$TMP/noop-setter.sh"
mkroot "$TMP/b6" arca
fb "$TMP/b6" "$URAND_OK" "$TMP/noop-setter.sh"
control "B8 control: with a setter that does nothing the named assertion fails" named "$TMP/b6"
mkroot "$TMP/b7" arca
fb "$TMP/b7" "$TMP/urandom.empty"
control "B9 control: with no randomness the named assertion fails" named "$TMP/b7"

echo "C  sshd_off marker"
# A copy whose daemon and marker paths are fixtures; the logic under test is untouched.
mkdir -p "$TMP/c/ssh"
printf '#!/bin/sh\ntouch "%s/ran"\nexit 0\n' "$TMP/c" > "$TMP/c/sshd-decoy"
chmod +x "$TMP/c/sshd-decoy"
sed -e "s|^DAEMON=.*|DAEMON=$TMP/c/sshd-decoy|" -e "s|/etc/ssh/sshd_off|$TMP/c/ssh/sshd_off|" "$SSHD" > "$TMP/c/sshd.sh"
check "C0 the copy really points at the fixtures" grep -q "$TMP/c/ssh/sshd_off" "$TMP/c/sshd.sh"
: > "$TMP/c/ssh/sshd_off"
# off_quiet <script>: marker present, start prints exactly the one line, exits 0, never ran the daemon
off_quiet() {
    rm -f "$TMP/c/ran"
    local out rc
    out=$(dash "$1" start 2>&1); rc=$?
    [ "$rc" -eq 0 ] && [ "$out" = "sshd: off (Control Panel)" ] && [ ! -e "$TMP/c/ran" ]
}
check "C1 marker present: one quiet line, exit 0, daemon never run" off_quiet "$TMP/c/sshd.sh"
printf '#!/bin/sh\nexit 0\n' > "$TMP/c/true.sh"
control "C2 control: a script that ignores the marker fails the assertion" off_quiet "$TMP/c/true.sh"

echo ""
TOTAL=$((PASS + FAIL))
echo "  $PASS passed, $FAIL failed"
MIN_CASES=24
if [ "$TOTAL" -lt "$MIN_CASES" ]; then
    echo -e "  ${RED}HARNESS ERROR${NC}: only $TOTAL cases ran, expected at least $MIN_CASES."
    exit 2
fi
[ "$FAIL" -eq 0 ] || exit 1
echo -e "  ${GREEN}all good${NC}"
