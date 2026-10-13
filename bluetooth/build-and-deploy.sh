#!/bin/bash
# build-and-deploy.sh — Bluetooth for the Arca: kernel modules, RTL8761CU firmware,
# bluetoothd + bluetoothctl, and the boot start.
#
# Usage:
#   ./build-and-deploy.sh                    # build the artifacts only
#   ./build-and-deploy.sh <ip>               # build + deploy + start it now (no reboot)
#   ./build-and-deploy.sh --bundle <dir>     # build + stage into an offline bundle
#
# Prerequisites — the host ones come from ../setup-build-env.sh:
#   - arm-linux-gnueabi-gcc, dpkg-deb, wget, pkg-config   (BlueZ, build-bluez.sh)
#   - autoreconf, libtoolize, gdbus-codegen               (BlueALSA, build-bluealsa.sh)
#   - arm-linux-gnueabihf-gcc and ~/rw-kbuild-image from ../kernel/build-image.sh
#                                                         (modules, build-bt-modules.sh)
#   - SSH key auth to root@<ip>                           (deploy only)
#
# ── What goes where ─────────────────────────────────────────────────────────
#
#   built artifacts (this script, and the bundle):
#     BlueZ 5.66 bluetoothd, bluetoothctl, btmon, libbluetooth ← build-bluez.sh, staging/
#     BlueALSA 4.3.1 bluealsa, bluealsa-aplay, 2 plugins  ← build-bluealsa.sh, staging-bluealsa/
#     the Bluetooth module closure + load-order.txt       ← ../kernel/build-bt-modules.sh,
#                                                           modules/, to /lib/modules/4.14.52/bt
#     rtl8761cu_{fw,config}.bin + their licence           ← ../kernel/3rdparty/realtek/bluetooth
#   verbatim device files (device-files/provision-rules.conf, group `bluetooth`):
#     /etc/init.d/bluetooth (bluetoothd, then bluealsa), its S91 rc5.d link, the dbus
#     policies bluetooth.conf and bluealsa.conf, 20-bluealsa.conf,
#     /etc/bluetooth/main.conf (AutoEnable: the dongle re-enumerates with the USB tree)
#     and /etc/bluetooth/input.conf (ClassicBondedOnly: HID only from bonded devices)
#
# The device files are installed by this script AND by commissioning/provision.sh /
# commission-offline.sh, from the same records — the way usb_host handles the `usb`
# group. The .conf files cannot travel in the bundle: release.sh refuses any *.conf.
#
# Pairing is manual (bluetoothctl); records live in /var/lib/bluetooth.

# shellcheck source-path=SCRIPTDIR
set -e
_START_SECONDS=$(date +%s)

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
KERNEL_VERSION="4.14.52"
MODULES_DIR="$SCRIPT_DIR/modules"
STAGING="$SCRIPT_DIR/staging"
FW_DIR="$REPO_ROOT/kernel/3rdparty/realtek/bluetooth"

# shellcheck source=../lib/arca-ssh.sh
. "$REPO_ROOT/lib/arca-ssh.sh"
# shellcheck source=../lib/arca-bundle.sh
. "$REPO_ROOT/lib/arca-bundle.sh"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; BLUE='\033[0;34m'; NC='\033[0m'
ok()   { echo -e "[$(date '+%H:%M:%S')] ${GREEN}  ✓ $*${NC}"; }
info() { echo -e "[$(date '+%H:%M:%S')] ${YELLOW}  → $*${NC}"; }
warn() { echo -e "[$(date '+%H:%M:%S')] ${BLUE}  ! $*${NC}"; }
err()  { echo -e "[$(date '+%H:%M:%S')] ${RED}  ✗ $*${NC}" >&2; exit 1; }
ts()   { echo "[$(date '+%H:%M:%S')] $*"; }

usage() {
    echo "Usage: $0 [<ip>]"
    echo "       $0 --bundle <dir>"
    echo ""
    echo "  <ip>             Device IPv4 address; omit to build without deploying."
    echo "                   Deploying restarts /etc/init.d/bluetooth; no reboot."
    echo "  --bundle <dir>   Build, then stage the artifacts under <dir>/root/ with"
    echo "                   a declared-mode manifest. No device needed."
    echo ""
    echo "  The init script, its rc5.d link, the dbus policies, main.conf and input.conf are"
    echo "  device-files/provision-rules.conf's \`bluetooth\` group: this script runs"
    echo "  that plan, and ./commissioning/provision.sh <ip> and commission-offline.sh"
    echo "  install it too, from the same records."
    exit 1
}

# ── arguments ───────────────────────────────────────────────────────────────
BUNDLE_DIR=""
DEVICE_IP=""
if [[ "${1:-}" == "--bundle" ]]; then
    BUNDLE_DIR="${2:-}"
    [[ -n "$BUNDLE_DIR" ]] || { echo "--bundle requires a directory"; echo ""; usage; }
    [[ -z "${3:-}" ]] || { echo "Unexpected argument after --bundle <dir>: $3"; exit 1; }
else
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --help|-h) usage ;;
            -*)        echo "Unknown option: $1"; echo ""; usage ;;
            *)
                [[ -z "$DEVICE_IP" ]] || { echo "Unexpected argument: $1"; echo ""; usage; }
                DEVICE_IP="$1"; shift ;;
        esac
    done
    IPV4_RE='^(25[0-5]|2[0-4][0-9]|1[0-9][0-9]|[1-9]?[0-9])(\.(25[0-5]|2[0-4][0-9]|1[0-9][0-9]|[1-9]?[0-9])){3}$'
    if [[ -n "$DEVICE_IP" && ! "$DEVICE_IP" =~ $IPV4_RE ]]; then
        echo "Not an IPv4 address: $DEVICE_IP"; echo ""; usage
    fi
fi
DEVICE="root@$DEVICE_IP"

echo ""
echo "════════════════════════════════════════"
echo " Arca Bluetooth"
echo "════════════════════════════════════════"
ts "Started — $(date '+%Y-%m-%d %H:%M:%S')"
[[ -n "$BUNDLE_DIR" ]] && info "Staging a bundle: $BUNDLE_DIR"
[[ -n "$DEVICE_IP" ]] && info "Target: $DEVICE"
echo ""

# ── 0. SSH, before any build ────────────────────────────────────────────────
ts "[0/6] Prerequisites"
if [[ -n "$DEVICE_IP" ]]; then
    arca_ssh_gate "$DEVICE" || err "Cannot continue without SSH to $DEVICE"
    ok "SSH to $DEVICE"
    arca_bundle_clear_stamp "$DEVICE" \
        || warn "could not clear /opt/arca/bundle.info — it may still name an older release"
fi
echo ""

# ── 1. BlueZ userspace ──────────────────────────────────────────────────────
# build-bluez.sh short-circuits on its own artifacts; --force when the recipe is newer.
ts "[1/6] BlueZ userspace"
if [[ -f "$STAGING/usr/libexec/bluetooth/bluetoothd" \
      && "$SCRIPT_DIR/build-bluez.sh" -nt "$STAGING/usr/libexec/bluetooth/bluetoothd" ]]; then
    info "build-bluez.sh is newer than the staged build — rebuilding"
    bash "$SCRIPT_DIR/build-bluez.sh" --force
else
    bash "$SCRIPT_DIR/build-bluez.sh"
fi
ok "staging/"
# BlueALSA: staging-bluealsa/ supplies the daemon and the plugins; its two .conf files
# are device-files/ copies in the `bluetooth` provision group (no *.conf in a bundle).
BA_STAGING="$SCRIPT_DIR/staging-bluealsa"
if [[ -f "$BA_STAGING/usr/bin/bluealsa" \
      && "$SCRIPT_DIR/build-bluealsa.sh" -nt "$BA_STAGING/usr/bin/bluealsa" ]]; then
    info "build-bluealsa.sh is newer than the staged build — rebuilding"
    bash "$SCRIPT_DIR/build-bluealsa.sh" --force
else
    bash "$SCRIPT_DIR/build-bluealsa.sh"
fi
for pair in "etc/dbus-1/system.d/bluealsa.conf|bluealsa.conf" \
            "etc/alsa/conf.d/20-bluealsa.conf|20-bluealsa.conf"; do
    cmp -s "$BA_STAGING/${pair%%|*}" "$REPO_ROOT/device-files/${pair#*|}" \
        || err "device-files/${pair#*|} differs from the build's ${pair%%|*} — copy it over"
done
ok "staging-bluealsa/ (its .conf files match device-files/)"
echo ""

# ── 2. kernel modules ───────────────────────────────────────────────────────
# load-order.txt is build-bt-modules.sh's output and the one list of modules: the
# artifact rows below and the init script's insmod order both come from it.
ts "[2/6] Kernel modules (kernel/build-bt-modules.sh)"
ORDER="$MODULES_DIR/load-order.txt"
need_mod_build=0
if [[ ! -f "$ORDER" ]]; then
    need_mod_build=1
else
    while read -r ko; do
        [[ -z "$ko" || -f "$MODULES_DIR/$ko" ]] || need_mod_build=1
    done < "$ORDER"
    if [[ -n "$(find "$REPO_ROOT/kernel/build-bt-modules.sh" "$REPO_ROOT/kernel/patches-modules" \
                -newer "$ORDER" -print -quit 2>/dev/null)" ]]; then
        info "the module recipe or a module patch is newer than modules/ — rebuilding"
        need_mod_build=1
    fi
fi
if [[ $need_mod_build -eq 1 ]]; then
    [[ -f "$HOME/rw-kbuild-image/linux-4.14.52/vmlinux" ]] \
        || err "$HOME/rw-kbuild-image is not built, and the Bluetooth modules are built against it.
     Run kernel/build-image.sh first (in WSL), then re-run this."
    REUSE=()
    [[ -f "$HOME/rw-kbuild-bt/linux-4.14.52/Makefile" ]] && REUSE=(--reuse)
    bash "$REPO_ROOT/kernel/build-bt-modules.sh" --out "$MODULES_DIR" "${REUSE[@]}"
fi
mapfile -t BT_MODULES < <(grep -v '^$' "$ORDER")
[[ ${#BT_MODULES[@]} -gt 0 ]] || err "$ORDER lists no modules"
for ko in "${BT_MODULES[@]}"; do
    [[ -f "$MODULES_DIR/$ko" ]] || err "module $ko not in $MODULES_DIR"
done
ok "${#BT_MODULES[@]} modules"
echo ""

# ── the artifacts, declared ONCE ────────────────────────────────────────────
# "<mode>|<local path>|<device path>". The bundle and the scp deploy both read this.
# Modes are DECLARED, never read off disk: /mnt/c reports every file 0777.
# libbluetooth goes in under its soname, the name the loader asks for.
LIB_REAL="$(readlink -f "$STAGING/usr/lib/libbluetooth.so.3")"
BT_ARTIFACTS=(
    "0755|$STAGING/usr/libexec/bluetooth/bluetoothd|/usr/libexec/bluetooth/bluetoothd"
    "0755|$STAGING/usr/bin/bluetoothctl|/usr/bin/bluetoothctl"
    "0755|$STAGING/usr/bin/btmon|/usr/bin/btmon"
    "0755|$LIB_REAL|/usr/lib/libbluetooth.so.3"
)
ARM_TARGETS=("$STAGING/usr/libexec/bluetooth/bluetoothd" "$STAGING/usr/bin/bluetoothctl" "$STAGING/usr/bin/btmon" "$LIB_REAL")
for ko in "${BT_MODULES[@]}"; do
    BT_ARTIFACTS+=("0644|$MODULES_DIR/$ko|/lib/modules/$KERNEL_VERSION/bt/$ko")
    ARM_TARGETS+=("$MODULES_DIR/$ko")
done
BT_ARTIFACTS+=("0644|$ORDER|/lib/modules/$KERNEL_VERSION/bt/load-order.txt")
# BlueALSA: the daemon, bluealsa-aplay, and the two plugins in the vendor libasound's
# compiled-in plugin directory (our alsa-lib build is never deployed).
BA_PLUGINS="$BA_STAGING/usr/lib/alsa-lib"
for f in "$BA_STAGING/usr/bin/bluealsa" "$BA_STAGING/usr/bin/bluealsa-aplay"; do
    BT_ARTIFACTS+=("0755|$f|/usr/bin/${f##*/}")
    ARM_TARGETS+=("$f")
done
for f in "$BA_PLUGINS/libasound_module_pcm_bluealsa.so" "$BA_PLUGINS/libasound_module_ctl_bluealsa.so"; do
    BT_ARTIFACTS+=("0644|$f|/usr/lib/alsa-lib/${f##*/}")
    ARM_TARGETS+=("$f")
done
# The firmware licence allows binary redistribution only with its licence beside it.
for f in rtl8761cu_fw.bin rtl8761cu_config.bin LICENCE.rtlwifi_firmware.txt; do
    BT_ARTIFACTS+=("0644|$FW_DIR/$f|/lib/firmware/rtl_bt/$f")
done

# ── 3. the ARM-safety gate ──────────────────────────────────────────────────
# No hardware divide on Cortex-A8. Exit 2 ("could not judge": stripped) is a refusal,
# as in usb_host: nothing here is stripped of its symbol table by construction.
ts "[3/6] ARM-safety gate (no sdiv/udiv)"
arm_rc=0
bash "$REPO_ROOT/native_apps/check-arm-safe.sh" "${ARM_TARGETS[@]}" || arm_rc=$?
case "$arm_rc" in
    0) ok "hard zero across ${#ARM_TARGETS[@]} artifact(s)" ;;
    2) err "the gate could not judge an artifact (stripped?) — refusing to deploy or bundle" ;;
    *) err "an artifact would SIGILL on this device — refusing to deploy or bundle" ;;
esac
echo ""

# ── 4. --bundle: stage and stop ─────────────────────────────────────────────
if [[ -n "$BUNDLE_DIR" ]]; then
    ts "[4/6] Staging → $BUNDLE_DIR"
    arca_bundle_init "$BUNDLE_DIR" bluetooth || err "could not prepare $BUNDLE_DIR"
    for a in "${BT_ARTIFACTS[@]}"; do
        mode="${a%%|*}"; rest="${a#*|}"; src="${rest%%|*}"; dev="${rest#*|}"
        arca_bundle_add "$BUNDLE_DIR" bluetooth "$mode" "$src" "$dev" || err "staging failed: $dev"
    done
    warn "The init script and the two .conf files are provision-rules.conf's, not the bundle's"
    ok "Staged $(arca_bundle_finish "$BUNDLE_DIR" bluetooth) file(s)"
    echo ""
    exit 0
fi

if [[ -z "$DEVICE_IP" ]]; then
    echo "No IP supplied — built only. To deploy:"
    echo "  ./build-and-deploy.sh <ip>"
    exit 0
fi

# ── 4. the `bluetooth` group of the provision plan ──────────────────────────
# The same records and the same generated executor commissioning/provision.sh uses.
ts "[4/6] Init script, boot link, dbus policies, main.conf, input.conf (provision-rules.conf, group bluetooth)"
# shellcheck source=../lib/arca-identify.sh
. "$REPO_ROOT/lib/arca-identify.sh"
# shellcheck source=../lib/arca-provision.sh
. "$REPO_ROOT/lib/arca-provision.sh"

PROV_RULES="$REPO_ROOT/device-files/provision-rules.conf"
if ! PCHECK="$(arca_provision_validate "$PROV_RULES" "$REPO_ROOT")"; then
    echo "$PCHECK"; err "provision-rules.conf does not validate"
fi

BT_PLAN=$(mktemp)
trap 'rm -f "$BT_PLAN"' EXIT INT TERM
arca_provision_plan_component "$PROV_RULES" bluetooth > "$BT_PLAN" \
    || err "could not compile the bluetooth provision plan"
info "$(arca_provision_plan_summary "$BT_PLAN")"
arca_provision_push_installs "$BT_PLAN" "$REPO_ROOT" "$DEVICE" \
    || err "could not copy the bluetooth provision sources to the device"
ssh "$DEVICE" "cat > /tmp/arca-bt-plan" < "$BT_PLAN"
arca_provision_online_script | ssh "$DEVICE" "cat > /tmp/arca-bt-provision.sh"
ssh "$DEVICE" "sh /tmp/arca-bt-provision.sh /tmp/arca-bt-plan; rc=\$?; rm -f /tmp/arca-bt-provision.sh /tmp/arca-bt-plan; exit \$rc" \
    || err "the bluetooth provision step failed on the device"
ok "init script, dbus policies, main.conf and input.conf installed, S91bluetooth linked"
echo ""

# ── 5. the built artifacts ──────────────────────────────────────────────────
# The init script's stop goes first: it stops bluetoothd and unloads the modules, so
# the new .ko files are the ones the start below loads.
ts "[5/6] Built artifacts"
ssh "$DEVICE" "/etc/init.d/bluetooth stop" || warn "stop reported a failure"
DIRS=()
for a in "${BT_ARTIFACTS[@]}"; do dev="${a##*|}"; DIRS+=("${dev%/*}"); done
# shellcheck disable=SC2029 # expanded here on purpose
ssh "$DEVICE" "mkdir -p $(printf '%s\n' "${DIRS[@]}" | sort -u | tr '\n' ' ')"
for a in "${BT_ARTIFACTS[@]}"; do
    mode="${a%%|*}"; rest="${a#*|}"; src="${rest%%|*}"; dev="${rest#*|}"
    # Beside the target, then a rename: a running btmon (which stop does not end) makes a
    # write over /usr/bin/btmon fail with "Text file busy"; replacing the entry does not.
    scp -q "$src" "$DEVICE:$dev.arca-new" || err "could not copy $src to $dev"
    # shellcheck disable=SC2029
    ssh "$DEVICE" "chmod $mode '$dev.arca-new' && mv -f '$dev.arca-new' '$dev'" \
        || err "could not install $dev"
done
ok "${#BT_ARTIFACTS[@]} file(s) copied"
echo ""

# ── 6. start it now, through the boot path ──────────────────────────────────
ts "[6/6] Starting Bluetooth and verifying"
ssh "$DEVICE" "/etc/init.d/bluetooth start" || warn "start reported a failure"
ssh "$DEVICE" "sleep 2; /etc/init.d/bluetooth status; ls -l /etc/rc5.d/S91bluetooth"
echo ""
echo "  No adapter listed? The dongle needs USB host mode (usb_host component):"
echo "    ssh $DEVICE '/etc/init.d/usb-host status; lsusb'"

_ELAPSED=$(( $(date +%s) - _START_SECONDS ))
echo ""
echo "════════════════════════════════════════"
printf "  Done — %dm%02ds (no reboot needed)\n" $((_ELAPSED / 60)) $((_ELAPSED % 60))
echo "════════════════════════════════════════"
echo ""
