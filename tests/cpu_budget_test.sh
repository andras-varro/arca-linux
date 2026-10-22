#!/bin/bash
#
# cpu_budget_test.sh — host regression for native_apps/cpu-budget.sh
#
# Host-only, no device, no root. The runner needs a device to RUN but not to be
# proven: the classifier is called directly over fabricated /proc lines, the whole
# script is driven through an `ssh` stub on PATH that plays a scripted device.
#
#   wsl.exe -e bash -lc "cd /mnt/c/work/roomwizard && ./tests/cpu_budget_test.sh"
#
#   A  cpu_stat_ticks — utime+stime from a stat line, comm with spaces/parens,
#      garbage and short lines refused
#   B  cpu_ctxt_sum — voluntary + nonvoluntary, refused when either is missing
#   C  cpu_classify — the budget boundary (budget passes, budget+1 fails), exited,
#      unparsable /proc, missing binary, silence, no budget, the context-switch delta
#   D  the budget table covers every target the smoke runner lists, each a positive
#      integer: a new app without a budget fails here, not on a device
#   E  the whole runner through the stub: verdict lines, exit codes, and the init
#      script's `start` on every path that issued `stop`, including failure and TERM
#
# $CPU_RUNNER points the suite at a staged copy — the hook a sabotage run uses.
# The directive below resolves the sourced runner from this script's directory.
# shellcheck source-path=SCRIPTDIR
set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
RUNNER="${CPU_RUNNER:-$REPO_DIR/native_apps/cpu-budget.sh}"

# shellcheck source=../native_apps/cpu-budget.sh
. "$RUNNER"
CPU_SETTLE_SECS=0; CPU_WINDOW_SECS=10

RED='\033[0;31m'; GREEN='\033[0;32m'; NC='\033[0m'
PASS=0; FAIL=0
ok()  { PASS=$((PASS + 1)); echo -e "  ${GREEN}pass${NC}  $1"; }
bad() { FAIL=$((FAIL + 1)); echo -e "  ${RED}FAIL${NC}  $1"; }
assert_eq() { if [ "$1" = "$2" ]; then ok "$3"; else bad "$3 (want '$1', got '$2')"; fi; }
assert_verdict() { assert_eq "$1" "${2%% *}" "$3 → $2"; }
has()   { if printf '%s\n' "$1" | grep -q -- "$2"; then ok "$3"; else bad "$3 (no '$2' in: $(printf '%s' "$1" | tr '\n' '|'))"; fi; }
hasnt() { if printf '%s\n' "$1" | grep -q -- "$2"; then bad "$3 (found '$2')"; else ok "$3"; fi; }

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT INT TERM

# stat_line <comm> <utime> <stime>  — a /proc/<pid>/stat line; utime is field 14
stat_line() { echo "123 ($1) S 1 123 123 0 -1 4194560 100 0 0 0 $2 $3 0 0 20 0 1 0 5000 1000000 200 4294967295"; }
ctx_line() { echo "voluntary_ctxt_switches: $1 nonvoluntary_ctxt_switches: $2"; }
# mk_out <u1> <s1> <u2> <s2> [c1v c1n c2v c2n]  — what a healthy measurement prints
mk_out() {
    printf 'CPU_PID=123\nCPU_S1 %s\nCPU_C1 %s\nCPU_S2 %s\nCPU_C2 %s\nCPU_DONE\n' \
        "$(stat_line app "$1" "$2")" "$(ctx_line "${5:-10}" "${6:-1}")" \
        "$(stat_line app "$3" "$4")" "$(ctx_line "${7:-110}" "${8:-4}")"
}

# ═══════════════════════════════════════════════════════════════════════════
echo ""; echo "A. cpu_stat_ticks"
assert_eq 45  "$(cpu_stat_ticks "$(stat_line app 30 15)")"              "A1 utime + stime"
assert_eq 7   "$(cpu_stat_ticks "$(stat_line 'my) app (x)' 5 2)")"       "A2 a comm that itself contains a close-paren and space"
assert_eq 0   "$(cpu_stat_ticks "$(stat_line app 0 0)")"               "A3 zero is a number, not an error"
if cpu_stat_ticks "garbage" >/dev/null; then bad "A4 garbage refused"; else ok "A4 garbage refused"; fi
if cpu_stat_ticks "" >/dev/null; then bad "A5 empty refused"; else ok "A5 empty refused"; fi
if cpu_stat_ticks "123 (a) S 1 2 3" >/dev/null; then bad "A6 a short line refused"; else ok "A6 a short line refused"; fi
if cpu_stat_ticks "$(stat_line app x 5)" >/dev/null; then bad "A7 non-numeric utime refused"; else ok "A7 non-numeric utime refused"; fi

# ═══════════════════════════════════════════════════════════════════════════
echo ""; echo "B. cpu_ctxt_sum"
assert_eq 11 "$(cpu_ctxt_sum "$(ctx_line 10 1)")" "B1 voluntary + nonvoluntary"
assert_eq 11 "$(cpu_ctxt_sum $'voluntary_ctxt_switches:\t10\nnonvoluntary_ctxt_switches:\t1')" "B2 tab separated, one per line"
if cpu_ctxt_sum "voluntary_ctxt_switches: 10" >/dev/null; then bad "B3 one half alone refused"; else ok "B3 one half alone refused"; fi
if cpu_ctxt_sum "" >/dev/null; then bad "B4 empty refused"; else ok "B4 empty refused"; fi

# ═══════════════════════════════════════════════════════════════════════════
echo ""; echo "C. the classifier"
V=$(cpu_classify "$(mk_out 100 0 100 50)" 50)
assert_verdict pass "$V" "C1 ticks == budget passes"
has "$V" 'ticks=50 pct=5.0 budget=50 ctxsw=103' "C2 the line carries ticks, percent, budget and the context-switch delta"
V=$(cpu_classify "$(mk_out 100 0 100 51)" 50)
assert_verdict over-budget "$V" "C3 ticks == budget+1 fails"
has "$V" 'ticks=51 pct=5.1' "C4 ...and says by how much"
assert_verdict pass "$(cpu_classify "$(mk_out 100 0 100 49)" 50)" "C5 ticks == budget-1 passes"
assert_verdict pass "$(cpu_classify "$(mk_out 100 20 110 30)" 20)" "C6 stime counts as well as utime (20 = 10 + 10)"
assert_verdict over-budget "$(cpu_classify "$(mk_out 100 20 110 31)" 20)" "C7 ...and 21 fails"
assert_verdict pass "$(cpu_classify "$(mk_out 7 0 7 0)" 0)" "C8 a budget of 0 passes 0 ticks"
assert_verdict exited "$(cpu_classify $'CPU_PID=9\nCPU_EXIT=1' 50)" "C9 gone before the window starts"
assert_verdict exited "$(cpu_classify $'CPU_PID=9\nCPU_S1 x\nCPU_C1 y\nCPU_EXIT=139' 50)" "C10 gone during the window"
assert_verdict exited "$(cpu_classify $'CPU_PID=9\nCPU_EXIT=0' 50)" "C11 a clean exit is still not a pass"
has "$(cpu_classify $'CPU_PID=9\nCPU_EXIT=132' 50)" 'exit 132' "C12 the exit status is reported"
assert_verdict could-not-tell "$(cpu_classify CPU_NOEXEC 50)" "C13 missing binary"
assert_verdict could-not-tell "$(cpu_classify '' 50)" "C14 silence is not success"
assert_verdict could-not-tell "$(cpu_classify $'CPU_PID=9\nCPU_S1 x' 50)" "C15 no CPU_DONE (ssh dropped mid-window)"
assert_verdict could-not-tell "$(cpu_classify "$(mk_out 100 0 100 50)" '')" "C16 no budget for the app"
assert_verdict could-not-tell "$(cpu_classify "$(mk_out 100 0 100 50 | sed 's/^CPU_S2 .*/CPU_S2 junk/')" 50)" "C17 unparsable second stat"
assert_verdict could-not-tell "$(cpu_classify "$(mk_out 100 0 100 50 | sed 's/^CPU_S1 .*/CPU_S1/')" 50)" "C18 empty first stat"
assert_verdict could-not-tell "$(cpu_classify "$(mk_out 100 50 100 0)" 50)" "C19 a counter that went backwards"
has "$(cpu_classify "$(mk_out 100 0 100 5 | sed 's/^CPU_C2 .*/CPU_C2 nothing/')" 50)" 'pass .*ctxsw=?' "C20 unreadable ctxt switches do not sink the CPU verdict"

# ═══════════════════════════════════════════════════════════════════════════
echo ""; echo "D. the budget table"
n=0; miss=""
while IFS='|' read -r name _; do
    n=$((n + 1))
    b=$(cpu_budget "$name") || b=""
    printf '%s\n' "$b" | grep -Eq '^[1-9][0-9]*$' || miss="$miss $name"
done <<< "$(smoke_targets)"
assert_eq "" "${miss# }" "D1 every smoke target has a positive integer budget"
assert_eq 15 "$n" "D2 15 targets were looked at (a zero-subject loop is not a pass)"
if cpu_budget no_such_app >/dev/null; then bad "D3 an unknown app has no budget"; else ok "D3 an unknown app has no budget"; fi

# ═══════════════════════════════════════════════════════════════════════════
echo ""; echo "E. the runner end to end, through an ssh stub"
SC="$TMP/sc"; mkdir -p "$TMP/bin"
# ⚠️ The stub MUST slurp its stdin unless given -n, as a real ssh does: the runner
# calls ssh inside a `while read` loop, and a stub that never reads would let a
# missing -n through and E-all (15 graded) would pass vacuously.
cat > "$TMP/bin/ssh" <<'STUB'
#!/bin/bash
n=0
while :; do case "${1:-}" in -o) shift 2 ;; -n) n=1; shift ;; *) break ;; esac; done
shift
cmd="$*"
[ "$n" = 1 ] || cat > /dev/null
printf '%s\n' "$cmd" | tr '\n' ' ' >> "$SC/calls"; echo >> "$SC/calls"
case "$cmd" in
    true)  [ -f "$SC/down" ] && { echo "ssh: connect refused" >&2; exit 255; }
           exit 0 ;;
    *"arca-app stop"*)  exit 0 ;;
    *"arca-app start"*) exit 0 ;;
    *CPU_NOEXEC*)
        if [ -f "$SC/term" ]; then
            for _ in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do [ -s "$SC/mainpid" ] && break; sleep 0.1; done
            kill -TERM "$(cat "$SC/mainpid")"
        fi
        [ -f "$SC/sshfail" ] && exit 255
        cat "$SC/launch.out"; exit 0 ;;
    kill*) exit 0 ;;
esac
exit 0
STUB
chmod +x "$TMP/bin/ssh"
export SC
# scenario <launch-out> [touch-files…]
scenario() { rm -rf "$SC"; mkdir -p "$SC"; printf '%s\n' "$1" > "$SC/launch.out"; shift; for f in "$@"; do : > "$SC/$f"; done; }
run_runner() {
    PATH="$TMP/bin:$PATH" bash "$RUNNER" "$@" > "$SC/stdout" 2>&1 &
    local rp=$!; echo "$rp" > "$SC/mainpid"; RC=0; wait "$rp" || RC=$?
    OUT=$(cat "$SC/stdout"); CALLS=$(cat "$SC/calls" 2>/dev/null)
}
SB=$(cpu_budget snake)

scenario "$(mk_out 0 0 0 "$SB")"
run_runner 1.2.3.4 snake
assert_eq 0 "$RC" "E1 an app exactly at its budget exits 0"
has "$OUT" "snake *pass ticks=$SB " "E2 the verdict line names the app, outcome and ticks"
has "$OUT" '1 of 1 within budget' "E3 the summary"
assert_eq 1 "$(printf '%s\n' "$CALLS" | grep -n 'arca-app stop' | cut -d: -f1 | head -n1 | grep -c '^2$')" "E4 stop is the first call after the gate probe"
has "$(printf '%s\n' "$CALLS" | tail -n1)" 'arca-app start' "E5 start is the LAST call"
has "$CALLS" 'b=/opt/games/snake; .* /dev/fb0 /dev/input/touchscreen0 </dev/null' "E6 the production argv reaches the launch"
# shellcheck disable=SC2016  # literal remote text
has "$CALLS" '/proc/\$p/stat' "E7 the sample reads /proc/<pid>/stat"
has "$CALLS" 'sleep 5 .*CPU_S1 .*sleep 10 .*CPU_S2' "E8 settle sleep, first read, window sleep, second read: all inside the one remote command"
hasnt "$CALLS" 'killall' "E9 no killall, ever"
has "$CALLS" '^kill 123' "E10 the launched PID — and only it — is killed afterwards"

scenario "$(mk_out 0 0 0 $((SB + 1)))"
run_runner 1.2.3.4 snake
assert_eq 1 "$RC" "E11 one tick over budget exits 1"
has "$OUT" 'snake *over-budget' "E12 over-budget reported"
has "$(printf '%s\n' "$CALLS" | tail -n1)" 'arca-app start' "E13 start runs after an over-budget verdict"

scenario $'CPU_PID=9\nCPU_EXIT=1'
run_runner 1.2.3.4 tetris
assert_eq 1 "$RC" "E14 an app that died exits 1"
has "$OUT" 'tetris *exited' "E15 exited reported"
has "$(printf '%s\n' "$CALLS" | tail -n1)" 'arca-app start' "E16 start runs after a death"

scenario "" sshfail
run_runner 1.2.3.4 pong
assert_eq 1 "$RC" "E17 a failed measurement exits 1"
has "$OUT" 'pong *could-not-tell' "E18 ssh failure is could-not-tell"
has "$(printf '%s\n' "$CALLS" | tail -n1)" 'arca-app start' "E19 start runs after a harness failure"

scenario "$(mk_out 0 0 0 1)" term
run_runner 1.2.3.4 frogger
assert_eq 143 "$RC" "E20 TERM mid-run exits 143"
has "$(printf '%s\n' "$CALLS" | tail -n1)" 'arca-app start' "E21 start runs on TERM (the trap)"

scenario "$(mk_out 0 0 0 1)" down
run_runner 1.2.3.4 snake
assert_eq 2 "$RC" "E22 an unreachable device exits 2"
hasnt "$CALLS" 'arca-app' "E23 nothing is stopped (or started) on a device never reached"

scenario "$(mk_out 0 0 0 1)"
run_runner 1.2.3.4 no_such_app
assert_eq 2 "$RC" "E24 an unknown app is refused"
run_runner
assert_eq 2 "$RC" "E25 no <ip> is a usage error"

scenario "$(mk_out 0 0 0 1)"
run_runner 1.2.3.4
assert_eq 15 "$(printf '%s\n' "$OUT" | grep -cE '^  [a-z_]+ +pass ')" "E26 no subset: every one of the 15 targets is graded"
assert_eq 0 "$RC" "E27 ...and all passing exits 0"

# ═══════════════════════════════════════════════════════════════════════════
echo ""
echo "════════════════════════════════════════"
TOTAL=$((PASS + FAIL))
echo "  $PASS passed, $FAIL failed, $TOTAL total"
if [ "$TOTAL" -lt 60 ]; then
    echo -e "  ${RED}✗ only $TOTAL cases ran — the harness itself is broken${NC}"
    exit 1
fi
if [ "$FAIL" -gt 0 ]; then
    echo -e "  ${RED}✗ $FAIL failure(s)${NC}"
    exit 1
fi
echo -e "  ${GREEN}✓ all $TOTAL cases passed${NC}"
echo ""
exit 0
