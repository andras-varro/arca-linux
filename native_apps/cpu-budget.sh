#!/bin/bash
#
# cpu-budget.sh — launch each native app on a device, let it sit on its first
# screen, measure its CPU over a fixed window and grade it against a per-app budget.
#
#   ./native_apps/cpu-budget.sh <ip> [app ...]
#   ./native_apps/cpu-budget.sh --list
#
# DEVICE ONLY to run, host-only to test: tests/cpu_budget_test.sh sources this file
# (the main-guard at the bottom keeps it from running) and drives it through an
# `ssh` stub on PATH.
#
# Why it exists: a 1 Hz full-frame header redraw took jellyfin's idle CPU from ~7%
# to ~10-15% and nobody saw it until audio broke up. Idle cost is invisible on the
# panel, so it is measured, every app, against a number written down here.
#
# One list, one home: the targets, their production argv, the init-script stop and
# the restore-on-every-exit-path all come from smoke-first-screen.sh, sourced.
#
# ── Verdicts ────────────────────────────────────────────────────────────────
#   pass           ticks over the window <= the app's budget
#   over-budget    ticks > budget
#   exited         the process was gone before the window ended (or before it began):
#                  an app that died is never a pass
#   could-not-tell ssh failed, /proc unparsable, no budget for the app, or the
#                  binary is missing
#
# ── The measurement ─────────────────────────────────────────────────────────
# ONE ssh command per app: start it, sleep CPU_SETTLE_SECS (startup work excluded),
# read /proc/<pid>/stat, sleep CPU_WINDOW_SECS, read it again, kill it. Nothing
# else runs on the device during the window: a once-a-second grep loop of ours
# inflated readings. CPU = utime+stime (stat fields 14+15, USER_HZ = 100 ticks/s)
# delta; the context-switch count is voluntary+nonvoluntary from /proc/<pid>/status.
# The app sits on its first screen with no input.
#
# Exit: 0 every app passed; 1 any did not; 2 usage or unreachable device.

# shellcheck source-path=SCRIPTDIR
# shellcheck source=smoke-first-screen.sh
. "$(dirname "${BASH_SOURCE[0]}")/smoke-first-screen.sh"

CPU_SETTLE_SECS="${CPU_SETTLE_SECS:-5}"
CPU_WINDOW_SECS="${CPU_WINDOW_SECS:-10}"

# ── Budgets: clock ticks (1/100 s) of CPU per CPU_WINDOW_SECS (10 s), one home ──
# MEASURED on unit .115, 2026-10-22, native_apps just deployed, each app on its first
# screen with no input, three repeats (ticks per 10 s, repeat 1 / 2 / 3). Budget =
# the worst repeat + 25% + 3 ticks, rounded up to a multiple of 5 (the repeats of one
# app differ by up to 7 ticks: app_launcher 23-30, control_panel 23-28), except the two
# busy loops, which get +10% (a 25% margin would pass 100% of the core).
# A budget is a tripwire for a regression, not a target: raise one only with a new
# measurement beside it. ludfin's budget (30) sits far below its bad builds (102
# ticks with the 1 Hz full-frame header redraw; 69-74 before the DSP-skip and
# header-only-repaint fixes) — measured here on its first screen, no music.
cpu_budget() {
    case "$1" in
        app_launcher)   echo 45 ;;   # 30 26 23
        control_panel)  echo 40 ;;   # 28 24 23
        file_manager)   echo 55 ;;   # 38 38 35
        terminal)       echo 10 ;;   #  4  4  4
        ludfin)         echo 30 ;;   # 19 20 19  (ctxsw ~305)
        ludkino)        echo 25 ;;   # 15 17 15  (ctxsw ~297, audio stream open)
        snake)          echo 80 ;;   # 56 55 58
        tetris)         echo 75 ;;   # 52 54 53
        pong)           echo 80 ;;   # 57 59 59
        samegame)       echo 80 ;;   # 60 58 59
        frogger)        echo 70 ;;   # 53 53 53
        platformer)     echo 75 ;;   # 54 53 55
        brick_breaker)  echo 75 ;;   # 53 54 53
        audio_mix_test) echo 25 ;;   # 13 13 14
        retro_picker)   echo 60 ;;   # 42 41 43  (measured 2026-10-26, idle list, no input)
        frame)          echo 45 ;;   # 31 30 (measured 2026-10-30, start menu, no input; the slideshow itself idles near 1%)
        clock)          echo 15 ;;   # 3 (measured 2026-10-30, main screen idle, 20 ticks over 60 s, n=1; cached seven-segment glyphs, only the seconds box redrawn each second)
        theremin)       echo 960 ;;  # 872 862 863: synthesises continuously, ~87% of the core
        touch_raw)      echo 760 ;;  # 682 683 686: polls its input flat out, ~68%
        *) return 1 ;;
    esac
}

# ═══ Pure functions ═══════════════════════════════════════════════════════════

# cpu_stat_ticks <line>  →  utime+stime, or return 1. The comm field (2) may hold
# spaces, so everything through the LAST ")" is dropped; field 3 (state) is then $1.
cpu_stat_ticks() {
    local rest
    rest=${1##*) }
    [ "$rest" != "$1" ] || return 1
    printf '%s\n' "$rest" | awk 'NF >= 13 && $12 ~ /^[0-9]+$/ && $13 ~ /^[0-9]+$/ { print $12 + $13; ok = 1 } END { exit !ok }'
}

# cpu_ctxt_sum <status-lines>  →  voluntary + nonvoluntary, or return 1
cpu_ctxt_sum() {
    printf '%s\n' "$1" | grep -o 'ctxt_switches:[[:space:]]*[0-9][0-9]*' | awk '{ s += $2; n++ } END { if (n < 2) exit 1; print s }'
}

# cpu_classify <launch-out> <budget|""> → "<verdict> ticks=N pct=X.Y budget=B ctxsw=C" or "<verdict> reason"
cpu_classify() {
    local out="$1" budget="$2" s1 s2 c1 c2 t1 t2 k1 k2 ticks pct ctx
    if printf '%s\n' "$out" | grep -qx CPU_NOEXEC; then
        echo "could-not-tell binary missing or not executable"; return
    fi
    if printf '%s\n' "$out" | grep -q '^CPU_EXIT='; then
        echo "exited gone before the ${CPU_WINDOW_SECS}s window ended ($(printf '%s\n' "$out" | sed -n 's/^CPU_EXIT=/exit /p' | head -n1))"; return
    fi
    if ! printf '%s\n' "$out" | grep -qx CPU_DONE; then
        echo "could-not-tell no verdict from the device (ssh failed?)"; return
    fi
    if [ -z "$budget" ]; then
        echo "could-not-tell no budget for this app in cpu_budget()"; return
    fi
    s1=$(printf '%s\n' "$out" | sed -n 's/^CPU_S1 //p' | head -n1)
    s2=$(printf '%s\n' "$out" | sed -n 's/^CPU_S2 //p' | head -n1)
    if ! t1=$(cpu_stat_ticks "$s1") || ! t2=$(cpu_stat_ticks "$s2"); then
        echo "could-not-tell /proc/<pid>/stat unparsable"; return
    fi
    ticks=$((t2 - t1))
    [ "$ticks" -ge 0 ] || { echo "could-not-tell tick counter went backwards"; return; }
    pct=$((ticks * 10 / CPU_WINDOW_SECS)); pct="$((pct / 10)).$((pct % 10))"
    c1=$(printf '%s\n' "$out" | sed -n 's/^CPU_C1 //p' | head -n1)
    c2=$(printf '%s\n' "$out" | sed -n 's/^CPU_C2 //p' | head -n1)
    if k1=$(cpu_ctxt_sum "$c1") && k2=$(cpu_ctxt_sum "$c2"); then ctx=$((k2 - k1)); else ctx="?"; fi
    if [ "$ticks" -gt "$budget" ]; then
        echo "over-budget ticks=$ticks pct=$pct budget=$budget ctxsw=$ctx"
    else
        echo "pass ticks=$ticks pct=$pct budget=$budget ctxsw=$ctx"
    fi
}

# ═══ The device side ═══════════════════════════════════════════════════════

# cpu_one <name> <exec> <argv>  →  prints the verdict line
cpu_one() {
    local name="$1" exe="$2" argv="$3" out pid budget v
    out=$(smoke_ssh "b=$exe; [ -x \"\$b\" ] || { echo CPU_NOEXEC; exit 0; }
\"\$b\" $argv </dev/null >/tmp/cpu-$name.log 2>&1 &
p=\$!; echo CPU_PID=\$p; sleep $CPU_SETTLE_SECS
if kill -0 \$p 2>/dev/null; then
  echo CPU_S1 \$(cat /proc/\$p/stat); echo CPU_C1 \$(grep ctxt_switches /proc/\$p/status)
  sleep $CPU_WINDOW_SECS
  if kill -0 \$p 2>/dev/null; then
    echo CPU_S2 \$(cat /proc/\$p/stat); echo CPU_C2 \$(grep ctxt_switches /proc/\$p/status); echo CPU_DONE
  else wait \$p; echo CPU_EXIT=\$?; fi
else wait \$p; echo CPU_EXIT=\$?; fi
kill \$p 2>/dev/null; sleep 1; kill -9 \$p 2>/dev/null; true" 2>/dev/null)
    pid=$(printf '%s\n' "$out" | sed -n 's/^CPU_PID=\([0-9][0-9]*\)$/\1/p' | head -n1)
    if [ -n "$pid" ]; then
        # the remote command normally did this; this covers an ssh that dropped mid-window
        smoke_ssh "kill $pid 2>/dev/null; true" >/dev/null 2>&1
    fi
    budget=$(cpu_budget "$name") || budget=""
    v=$(cpu_classify "$out" "$budget")
    printf '  %-16s %s\n' "$name" "$v"
}

cpu_usage() {
    cat <<'USAGE'
Usage: native_apps/cpu-budget.sh <ip> [app ...]
       native_apps/cpu-budget.sh --list

Stops the running app (/etc/init.d/arca-app stop), launches each app with its
production argv, waits CPU_SETTLE_SECS (default 5), measures utime+stime over
CPU_WINDOW_SECS (default 10) in one ssh command, grades the clock ticks against
the per-app budget in cpu_budget(), and restarts the app on every exit path.

Verdicts: pass | over-budget | exited | could-not-tell
Exit: 0 all passed, 1 any did not, 2 usage error or device unreachable.
USAGE
}

cpu_main() {
    local names=() t name exe argv sel v pass=0 total=0 lines b
    case "${1:-}" in
        -h|--help) cpu_usage; return 0 ;;
        --list) smoke_targets | while IFS='|' read -r name exe argv; do
                    b=$(cpu_budget "$name") || b="(none)"
                    printf '  %-16s budget %-6s %s %s\n' "$name" "$b" "$exe" "$argv"; done
                return 0 ;;
        ""|-*) cpu_usage >&2; return 2 ;;
    esac
    SMOKE_TARGET_HOST="$1"; shift
    case "$SMOKE_TARGET_HOST" in *@*) ;; *) SMOKE_TARGET_HOST="root@$SMOKE_TARGET_HOST" ;; esac
    names=("$@")
    lines=$(smoke_targets)
    for name in "${names[@]}"; do
        printf '%s\n' "$lines" | grep -q "^$name|" \
            || { echo "cpu-budget: unknown app '$name' — see --list" >&2; return 2; }
    done

    # shellcheck source=../lib/arca-ssh.sh
    . "$SMOKE_REPO_DIR/lib/arca-ssh.sh"
    arca_ssh_gate "$SMOKE_TARGET_HOST" || return 2

    trap 'smoke_restore' EXIT
    trap 'smoke_restore; exit 130' INT
    trap 'smoke_restore; exit 143' TERM
    SMOKE_STOPPED=1
    echo "  stopping: /etc/init.d/arca-app stop"
    smoke_ssh "/etc/init.d/arca-app stop" >/dev/null 2>&1 \
        || echo "  ⚠️  stop reported failure; continuing — verdicts may be could-not-tell" >&2

    while IFS='|' read -r name exe argv; do
        if [ ${#names[@]} -gt 0 ]; then
            sel=0; for t in "${names[@]}"; do [ "$t" = "$name" ] && sel=1; done
            [ "$sel" = 1 ] || continue
        fi
        v=$(cpu_one "$name" "$exe" "$argv")
        echo "$v"
        total=$((total + 1))
        case "$v" in *" pass "*) pass=$((pass + 1)) ;; esac
    done <<< "$lines"

    smoke_restore
    echo ""
    echo "  $pass of $total within budget  (${CPU_WINDOW_SECS}s window after ${CPU_SETTLE_SECS}s settle)"
    [ "$total" -gt 0 ] || { echo "  no app was graded" >&2; return 2; }
    [ "$pass" -eq "$total" ]
}

if [ "${BASH_SOURCE[0]}" = "$0" ]; then
    cpu_main "$@"
    exit $?
fi
