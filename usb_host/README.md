# USB Host Mode for RoomWizard

## Quick Start

One command to enable USB host mode with game controller support:

```bash
./build-and-deploy.sh 192.168.50.73
```

This will:
1. Cross-compile the `devmem_write` tool and Xbox controller kernel modules
2. Report the USB power budget the booted device tree carries
3. Deploy everything to the device
4. Enable USB host mode (runtime kernel patch)
5. Load game controller modules
6. Install boot persistence for both

## Prerequisites

- **Linux/WSL** with `arm-linux-gnueabihf-gcc` installed
- **Additional packages**: `bc`, `libssl-dev`, `bison`, `flex` (for kernel module build)
- **Python 3** (for DTB patching script)
- **SSH root access** to the RoomWizard (key-based auth recommended)
- **Micro USB OTG adapter** (Micro-B male → USB-A female)
- **Powered USB hub** — *optional.* Measured 2026-08-10: the port **does** source VBUS, enough to run an
  Xbox pad through a passive adapter with no hub. Use one for a device whose draw exceeds the 500 mA
  budget, not as a fix for a device that is not detected (`../SYSTEM_ANALYSIS.md#36-usb`)

Install all prerequisites:
```bash
sudo apt-get install gcc-arm-linux-gnueabihf bc libssl-dev bison flex
```

## How It Works

### Problem 1: USB Host Mode Disabled

The OEM kernel (`4.14.52`) has a broken MUSB USB configuration: the driver expects DMA
(`CONFIG_MUSB_PIO_ONLY` is off), but no DMA backend is compiled in (`CONFIG_USB_INVENTRA_DMA`
and `CONFIG_USB_TI_CPPI41_DMA` are both off). This causes the MUSB driver to always fail with
`"DMA controller not set"` (`-ENODEV`).

### Solution 1: Runtime Memory Patching

Runtime `/dev/mem` patching of the `omap2430_ops` struct in kernel memory. We write noop stub
function pointers into the `dma_init` and `dma_exit` fields, forcing a PIO (Programmed I/O)
mode fallback. The patch is:

- **Non-destructive** — original kernel on disk is unchanged
- **Not persistent** — must be re-applied on each boot (handled by init script)
- **Idempotent** — safe to run multiple times

After patching, we rebind the MUSB driver, which re-probes successfully and registers a USB
host bus.

### Problem 2: Game Controllers Not Detected

Even with USB host mode working, game controllers (specifically Xbox 360 controllers,
`045e:028e`) appear in `lsusb` but do NOT create `/dev/input/event*` device nodes.

**Root cause**: The kernel was compiled with the entire joystick subsystem disabled:

```
# CONFIG_INPUT_JOYSTICK is not set   # No joystick drivers at all
# CONFIG_INPUT_JOYDEV is not set     # No /dev/input/jsX interface
# CONFIG_INPUT_FF_MEMLESS is not set # No force-feedback support
```

The Xbox 360 controller uses a **vendor-specific USB interface class** (`bInterfaceClass=ff`,
`bInterfaceSubClass=5d`, `bInterfaceProtocol=01`) — it is NOT standard HID. The `usbhid`
driver (the only HID driver in this kernel) only binds to devices with `bInterfaceClass=03`
(HID). Without the `xpad` kernel driver, no driver claims the controller.

### Solution 2: Cross-Compiled Kernel Modules

The kernel supports loadable modules (`CONFIG_MODULES=y`) with force loading
(`CONFIG_MODULE_FORCE_LOAD=y`) and no signature requirement (`CONFIG_MODULE_SIG` not set).
We cross-compile three kernel modules from the matching kernel source (4.14.52):

| Module | Config Option | Purpose |
|--------|--------------|---------|
| `ff-memless.ko` | `CONFIG_INPUT_FF_MEMLESS=m` | Force-feedback memless support (xpad dependency) |
| `joydev.ko` | `CONFIG_INPUT_JOYDEV=m` | Joystick device interface (`/dev/input/jsX`) |
| `xpad.ko` | `CONFIG_JOYSTICK_XPAD=m` | Xbox gamepad driver (Xbox, Xbox 360, Xbox One) |

After loading these modules, the controller appears as:
- `/dev/input/eventX` — evdev interface (used by `control_panel`'s Input page testers and games)
- `/dev/input/jsX` — joystick interface
- Name: `Microsoft X-Box 360 pad`
- Capabilities: `EV_KEY` (buttons), `EV_ABS` (sticks/triggers), `EV_FF` (force feedback)

### Problem 3: USB Bus Power Budget Too Low

Even with USB host mode working and the xpad driver loaded, the Xbox 360 controller was
sometimes rejected with `"rejected 1 configuration due to insufficient available bus power"`
when connected **directly** (without a powered hub). The kernel's USB hub driver enforces
a power budget based on the MUSB controller's device tree `power` property.

**Root cause**: The device tree blob (DTB) embedded in `uImage-system` sets the MUSB
controller's power budget to only **100mA**:

```dts
usb_otg_hs@480ab000 {
    compatible = "ti,omap3-musb";
    ...
    power = <0x32>;   /* 50 × 2mA = 100mA — too low for Xbox 360 controller */
};
```

The MUSB driver interprets this value as "units of 2mA", so `power = <0x32>` (50 decimal)
means 100mA total bus power. The Xbox 360 controller requests 500mA via its USB
configuration descriptor (`bMaxPower = 250`, also in 2mA units).

### Power budget in our own image

Our image's DTB carries `power = <250>` (`0xfa`, 500 mA), set in `usb_otg_hs` of `kernel/dts/rw20.dts`: **measured 2026-10-06 on
`.188`** (an image whose DTB carried the same value from an earlier scripted edit), `/proc/device-tree` reads `0xfa`, and an Xbox 360 pad plugged
directly (no hub) enumerated as `input1` with no `insufficient available bus power` in `dmesg` (operator, n=1).
The vendor image keeps `0x32` (100 mA), where a powered hub is needed; its byte patch is deleted with
vendor-kernel support. `build-and-deploy.sh` prints the live value at the end:
`/proc/device-tree/ocp@68000000/usb_otg_hs@480ab000/power`.


#### DTB Location in uImage

```
uImage-system (5,225,796 bytes)
├── uImage header (64 bytes)
├── zImage kernel (5,158,728 bytes, uncompressed)
└── Appended DTB at offset 0x4eb788 (67,004 bytes)
    └── usb_otg_hs node
        └── power property at DTB internal offset 0xeb44
            → Absolute uImage offset: 0x4fa2cc
```

#### Power Budget Values

| `power` value | Hex | Bus power | Result |
|:---:|:---:|:---:|---|
| 50 (original) | `0x32` | 100mA | Xbox 360 controller **rejected** |
| 250 (raised) | `0xfa` | 500mA | Xbox 360 controller **accepted** |

---

## Step-by-Step Manual Playbook

For those who want to understand or do it manually.

### Step 1: Install cross-compiler and build tools

On your WSL/Linux workstation:

```bash
sudo apt-get install gcc-arm-linux-gnueabihf bc libssl-dev bison flex
```

### Step 2: Cross-compile devmem_write

```bash
cd usb_host
arm-linux-gnueabihf-gcc -static -O2 -o devmem_write devmem_write.c
```

### Step 3: Build Xbox controller kernel modules

```bash
bash build-kernel-modules.sh
```

This downloads the Linux 4.14.52 kernel source, configures it with the device's config,
enables the joystick subsystem, and cross-compiles the three modules into `modules/`.

### Step 4: Install the device files — what `build-and-deploy.sh` does for you

⚠️ **This step is automated and the commands below are a *description*, not the instructions.** The three
device scripts and the two `rc5.d` links are `usb`-group records in
[`device-files/provision-rules.conf`](../device-files/provision-rules.conf), compiled by
`rw_provision_plan_component` and executed through the *same* generated interpreter that
`commissioning/provision.sh` uses. Do not hand-`scp` them: a second copy of the install would drift from
the rules file, and the modes are **declared** there rather than read off disk. What the plan resolves to:

```bash
DEVICE=192.168.50.73

# 0755, from device-files/ — the same bytes commissioning installs
../device-files/enable-usb-host.sh  ->  /usr/local/bin/enable-usb-host.sh
../device-files/usb-host            ->  /etc/init.d/usb-host
../device-files/xpad-modules        ->  /etc/init.d/xpad-modules

# from the bundle, or built here: devmem_write 0755, the three .ko 0644
devmem_write   ->  /usr/local/bin/devmem_write
modules/*.ko   ->  /lib/modules/4.14.52/extra/
```

Run it with `./build-and-deploy.sh $DEVICE`, or get the same result from either commissioning path.

### Step 5: Enable USB host mode and load modules

```bash
ssh root@$DEVICE /usr/local/bin/enable-usb-host.sh
ssh root@$DEVICE "depmod -a 4.14.52 && insmod /lib/modules/4.14.52/extra/ff-memless.ko && insmod /lib/modules/4.14.52/extra/joydev.ko && insmod /lib/modules/4.14.52/extra/xpad.ko"
```

### Step 6: Boot persistence — also part of the plan above

Two `link` records, not two `ln -sf` calls. ⚠️ **Both sources are RELATIVE**: an absolute symlink target
is correct on a running device and *dangling on a mounted card*, and a dangling `rc5.d` link is skipped in
silence at boot.

```bash
/etc/rc5.d/S89xpad-modules  ->  ../init.d/xpad-modules
/etc/rc5.d/S90usb-host      ->  ../init.d/usb-host
```

### Step 7: Verify

```bash
ssh root@$DEVICE "lsusb && echo '' && lsmod && echo '' && cat /proc/bus/input/devices"
```

Expected: Xbox controller visible in lsusb, xpad/joydev/ff_memless in lsmod,
`Microsoft X-Box 360 pad` in input devices.

---

## Technical Details

### MUSB DMA Patching

#### Root Cause

The OEM kernel config (from `/proc/config.gz`) shows:

```
CONFIG_USB_MUSB_HDRC=y          # MUSB driver: enabled (built-in)
CONFIG_USB_MUSB_HOST=y          # MUSB host mode: enabled
CONFIG_USB_MUSB_OMAP2PLUS=y     # OMAP2+ glue layer: enabled
# CONFIG_MUSB_PIO_ONLY is not set      # PIO fallback: DISABLED
# CONFIG_USB_INVENTRA_DMA is not set   # DMA backend: MISSING
# CONFIG_USB_TI_CPPI41_DMA is not set  # Alt DMA backend: MISSING
```

#### What the Patch Does

Patches two function pointers in the `omap2430_ops` struct via `/dev/mem` `mmap()`:

| Field | Address (phys) | Before | After |
|-------|---------------|--------|-------|
| `dma_init` | `0x80851b58` | `0x00000000` (NULL) | `0xc01aa83c` (`noop_ret`) |
| `dma_exit` | `0x80851b5c` | `0x00000000` (NULL) | `0xc01aa838` (`noop`) |

#### Why mmap() Works but write() Doesn't

`CONFIG_STRICT_KERNEL_RWX=y` marks kernel `.data` pages as read-only in the linear mapping
used by `/dev/mem`'s `write()` syscall. However, `mmap()` creates **new page table entries**
in userspace that map the same physical pages with `PROT_READ | PROT_WRITE`, bypassing the
kernel's read-only protection.

#### Memory Addresses

These are specific to OEM kernel `4.14.52` (built by `oe-user@oe-host`, 2018-06-26).

| Symbol | Virtual | Physical | Notes |
|--------|---------|----------|-------|
| `omap2430_ops` | `0xc0851b10` | `0x80851b10` | Struct base |
| `.dma_init` | `0xc0851b58` | `0x80851b58` | Offset +0x48 |
| `.dma_exit` | `0xc0851b5c` | `0x80851b5c` | Offset +0x4c |
| `noop_ret` | `0xc01aa83c` | — | Returns 0 |
| `noop` | `0xc01aa838` | — | Void return |

### Xbox Controller Kernel Module Details

#### Why the Controller Needs a Special Driver

The Xbox 360 controller (`045e:028e`) identifies itself as:
- `bDeviceClass=ff` (vendor-specific)
- `bInterfaceClass=ff` (vendor-specific)
- `bInterfaceSubClass=5d`
- `bInterfaceProtocol=01`

Standard USB HID devices use `bInterfaceClass=03`. The `usbhid` driver only binds to class
`03` devices, so it ignores the Xbox controller entirely. The `xpad` driver has a specific
USB device ID table that matches `045e:028e` regardless of the interface class.

#### Kernel Module Loading

The kernel has zero built-in loadable modules (the `/lib/modules/4.14.52/` directory was
empty). Key module-related config:

```
CONFIG_MODULES=y              # Loadable modules: ENABLED
CONFIG_MODULE_FORCE_LOAD=y    # Force loading despite version mismatch: ENABLED
CONFIG_MODULE_FORCE_UNLOAD=y  # Force unloading: ENABLED
CONFIG_MODVERSIONS=y          # CRC versioning: ENABLED (but force-load bypasses)
# CONFIG_MODULE_SIG is not set # Module signing: NOT REQUIRED
```

Modules are loaded with `insmod` (direct path) rather than `modprobe` since there's no
pre-existing `modules.dep` database. Boot persistence uses an init script that runs
`insmod` for each module in dependency order.

#### Boot Sequence

```
S89xpad-modules  →  loads ff-memless.ko, joydev.ko, xpad.ko
S90usb-host      →  patches MUSB DMA, rebinds driver, USB bus comes up
                     xpad driver auto-binds to any Xbox controllers found
```

### Key Kernel Config Values

| Config | Value | Significance |
|--------|-------|-------------|
| `CONFIG_USB_MUSB_HDRC` | `=y` | MUSB driver built-in |
| `CONFIG_USB_MUSB_HOST` | `=y` | Host mode enabled |
| `CONFIG_MUSB_PIO_ONLY` | not set | PIO fallback disabled (**THE BUG**) |
| `CONFIG_USB_INVENTRA_DMA` | not set | DMA backend missing (**THE BUG**) |
| `CONFIG_INPUT_JOYSTICK` | not set | Joystick subsystem missing (**FIXED**) |
| `CONFIG_INPUT_JOYDEV` | not set | Joystick device interface missing (**FIXED**) |
| `CONFIG_INPUT_FF_MEMLESS` | not set | Force-feedback missing (**FIXED**) |
| `CONFIG_MODULES` | `=y` | Loadable modules supported |
| `CONFIG_MODULE_FORCE_LOAD` | `=y` | Force-load despite CRC mismatch |
| `CONFIG_MODULE_SIG` | not set | No signature verification |
| `CONFIG_DEVMEM` | `=y` | `/dev/mem` available |
| `CONFIG_STRICT_DEVMEM` | not set | No memory access restrictions |
| `CONFIG_STRICT_KERNEL_RWX` | `=y` | Kernel pages read-only (blocks `write()`) |
| `CONFIG_HID_GENERIC` | `=y` | Generic HID driver (doesn't help Xbox) |
| `CONFIG_USB_HID` | `=y` | USB HID transport (class 03 only) |

### omap2430_ops Struct Layout

Dumped from physical address `0x80851b10` (128 bytes):

```
+0x00: 00000004  quirks = MUSB_DMA_INVENTRA
+0x04: c05d01dc  init = omap2430_musb_init
+0x08: c05d00cc  exit = omap2430_musb_exit
+0x0c: c05cfe80  enable = omap2430_musb_enable
+0x10: c05cfe5c  disable = omap2430_musb_disable
+0x14: 00000000  (fifo_mode / ep_offset / ep_select etc.)
...
+0x48: 00000000  dma_init = NULL  ← PATCHED to 0xc01aa83c (noop_ret)
+0x4c: 00000000  dma_exit = NULL  ← PATCHED to 0xc01aa838 (noop)
...
+0x60: c05cfca4  set_vbus
+0x70: c05d0698  clear_ep_rxintr
+0x78: "omap2430" (name string)
```

### Why DTB `dr_mode` Modification Alone Doesn't Fix USB Host

DTB changes (e.g., setting `dr_mode = "host"`) were investigated but don't solve the
DMA problem. The root cause is in the kernel's compiled-in code path, not the device tree
configuration. The `omap2430_ops.dma_init` pointer is NULL at compile time because
`CONFIG_USB_INVENTRA_DMA` is not set — no device tree change can populate it.

The DTB `power` property is a separate matter (see Problem 3 above): it sets the USB bus power budget, and a device tree can express that.

---

## File Reference

| File | Runs On | Purpose |
|------|---------|---------|
| `build-and-deploy.sh` | Workstation (WSL/Linux) | End-to-end build and deploy automation |
| `build-kernel-modules.sh` | Workstation (WSL/Linux) | Cross-compile Xbox controller kernel modules |
| `devmem_write.c` | Compiled for ARM | `/dev/mem` mmap-based read/write tool |
| [`../probes/*.sh`](../probes/README.md) | Device (need `devmem_write`) | UART3 / pinmux / GPIO measurement probes. Not built, deployed or packaged by anything — a shelf, not a component |
| `../device-files/enable-usb-host.sh` | Device | Runtime kernel patch + MUSB driver rebind |
| `../device-files/usb-host` | Device | SysV init.d wrapper for USB host boot persistence |
| `../device-files/xpad-modules` | Device | SysV init.d script for loading controller modules at boot |

The three device scripts live in [`device-files/`](../device-files/) rather than here, and are named as
they are *deployed*, because three paths now install the same bytes — `commissioning/provision.sh`,
`commissioning/commission-offline.sh` and this component's `build-and-deploy.sh`. They are declared as
`usb`-group records in [`device-files/provision-rules.conf`](../device-files/provision-rules.conf); the
four **built** artifacts travel in the release bundle.

### Files Deployed to Device

| Device Path | Source | Purpose |
|-------------|--------|---------|
| `/usr/local/bin/devmem_write` | `devmem_write` (compiled ARM binary) | Physical memory read/write |
| `/usr/local/bin/enable-usb-host.sh` | `../device-files/enable-usb-host.sh` | USB host mode enabler |
| `/etc/init.d/usb-host` | `../device-files/usb-host` | Boot persistence for USB host |
| `/etc/rc5.d/S90usb-host` | Symlink → `../init.d/usb-host` | Runlevel 5 boot hook |
| `/lib/modules/4.14.52/extra/ff-memless.ko` | `modules/ff-memless.ko` | Force-feedback module |
| `/lib/modules/4.14.52/extra/joydev.ko` | `modules/joydev.ko` | Joystick device module |
| `/lib/modules/4.14.52/extra/xpad.ko` | `modules/xpad.ko` | Xbox controller driver |
| `/etc/init.d/xpad-modules` | `../device-files/xpad-modules` | Boot persistence for modules |
| `/etc/rc5.d/S89xpad-modules` | Symlink → `../init.d/xpad-modules` | Runlevel 5 boot hook |

⚠️ **The module loader's installed name is `/etc/init.d/xpad-modules`; `S89xpad-modules` is the *link*.**
It used to be installed under the `S89` name in `init.d` as well, and nothing sweeps `/etc/init.d`, so
`provision-rules.conf:94` carries an `unlink` of the old path to clear it off units commissioned before
the move.

⚠️ **`uImage-system` is never shipped and never written by a script.** `release.sh` refuses to publish any manifest entry whose basename matches `uImage*`.

---

## Troubleshooting

| Symptom | Likely Cause | Fix |
|---------|-------------|-----|
| SSH connection fails | Device not on network | Check Ethernet, ping device IP |
| `devmem_write` won't compile | Missing cross-compiler | `sudo apt-get install gcc-arm-linux-gnueabihf` |
| Struct verification fails | Different kernel version | Re-derive addresses from `vmlinux` analysis |
| "DMA controller not set" in dmesg | Normal (from initial boot) | Check for `MUSB HDRC host driver` **after** patch |
| USB device not detected | OTG adapter issue or no VBUS | Use powered USB hub, check adapter polarity |
| Controller in lsusb but no /dev/input | xpad module not loaded | `insmod /lib/modules/4.14.52/extra/xpad.ko` |
| insmod fails "Invalid module format" | Version mismatch | Use `insmod -f` (force) — `MODULE_FORCE_LOAD=y` allows this |
| Module build fails | Missing build deps | `sudo apt-get install bc libssl-dev bison flex` |
| No event device after xpad loads | Controller unplugged during load | Replug controller, or unbind/rebind via sysfs |
| **Worked at boot, dead after a replug** | On `.188` a 95 s unplug/replug **works** — VBUS stays up and the disconnect is not processed until the device returns. The `.225` 60 s failure has no confirmed counterpart on current hardware | If it does happen: plug the device in, then `/etc/init.d/usb-host recover` (a driver re-probe). ⚠️ **`echo host > .../mode` is a silent no-op on this SoC** — `omap2430_ops` has no `.set_mode`. Detail `../SYSTEM_ANALYSIS.md#36-usb` |
| **Nothing enumerates unless it was plugged in at boot** | **A standing property of this hardware, not an open bug** (settled 2026-08-14). MUSB powers the port only when a device is present as the driver probes; otherwise VBUS collapses seconds later and stays off. Three mechanisms read out of the driver source have each been applied and **refuted on hardware** | **Control Panel → USB → RESCAN** — one tap, ~5 s, dead port → playable pad, verified on a panel. Over SSH it is `/etc/init.d/usb-host recover` with the device already plugged in. `$MUSB/vbus` is the diagnostic (`Vbus off` = dead port); `mode` is not — it reads `a_idle` while a pad works. `../IMPROVEMENT_PLAN.md` |
| "rejected configuration due to insufficient bus power" | Our DTB keeps the vendor `power` `0x32` (100 mA, inferred from the DTB, not measured on a unit) | Connect the controller through a powered hub |

**Simulating an unplug from sysfs (measured on `.188`):** `echo 1 > /sys/bus/usb/devices/1-1/remove` drops the node (the control panel USB page empties within ~3 s); `authorized=0` on `1-1` does **not** remove it from sysfs or `lsusb`, and toggling `usb1/authorized` did not re-enumerate — `/etc/init.d/usb-host recover` did (attempt 1).

---

## Failed Approaches (for reference)

| Approach | Why It Failed |
|----------|---------------|
| Vanilla kernel *image* build | **Not a failed approach — it is the current plan**, and no attempt was ever recorded here: "bricked" was a prediction, not an outcome. The policy, the one blocking driver and the per-symbol costs are `../SYSTEM_ANALYSIS.md#7-kernel-policy` |
| Binary patching of OEM zImage | Address relocation + inlining made patch point unfindable |
| `/dev/mem` via `write()` syscall | `CONFIG_STRICT_KERNEL_RWX` blocks write to kernel pages |
| DTB `dr_mode` modification only | Host mode needs the compiled-in `dma_init`/`dma_exit` stubbed out; the device tree cannot express that |
| EHCI/OHCI alternative | `CONFIG_USB_EHCI_HCD` and `CONFIG_USB_OHCI_HCD` not compiled |
| usbhid for Xbox controller | Controller uses vendor-specific class `ff`, not HID class `03` |
| Runtime DTB power override **via sysfs** | MUSB reads `power` at probe and `hcd->power_budget` is not exposed; there is no sysfs node to write |
| DTB `mode` 3 → 1 (`MUSB_PORT_MODE_HOST`) to revive a cold port | **Refuted on hardware 2026-08-14.** Written to `.188`'s p1 and verified live in the booted tree (`mode` = `00 00 00 01`); a pad plugged in after an empty-socket boot still stayed dark, while `usb-host recover` on the same firmware brought it up on attempt 1. The p1 byte-patch writer that applied it is deleted with vendor-kernel support |
| `echo host > $MUSB/mode` | Silent no-op that returns success: `omap2430_ops` has no `.set_mode` on this SoC |
| Writing `$MUSB/vbus` | Sets `a_wait_bcon`, which nothing on omap2430 reads |
| debugfs `softconnect` | Sets `SESSION` **only** in `OTG_STATE_A_WAIT_BCON`; a cold port sits in `a_idle` |
| A hub left permanently attached (ID-ground) | Measured 2026-08-13: `Vbus off` at 1, 2 and 3 min, and a device plugged into the hub enumerated nothing. An ID event replays the *cached* DEVCTL, and a cold port has no `SESSION` bit to resume |

⚠️ **Require this of a fourth candidate before spending a session on it: how does a port that probed
with an EMPTY socket ever obtain a session?** That is the state every measurement above started from,
and each refuted mechanism silently assumed a session it did not have — which is the single reason all
three failed. A candidate that cannot answer it is the same theory again in new clothes. The answer that
ships is the RESCAN button, verified on a panel; enumeration-at-probe is a standing property of this
hardware with a one-tap remedy, not an open bug.

⚠️ **The `/dev/mem` patch forces IRQ-driven PIO — it does not fix DMA.** Anything above or below reading
as "the patch repairs a broken DMA configuration" has it backwards: the noop `dma_init`/`dma_exit` stubs
make MUSB **give up** on DMA and fall back to interrupt-driven programmed I/O, which is what makes host
mode work at all. DMA on this block is not repaired, not used, and not needed — `musbhsdma.c` is not even
compiled (`# CONFIG_USB_INVENTRA_DMA is not set`). PIO is also why the failure mode is safe: a
misbehaving DMA controller would scribble into RAM, whereas stubs simply cost CPU.

⚠️ **The failed sysfs override above is not the untried RAM experiment.** `IMPROVEMENT_PLAN.md` keeps a
scoped future item — locate the **in-RAM unflattened device tree** and patch its `power` property with
`devmem_write`, the same mechanism as the `omap2430_ops` patch aimed at a different target. That has never
been attempted; what failed was writing through *sysfs*, which is a different thing entirely. The open
risk is address stability: `omap2430_ops` is a static symbol at a fixed address, while the unflattened DT
is early-boot allocated. If it works, a 500 mA budget would need no new kernel image.

⚠️ **The other untried `/dev/mem` target is DEVCTL `SESSION`, and it carries a hazard worth stating
before anyone tries it.** MUSB's DEVCTL register is at physical `0x480AB060`, and bit 0 (`SESSION`) is
the bit `musb_start()` sets — so poking it is the most direct expression of "make a cold port start a
session". It has **not** been attempted. The hazard is not the write's effect but the access itself: an
OMAP IP left in forced standby has its interface clock gated, and a register access to a clock-gated IP
raises an **external abort** rather than reading zero. The IP has to be held resumed first (via the
driver's `power/control`, or by keeping a session-capable consumer bound) before `devmem_write` touches
that address. ⚠️ **Reaching a session is also not the same as fixing the cause** — three mechanisms read
out of this driver have been applied and refuted on hardware; see `../IMPROVEMENT_PLAN.md` for what
a fourth candidate has to explain first.

---

## Device Info

| Property | Value |
|----------|-------|
| Device | Steelcase RoomWizard II |
| SoC | TI OMAP3503 (OMAP34xx family), ARM Cortex-A8 |
| Kernel | Linux 4.14.52 |
| Build | `oe-user@oe-host` (Yocto/OpenEmbedded), 2018-06-26 |
| Boot | U-Boot → uImage from SD card FAT32 |
| USB Port | Micro-B OTG (single port) |
