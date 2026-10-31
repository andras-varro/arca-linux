# kernel/ — building our own 4.14.52 image

The working notes for the kernel we build ourselves: how the image is produced, what we patch and why,
the drivers still missing, and the methods that recovered what the vendor never published. **Device
facts found here move to `SYSTEM_ANALYSIS.md`**: the policy and the dropped-driver table are in
[§7](../SYSTEM_ANALYSIS.md#7-kernel-policy), the panel timings in
[§3.2](../SYSTEM_ANALYSIS.md#32-display), and the touch register map in
[§3.3](../SYSTEM_ANALYSIS.md#33-touch). Open work is in `IMPROVEMENT_PLAN.md`. Rules for editing this
directory are in [`CLAUDE.md`](CLAUDE.md).

| Path | What it is |
|---|---|
| `build-image.sh` | extract → patch → configure → `zImage` → DTB → `uImage`, all inside WSL; `--help` is current |
| `patches/*.patch` | kernel source patches, `patch -p1` from the tree root, GPL-2.0-only |
| `dts/rw20.dts` | our board's device tree on the mainline `omap34xx.dtsi`, GPL-2.0-only; `build-image.sh` compiles it inside the kernel tree |
| `drivers/cy8ctmg120_ts/` | out-of-tree touch driver (`.c` + `Kbuild`), GPL-2.0-only, adapted from vanilla `cy8ctmg110_ts.c`; binds the unchanged `panjit_ts` DT node and names its input device `panjit_ts` |
| `build-modules.sh` | builds every `drivers/*/` with a `Kbuild` via `M=` against the tree `build-image.sh` left in WSL `$HOME`; `--out <dir>` receives the `.ko`, which loads only on our image |
| `build-bt-modules.sh` | `--set bt` (default) builds the 18 Bluetooth modules (`BT=m` and dependencies) from a copy of the image's tree, so no p1 write; `kernel/patches-modules/*.patch` are applied here only and `build-image.sh` never reads them. `--set storage` builds `usb-storage.ko` (USB sticks; `USB_STORAGE` is unset in the image) the same way; `usb_host/build-and-deploy.sh` runs it, `ARCA_KBUILD_IMAGE` names a test-kernel tree |
| `tools/i2c_touch_read.c` | userspace burst reader for the touch controller over `/dev/i2c-N`; it never writes to the part |

## Building an image

```bash
wsl.exe -e bash -lc "cd /mnt/c/work/roomwizard && kernel/build-image.sh --out /mnt/c/work/rw-scratch/img"
```

The script extracts a fresh tree from `usb_host/linux-4.14.52.tar.xz` into WSL `$HOME` and applies
`patches/`. It configures from the device's own `usb_host/device_config` with `olddefconfig`, then
writes `dropped-symbols.txt`. It builds `zImage` (about 1.5 min fresh; `--reuse` is incremental) and
appends a DTB, then wraps the result:

```
cat arch/arm/boot/zImage board.dtb > zImage-with-dtb
mkimage -A arm -O linux -T kernel -C none -a 0x80008000 -e 0x80008000 -n '' -d zImage-with-dtb uImage
```

- **The DTB, by default, is ours:** `dts/rw20.dts`, copied into the tree's `arch/arm/boot/dts` and built there with `make rw20.dtb`
  (the tree's own cpp include path and `scripts/dtc`; no host `dtc`). `--dtb <file>` appends another instead. The board has no
  separate `.dtb`; it is appended to `uImage-system`. Against `/sys/firmware/fdt` of `.188` (phandles resolved, 2026-10-12) it differs in
  four hunks and no hardware property: an empty `i2c0` alias, `/chosen` bootargs (U-Boot fills them), `ok` against `okay`, and four
  never-instantiated `hsusb1/2` phy and regulator nodes it leaves out. Every DT-borne measurement below dated earlier was taken on an
  image whose DTB was the vendor's with scripted edits.
- ⚠️ **`-C none`, never `-C gzip`.** `-C` describes the blob handed to `mkimage`, and a `zImage`
  already decompresses itself, so `CONFIG_KERNEL_GZIP` does not enter into it. **Measured** from byte
  31 of the vendor header. The recipe is proven by round trip: re-wrapping the vendor's *own* payload
  under `SOURCE_DATE_EPOCH=1529971593` reproduces `edc637ac14f90e0187b1ed65ffedf6d7` with zero
  differing bytes.
- `mkimage` (in `setup-build-env.sh`'s `kmod` group) packages the image. A clone without it can build
  modules but cannot package an image.
- **The USB power budget is ours (`usb_otg_hs` in `dts/rw20.dts`)**: `power = <250>` (units of 2 mA, 500 mA), where the vendor DTB has `0x32` (100 mA, [§3.6](../SYSTEM_ANALYSIS.md#36-usb)). Booted on `.188`: `/proc/device-tree` reads `0xfa` (measured 2026-10-06).

Installing the image on a unit is a **manual operator step**, and nothing scripted writes `uImage-system` ([`lib/CLAUDE.md`](../lib/CLAUDE.md)). The permission classifier refuses an agent's write there, so the operator takes a backup of the running
`uImage-system`, copies the new one over it and reboots (`.188` runs our image this way). The undo is
copying the backup back, by SSH if the image answers or with a card reader if it does not. `build-image.sh`
itself writes only `uImage-test`; `mlo`, `u-boot.bin` and `ctrlblock.bin` stay untouched.

`.188`'s p1 holds, **measured 2026-10-06**, the boot files, `uImage-bootstrap`, `uImage-system.vendor`, `.f103undo`
(`24ab7f08…`, the flush-patch image), `.f125undo` (`fb4f2c94…`, the image before the TCP patches), `.f4undo`
(`022be99b…`, the image before `OMAP3_THERMAL`), `.f101vbusundo` (`72683498…`, the image before the 500 mA
budget, so the undo for that change) and `uImage-system`, our image with every `kernel/patches/`
patch, our panel DTB, the power node below, `OMAP3_THERMAL` and the 500 mA budget (`45b05f77…`). p1 is 54% used; an image is ~5.2 MB, so delete a stale one before the next.

**Software power-off (the `power` node of `dts/rw20.dts`): booted on `.188`, powers the board down — measured 2026-10-04.**
`/sys/bus/platform/drivers/twl4030_power/` holds `48070000.i2c:twl@48:power` after boot, and `poweroff` darkened
the panel and backlight and the RJ45 LEDs, and the operator thought USB power went too ("I think"). **PoE draw was
not measured.** Replugging PoE cold-boots normally (SSH within ~1 min, node bound again): there is no power button,
so pulling PoE is the way back. The vendor kernel and our earlier images only halt: panel white, backlight on,
down over 3.5 min with no watchdog reboot, because `twl4030-power.c` installs `pm_power_off` only with
`ti,system-power-controller`. Rollback is copying `.f103undo` back.

## Bluetooth modules

`build-bt-modules.sh` makes 18 modules with `BT=m`: `bluetooth`, `btusb`, `btrtl`, `btbcm`, `btintel`, `rfcomm`, `hidp`, `uhid`, `uinput`,
`ecdh_generic`, `af_alg`, `algif_hash`, `algif_skcipher` and the crypto set `cmac`, `ecb`, `sha256_generic`,
`hmac`, `drbg`. **Measured:** the relinked `vmlinux` is byte-identical to the image's and all 836 imported
CRCs match; on `.188` all load by `insmod`. `jitterentropy_rng` is not collected: `insmod` refuses it there
(`host not compliant with requirements: 2`) and `drbg` loads without it. Loadable because `CONFIG_MODULES=y`,
`CONFIG_MODULE_FORCE_LOAD=y` and `CONFIG_MODULE_SIG` is unset. `load-order.txt` puts the crypto modules first,
ahead of `btusb`, whose adapter probe allocates `cmac(aes)` and `ecdh`. The `bluetooth/` component deploys them
to `/lib/modules/4.14.52/bt/` with the firmware, and its init script loads them in that order.

**Hardening in the module build (commit `59111a1`).** `build-bt-modules.sh` sets `BT_HS` off, which compiles A2MP out
of `bluetooth.ko` — the BleedingTooth entry point (CVE-2020-12351/12352, needs no pairing); the built module has no
`a2mp` symbols [measured]. `patches-modules/` also carries three backports (fix versions from memory [inferred]):
`bluetooth-min-enc-key-size.patch` (KNOB minimum encryption key size, CVE-2019-9506), `hidp-report-len-unsigned.patch`
(HIDP signed report length, CVE-2018-9363) and `l2cap-conf-req-efs-uninit.patch` (L2CAP configuration response echoing
uninitialised memory, CVE-2022-42895). Built, CRC-checked against the image and deployed on `.188`: the modules load
and `hci0` is up [measured]. **Operator, on `.188`: a pad reconnects after the pad is power-cycled** [measured by the
operator]. Not covered: the BT keyboard and the headset after this build, and any reconnect after a panel reboot.

The operator's dongle `0b05:1bf6` is a Realtek **RTL8761CU**: `btrtl` logs `hci_ver=0d hci_rev=000e
lmp_ver=0d lmp_subver=8761`, rom_version 1. Mainline knows the 8761CU from v6.19 (`ic_id_table`, lmp `0x8761`,
hci_rev `0x0e`) and has no `0b05:1bf6` in any tag (nearest `0b05:1bef`); 4.14's `btrtl` keys on `lmp_subver`
alone, which 8761A, 8761B and 8761CU share. Its class `e0-01-01` binds `btusb` generically with `driver_info
== 0`, so `BTUSB_REALTEK` is never taken without our patch. `patches-modules/btusb-asus-1bf6-realtek.patch`
adds the ID and `btrtl-rtl8761cu.patch` the chip (8761CU firmware is epatch v1, which 4.14 parses; project id
51; an unknown 8761 `hci_rev` is refused). With the firmware from `3rdparty/realtek/bluetooth/` (provenance and
md5s in its `README.md`) the 263-byte download succeeds: `hci_revision` goes `0x000e` to `0x7bf1`,
manufacturer 93. No MUSB DMA question stands in the way: A2DP is tens of KB/s, which PIO carries.

## What we patch, and why

| Patch | Without it | Found by |
|---|---|---|
| `patches/smsc911x-reset-pulse.patch` | no network: the DT marks the LAN9221 reset line active-high, and vanilla `smsc911x.c` requests it low and never releases it | function-size diff: the vendor probe adds `set(0); msleep(100); set(1)` |
| `patches/omapdss-honour-syncclk-active.patch` | `videomode_to_omap_video_timings` copies the pixel-data clock edge onto the sync edge, so DT cannot express this panel's sync-on-falling, data-on-rising | reading `dss/display.c`; changes nothing for a DT without `syncclk-active` |
| `patches/omapdss-scale-mode-param.patch` | the DSS overlay scaler's upscale tables (`dispc_coefs.c`) are stock-only, which blur a 2x pixel-art upscale; the patch adds `omapdss.scale_mode` (0 stock, 1 pixel-repeat, 2 pixel-repeat with a short edge blend), 0644, read when an overlay is set up and upscaling only. **Built, not yet booted** | the design notes for the scaler; the `dispc_ovl_get_scale_coef` table layout |
| `dts/rw20.dts` (`/display`) | no `/dev/fb0`: the vendor `/display` node names `sharp,lq070y3lg4a`, which no vanilla driver claims | disassembling the vendor `sharp_lq_*` driver (below) |
| `patches/omapfb-panel-dpi-data-lines.patch` | `panel_dpi_probe_of` never reads `data-lines`, so `omapdss_default_get_recommended_bpp` (`dss/display.c:47`) says 16 and fb0 is sized for one 16bpp frame | reading the source; `dts/rw20.dts` sets `data-lines = <24>` |
| `patches/leds-pwm-dt-brightness.patch` | vanilla `led_pwm_add` starts every PWM LED at `LED_OFF`, so the backlight boots dark; the DTB already carries `brightness` (backlight 100, red/green 50, measured with `fdtget`) | `led_pwm_probe` function-size diff (below) |
| `patches/musb-release-mstandby-on-probe.patch` | with `USB_INVENTRA_DMA`, DMA dies at the first `usb-host recover`: the unbind's `omap2430_low_level_exit` sets `OTG_FORCESTDBY.ENABLEFORCE`, only `omap2430_runtime_resume` clears it, and the glue stays runtime-active across the rebind — the channel stays enabled with its count unmoved, the `dma` IRQ never fires, the audio writer blocks. **Booted — measured 2026-09-27 on `.188`**: after replug + `recover`, `0x480AB414 = 0`, `hw_ptr` advances ~49.5k frames/s, the `dma` IRQ keeps rising, and the operator heard it play | unpatched, after a recover: `0x480AB414 = 1`, DMA ch0 `CNTL 0x6a9 COUNT 4`; writing `0` there by hand completed the transfer within 1 s |
| `patches/musb-a-idle-disconnect.patch` | a clean unplug hits the `default:` arm of the `MUSB_INTR_DISCONNECT` switch (`unhandled DISCONNECT transition (a_idle)`), so `is_active` stays set and the child is never disconnected — no `USB disconnect` logged, and once the child goes the root hub's autosuspend loops on `musb_bus_suspend` `-EBUSY`. The patch adds `A_IDLE`/`A_WAIT_BCON` arms calling `musb_host_resume_root_hub()` + `musb_root_disconnect()`. **Booted 2026-09-28 on `.188`** (uImage md5 `926896a5…`): a hub pulled from the root port logs `USB disconnect` for every child, `1-1` leaves sysfs, and `unhandled DISCONNECT`/`musb_bus_suspend` count 0. Its replug miss (no `CONNECT`, mode `a_idle`) is gone with the patch below | reading `musb_core.c` against the gadget-side twin, which clears `is_active` |
| `patches/musb-omap2430-session-on-id-ground.patch` | `omap_musb_set_mailbox()` acts on `MUSB_ID_GROUND` and `MUSB_VBUS_OFF` only `if (musb->gadget_driver)`, always NULL here (`# CONFIG_USB_GADGET is not set`), so an adapter plug never sets `SESSION` and a cold port stays dead until a rebind ([§3.6](../SYSTEM_ANALYSIS.md#36-usb)). The patch drops the guard on both arms: ID-ground → `omap_control_usb_set_mode(HOST)` + `set_vbus(1)`, VBUS-off → `set_vbus(0)`. **Booted 2026-09-29 on `.188`** (uImage md5 `f3b446c6…`, every patch here): adapter pulled with a hub and 6 devices → `status=4`, `set_vbus(0)`, all disconnect; replugged → `status=1`, `set_vbus(1)`, the hub enumerates 0.5 s later and all 7 are back within 3 s with no RESCAN, mode `a_host`. A hub swapped behind a seated adapter (no ID edge) re-enumerates too, mode staying `a_host`, which the previous image missed — why, **[inferred]**: the port now reaches a proper `a_host` | debugfs kprobes `p:rwmb omap2430_musb_mailbox status=%r0:u32` and `p:rwsv omap2430_musb_set_vbus on=%r1:u32` (no `dynamic_debug` on our image); `twl4030_usb` in `/proc/interrupts` ticks once per ID/VBUS edge |
| `patches/musb-host-flush-gone-device.patch` | `musb_h_tx_flush_fifo()` (upstream `FIXME`, `musb_host.c`) retries 1000 × `mdelay(1)` under `musb->lock` with IRQs off while `FIFONOTEMPTY`; behind an unplugged hub the FIFO never drains, so each pull of a hub carrying a streaming USB card stalls ~1 s with IRQs off. `dev_WARN_ONCE` hides the second and later prints, not the stall. `musb_h_tx_flush_fifo_urb()` tries 10 times (silently) when `urb->dev->state == USB_STATE_NOTATTACHED`, at three call sites: `musb_ep_program`, the `musb_host_tx` error path and `musb_cleanup_urb`. Cleanup alone was not enough: `musb_cleanup_urb` → `musb_advance_schedule` → `musb_start_urb` → `musb_ep_program` flushes (1000 tries) once per URB the dead device still had queued, and the first version only shortened the last one (**measured**, five pulls with a kprobe pair on `musb_cleanup_urb`: 1-2 calls of ~1003 ms each, plus one WARNING from `musb_start_urb` in `hub_event`). `musb_rx_reinit`'s shared-FIFO flush keeps 1000 (no URB of its own; not seen in the traces). **Booted on `.188`** (uImage md5 `24ab7f08…`; undo `.b40`, `f3b446c6…`), streaming USB audio, five hub pulls: 28 `musb_cleanup_urb` calls, longest 20 ms, no `Could not flush host TX10 fifo` WARNING, and the operator saw failover "almost instantaneous" (was ~1 s) (**measured, n=5**) | reading `musb_h_tx_flush_fifo*`, `musb_cleanup_urb`, `musb_start_urb`; kprobe timings on `.188` |
| `patches/musb-omap2430-set-vbus-report.patch` | `omap2430_musb_set_vbus()` polls DEVCTL `BDEVICE` 100 × `mdelay(5)`, so upstream's 1 s jiffies deadline never ends the wait: it gives up after ~505 ms **[inferred from source, not timed on the device]** and prints a bare `configured as A device timeout`, then carries on as on success. The patch waits on a `ktime` deadline (1 s from the mailbox work, 500 ms from the SESSREQ hard IRQ) and prints `configured as A device timeout: devctl %02x after %lld ms[ (irq)]`; carry-on is kept. Booted on `.188` with the patch above | reading `omap2430.c`; the message makes the next occurrence measure itself |
| `patches/tcp-01…12-*.patch` | remote TCP DoS: SegmentSmack CVE-2018-5390 (01-05, out-of-order queue CPU burn) and SACK Panic/slowness/low MSS CVE-2019-11477/78/79 (06-12, `BUG_ON` in `tcp_shifted_skb`, unbounded `tcp_fragment`, MSS 48). The stable 4.14.59/.127/.131/.138/.142 commits **verbatim** from `git.kernel.org`, each keeps its upstream `commit … upstream.` line; apply with offsets, no fuzz. Adds sysctl `net.ipv4.tcp_min_snd_mss` (default 48). FragmentSmack CVE-2018-5391 is not here (an rbtree rework of `inet_frag`) | grep of the pristine tree: no `tcp_min_snd_mss`, `tcp_ooo_try_coalesce` or `rb_fragments` |
| `patches/ti-bandgap-unreliable-info.patch` | with `OMAP3_THERMAL` the probe prints "This OMAP thermal sensor is unreliable. You've been warned" at warning level on every boot, a boot-console defect | demotes it to info, text unchanged. The config half is `enable OMAP3_THERMAL` in `config-changes`: the vendor DTB already has `bandgap@48002524` (`ti,omap34xx-bandgap`) and a `cpu_thermal` zone and `THERMAL`/`THERMAL_OF`/`TI_SOC_THERMAL`/`TI_THERMAL` were `=y`, but without it `ti-bandgap.c` has no OMAP3 match entry and the zone read failed EINVAL; no DT edit [measured, `.188` 2026-10-05] |
| `dts/rw20.dts` (`usb_otg_hs`) | `power` is `0x32` in the vendor DTB; `omap2430.c` copies it to `pdata->power` and `musb_host_setup()` sets `hcd->power_budget = 2 * power` mA, which the hub driver takes as the root hub budget, so a device asking more than 100 mA is refused (`insufficient available bus power`). The DTS sets `0xfa` (500 mA) and leaves `mode` 3. **Booted on `.188`** (uImage md5 `45b05f77…`; undo `.f101vbusundo`, `72683498…`): `/proc/device-tree` `power` reads `0xfa` (measured); an Xbox 360 pad plugged directly, no hub, enumerated as `input1` with no `insufficient available bus power` line in `dmesg` (operator, 2026-10-06, n=1). That the budget is what the hub code uses is **[inferred from source]** (only a `dev_dbg` prints it) | reading `musb_host.c` and `omap2430.c`; `/proc/device-tree/ocp@68000000/usb_otg_hs@480ab000/power` |
| `dts/rw20.dts` (`power` under `twl@48`) | `poweroff` is a halt: the vendor `twl@48` has no power child, so `drivers/mfd/twl4030-power.c` binds nothing and `pm_power_off` stays NULL (measured, `CONFIG_TWL4030_POWER=y`). The DTS has a `power` child (`ti,twl4030-power`, `ti,system-power-controller`): the probe installs `pm_power_off` (writes `PWR_DEVOFF`) and, with the plain compatible, loads no sequence scripts. **Booted on `.188`, powers down** (result above) | reading `twl4030-power.c`; `twl4030_power_off` in `/proc/kallsyms` with nothing bound |
| `dts/rw20.dts` (`mmc1` `disable-wp`, `pmu@54000000` `interrupt-affinity`) | two boot warnings: `mmc0: host does not support reading read-only switch` (the slot has no write-protect line; `disable-wp` sets `MMC_CAP2_NO_WRITE_PROTECT`, `drivers/mmc/core/host.c:272`) and `hw perfevents: no interrupt-affinity property` (`drivers/perf/arm_pmu_platform.c:135`; `<&cpu>` resolves to `cpu@0`). **Booted on `.188`** as `uImage-b68` (md5 `1ea74309…`) through `uEnv.txt` `kernel=`, undo `uEnv.txt.b68undo`: both lines gone, both properties in `/proc/device-tree` (measured 2026-10-15) | the boot log; `/proc/device-tree` |
| `dts/rw20.dts` (LAN9221 node, `gpmc,*-ns`) | Ethernet was bus-bound: the vendor GPMC CS5 timings (cycles 155 ns, turnaround and cycle-to-cycle 75) leave the 16-bit PIO access slow. The DTS programs CONFIG2/4/5/6 = `00090901` / `09020902` / `04080C0C` / `800002C2` (6.024 ns tick, 166 MHz **[inferred]**; the driver rounds ns up). Same registers written by `devmem` on a running unit: RX 5.0-5.7 -> 7.2-9.0 MB/s, TX 3.4 -> 4.8 MB/s, no errors (measured, one unit). Datasheet margins (DS00002416A tables 6-3, 6-7): asserted 42 vs 32 ns, cycle 72 vs 45, deasserted 24 vs 13, read data 48 sampled vs 42 needed (tightest, 6 ns). **Booted on `.115` 2026-10-31: CONFIG1-7 read back exactly as above** (measured); a 32 MB copy on that boot: RX 6.4-7.5, TX 5.6-5.7 MB/s, eth0 0 errors and 0 drops | the register A/B; `drivers/memory/omap-gpmc.c`; LAN9221 datasheet |

**The panel patch, in detail.**
- `/display` is `compatible = "panel-dpi"` with `enable-gpios` = pwrdn and a `panel-timing` node
  carrying the vendor's numbers and polarity.
- The lvds and backlight lines are held high by `gpio-hog` nodes on gpio1.
- The LCD pinmux is a pinctrl hog on its pad controller, not a property of `/display`, so it applies whether or
  not a panel binds.
- ⚠️ **Booted and lit on `.188` (measured); the power sequence below has shown no visible fault yet.** It diverges from the vendor driver in two ways, both **inferred** from its
  disassembly. The vendor raises pwrdn → lvds → backlight only after DPI enable plus 50 ms; the hogs
  raise lvds and backlight at gpio probe, and `panel-dpi` adds no delay. And blanking can drop only
  pwrdn. If the panel starts badly, the fallback is a ~250-line clone of `panel-dpi` that drives all
  three lines with the vendor's delays.

## Spectre

The SoC core is a Cortex-A8 r1p7 (measured), affected by Spectre v1 and v2 only (inferred, ARM's table). Vanilla
4.14.52 already invalidates the BTB on a context switch (`cpu_ca8_switch_mm`, `proc-v7-2level.S:44-47`, in our
`System.map` — measured). The vendor kernel is at parity: `vendor.kallsyms` has `cpu_v7_btbinv_switch_mm` only,
no `harden_branch_predictor` or `cpu_v7_ca8_ibe`, and `vendor-Image` has no "spectre" string (measured). ⚠️ **The
flush does nothing unless `ACTLR.IBE` is set.** The kernel's IBE write (`__ca8_errata`) is compiled out under
`ARCH_MULTIPLATFORM`; on a GP OMAP3 `ACTLR` is writable through the ROM SMC (`r12=3`, as `sleep34xx.S:471-473`
does). **`ACTLR` is `0x000000e2` on `.188` under our image** — IBE, L1NEON, DBSM, L2EN — read with
`drivers/ca8_ibe` [measured, one boot on one unit], so the flush is effective; the bootloader sets it (reset value is
`0x2`, `0xE0` is U-Boot's `omap3_setup_aux_cr` [inferred]). It survives idle (C-state 5 +354 entries in 65 s, `ACTLR`
unchanged) and the OFF states C4/C6/C7 were never entered [measured]. The GP ROM call is `r12=3`, `r0=value`,
`smc #0`; its write path is untested on hardware, so `ca8_ibe` stays an instrument, loaded by hand and not at boot
(`build-modules.sh --deploy` copies it to `extra/`, where nothing loads it). The ARM32 v1/v2 series reached 4.14.77
upstream (inferred).

## Reading the vendor kernel

The vendor source is not available, and that has not mattered. The vendor's `/proc/kallsyms` is compared with
our `System.map`, each function's size being the distance to the next symbol. Functions whose size differs, or
that exist only in the vendor image, are what it patched; each is disassembled from the decompressed vendor
`Image` with its `bl` targets and literal-pool strings resolved. That found the Ethernet reset pulse and the
whole panel driver: timings, GPIO order, delays, and the signal polarity sysfs never exposed.

The tools are scratch-grade and live outside the repo, in `C:\work\rw-scratch`: `fsize.py` for sizes,
`calls.py <fn> <vendor_size> <our_size>` for the call-sequence diff, `dis.sh`, and `sharp_dis.py`. Their
data files are `vendor-Image` and `vendor.kallsyms`. `led_pwm_probe` is read: the vendor adds a u32 `brightness`
DT property and applies it at probe, which vanilla hard-codes to `LED_OFF`. Still unexamined: `fb_find_logo`.

## Mainline and omapfb

**Mainline still carries `omapfb`** — **measured** 2026-10-05 by reading torvalds/linux master on GitHub, not on a
device. `drivers/video/fbdev/omap2/omapfb/` is present with its own `dss/` and `displays/` (`omapfb-main.c`,
`omapfb-ioctl.c`, `omapfb-sysfs.c`, `vrfb.c`); `FB_OMAP2` depends on `FB`, `GPIOLIB` and `DRM_OMAP = n` (exclusive
with `omapdrm`) and carries no deprecation text; `FB_OMAP2_DSS_DPI` defaults y, the parallel panel this board uses.
The earlier "removed during 5.x" claim was wrong; the likely origin is the upstream split (~4.15) giving omapfb a
private `omapdss` copy **[inferred]**. The DRM/KMS objection in `SYSTEM_ANALYSIS.md` ([Kernel policy](../SYSTEM_ANALYSIS.md#7-kernel-policy))
therefore applies to `omapdrm` only, which nobody has to use. Staying on our 4.14 build remains the operator's
decision; a mainline-omapfb kernel is **unevaluated**.

**Unproven on this device (all inferred):** the panel lights under mainline omapfb with a DT panel node; the runtime
32/16bpp switch and the overlay sysfs still behave; the vendor U-Boot boots a modern zImage+DTB; the RAM footprint
fits 234 MB.

**Would gain (inferred):** security and network fixes (4.14 is EOL, we hand-backport); a much newer Bluetooth stack
(8761CU native from v6.19, see *Bluetooth modules*), possibly relief for the L2CAP lockdep and 8BitDo-reconnect issues
(unverified guess); newer USB/xpad.  **Would cost:** porting
`drivers/cy8ctmg120_ts`, redoing the twl4030 power-node and DT changes, rebuilding every shipped module, a larger
image, some speed on a 600 MHz core; likely several sessions.

**Cheapest first step:** boot the newest LTS with omapfb on a spare card, asking only whether the panel lights and
the bpp switch works.

## Drivers still missing from our image

| Function | State | Route |
|---|---|---|
| Panel | **works — measured 2026-09-23 on `.188`**: at boot fb0 is `800x480` 32bpp with 1536000 B, the backlight comes up at 100 with Tux on the glass, and a full 32bpp frame of noise written to `/dev/fb0` fills the whole panel | `dts/rw20.dts` and the two patches above |
| Touch | **works, with type-B multi-touch — measured 2026-09-23 on `.188`**: launcher paging, tile taps and Brick Breaker respond (operator); `ABS_X`/`ABS_Y` `0..4095`. Two MT slots — slot 0 is X1/Y1, slot 1 is X2/Y2, each active while the burst's count byte covers it — plus `INPUT_PROP_DIRECT`; the legacy `ABS_X`/`ABS_Y`/`BTN_TOUCH` follow slot 0. **Slots are reported by hand**, because `input_mt_sync_frame()`'s pointer emulation emits `BTN_TOUCH` *before* `ABS_X`/`ABS_Y`, the wrong order for `touch_input.c`. An evdev capture with two fingers (operator) showed both slots streaming with distinct tracking IDs; lifting the second finger ends only slot 1. The driver works around three chip behaviours ([§3.3](../SYSTEM_ANALYSIS.md#33-touch)), each verified by the operator on the panel 2026-09-23: a **count-0 burst with a real X1** is a finger joining or leaving, skipped (≤10 in a row), so adding a finger no longer releases the first; **X2's unresolved marker** holds the slot's last X on a one-finger frame, or 4095 if it has none — the blue dot now always shows; **the chip is reset after every two-finger touch** (`reset_after_mt`, default on, ~120 ms blind), without which later touches never resolve X2. **Two-finger positions come from the electrode profiles** ([§3.3](../SYSTEM_ANALYSIS.md#33-touch)), read in the same I2C transfer as the burst: `cy8_peaks()` takes the two tallest peaks per axis at their 3-point centroid, and `cy8_axis()` uses the chip's own value only where a peak confirms it to within one electrode (it can be stale), holds a fading end as a lifting finger, and flags ends within 2.5 electrodes as *near*. `cy8_pair()` then picks the corner pairing closest to each slot's velocity prediction, with peak-height mismatch as the tiebreak; a new scan is keyed on the source the frame uses (profile for two fingers, burst for one), and while an axis is *near* the prediction coasts on its velocity for up to `DR_MAX` scans. **Operator, 2026-09-24 on `.188`:** X and Y swaps track through the crossing, including a pause at the crossing; a two-finger hold never swaps; lifting either finger keeps the other in its slot. ⚠️ **Two inputs remain undecidable, not buggy:** both fingers landing within one 60 ms scan with equal pressure (a coin toss, and it lost in the logged case), and a path that *reverses* at the crossing (a D and a mirrored D), which coasting reads as a crossing. Parameters `debug` (one line per changed burst, with the pairing `k`, a `nx`/`ny` near flag, both slots and velocities), `regdump` + `regdump_len` (registers 0..`regdump_len`−1, up to 255, in 32-byte rows); read `dmesg`, not `/proc/kmsg`, which the device's log daemon also drains — a `dmesg -c` loop to a file survives a long capture. `ABS_PRESSURE` is deliberately absent. Out-of-tree `.ko` in `/lib/modules/4.14.52/extra/`, loaded at boot by `device-files/touch-module` (`S31`; written 2026-09-24, **not yet run on a unit**) only while I2C client `2-0003` has no driver, so on the vendor kernel, whose `panjit_ts` owns it, the script is a no-op; `build-modules.sh --deploy <ip>` installs it and reloads it. `rmmod` and a hand `insmod` with debug parameters still work for a debugging session | `drivers/cy8ctmg120_ts/` via `build-modules.sh`; handshake and register map in [§3.3](../SYSTEM_ANALYSIS.md#33-touch); `tools/i2c_touch_read.c` is the witness |
| USB host | **works with `USB_INVENTRA_DMA` — measured 2026-09-27 on `.188`**: the C-Media `0d8c:0014` dongle enumerates at boot as ALSA card 1, directly or behind the Terminus `1a40:0101` hub, and `aplay -D plughw:1,0` plays (operator heard it); the vendor-built `snd-usb-audio`/`xpad` modules load. **Why a backend must be chosen:** the vendor config sets neither `MUSB_PIO_ONLY` nor a DMA backend, so `omap2430_ops` has no `.dma_init` and `musb_core.c:2264-2268` fails the probe with `-ENODEV` — the same defect `enable-usb-host.sh` stubs over on the vendor kernel, whose addresses our image does not have (hence its `Struct verification failed`, harmless: it exits early once `usb1` exists). ⚠️ **Without `musb-release-mstandby-on-probe.patch` DMA streams until the first `usb-host recover`, then never again** (measured, `.188`): after unplug, replug and `recover` the IRQ freezes and the settings app hangs until ALSA's write timeout. ⚠️ **`usbcore.autosuspend=-1` stays on the cmdline as a defence.** Its failures were in `a_idle` during `recover` (measured on `.188`, 2026-09-27, same DMA image, only that parameter changed): at the default `2`, `recover` with hub + dongle lost the dongle or found it only on attempt 3, amid a 638/783-line `musb_bus_suspend … a_idle while active` burst; at `-1`, attempt 1 both times with no burst. On the ID-ground image one run at the default was clean (2026-09-29, n=1, emulated at runtime via `/sys/module/usbcore/parameters/autosuspend` and an adapter cycle, not booted with it): with HID devices and `xpad` bound hubs never suspend; a non-hub child such as the audio dongle keeps `power/control=on` and so its hub awake; an empty hub left 30 s suspended with `usb1` (mode `a_suspend`), and the dongle plugged into it woke it by remote wakeup, enumerated and played, with `musb_bus_suspend`/`unhandled DISCONNECT` at 0 throughout. Operator stress test, same day, n=1: a keyboard with its own hub and touchpad, the audio dongle, a USB-powered speaker, an Xbox 360 pad and a 2.4 GHz receiver all worked through one hub, all vanished when the hub was pulled and all returned when it was replugged, with no burst. The musb `mode` sysfs file can read `a_idle` while a device streams — not a health signal | `kernel/config-changes` |

Building `tools/i2c_touch_read.c`: `arm-linux-gnueabihf-gcc -O2 -static -o i2c_touch_read
i2c_touch_read.c`, then run `native_apps/check-arm-safe.sh` on the result, as for any device binary.
The device has no `i2cget`/`i2cdump` (measured 2026-09-21), hence the tool. ⚠️ **`scp` does not preserve the exec
bit**: `chmod +x` on the device, or the run dies with *Permission denied* into a redirect and reads as an
empty result. ⚠️ **Stop whatever owns the screen before an `--evdev` witness**: `app_launcher` may hold an
`EVIOCGRAB` on the touch node, and a grabbed device silences the witness exactly like a finger that never landed.
Building the drivers, after `build-image.sh`: `wsl.exe -e bash -lc "cd /mnt/c/work/roomwizard && kernel/build-modules.sh --out /mnt/c/work/rw-scratch/ko [--deploy <ip>]"`.

⚠️ **Any change to an image patch means rebuilding both module sets.** `MODVERSIONS=y`, and the TCP patches change
`struct netns_ipv4`, so exported CRCs move: the touch `.ko` and the Bluetooth set refused to load (`disagrees about
version of symbol`) until rebuilt against the same tree [measured, `.188`]. Touch + 18 BT modules rebuilt for the thermal image (BT build vermagic/CRCs PASS); old ones on `.188` at `/home/root/s1200-f4undo/`. `build-modules.sh` and
`build-bt-modules.sh` take `--image <build-image --work dir>`; the default `~/rw-kbuild-image` was rebuilt with all
patches and its `Module.symvers` is identical to the booted image's [measured]. The `usb_host` modules (xpad,
snd-usb-audio, …) carry no `__versions` at all, so they load regardless and nothing checks their ABI [measured].
⚠️ `bluetooth/build-and-deploy.sh` rebuilds modules only when a recipe or patch is newer than `load-order.txt`, so a
new image does not trigger one, and it checks `~/rw-kbuild-image` (~line 168) [inferred from source].
