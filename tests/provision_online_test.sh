#!/bin/bash
#
# provision_online_test.sh — regression for the ORDER of commissioning/provision.sh
#
# Host-only, no device, no root. Run it:
#
#   wsl.exe -e bash -lc "cd /mnt/c/work/roomwizard && ./tests/provision_online_test.sh"
#
# provision.sh is the update path for a unit running our own root. What it must do is
# a sequence, and a sequence is only visible from outside: this suite puts a fake
# `ssh` and `scp` first on PATH, runs the REAL script against them, and reads the
# one log every fake call appends to. The fake ssh answers just enough for
# commissioning/backup.sh to succeed (a hostname, a one-file state list, a small tar
# on stdout); everything else it records and accepts.
#
#   A  a full run: the backup fetch precedes the first write; no vendor-clean plan
#      is ever sent; disable-steelcase.sh is never run; the reboot is the last remote command
#      and happens once; an archive was produced.
#   B  --dry-run: prints the provision plan, makes no remote call but the SSH probe,
#      takes no backup.
#   C  negative control: the backup fails (the fake lister exits 1) -> provision
#      aborts non-zero before any write, and never reboots.
#   D  --status is read-only: no backup, no write, no reboot.
#   E  the clean and sshd-policy vocabulary is gone (sshd_config is the root overlay's alone): each removed option is refused, with no
#      remote call at all.
#   F  harness witness: the fakes were reached.
#
# ⚠️ The archive lands in the repo's backups/ (backup.sh's default, gitignored). This
# test snapshots that directory and removes only what it created.
# Host-only: a fake ssh proves ORDER, not that the device accepted anything.

set -u
REPO_DIR="$(cd "$(dirname "$0")/.." && pwd)"
PROVISION="$REPO_DIR/commissioning/provision.sh"

RED='\033[0;31m'; GREEN='\033[0;32m'; NC='\033[0m'
PASS=0; FAIL=0
ok()  { PASS=$((PASS + 1)); echo -e "  ${GREEN}pass${NC}  $1"; }
bad() { FAIL=$((FAIL + 1)); echo -e "  ${RED}FAIL${NC}  $1"; }
chk() { local rc=$?; if [ "$rc" -eq 0 ]; then ok "$1"; else bad "$1"; fi; }

TMP=$(mktemp -d)
BK="$REPO_DIR/backups"
BK_EXISTED=0; [ -d "$BK" ] && BK_EXISTED=1
for f in "$BK"/*; do [ -e "$f" ] && echo "${f##*/}"; done > "$TMP/backups.before"
# count of files in backups/ that were not there at the start
new_backups() {
    local f n=0
    for f in "$BK"/*; do
        [ -e "$f" ] || continue
        grep -qxF "${f##*/}" "$TMP/backups.before" || n=$((n + 1))
    done
    echo "$n"
}
cleanup() {
    local f
    if [ -d "$BK" ]; then
        for f in "$BK"/*; do
            [ -e "$f" ] || continue
            grep -qxF "${f##*/}" "$TMP/backups.before" || rm -f "$f"
        done
        [ "$BK_EXISTED" = 0 ] && rmdir "$BK" 2>/dev/null
    fi
    rm -rf "$TMP"
}
trap cleanup EXIT INT TERM

# ── the fakes ────────────────────────────────────────────────────────────────
mkdir -p "$TMP/bin" "$TMP/fix/etc"
echo rwtest > "$TMP/fix/etc/hostname"
cat > "$TMP/bin/ssh" <<'FAKE'
#!/bin/bash
# fake ssh: log "SSH<TAB><command, newlines collapsed>", answer what backup.sh asks.
while [ $# -gt 0 ]; do
    case "$1" in -o|-i|-p|-l) shift 2 ;; -*) shift ;; *) break ;; esac
done
shift                                   # the target
cmd="$*"
printf 'SSH\t%s\n' "$(printf '%s' "$cmd" | tr '\n' ' ')" >> "$FAKE_LOG"
case "$cmd" in
    "cat > "*)  cat > /dev/null ;;
    "head -n 1 /etc/hostname"*) echo rwtest ;;
    "cd / || exit 1"*) [ -n "${FAKE_LISTER_FAIL:-}" ] && exit 1; echo etc/hostname ;;
    "cd / && tar -cf -"*) tar -cf - -C "$FAKE_FIX" etc/hostname ;;
    *) [ -t 0 ] || cat > /dev/null ;;
esac
exit 0
FAKE
cat > "$TMP/bin/scp" <<'FAKE'
#!/bin/bash
printf 'SCP\t%s\n' "$*" >> "$FAKE_LOG"
exit 0
FAKE
chmod +x "$TMP/bin/ssh" "$TMP/bin/scp"

# run <name> <args...>  -> $TMP/<name>.log (remote calls), <name>.out, RC
run() {
    local name="$1"; shift
    : > "$TMP/$name.log"
    PATH="$TMP/bin:$PATH" FAKE_LOG="$TMP/$name.log" FAKE_FIX="$TMP/fix" \
        timeout 120 bash "$PROVISION" "$@" > "$TMP/$name.out" 2>&1 < /dev/null
    RC=$?
}
# line number of the first log line matching ERE $2 in run $1 (empty if none)
first() { grep -nE "$2" "$TMP/$1.log" | head -1 | cut -d: -f1; }
# a line that WRITES to the device: an scp, a plan/script upload, a mkdir, sysctl, reboot
WRITE_RE='^SCP|cat > /tmp/rw-|mkdir -p|sysctl|reboot'

# ═══════════════════════════════════════════════════════════════════════════
echo ""
echo "A. a full run — backup first, no clean, reboot last"
# ═══════════════════════════════════════════════════════════════════════════
run full 192.168.50.99
[ "$RC" -eq 0 ]; chk "A1 exits 0"
TAR=$(first full 'tar -cf -'); WR=$(first full "$WRITE_RE"); PL=$(first full 'cat > /tmp/rw-provision-plan')
[ -n "$TAR" ]; chk "A2 the backup fetch happened"
[ -n "$WR" ] && [ -n "$PL" ]; chk "A3 a write happened (witness for A4)"
[ -n "$TAR" ] && [ -n "$WR" ] && [ "$TAR" -lt "$WR" ]; chk "A4 the backup fetch precedes the first write"
! grep -q "rw-clean-plan" "$TMP/full.log"; chk "A5 no rw-clean-plan is ever sent"
! grep -qP "^SSH	/opt/roomwizard/disable-steelcase" "$TMP/full.log"; chk "A6 disable-steelcase.sh is never RUN"
! grep -qP "^SCP	.*disable-steelcase" "$TMP/full.log"; chk "A6b disable-steelcase.sh is never PUSHED"
LAST=$(grep '^SSH' "$TMP/full.log" | tail -1 | cut -f2)
[ "$LAST" = "reboot" ]; chk "A7 the reboot is the last remote command"
[ "$(grep -cP "^SSH\treboot$" "$TMP/full.log")" -eq 1 ]; chk "A8 reboot happens once"
NEWBK=$(new_backups)
[ "$NEWBK" -ge 1 ]; chk "A9 an archive was produced"
# backup.sh names its archive <unit>-<YYYYmmdd-HHMMSS>.tar.gz and refuses an existing
# name ("exists", before it ever calls the lister). The whole suite takes about a second,
# so run C could land in the same second as run A and die there, with C1 still passing
# and C2 failing. Remove what A made, so no later run can collide with it.
for f in "$BK"/*; do
    [ -e "$f" ] || continue
    grep -qxF "${f##*/}" "$TMP/backups.before" || rm -f "$f"
done

# ═══════════════════════════════════════════════════════════════════════════
echo ""
echo "B. --dry-run — prints the plan, changes nothing, takes no backup"
# ═══════════════════════════════════════════════════════════════════════════
run dry 192.168.50.99 --dry-run
[ "$RC" -eq 0 ]; chk "B1 exits 0"
! grep -q "tar -cf -" "$TMP/dry.log"; chk "B2 no backup fetch"
! grep -qE "$WRITE_RE" "$TMP/dry.log"; chk "B3 no write of any kind"
[ "$(grep -vcP "^SSH\ttrue$" "$TMP/dry.log")" -eq 0 ]; chk "B4 the only remote call is the SSH probe"
grep -q "Provision plan" "$TMP/dry.out"; chk "B5 it prints the provision plan"
grep -qE "^ *install" "$TMP/dry.out"; chk "B6 it prints plan records (an install line)"

# ═══════════════════════════════════════════════════════════════════════════
echo ""
echo "C. negative control — a failing backup stops the run before any write"
# ═══════════════════════════════════════════════════════════════════════════
FAKE_LISTER_FAIL=1 run nobk 192.168.50.99
[ "$RC" -ne 0 ]; chk "C1 exits non-zero"
grep -q "cd / || exit 1" "$TMP/nobk.log"; chk "C2 the backup was attempted (lister ran)"
! grep -qE "$WRITE_RE" "$TMP/nobk.log"; chk "C3 no write reached the device"
! grep -q "rw-provision-plan" "$TMP/nobk.log"; chk "C4 no plan was written"

# ═══════════════════════════════════════════════════════════════════════════
echo ""
echo "D. --status — read-only"
# ═══════════════════════════════════════════════════════════════════════════
run stat 192.168.50.99 --status
[ "$RC" -eq 0 ]; chk "D1 exits 0"
! grep -q "tar -cf -" "$TMP/stat.log"; chk "D2 no backup fetch"
! grep -qE "$WRITE_RE" "$TMP/stat.log"; chk "D3 no write, no reboot"
[ "$(grep -c "^SSH" "$TMP/stat.log")" -ge 2 ]; chk "D4 it did query the device (witness)"

# ═══════════════════════════════════════════════════════════════════════════
echo ""
echo "E. the clean vocabulary and the sshd-policy vocabulary are gone"
# ═══════════════════════════════════════════════════════════════════════════
for opt in --no-clean --keep-factory --keep-sweeps --remove --deep-clean --ssh-auth=key --ssh-auth=password --sshd-only --no-sshd; do
    run "gone$opt" 192.168.50.99 $opt
    [ "$RC" -ne 0 ]; chk "E $opt is refused"
    [ ! -s "$TMP/gone$opt.log" ]; chk "E $opt reached the device not at all"
done

# ═══════════════════════════════════════════════════════════════════════════
echo ""
echo "F. harness witness"
# ═══════════════════════════════════════════════════════════════════════════
grep -q "^SSH" "$TMP/full.log" && grep -q "^SCP" "$TMP/full.log"; chk "F1 the full run reached the fake ssh and scp"

# 9 (A) + 6 (B) + 4 (C) + 4 (D) + 18 (E) + 1 (F) = 42. A suite that runs nothing
# reports success.
TOTAL=$((PASS + FAIL))
MIN_CASES=43
echo ""
echo "  $PASS passed, $FAIL failed"
if [ "$TOTAL" -lt "$MIN_CASES" ]; then
    echo -e "  ${RED}HARNESS ERROR${NC}: only $TOTAL cases ran, expected at least $MIN_CASES."
    exit 2
fi
[ "$FAIL" -eq 0 ] || exit 1
echo -e "  ${GREEN}all good${NC}"
