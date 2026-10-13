#!/bin/bash
#
# dump_boot_regs_test.sh -- regression for uboot/tools/dump-boot-regs.sh:
#
#   A  with a fake ssh (runs the remote command locally) and a fake devmem (logs its
#      arguments): every devmem call has exactly two arguments (a read), the address
#      list is not empty and holds a padconf, an SDRC and a CM register, a register
#      whose read faults becomes "0xADDR=FAULT" and the dump continues, and the
#      output is sorted "0xADDR=0xVALUE" lines.
#   B  the script has no write form: a second argument is refused with exit 2 before
#      any ssh, and no devmem line in its text carries a value operand.
#
# Every assertion is run against decoy copies that break the rule (outside the
# repo, under the test's temp dir); each must FAIL, so a green run is not vacuous.
#
# Host-only, no device, no root. Run it:
#   wsl.exe -e bash -lc "cd /mnt/c/work/roomwizard && ./tests/dump_boot_regs_test.sh"

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
SUBJECT="$REPO_DIR/uboot/tools/dump-boot-regs.sh"

RED='\033[0;31m'; GREEN='\033[0;32m'; NC='\033[0m'
PASS=0
FAIL=0
ok()  { PASS=$((PASS + 1)); echo -e "  ${GREEN}pass${NC}  $1"; }
bad() { FAIL=$((FAIL + 1)); echo -e "  ${RED}FAIL${NC}  $1"; }
check() { local d="$1"; shift; if "$@"; then ok "$d"; else bad "$d"; fi; }
# control: ok when the command FAILS (the assertion can fail)
control() { local d="$1"; shift; if "$@"; then bad "$d (the assertion passed against a decoy)"; else ok "$d"; fi; }

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT INT TERM

[ -f "$SUBJECT" ] || { echo "subject missing: $SUBJECT"; exit 2; }

# fake ssh: the last argument is the remote command; run it here with the fake devmem.
mkdir -p "$TMP/bin"
cat > "$TMP/bin/ssh" <<'EOF'
#!/bin/sh
echo "ssh $*" >> "$FAKE_LOG_SSH"
for last; do :; done
exec sh -c "$last"
EOF
# fake devmem: log the arguments; fault (SIGBUS-like exit) on FAKE_FAULT_ADDR.
cat > "$TMP/bin/devmem" <<'EOF'
#!/bin/sh
echo "$*" >> "$FAKE_LOG_DEVMEM"
if [ "$1" = "${FAKE_FAULT_ADDR:-none}" ]; then exit 135; fi
echo 0x00000118
EOF
chmod +x "$TMP/bin/ssh" "$TMP/bin/devmem"

FAULT_ADDR=0x6d000080

# run_subject <script> <tag> [args...]: output in $TMP/<tag>.out, devmem log in .dm, ssh log in .ssh
run_subject() {
    local script="$1" tag="$2"
    shift 2
    : > "$TMP/$tag.dm"; : > "$TMP/$tag.ssh"
    PATH="$TMP/bin:$PATH" FAKE_LOG_DEVMEM="$TMP/$tag.dm" FAKE_LOG_SSH="$TMP/$tag.ssh" \
        FAKE_FAULT_ADDR="$FAULT_ADDR" bash "$script" "$@" > "$TMP/$tag.out" 2> "$TMP/$tag.err"
    echo $? > "$TMP/$tag.rc"
}

# --- the assertions, as functions of a tag, so a decoy can be fed to the same ones
only_reads()   { [ -s "$TMP/$1.dm" ] && ! grep -qvE '^0x[0-9a-f]{8} 32$' "$TMP/$1.dm"; }
list_has()     { # the devmem log holds a padconf, an SDRC and a CM register
    grep -q '^0x48002030 ' "$TMP/$1.dm" && grep -q '^0x6d000080 ' "$TMP/$1.dm" \
        && grep -q '^0x48004d40 ' "$TMP/$1.dm"
}
list_big()     { [ "$(wc -l < "$TMP/$1.dm")" -ge 200 ]; }
out_shape()    { [ -s "$TMP/$1.out" ] && ! grep -qvE '^0x[0-9a-f]{8}=(0x[0-9A-Fa-f]{8}|FAULT)$' "$TMP/$1.out"; }
out_complete() { [ "$(wc -l < "$TMP/$1.out")" -eq "$(wc -l < "$TMP/$1.dm")" ]; }
out_sorted()   { LC_ALL=C sort -c "$TMP/$1.out" 2>/dev/null; }
fault_row()    { grep -qx "$FAULT_ADDR=FAULT" "$TMP/$1.out" && [ "$(grep -c '=FAULT' "$TMP/$1.out")" -eq 1 ]; }
one_ssh()      { [ "$(grep -c devmem "$TMP/$1.ssh")" -eq 1 ]; }

echo "A  fake ssh + fake devmem"
run_subject "$SUBJECT" real 192.0.2.7
check "A1 exits 0" [ "$(cat "$TMP/real.rc")" = 0 ]
check "A2 every devmem call is 'ADDR 32' (two arguments, a read)" only_reads real
check "A3 the address list holds a padconf, an SDRC and a CM register" list_has real
check "A4 the list is not a stub (at least 200 registers)" list_big real
check "A5 output lines are 0xADDR=0xVALUE or 0xADDR=FAULT" out_shape real
check "A6 one output line per devmem call (a fault does not abort the dump)" out_complete real
check "A7 the faulting register is recorded as FAULT, and only it" fault_row real
check "A8 output is sorted by address" out_sorted real
check "A9 one ssh session for the whole dump" one_ssh real
check "A10 the dump reaches the last GPMC register after the fault" grep -q '^0x6e000108=' "$TMP/real.out"

echo "B  no write form"
run_subject "$SUBJECT" extra 192.0.2.7 0x48002030 0x1
check "B1 a second argument exits 2" [ "$(cat "$TMP/extra.rc")" = 2 ]
b2() { [ ! -s "$TMP/extra.ssh" ] && [ ! -s "$TMP/extra.dm" ]; }
check "B2 ...before any ssh or devmem" b2
# every executable devmem mention in the script is the two-operand read
b3() {
    local f="$1" n
    n=$(grep -v '^[[:space:]]*#' "$f" | grep -c 'devmem')
    # shellcheck disable=SC2016  # the pattern holds a literal backslash-dollar, not an expansion
    [ "$n" -ge 1 ] && ! grep -v '^[[:space:]]*#' "$f" | grep 'devmem' | grep -vq 'devmem \\\$a 32 2>/dev/null'
}
check "B3 the script text has no devmem form except 'devmem \$a 32'" b3 "$SUBJECT"

echo "C  decoys: each assertion must fail on a script that breaks its rule"
# decoys sit at the same depth as the subject so that their ../../lib/arca-ssh.sh resolves
DECOY="$TMP/d/uboot/tools"; mkdir -p "$DECOY" "$TMP/d/lib"; cp "$REPO_DIR/lib/arca-ssh.sh" "$TMP/d/lib/"
# C1: the remote command writes (three operands)
sed 's|devmem \\\$a 32 2>/dev/null|devmem \\$a 32 0 2>/dev/null|' "$SUBJECT" > "$DECOY/decoy-write.sh"
cmp -s "$SUBJECT" "$DECOY/decoy-write.sh" && bad "C0 the write decoy differs from the subject"
run_subject "$DECOY/decoy-write.sh" dwrite 192.0.2.7
control "C1 a script that issues a 3-operand devmem fails A2" only_reads dwrite
control "C2 ...and fails B3" b3 "$DECOY/decoy-write.sh"
# C3: no padconf block
grep -v 'add "PADCONF' "$SUBJECT" > "$DECOY/decoy-nopad.sh"
run_subject "$DECOY/decoy-nopad.sh" dnopad 192.0.2.7
control "C3 a script without the padconf rows fails A3" list_has dnopad
control "C4 ...and fails A4 (size)" list_big dnopad
# C5: a fault is not recorded (the FAULT branch dropped)
sed 's|echo \\"\\$a=FAULT\\"|true|' "$SUBJECT" > "$DECOY/decoy-nofault.sh"
cmp -s "$SUBJECT" "$DECOY/decoy-nofault.sh" && bad "C0 the no-fault decoy differs from the subject"
run_subject "$DECOY/decoy-nofault.sh" dnofault 192.0.2.7
control "C5 a script that drops a faulting row fails A7" fault_row dnofault
control "C6 ...and fails A6" out_complete dnofault
# C7: a second argument accepted
grep -v '"\$#" -eq 1' "$SUBJECT" > "$DECOY/decoy-args.sh"
run_subject "$DECOY/decoy-args.sh" dargs 192.0.2.7 0x48002030 0x1
control "C7 a script that accepts extra arguments fails B1" [ "$(cat "$TMP/dargs.rc")" = 2 ]

echo
echo "dump_boot_regs_test: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
