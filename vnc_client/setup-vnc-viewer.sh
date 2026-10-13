#!/usr/bin/env bash
# setup-vnc-viewer.sh — Set up a Raspberry Pi as a VNC viewer client (Remmina)
#
# Configures Remmina to auto-connect to an x11vnc server on boot with:
#   - Fullscreen display with automatic scaling
#   - Standard VncAuth (no security prompts)
#   - Desktop shortcut for manual launch
#   - Optional nightly reboot cron job
#
# Supports two execution modes:
#   Local:  Run directly on the RPi that will be the viewer
#   Remote: Run from a workstation, executes via SSH on the target RPi
#
# Prerequisites for remote execution:
#   - SSH key-based auth to the target RPi (see README.md for setup)
#
# Usage:
#   # Local (on the Pi itself):
#   ./setup-vnc-viewer.sh install
#
#   # Remote (from workstation):
#   ./setup-vnc-viewer.sh --remote pi@192.168.50.121 install
#
#   # Custom server:
#   ./setup-vnc-viewer.sh --remote pi@192.168.50.121 --server 192.168.50.56 --port 5900 install

set -euo pipefail

# ── Defaults ────────────────────────────────────────────────────────────────
VNC_SERVER="192.168.50.56"
VNC_PORT="5900"
VNC_PASSWORD="changeme"
REMOTE_HOST=""
NO_REBOOT_CRON=false
ACTION="install"

# ── Colors ──────────────────────────────────────────────────────────────────
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m' # No Color

info()  { echo -e "${GREEN}[INFO]${NC}  $*"; }
warn()  { echo -e "${YELLOW}[WARN]${NC}  $*"; }
error() { echo -e "${RED}[ERROR]${NC} $*" >&2; }
step()  { echo -e "${CYAN}[STEP]${NC}  $*"; }

# ── Usage ───────────────────────────────────────────────────────────────────
usage() {
    cat <<'EOF'
Usage: ./setup-vnc-viewer.sh [OPTIONS] [ACTION]

Actions:
  install          Full setup: install Remmina, configure viewer, autostart,
                   desktop shortcut (default)
  verify           Check the current viewer setup status
  uninstall        Remove Remmina config, autostart, desktop shortcut

Options:
  --remote HOST    Run on a remote Pi via SSH (e.g., --remote pi@192.168.50.121)
  --server IP      VNC server IP (default: 192.168.50.56)
  --port PORT      VNC server port (default: 5900)
  --password PW    VNC password (prompted if not given, NEVER saved to disk in plaintext)
  --no-reboot-cron Skip nightly reboot cron job setup
  --help           Show this usage information
EOF
    exit 0
}

# ── Argument Parsing ────────────────────────────────────────────────────────
while [[ $# -gt 0 ]]; do
    case "$1" in
        --remote)
            REMOTE_HOST="$2"; shift 2 ;;
        --server)
            VNC_SERVER="$2"; shift 2 ;;
        --port)
            VNC_PORT="$2"; shift 2 ;;
        --password)
            VNC_PASSWORD="$2"; shift 2 ;;
        --no-reboot-cron)
            NO_REBOOT_CRON=true; shift ;;
        --help|-h)
            usage ;;
        install|verify|uninstall)
            ACTION="$1"; shift ;;
        *)
            error "Unknown option: $1"
            usage ;;
    esac
done

# ── Remote Execution ────────────────────────────────────────────────────────
if [[ -n "${REMOTE_HOST}" ]]; then
    info "Remote mode: deploying to ${REMOTE_HOST}..."
    scp "$0" "${REMOTE_HOST}:/tmp/setup-vnc-viewer.sh"

    # Build argument list for remote execution (without --remote)
    REMOTE_ARGS="--server ${VNC_SERVER} --port ${VNC_PORT}"
    if [[ -n "${VNC_PASSWORD}" ]]; then
        REMOTE_ARGS="${REMOTE_ARGS} --password ${VNC_PASSWORD}"
    fi
    if [[ "${NO_REBOOT_CRON}" == true ]]; then
        REMOTE_ARGS="${REMOTE_ARGS} --no-reboot-cron"
    fi

    ssh -t "${REMOTE_HOST}" "bash /tmp/setup-vnc-viewer.sh ${REMOTE_ARGS} ${ACTION}"
    exit $?
fi

# ── Environment Checks ─────────────────────────────────────────────────────
check_platform() {
    if [[ -f /proc/device-tree/model ]]; then
        local model
        model=$(cat /proc/device-tree/model 2>/dev/null || true)
        info "Running on: ${model}"
    else
        warn "Not running on a Raspberry Pi (no /proc/device-tree/model)."
        warn "This script is designed for Raspberry Pi OS — proceed with caution."
    fi
}

check_display() {
    if [[ -z "${DISPLAY:-}" ]] && [[ -z "${WAYLAND_DISPLAY:-}" ]]; then
        warn "No DISPLAY or WAYLAND_DISPLAY set. Desktop environment may not be active."
        warn "Autostart and desktop shortcuts require a graphical session."
    fi
}

# ── Remmina Profile Paths ──────────────────────────────────────────────────
REMMINA_DIR="${HOME}/.local/share/remmina"
REMMINA_PROFILE="${REMMINA_DIR}/arca.remmina"
AUTOSTART_DIR="${HOME}/.config/autostart"
AUTOSTART_ENTRY="${AUTOSTART_DIR}/vnc-viewer.desktop"
DESKTOP_SHORTCUT="${HOME}/Desktop/vnc-viewer.desktop"
CRON_FILE="/etc/cron.d/nightly-reboot"

# ── Install Action ──────────────────────────────────────────────────────────
do_install() {
    info "=== VNC Viewer Setup (Remmina) ==="
    echo ""
    check_platform
    check_display
    echo ""

    # Step 1: Remove RealVNC Viewer if installed
    step "1/7 Removing RealVNC Viewer (if installed)..."
    sudo apt-get remove -y realvnc-vnc-viewer 2>/dev/null || true

    # Step 2: Install Remmina
    step "2/7 Installing Remmina + VNC plugin..."
    sudo apt-get update -qq
    sudo apt-get install -y remmina remmina-plugin-vnc

    # Step 3: Prompt for VNC password if not provided
    if [[ -z "${VNC_PASSWORD}" ]]; then
        step "3/7 VNC password configuration..."
        echo ""
        read -sp "Enter VNC password for ${VNC_SERVER}:${VNC_PORT}: " VNC_PASSWORD
        echo ""
        if [[ -z "${VNC_PASSWORD}" ]]; then
            error "Password cannot be empty."
            exit 1
        fi
    else
        step "3/7 VNC password provided via argument."
    fi

    # Encrypt password using Remmina's base64 fallback (prefix with '.')
    local ENCRYPTED_PW
    ENCRYPTED_PW=".$(echo -n "${VNC_PASSWORD}" | base64)"

    # Step 4: Create Remmina connection profile
    step "4/7 Creating Remmina profile at ${REMMINA_PROFILE}..."
    mkdir -p "${REMMINA_DIR}"
    cat > "${REMMINA_PROFILE}" <<REMMINA_EOF
[remmina]
name=Arca VNC
protocol=VNC
server=${VNC_SERVER}:${VNC_PORT}
password=${ENCRYPTED_PW}
colordepth=32
quality=2
viewmode=4
scale=1
window_maximize=1
enable-autostart=1
REMMINA_EOF
    info "  Profile created."

    # Step 5: Create autostart desktop entry
    step "5/7 Creating autostart entry at ${AUTOSTART_ENTRY}..."
    mkdir -p "${AUTOSTART_DIR}"
    cat > "${AUTOSTART_ENTRY}" <<'AUTOSTART_EOF'
[Desktop Entry]
Type=Application
Name=VNC to Arca
Comment=Connect to Arca RPi via VNC (Remmina, fullscreen, scaled)
Icon=org.remmina.Remmina
Exec=/bin/bash -c 'sleep 5 && remmina -c ~/.local/share/remmina/arca.remmina'
Terminal=false
Categories=Network;RemoteAccess;
X-GNOME-Autostart-enabled=true
Hidden=false
AUTOSTART_EOF
    info "  Autostart entry created."

    # Step 6: Create desktop shortcut
    step "6/7 Creating desktop shortcut at ${DESKTOP_SHORTCUT}..."
    mkdir -p "${HOME}/Desktop"
    cat > "${DESKTOP_SHORTCUT}" <<'DESKTOP_EOF'
[Desktop Entry]
Type=Application
Name=VNC to Arca
Comment=Connect to Arca RPi via VNC (Remmina, fullscreen, scaled)
Icon=org.remmina.Remmina
Exec=/bin/bash -c 'sleep 5 && remmina -c ~/.local/share/remmina/arca.remmina'
Terminal=false
Categories=Network;RemoteAccess;
DESKTOP_EOF
    chmod +x "${DESKTOP_SHORTCUT}"
    # Mark as trusted (LXDE/PCManFM on Raspberry Pi OS)
    if command -v gio &>/dev/null; then
        gio set "${DESKTOP_SHORTCUT}" metadata::trusted true 2>/dev/null || true
    fi
    info "  Desktop shortcut created."

    # Step 7: Nightly reboot cron job
    if [[ "${NO_REBOOT_CRON}" == true ]]; then
        step "7/7 Skipping nightly reboot cron job (--no-reboot-cron)."
    else
        step "7/7 Setting up nightly reboot at 3:00 AM..."
        sudo bash -c "cat > ${CRON_FILE}" <<CRON_EOF
# Nightly reboot for VNC viewer reliability
# Installed by setup-vnc-viewer.sh
0 3 * * * root /sbin/reboot
CRON_EOF
        info "  Cron job created at ${CRON_FILE}."
    fi

    # Summary
    echo ""
    info "=== Setup Complete ==="
    echo ""
    echo "  VNC Server:     ${VNC_SERVER}:${VNC_PORT}"
    echo "  Remmina Profile: ${REMMINA_PROFILE}"
    echo "  Autostart:       ${AUTOSTART_ENTRY}"
    echo "  Desktop Shortcut: ${DESKTOP_SHORTCUT}"
    if [[ "${NO_REBOOT_CRON}" == false ]]; then
        echo "  Nightly Reboot:  ${CRON_FILE} (3:00 AM)"
    else
        echo "  Nightly Reboot:  skipped"
    fi
    echo ""
    info "Reboot or log out/in for autostart to take effect."
    info "Or launch manually: remmina -c ${REMMINA_PROFILE}"
}

# ── Verify Action ───────────────────────────────────────────────────────────
do_verify() {
    info "=== Verifying VNC Viewer Setup ==="
    echo ""
    local all_ok=true

    # Check Remmina installed
    if command -v remmina &>/dev/null; then
        echo -e "  ${GREEN}✓${NC} Remmina installed: $(which remmina)"
    else
        echo -e "  ${RED}✗${NC} Remmina NOT installed"
        all_ok=false
    fi

    # Check RealVNC Viewer absent
    if dpkg -l 2>/dev/null | grep -q realvnc-vnc-viewer; then
        echo -e "  ${YELLOW}!${NC} RealVNC Viewer is still installed (should be removed)"
        all_ok=false
    else
        echo -e "  ${GREEN}✓${NC} RealVNC Viewer not installed"
    fi

    # Check profile exists
    if [[ -f "${REMMINA_PROFILE}" ]]; then
        echo -e "  ${GREEN}✓${NC} Remmina profile: ${REMMINA_PROFILE}"
    else
        echo -e "  ${RED}✗${NC} Remmina profile MISSING: ${REMMINA_PROFILE}"
        all_ok=false
    fi

    # Check autostart exists
    if [[ -f "${AUTOSTART_ENTRY}" ]]; then
        echo -e "  ${GREEN}✓${NC} Autostart entry: ${AUTOSTART_ENTRY}"
    else
        echo -e "  ${RED}✗${NC} Autostart entry MISSING: ${AUTOSTART_ENTRY}"
        all_ok=false
    fi

    # Check desktop shortcut exists
    if [[ -f "${DESKTOP_SHORTCUT}" ]]; then
        echo -e "  ${GREEN}✓${NC} Desktop shortcut: ${DESKTOP_SHORTCUT}"
    else
        echo -e "  ${RED}✗${NC} Desktop shortcut MISSING: ${DESKTOP_SHORTCUT}"
        all_ok=false
    fi

    # Check cron job exists
    if [[ -f "${CRON_FILE}" ]]; then
        echo -e "  ${GREEN}✓${NC} Nightly reboot cron: ${CRON_FILE}"
    else
        echo -e "  ${YELLOW}!${NC} Nightly reboot cron not found: ${CRON_FILE}"
    fi

    echo ""
    if [[ "${all_ok}" == true ]]; then
        info "All checks passed."
    else
        warn "Some checks failed — run 'install' to fix."
    fi
}

# ── Uninstall Action ────────────────────────────────────────────────────────
do_uninstall() {
    info "=== Uninstalling VNC Viewer Setup ==="
    echo ""

    # Remove Remmina profile
    if [[ -f "${REMMINA_PROFILE}" ]]; then
        rm -f "${REMMINA_PROFILE}"
        info "Removed Remmina profile: ${REMMINA_PROFILE}"
    else
        info "Remmina profile not found (already removed)."
    fi

    # Remove autostart entry
    if [[ -f "${AUTOSTART_ENTRY}" ]]; then
        rm -f "${AUTOSTART_ENTRY}"
        info "Removed autostart entry: ${AUTOSTART_ENTRY}"
    else
        info "Autostart entry not found (already removed)."
    fi

    # Remove desktop shortcut
    if [[ -f "${DESKTOP_SHORTCUT}" ]]; then
        rm -f "${DESKTOP_SHORTCUT}"
        info "Removed desktop shortcut: ${DESKTOP_SHORTCUT}"
    else
        info "Desktop shortcut not found (already removed)."
    fi

    # Remove cron job
    if [[ -f "${CRON_FILE}" ]]; then
        sudo rm -f "${CRON_FILE}"
        info "Removed nightly reboot cron: ${CRON_FILE}"
    else
        info "Nightly reboot cron not found (already removed)."
    fi

    # Optionally remove Remmina itself
    echo ""
    read -p "Also remove Remmina package? [y/N]: " REMOVE_REMMINA
    if [[ "${REMOVE_REMMINA}" =~ ^[Yy]$ ]]; then
        sudo apt-get remove -y remmina remmina-plugin-vnc
        info "Remmina removed."
    else
        info "Keeping Remmina installed."
    fi

    echo ""
    info "Uninstall complete."
}

# ── Main ────────────────────────────────────────────────────────────────────
case "${ACTION}" in
    install)   do_install   ;;
    verify)    do_verify    ;;
    uninstall) do_uninstall ;;
    *)
        error "Unknown action: ${ACTION}"
        usage
        ;;
esac
