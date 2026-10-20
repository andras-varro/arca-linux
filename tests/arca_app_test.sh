#!/bin/bash
#
# arca_app_test.sh — regression for device-files/arca-app (the respawn wrapper's start/stop)
#
# Host-only, no device, no root. Run it:
#
#   wsl.exe -e bash -lc "cd /mnt/c/work/roomwizard && ./tests/arca_app_test.sh"
#
# WHAT IT COVERS: the init script is run from a COPY whose /opt and /var paths point at a
# throwaway tree, with a start-stop-daemon shim and the REAL device busybox applets (the
# WSL home busybox, 1.35 - same applet set as the device's 1.37 for these purposes) on PATH
# for pidof and killall.  The app is a sleeper.
#
#   A  start leaves exactly one respawn wrapper
#   B  a second start leaves still exactly one (busybox pidof has no -x, so the old guard
#      never fired: measured on 192.168.50.115 as two wrapper loops after one deploy)
#   C  stop leaves no wrapper, and nothing restarts the app afterwards
#   D  stop also reaches a wrapper the pidfile does not name (an orphan of an earlier start)
#
# NOT COVERED: app_pids() (it matches /proc/*/exe, and a shell-script app's exe is the
# shell), and the real start-stop-daemon.  The count at the end includes a floor.

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BB="${ARCA_TEST_BUSYBOX:-$HOME/busybox-new}"
if [ ! -x "$BB" ]; then
    echo "SKIP  no busybox at $BB (set ARCA_TEST_BUSYBOX)"; exit 0
fi

T="$(mktemp -d)"
cleanup() {
    for p in /proc/[0-9]*; do
        case "$(tr '\0' ' ' < "$p/cmdline" 2>/dev/null)" in
            *"$T"*) kill -9 "${p#/proc/}" 2>/dev/null ;;
        esac
    done
    rm -rf "$T"
}
trap cleanup EXIT

RED='\033[0;31m'; GREEN='\033[0;32m'; NC='\033[0m'
pass=0; fail=0
ok()  { pass=$((pass+1)); echo -e "  ${GREEN}PASS${NC}  $*"; }
bad() { fail=$((fail+1)); echo -e "  ${RED}FAIL${NC}  $*"; }
assert_eq() { if [ "$1" = "$2" ]; then ok "$3"; else bad "$3 (want '$2', got '$1')"; fi; }

mkdir -p "$T/bin" "$T/opt/arca" "$T/var/run" "$T/var/log"
ln -s "$BB" "$T/bin/pidof"; ln -s "$BB" "$T/bin/killall"
cat > "$T/bin/start-stop-daemon" <<'SSD'
#!/bin/sh
# shim: --start --background --make-pidfile --pidfile P --exec X
while [ $# -gt 0 ]; do
    case "$1" in --pidfile) P=$2; shift ;; --exec) X=$2; shift ;; esac
    shift
done
"$X" >/dev/null 2>&1 &
echo $! > "$P"
SSD
chmod +x "$T/bin/start-stop-daemon"
printf '#!/bin/sh\nexec sleep 300\n' > "$T/opt/arca/app_launcher"; chmod +x "$T/opt/arca/app_launcher"
echo "$T/opt/arca/app_launcher" > "$T/opt/arca/default-app"
sed -e "s#/opt/#$T/opt/#g" -e "s#/var/#$T/var/#g" \
    -e "s#^PATH=#PATH=$T/bin:#" "${ARCA_APP_SRC:-$REPO_DIR/device-files/arca-app}" > "$T/arca-app"
chmod +x "$T/arca-app"

loops() {
    local n=0 p
    for p in /proc/[0-9]*; do
        case " $(tr '\0' ' ' < "$p/cmdline" 2>/dev/null)" in
            *" $T/opt/arca/respawn.sh "*) n=$((n+1)) ;;
        esac
    done
    echo "$n"
}
apps() {
    local n=0 p
    for p in /proc/[0-9]*; do
        case "$(tr '\0' ' ' < "$p/cmdline" 2>/dev/null)" in
            "sleep 300 ") n=$((n+1)) ;;
        esac
    done
    echo "$n"
}

timeout 60 "$T/arca-app" start >"$T/out1" 2>&1; sleep 1
assert_eq "$(loops)" 1 "A start leaves one wrapper"
timeout 60 "$T/arca-app" start >"$T/out2" 2>&1; sleep 1
assert_eq "$(loops)" 1 "B a second start leaves one wrapper"
timeout 60 "$T/arca-app" stop >"$T/out3" 2>&1
assert_eq "$(loops)" 0 "C stop leaves no wrapper"
sleep 4
assert_eq "$(loops)/$(apps)" "0/0" "C2 nothing restarted the app afterwards"

timeout 60 "$T/arca-app" start >"$T/out4" 2>&1; sleep 1
rm -f "$T/var/run/arca-app.pid"
timeout 60 "$T/arca-app" stop >"$T/out5" 2>&1
assert_eq "$(loops)" 0 "D stop reaches a wrapper the pidfile does not name"
sleep 3
assert_eq "$(apps)" 0 "D2 and its app is gone"

echo "passed $pass, failed $fail"
[ $((pass+fail)) -ge 6 ] || { echo "FLOOR: fewer than 6 cases ran"; exit 1; }
[ "$fail" -eq 0 ]
