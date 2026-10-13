#!/bin/bash
# build-kernel-modules.sh - Cross-compile the out-of-tree kernel modules for Arca
# Run this script in WSL (Linux), from the usb_host/ directory or its parent.
#
# Prerequisites: every host package this needs comes from ../setup-build-env.sh, the one
# home for them (the kernel-module set is its `kmod` group).
#
# Produces: usb_host/modules/{ff-memless,joydev,xpad}.ko  (Xbox controller)
#           usb_host/modules/{snd-hwdep,snd-rawmidi,snd-usb-audio,snd-usbmidi-lib}.ko  (USB audio)

set -e

# It takes no arguments, so anything else is refused rather than read as "go": --help used to
# start a full build and overwrite linux-4.14.52/.config.
case "${1:-}" in
    "") ;;
    -h|--help) sed -n '2,9p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "usage: $0 [--help]   (takes no arguments)" >&2; exit 2 ;;
esac

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

KERNEL_VERSION="4.14.52"
KERNEL_DIR="linux-${KERNEL_VERSION}"
KERNEL_TARBALL="linux-${KERNEL_VERSION}.tar.xz"
KERNEL_URL="https://cdn.kernel.org/pub/linux/kernel/v4.x/${KERNEL_TARBALL}"
DEVICE_CONFIG="device_config"
MODULES_DIR="modules"
CROSS_COMPILE="arm-linux-gnueabihf-"
ARCH="arm"

echo "=============================================="
echo "  Kernel Module Builder (Xbox controller + USB audio)"
echo "  Target: Linux ${KERNEL_VERSION} / ARM (armv7l)"
echo "=============================================="

# --- Step 0: Check prerequisites ---
echo ""
echo "[0/8] Checking prerequisites..."
if ! command -v ${CROSS_COMPILE}gcc &>/dev/null; then
    echo "ERROR: ${CROSS_COMPILE}gcc not found."
    echo "  Install every host prerequisite with setup-build-env.sh, at the repo root."
    exit 1
fi
if ! command -v bc &>/dev/null; then
    echo "ERROR: bc not found. Install every host prerequisite with setup-build-env.sh, at the repo root."
    exit 1
fi
if [ ! -f "$DEVICE_CONFIG" ]; then
    echo "ERROR: Device config not found at ${DEVICE_CONFIG}"
    echo "  Copy it from the device: scp root@192.168.50.73:/proc/config.gz device_config.gz"
    echo "  Then: gunzip device_config.gz"
    exit 1
fi
echo "  Cross compiler: $(${CROSS_COMPILE}gcc --version | head -1)"
echo "  Device config: ${DEVICE_CONFIG}"

# --- Step 1: Download kernel source ---
echo ""
echo "[1/8] Downloading kernel source..."
if [ -f "$KERNEL_TARBALL" ]; then
    echo "  Tarball already exists, skipping download."
else
    echo "  Downloading ${KERNEL_URL}..."
    wget -q --show-progress "$KERNEL_URL" -O "$KERNEL_TARBALL"
    echo "  Download complete."
fi

# --- Step 2: Extract kernel source ---
echo ""
echo "[2/8] Extracting kernel source..."
if [ -d "$KERNEL_DIR" ]; then
    echo "  Source directory already exists, skipping extraction."
else
    echo "  Extracting ${KERNEL_TARBALL}..."
    tar xf "$KERNEL_TARBALL"
    echo "  Extraction complete."
fi

# --- Step 3: Copy device config ---
echo ""
echo "[3/8] Copying device kernel config..."
cp "$DEVICE_CONFIG" "${KERNEL_DIR}/.config"
echo "  Copied ${DEVICE_CONFIG} -> ${KERNEL_DIR}/.config"

# --- Step 4: Modify config for Xbox controller modules ---
echo ""
echo "[4/8] Modifying kernel config (Xbox controller + USB audio)..."

CONFIG_FILE="${KERNEL_DIR}/.config"

# Function to set a config option
set_config() {
    local key="$1"
    local value="$2"
    if grep -q "^${key}=" "$CONFIG_FILE"; then
        sed -i "s|^${key}=.*|${key}=${value}|" "$CONFIG_FILE"
        echo "  Updated: ${key}=${value}"
    elif grep -q "^# ${key} is not set" "$CONFIG_FILE"; then
        sed -i "s|^# ${key} is not set|${key}=${value}|" "$CONFIG_FILE"
        echo "  Enabled: ${key}=${value}"
    else
        echo "${key}=${value}" >> "$CONFIG_FILE"
        echo "  Added:   ${key}=${value}"
    fi
}

# Enable force-feedback memless support (module)
set_config "CONFIG_INPUT_FF_MEMLESS" "m"

# Enable joydev interface (module) - provides /dev/input/jsX
set_config "CONFIG_INPUT_JOYDEV" "m"

# Enable joystick subsystem (built-in, needed for xpad)
set_config "CONFIG_INPUT_JOYSTICK" "y"

# Enable Xbox gamepad driver (module)
set_config "CONFIG_JOYSTICK_XPAD" "m"

# Enable Xbox gamepad force feedback
set_config "CONFIG_JOYSTICK_XPAD_FF" "y"

# Enable Xbox gamepad LED support
set_config "CONFIG_JOYSTICK_XPAD_LEDS" "y"

# Enable the USB audio class driver (module). The ALSA core, its OSS shim and the
# TWL4030 codec are all compiled into this kernel already, so a USB DAC costs this
# one symbol -- but it SELECTS SND_HWDEP and SND_RAWMIDI, and the running kernel
# exports neither (measured: snd_hwdep_new and snd_rawmidi_new are both absent from
# /proc/kallsyms while snd_pcm_new and snd_card_new are present). Both therefore have
# to come out of the build as modules too; the assert below is what checks that.
set_config "CONFIG_SND_USB_AUDIO" "m"

# --- Step 5: Run olddefconfig ---
echo ""
echo "[5/8] Running olddefconfig to resolve dependencies..."
make -C "$KERNEL_DIR" ARCH=$ARCH CROSS_COMPILE=$CROSS_COMPILE olddefconfig
echo "  Config resolved."

# Verify our options survived olddefconfig.
# This ASSERTS rather than reports. olddefconfig re-resolves everything set above, and
# two of these symbols are not set here at all -- SND_HWDEP and SND_RAWMIDI have no
# prompt and exist only because SND_USB_AUDIO selects them. A selector forces a selected
# symbol to at least its own value, so =m here is what keeps them in the module build;
# resolved to =y they would silently vanish from `M=sound/core` while the running kernel
# exports no such symbols, and the only symptom would be insmod failing on an unresolved
# symbol that names nothing about the cause.
echo "  Verifying config options:"
config_assert_failed=0
for pair in \
    CONFIG_INPUT_FF_MEMLESS=m CONFIG_INPUT_JOYDEV=m CONFIG_INPUT_JOYSTICK=y \
    CONFIG_JOYSTICK_XPAD=m CONFIG_JOYSTICK_XPAD_FF=y CONFIG_JOYSTICK_XPAD_LEDS=y \
    CONFIG_SND_USB_AUDIO=m CONFIG_SND_HWDEP=m CONFIG_SND_RAWMIDI=m; do
    if grep -qx "$pair" "${KERNEL_DIR}/.config"; then
        echo "    OK    $pair"
    else
        got=$(grep -E "^(# )?${pair%%=*}[ =]" "${KERNEL_DIR}/.config" || echo "absent from .config")
        echo "    FAIL  expected $pair, got: $got"
        config_assert_failed=1
    fi
done
if [ "$config_assert_failed" -ne 0 ]; then
    echo ""
    echo "ERROR: olddefconfig did not resolve the config as required (see FAIL lines above)."
    echo "       Building on would produce modules that cannot load. Refusing."
    exit 1
fi

# --- Step 6: Prepare modules ---
echo ""
echo "[6/8] Running modules_prepare..."
make -C "$KERNEL_DIR" ARCH=$ARCH CROSS_COMPILE=$CROSS_COMPILE modules_prepare
echo "  Modules prepared."

# --- Step 7: Build the modules ---
echo ""
echo "[7/8] Building kernel modules..."

echo "  Building ff-memless.ko and joydev.ko (drivers/input/)..."
make -C "$KERNEL_DIR" ARCH=$ARCH CROSS_COMPILE=$CROSS_COMPILE M=drivers/input modules
echo ""

echo "  Building xpad.ko (drivers/input/joystick/)..."
make -C "$KERNEL_DIR" ARCH=$ARCH CROSS_COMPILE=$CROSS_COMPILE M=drivers/input/joystick modules
echo ""

# sound/core holds snd-hwdep.ko and snd-rawmidi.ko. Everything else under it is obj-y and
# an M= build skips obj-y, so this pass produces exactly those two and nothing more.
echo "  Building snd-hwdep.ko and snd-rawmidi.ko (sound/core/)..."
make -C "$KERNEL_DIR" ARCH=$ARCH CROSS_COMPILE=$CROSS_COMPILE M=sound/core modules
echo ""

echo "  Building snd-usb-audio.ko and snd-usbmidi-lib.ko (sound/usb/)..."
make -C "$KERNEL_DIR" ARCH=$ARCH CROSS_COMPILE=$CROSS_COMPILE M=sound/usb modules
echo ""

# --- Step 8: Collect and strip modules ---
echo ""
echo "[8/8] Collecting and stripping modules..."
mkdir -p "$MODULES_DIR"

# Every .ko this script produces, as a tree-relative path. One list, so a module added to a
# build pass above and forgotten here fails loudly on the next line rather than going missing
# from modules/ and then from USB_ARTIFACTS.
BUILT_MODULES=(
    "drivers/input/ff-memless.ko"
    "drivers/input/joydev.ko"
    "drivers/input/joystick/xpad.ko"
    "sound/core/snd-hwdep.ko"
    "sound/core/snd-rawmidi.ko"
    "sound/usb/snd-usb-audio.ko"
    "sound/usb/snd-usbmidi-lib.ko"
)
for rel in "${BUILT_MODULES[@]}"; do
    src="${KERNEL_DIR}/${rel}"
    if [ -f "$src" ]; then
        cp "$src" "${MODULES_DIR}/"
        echo "  Copied: $(basename "$rel")"
    else
        echo "  ERROR: ${src} not found!"
        exit 1
    fi
done

# Strip debug symbols to reduce size
echo "  Stripping debug symbols..."
for ko in "${MODULES_DIR}"/*.ko; do
    before=$(stat -c%s "$ko")
    ${CROSS_COMPILE}strip --strip-debug "$ko"
    after=$(stat -c%s "$ko")
    echo "    $(basename "$ko"): ${before} -> ${after} bytes"
done

echo ""
echo "=============================================="
echo "  Build complete! Modules in ${MODULES_DIR}/:"
ls -la "${MODULES_DIR}"/*.ko
echo ""
echo "  Next step: cd usb_host && ./build-and-deploy.sh <ip>"
echo "=============================================="
