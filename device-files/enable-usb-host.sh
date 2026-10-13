#!/bin/sh
# enable-usb-host.sh — Enable USB host mode on Arca via /dev/mem patching
#
# This script patches the omap2430_ops struct in kernel memory to fix a broken
# MUSB configuration in the OEM kernel (4.14.52). The kernel has DMA mode enabled
# but no DMA backend compiled in, causing "DMA controller not set" errors.
#
# The fix: write noop stub function pointers into the dma_init/dma_exit fields
# of omap2430_ops, then rebind the MUSB driver. The MUSB core sees non-NULL
# function pointers, calls the stubs (which return NULL DMA controller), and
# falls back to PIO mode — which works perfectly for USB host operations.
#
# Must run as root on the device.
# Requires: devmem_write binary in /usr/local/bin/ or /tmp/
#
# This script is idempotent — safe to run multiple times.
# If USB host bus is already active, it exits immediately.

# ============================================================================
# Memory addresses (specific to OEM kernel 4.14.52, build oe-user@oe-host)
# ============================================================================
#
# omap2430_ops struct base:  phys 0x80851b10  (virt 0xc0851b10)
#   +0x00: quirks           = 0x00000004 (MUSB_DMA_INVENTRA) — used for verification
#   +0x48: dma_init         = NULL → patched to noop_ret
#   +0x4c: dma_exit         = NULL → patched to noop
#
# Stub functions (existing kernel symbols):
#   noop_ret (returns 0):    virt 0xc01aa83c
#   noop     (void return):  virt 0xc01aa838

# ============================================================================
# Configuration
# ============================================================================
DMA_INIT_ADDR="0x80851b58"      # omap2430_ops.dma_init (phys)
DMA_EXIT_ADDR="0x80851b5c"      # omap2430_ops.dma_exit (phys)
NOOP_RET="0xc01aa83c"           # kernel noop_ret function (returns 0)
NOOP="0xc01aa838"               # kernel noop function (void)
STRUCT_BASE="0x80851b10"        # omap2430_ops base (for verification)
EXPECTED_QUIRKS="0x00000004"    # MUSB_DMA_INVENTRA
MUSB_DEVICE="musb-hdrc.0.auto" # sysfs device name for driver rebind

# ============================================================================
# Find devmem_write binary
# ============================================================================
DEVMEM_WRITE=""
for path in /usr/local/bin/devmem_write /tmp/devmem_write; do
    if [ -x "$path" ]; then
        DEVMEM_WRITE="$path"
        break
    fi
done

echo "=== USB Host Mode Enabler ==="

# ============================================================================
# Check if already enabled (idempotent)
# ============================================================================
if [ -d "/sys/bus/usb/devices/usb1" ]; then
    echo "USB host bus already active. Nothing to do."
    exit 0
fi

# ============================================================================
# Validate prerequisites
# ============================================================================
if [ -z "$DEVMEM_WRITE" ]; then
    echo "ERROR: devmem_write not found in /usr/local/bin or /tmp"
    exit 1
fi
echo "Using: $DEVMEM_WRITE"

# ============================================================================
# Verify struct identity (safety check before patching)
# ============================================================================
echo "Verifying omap2430_ops struct at $STRUCT_BASE..."
QUIRKS=$($DEVMEM_WRITE $STRUCT_BASE 2>&1 | grep "current value" | sed 's/.*= //')
if [ "$QUIRKS" != "$EXPECTED_QUIRKS" ]; then
    echo "ERROR: Struct verification failed (quirks=$QUIRKS, expected $EXPECTED_QUIRKS)"
    echo "Wrong kernel version or memory layout has changed."
    exit 1
fi
echo "Struct verified OK (quirks=$QUIRKS)."

# ============================================================================
# Patch dma_init → noop_ret (returns 0 = NULL DMA controller)
# ============================================================================
echo "Patching dma_init at $DMA_INIT_ADDR -> noop_ret ($NOOP_RET)..."
$DEVMEM_WRITE $DMA_INIT_ADDR $NOOP_RET
if [ $? -ne 0 ]; then
    echo "ERROR: Failed to patch dma_init"
    exit 1
fi

# ============================================================================
# Patch dma_exit → noop (void no-op cleanup)
# ============================================================================
echo "Patching dma_exit at $DMA_EXIT_ADDR -> noop ($NOOP)..."
$DEVMEM_WRITE $DMA_EXIT_ADDR $NOOP
if [ $? -ne 0 ]; then
    echo "ERROR: Failed to patch dma_exit"
    exit 1
fi

# ============================================================================
# Rebind MUSB driver to trigger re-probe with patched ops
# ============================================================================
echo "Rebinding MUSB driver ($MUSB_DEVICE)..."
echo "$MUSB_DEVICE" > /sys/bus/platform/drivers/musb-hdrc/bind 2>/dev/null

# Wait for USB bus to enumerate
sleep 2

# ============================================================================
# Verify result
# ============================================================================
if [ -d "/sys/bus/usb/devices/usb1" ]; then
    echo "SUCCESS: USB host mode enabled!"
    echo "USB bus: $(cat /sys/bus/usb/devices/usb1/product 2>/dev/null)"
    lsusb 2>/dev/null
else
    echo "WARNING: USB bus not detected after rebind. Check dmesg for details."
    dmesg | tail -10
fi
