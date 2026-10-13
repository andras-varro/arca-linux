#!/bin/bash
# Build and deploy the VNC client to a Arca device.
#
# Usage:
#   ./build-and-deploy.sh                        # build only
#   ./build-and-deploy.sh <ip>                   # build + deploy
#   ./build-and-deploy.sh <ip> run               # build + deploy + run
#   ./build-and-deploy.sh <ip> set-default       # build + deploy + set as default boot app
#   ./build-and-deploy.sh --bundle <dir>         # build + stage into an offline bundle
#
# System setup (bloatware cleanup, init service, audio, time-sync) is handled
# separately by commissioning/provision.sh.  Run that once before deploying for the first time.
#
# --bundle stages this component's artifacts under <dir>/root/<device-path> with
# a declared-mode manifest — the layout ../release.sh publishes and
# ../commissioning/commission-offline.sh installs from.  It stages the
# binary, the icon and the .app manifest and DELIBERATELY NOT vnc_client.conf:
# that file holds a plaintext VNC password, and a release must publish binaries
# only, never device config.

# Source-path directive: resolves the source= hints below against this script's directory.
# shellcheck source-path=SCRIPTDIR
set -e
_START_SECONDS=$(date +%s)

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# The two shared libraries every path needs. The SSH gate (../lib/arca-ssh.sh) because
# every deploy goes through it; ../lib/arca-bundle.sh because --bundle stages through it
# AND a deploy clears the provenance stamp through it.
# shellcheck source=../lib/arca-ssh.sh
. "$REPO_ROOT/lib/arca-ssh.sh"
# shellcheck source=../lib/arca-bundle.sh
. "$REPO_ROOT/lib/arca-bundle.sh"

BUNDLE_DIR=""
if [[ "${1:-}" == "--bundle" ]]; then
    BUNDLE_DIR="${2:-}"
    DEVICE_IP=""
    MODE=""
    if [[ -z "$BUNDLE_DIR" ]]; then
        echo "--bundle requires a directory"
        echo ""
        echo "Usage: $0 --bundle <dir>"
        exit 1
    fi
    if [[ -n "${3:-}" ]]; then
        echo "Unexpected argument after --bundle <dir>: $3"
        exit 1
    fi
else
    DEVICE_IP="${1:-}"
    MODE="${2:-}"
fi
DEVICE="root@${DEVICE_IP}"
REMOTE_DIR="/opt/vnc_client"

# ── colour helpers ──────────────────────────────────────────────────────────
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; BLUE='\033[0;34m'; NC='\033[0m'
ok()   { echo -e "[$(date '+%H:%M:%S')] ${GREEN}  ✓ $*${NC}"; }
info() { echo -e "[$(date '+%H:%M:%S')] ${YELLOW}  → $*${NC}"; }
warn() { echo -e "[$(date '+%H:%M:%S')] ${BLUE}  ! $*${NC}"; }
err()  { echo -e "[$(date '+%H:%M:%S')] ${RED}  ✗ $*${NC}"; exit 1; }

# ── argument validation ─────────────────────────────────────────────────────
# Validate before building, not at the first ssh — a bad argument otherwise surfaces
# only after every target has been compiled.
usage() {
    echo "Usage: $0 [<ip>] [run|set-default]"
    echo "       $0 --bundle <dir>"
    echo ""
    echo "  <ip>            Device IPv4 address; omit to build without deploying"
    echo "  run             Also start vnc_client on the device"
    echo "  set-default     Also make vnc_client the boot app"
    echo "  --bundle <dir>  Build, then stage the artifacts under <dir>/root/ with"
    echo "                  a declared-mode manifest. No device needed. The config"
    echo "                  file is NOT staged — it holds a plaintext password."
    exit 1
}

IPV4_RE='^(25[0-5]|2[0-4][0-9]|1[0-9][0-9]|[1-9]?[0-9])(\.(25[0-5]|2[0-4][0-9]|1[0-9][0-9]|[1-9]?[0-9])){3}$'
if [[ -n "$DEVICE_IP" ]] && [[ ! "$DEVICE_IP" =~ $IPV4_RE ]]; then
    echo "Not an IPv4 address: $DEVICE_IP"; echo ""; usage
fi

case "$MODE" in
    ""|run|set-default) ;;
    *) echo "Unknown mode: $MODE"; echo ""; usage ;;
esac

# ── 1. cross-compiler check ─────────────────────────────────────────────────
echo ""
echo "════════════════════════════════════════"
echo " VNC Client Build + Deploy"
echo "════════════════════════════════════════"
info "Started — $(date '+%Y-%m-%d %H:%M:%S')"

CC=arm-linux-gnueabihf-gcc
if ! command -v $CC &>/dev/null; then
    err "ARM cross-compiler not found. Install every host prerequisite with\n  setup-build-env.sh, at the repo root — it carries the one package set."
fi
info "Compiler: $($CC --version | head -1)"
echo ""

# ── 2. check dependencies ───────────────────────────────────────────────────
if [ ! -f deps/lib/libvncclient.a ] || [ ! -f deps/lib/libz.a ] || [ ! -f deps/lib/libjpeg.a ]; then
    info "Dependencies not found, building..."
    chmod +x build-deps.sh
    ./build-deps.sh
    ok "Dependencies built"
else
    ok "Dependencies present"
fi

# ── 3. build ─────────────────────────────────────────────────────────────────
info "Cleaning previous build..."
make clean 2>/dev/null || true

info "Building VNC client..."
make 2>&1 | tail -5
echo ""
ok "Build complete: $(ls -lh vnc_client_stripped | awk '{print $5}')"
echo ""

# ── 3b. ARM-safety gate ─────────────────────────────────────────────────────
# Cortex-A8 has no hardware integer divide; an sdiv/udiv means SIGILL (exit 132)
# with a blank screen and no output.  Checks the UNSTRIPPED binary on purpose:
# without symbols objdump disassembles literal pools as code and reports
# phantom hits (vnc_client_stripped yields a bogus "sdiv r4, sp, pc").
if [[ -x ../native_apps/check-arm-safe.sh ]]; then
    ../native_apps/check-arm-safe.sh vnc_client \
        || err "ARM-safety check failed — refusing to deploy"
else
    warn "../native_apps/check-arm-safe.sh missing — skipping hardware-divide gate"
fi
echo ""

# ── 3c. the .app manifest, written once ─────────────────────────────────────
# One heredoc, into a file, used by BOTH the deploy path and --bundle.  It used
# to live inside `ssh "$DEVICE" bash <<REMOTE`, so it existed only when a device
# was reachable; ../commissioning/commission-offline.sh needs the same bytes with
# no device.
STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT INT TERM
cat > "$STAGE/vnc_client.app" <<'APP'
name=VNC Client
exec=/opt/vnc_client/vnc_client
icon=/opt/arca/icons/vnc_client.ppm
args=none
APP

# ── 3d. --bundle: stage instead of deploying ────────────────────────────────
# After the ARM-safety gate, deliberately: a published bundle is installed by
# someone with no toolchain, so it must never carry an sdiv/udiv.
if [[ -n "$BUNDLE_DIR" ]]; then
    echo "════════════════════════════════════════"
    echo " Staging bundle → $BUNDLE_DIR"
    echo "════════════════════════════════════════"

    arca_bundle_init "$BUNDLE_DIR" vnc_client || err "could not prepare $BUNDLE_DIR"

    # The STRIPPED binary is what gets deployed and therefore what gets bundled.
    # (The ARM gate above ran against the unstripped one on purpose — see its
    # comment; the two are the same code.)
    arca_bundle_add "$BUNDLE_DIR" vnc_client 0755 vnc_client_stripped "$REMOTE_DIR/vnc_client" \
        || err "staging failed: vnc_client"
    arca_bundle_add "$BUNDLE_DIR" vnc_client 0644 "$STAGE/vnc_client.app" \
        /opt/arca/apps/vnc_client.app || err "staging failed: vnc_client.app"
    if [ -f vnc_client.ppm ]; then
        arca_bundle_add "$BUNDLE_DIR" vnc_client 0644 vnc_client.ppm \
            /opt/arca/icons/vnc_client.ppm || err "staging failed: icon"
    else
        warn "vnc_client.ppm not found — the launcher tile will have no icon"
    fi

    # vnc_client.conf is NOT staged.  It carries a plaintext VNC password, and a
    # release publishes binaries only.  The installer
    # tells the operator to create it; the app's own defaults do not connect
    # anywhere without one.
    warn "vnc_client.conf deliberately NOT bundled (plaintext password)"

    ok "Staged $(arca_bundle_finish "$BUNDLE_DIR" vnc_client) file(s)"
    echo ""
    exit 0
fi

# ── 4. deploy? ───────────────────────────────────────────────────────────────
if [[ -z "$DEVICE_IP" ]]; then
    echo "No IP supplied — build only. To deploy:"
    echo "  ./build-and-deploy.sh <ip>"
    echo "  ./build-and-deploy.sh <ip> run"
    echo "  ./build-and-deploy.sh <ip> run --kill"
    echo "  ./build-and-deploy.sh <ip> set-default"
    exit 0
fi

echo "════════════════════════════════════════"
echo " Deploying to $DEVICE_IP"
echo "════════════════════════════════════════"

# Check SSH reachable
info "Testing SSH connection..."
# The shared gate (../lib/arca-ssh.sh): "down" and "up but refusing our key" are
# different answers, and only the second has a remedy worth offering.
arca_ssh_gate "$DEVICE" || err "Cannot continue without SSH to $DEVICE"
ok "SSH OK"

# Locally built binaries, so any provenance stamp on the unit is about to stop being
# true. One shared writer (../lib/arca-bundle.sh), before the first file goes out.
arca_bundle_clear_stamp "$DEVICE" \
    || warn "could not clear /opt/arca/bundle.info — it may still name an older release"

# Stop whatever is running (avoids "Text file busy").
#
# One stop implementation, on the device, matching on the executable — it catches
# a vnc_client the launcher started, which the `killall vnc_client` that used to
# be here could not reliably do.  Do not re-add
# a killall here.
info "Stopping running apps (device init script)..."
ssh "$DEVICE" 'if [ -x /etc/init.d/arca-app ]; then
    /etc/init.d/arca-app stop
else
    echo "  /etc/init.d/arca-app not installed - run ../commissioning/provision.sh <ip>"
fi' || warn "stop reported a failure - a surviving process may hold the binary"
ok "Stopped"

# Ensure target directory exists
info "Creating $REMOTE_DIR on device..."
ssh "$DEVICE" "mkdir -p $REMOTE_DIR"

# Upload binary
info "Uploading vnc_client..."
scp vnc_client_stripped "$DEVICE:$REMOTE_DIR/vnc_client"
ssh "$DEVICE" "chmod +x $REMOTE_DIR/vnc_client"
ok "Binary uploaded ($(ls -lh vnc_client_stripped | awk '{print $5}'))"

# Upload config file (only if not already present on device, to preserve edits).
# vnc_client.conf is gitignored (it holds a plaintext password); fall back to
# the tracked template so a fresh clone can still deploy.
CONF_SRC="vnc_client.conf"
if [ ! -f "$CONF_SRC" ]; then
    CONF_SRC="vnc_client.conf.example"
    warn "vnc_client.conf not found locally - using $CONF_SRC (password is a placeholder)"
    warn "Create it with: cp vnc_client.conf.example vnc_client.conf  # then edit"
fi
if ssh "$DEVICE" "[ ! -f $REMOTE_DIR/vnc_client.conf ]" 2>/dev/null; then
    info "Uploading default config file..."
    scp "$CONF_SRC" "$DEVICE:$REMOTE_DIR/vnc_client.conf"
    ssh "$DEVICE" "chmod 600 $REMOTE_DIR/vnc_client.conf"
    ok "Config file deployed (mode 0600)"
else
    ok "Config file already present (preserved)"
    warn "To force-overwrite: scp $CONF_SRC $DEVICE:$REMOTE_DIR/vnc_client.conf"
fi

# Install app manifest (for app launcher).  The manifest was written to $STAGE
# above, from the one heredoc this script has; --bundle copies the same file.
info "Installing app manifest and icon..."
ssh "$DEVICE" "mkdir -p /opt/arca/apps /opt/arca/icons"
if [ -f vnc_client.ppm ]; then
    scp vnc_client.ppm "$DEVICE:/opt/arca/icons/vnc_client.ppm"
fi
scp "$STAGE/vnc_client.app" "$DEVICE:/opt/arca/apps/vnc_client.app"
ssh "$DEVICE" "chmod 644 /opt/arca/apps/vnc_client.app"
ok "App manifest installed"

echo ""

# ── 5. run / set-default / restart? ───────────────────────────────────────────
if [[ "$MODE" == "run" ]]; then
    echo "════════════════════════════════════════"
    echo " Starting VNC Client"
    echo "════════════════════════════════════════"

    info "Launching on device (Ctrl+C to stop)..."
    echo ""
    ssh -t "$DEVICE" "$REMOTE_DIR/vnc_client 2>&1"
elif [[ "$MODE" == "set-default" ]]; then
    info "Setting VNC client as default app..."
    ssh "$DEVICE" "mkdir -p /opt/arca && echo '$REMOTE_DIR/vnc_client' > /opt/arca/default-app"
    ok "Default app → $REMOTE_DIR/vnc_client"
    # Restart init service
    if ssh "$DEVICE" '[ -f /etc/init.d/arca-app ]' 2>/dev/null; then
        info "Restarting launcher..."
        ssh "$DEVICE" '/etc/init.d/arca-app start' 2>&1 | grep -v '^$'
        ok "Launcher running"
    fi
else
    # Restart init service (launcher will pick up updated manifest)
    if ssh "$DEVICE" '[ -f /etc/init.d/arca-app ]' 2>/dev/null; then
        info "Restarting launcher..."
        ssh "$DEVICE" '/etc/init.d/arca-app start' 2>&1 | grep -v '^$'
        ok "Launcher running"
    else
        echo "  Deployed. To run interactively:"
        echo "    ssh $DEVICE '$REMOTE_DIR/vnc_client'"
    fi
fi

_END_SECONDS=$(date +%s)
_ELAPSED=$((_END_SECONDS - _START_SECONDS))
printf "[$(date '+%H:%M:%S')] Total time: %dm%02ds\n" $((_ELAPSED / 60)) $((_ELAPSED % 60))
echo ""
