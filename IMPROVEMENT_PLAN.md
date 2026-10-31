# Improvement Plan

**Open work only.** Finished items are not listed here — the code and `git log` are the record.

**How to read this**

- **B**n = bug, **F**n = feature, **D**n = doc/infra, **C**n = cleanup. IDs are never reused or
  renumbered, because commit messages cite them. ⚠️ **Nothing outside this file may cite one** — an entry
  is deleted the moment it closes, so any citation elsewhere has a scheduled expiry built in
  (`tests/doc_check.sh` group B counts them, resolving or not). To find a closed one: `git log --grep=B13i`.
- **Status is one word after the heading:**

  | Status | Means |
  |---|---|
  | `open` | Nothing has shipped. Found by reading the code; not reproduced on the device. |
  | `open, confirmed <date>` | Reproduced — on the panel or by running the command. Still unfixed. |

- **A recorded cause and a prescribed fix are both hypotheses.** `open, confirmed` means the
  *symptom* reproduced and nothing more. Reproduce it, then find the cause yourself; and when an
  entry says "compare with X", read X. See `CLAUDE.md` → *Working style*.
- Kernel work is in scope; the image build is F101. What stays excluded is in [Out of Scope](#out-of-scope).

**Before starting anything, read [`SYSTEM_ANALYSIS.md`](SYSTEM_ANALYSIS.md) §1 — *Read this first*.**
Device facts live there; this file holds only what we intend to *do* about them.

---

## Needs a human at the panel

**Nothing is outstanding.** Past an app's first screen a check is scripted with `arca_tap` (`CLAUDE.md` → *Non-obvious
constraints*); what still needs a human is what an eye or a finger judges, and panel time is the project's scarce
resource: price the check before requesting it, group requests into **one checklist**, split an item when
only part of it is gated, and record the answer with the confidence it was given — "I think it works" is a
hedge, and a verdict is a weaker answer than a description, so say which one the next item needs.

---

## Correctness and verification

### B30. `brick_breaker` hides lives past the ninth — open, latent, cosmetic

Three games draw a capped HUD lives row and each caps it differently:

| Game | Shape | Verdict |
|---|---|---|
| `frogger.c:1269` | `lives_shown = min(lives, 5)`, then lays out from `lives_shown * LIFE_ICON_PITCH` | correct |
| `platformer.c:1541` | lays out from the capped number and draws one icon plus `x10` past the cap | correct |
| `brick_breaker.c:1398` | `for (i < game.lives && i < 9)`, each heart at a fixed anchor minus `i * 14` | **silently truncates** |

The extra-life power-up at `brick_breaker.c:765` does `game.lives++` with no cap, so a tenth life and
every one after it is **invisible**: the HUD reads nine whether you hold nine or fourteen, and losing one
appears to change nothing. Cosmetic, never a crash, and it needs a power-up-heavy run to reach, which is
why it has not been seen.

Fix is the rule the other two follow: cap first, lay out from the capped number, and make the number
appear somewhere once it exceeds what is drawn — a heart plus `x10`, or a raised cap.

### B41. An adapter replug can print `configured as A device timeout` on our image — open, seen once 2026-09-29

**Measured on `.188`** (uImage md5 `f3b446c6b2d731e0d118583cada62c36`, the ID-ground patch booted):
`musb-hdrc musb-hdrc.0.auto: configured as A device timeout` was printed once, at one adapter replug —
the one following a spell in which the root hub had been runtime-suspended; the earlier replug did not
print it. Enumeration succeeded anyway. The line comes from `omap2430_musb_set_vbus()`, which waits for
DEVCTL `BDEVICE` to clear — now reached on every ID-ground, since the patch calls it there. **Read from
source, not timed on the device:** upstream's loop is 100 × `mdelay(5)`, so it gave up after ~505 ms, before
the 1 s jiffies deadline it appeared to have, then carried on as on success. Console error lines are defects
even when harmless. **[inferred]** that the prior root-hub suspend is the difference; n=1. **Watch:** the
message now reads `configured as A device timeout: devctl %02x after %lld ms[ (irq)]` (`kernel/patches/musb-omap2430-set-vbus-report.patch`),
so the next occurrence measures itself; read the devctl value and elapsed ms there before reproducing, then
decide whether the wait needs the PHY/glue resumed first or the loop is simply too short.

### B47. Lockdep reports recursive L2CAP socket locking on the first incoming BT connection — open, seen twice (2026-10-01, 2026-10-05)

**Measured on `.188`:** at the 8BitDo pad's first incoming connection after boot the kernel printed `WARNING:
possible recursive locking detected` — `sk_lock-AF_BLUETOOTH-BTPROTO_L2CAP` taken twice, `l2cap_sock_new_connection_cb`
→ `bt_accept_enqueue` → `lock_sock_nested`, from `l2cap_connect` / `hci_rx_work`. It did not reappear on the later
reconnect. **Second sighting 2026-10-05, ~60 s after boot, during incoming BT connections:** the same splat in
`hci_rx_work` → `l2cap_recv_frame` → `l2cap_connect` → `l2cap_sock_new_connection_cb` → `bt_accept_enqueue` →
`lock_sock_nested`, held `conn->chan_lock`, `chan->lock#2/2` and `sk_lock-AF_BLUETOOTH-BTPROTO_L2CAP`, kernel 4.14.52
tainted `O` (our out-of-tree modules). Console warnings are defects even when harmless. **Measured 2026-10-12, `/proc/config.gz` on `.115`:** `CONFIG_PROVE_LOCKING`, `LOCKDEP`, `DEBUG_LOCK_ALLOC`, `DEBUG_MUTEXES` and `DEBUG_SPINLOCK` are all `=y`, and the syslog there carries the same lockdep trace; these are debug options with a CPU cost on a 600 MHz core. Whether they come from the vendor config or from us is unknown: none of them is named in `kernel/config-changes` (the base config is `usb_host/device_config`, tracked, so `grep` it to tell).
Cause not investigated;
**[inferred]** candidate: the 4.14 parent/child L2CAP socket false positive that upstream later annotated with a
nesting subclass. **Next:** read the upstream change to
`l2cap_sock_new_connection_cb`; decide between backporting the annotation and dropping lockdep from the image.

### B52. The 8BitDo Pro 2 does not reconnect by itself after a bluetoothd restart — open, confirmed 2026-10-03

**Measured, `.188`, `btmon`.** (1) `/etc/init.d/bluetooth restart` (`device-files/bluetooth`: stop kills `bluetoothd`,
then `rmmod`s every BT module; start reloads them) disconnects the pad with reason 0x15 "Remote Device Terminated due
to Power Off"; page scan is off for 8.2 s, then Page Scan on and every paired device is re-added "allow incoming". The
pad blinked its reconnect LED yet sent **zero** Connect Requests in ~45 s, then powered off; 2 of 2 runs. (2) A
`bluetoothd`-only restart with the modules kept: page-scan-off window 0.73 s, still zero Connect Requests in 65 s — so
the dead window is **refuted** as the cause. (3) After either, turning the pad off and on (Home) connects within 1 s:
Connect Request accepted, stored link key, E0 encryption, HID L2CAP channels; it is paired, bonded and trusted with the
key loaded. (4) A host-initiated `bluetoothctl connect` while the pad blinked failed with
`org.bluez.Error.Failed br-connection-create-socket`; the HCI reason was not captured (that `btmon` capture came back
empty). **Inferred, not measured:** the pad backs off after the 0x15 reason, and the same mechanism sits behind the
reconnect-on-Home-press behaviour after a reboot recorded in `SYSTEM_ANALYSIS.md`. Only a `bluetooth` component deploy
or a reboot triggers it; the operator workaround is to power-cycle the pad.

**Next steps.** Capture the host-initiated page with `btmon` (Create Connection status, page timeout) while the pad
blinks; if host paging works, have the init script or a `bluetoothd` policy connect trusted HID devices after start;
check whether `bluetoothd` can stop without powering the adapter off, so no 0x15 is sent. **Done when** a restart
leaves the pad connected, or the pad's behaviour is attributed and the workaround documented.

### B57. Does MADC ADCIN6 follow what is plugged into the USB port? — open, n=1, needs a joint session (operator replugs while ch6 is read)

`in_voltage6` read 26-29 mV on `.188` while an Xbox 360 pad was on the USB port and a steady 344-364 mV after the BT dongle replaced it
([`SYSTEM_ANALYSIS.md#311-adc-and-temperature-twl4030-madc`](SYSTEM_ANALYSIS.md#311-adc-and-temperature-twl4030-madc)); the two states
differ in more than the device, so the link is unproven. It needs a joint session (operator confirmed 2026-10-07): the operator replugs while ch6 is read. **Done when** an operator swaps pad and dongle back and forth on the port
(reading `in_voltage6_input` after each swap, and once with the port empty) and the table says which condition, if any, moves it.

### B68. The syslog shows red lines at every boot — open, operator-observed 2026-10-12

Boot-console errors are defects here. Every line the current image logs (`grep -E '\.(err|warn) ' /var/log/messages` on `.188`, 2026-10-15) is fixed in the tree or recorded as benign in [SYSTEM_ANALYSIS.md](SYSTEM_ANALYSIS.md#7-kernel-policy), except the ones below. **Not yet in an image:** the two `rw20.dts` warnings are fixed but booted only on `.188` as `uImage-b68` (`kernel/README.md`); the rc0/rc6 K links come with the next rootfs build (made by hand on `.188`). **Open:**

1. `twl4030_madc: supply vusb3v1 not found, using dummy regulator`. The fix at cause is `&twl_madc { vusb3v1-supply = <&vusb3v1>; };`, but MADC would then hold the LDO the USB PHY driver also switches on permanently, and ADCIN3..6 readings may change (B57) [inferred from `twl4030-madc.c:889`]. **Operator's call** before it is tried.
2. bluetoothd `Failed to reset Adv Monitors: Unknown Command (0x01)`: BlueZ 5.66 sends `MGMT_OP_REMOVE_ADV_MONITOR` on every index-added (`src/adapter.c` `reset_adv_monitors`) and our kernel's mgmt 1.14 lacks it. The fix is a BlueZ patch gating it on the supported-commands list, the way `read_commands_complete` sets `KERNEL_*` flags; `bluetooth/build-bluez.sh` has no patch step yet, and a patched BlueZ must reach the source offer. **Operator's call.**
3. `snd_hwdep: no symbol version for module_layout`: cause not established. First step: `grep -a module_layout` on the deployed `snd-hwdep.ko` and compare with the build that made it.
4. avahi `No NSS support for mDNS detected`: `BR2_PACKAGE_NSS_MDNS=y` silences it at a rootfs rebuild; the unit publishes and never resolves `.local` itself.

**Done when** each line is removed at its cause, or recorded as benign with the reason.

### B69. Does the SD slot's card-detect switch work, and is it on the pad the DT names? — open, unverified

`kernel/dts/rw20.dts:550` has `cd-gpios = <&gpio4 31 GPIO_ACTIVE_LOW>` (gpio_127, the vendor's value, kept) while the pad it muxes as a GPIO (`rw20.dts:278`) is `sdmmc1_dat4` = gpio_126. **[unverified]** Whether the switch is read at all has not been checked; the card boots, so nothing observed depends on it. **Done when** a card insert and removal is observed in the kernel log, or the property is shown to be inert.

### B74. The 8BitDo pad does not reconnect after it powers itself off for inactivity (`.115`) — open, reported 2026-10-27, not reproduced

**Reported, not measured** (operator, unit `.115`, no capture). The 8BitDo gamepad (Bluetooth) turns itself off after inactivity; turning it on again does not reconnect it. Two workarounds work: reboot the unit with the pad in re-pairing mode, or remove the device in the Bluetooth settings, search again and connect. Pressing Pair on the existing entry gives a Bluetooth error (text not captured). The pad model on `.115` is not confirmed to be the 8BitDo Pro 2 of B52. Unlike B52, where a pad power-cycle reconnects within 1 s after a `bluetoothd` restart, here the power-cycle after idle-off is what fails. **No cause is established.**

**Next steps.** On `.115`, capture `btmon` during the failing power-on and during the failing Pair. Record `bluetoothctl info <pad MAC>` (Paired, Bonded, Trusted, link key present) before and after the idle-off. Confirm the pad model and compare the capture with B52's. **Done when** the failing power-on is attributed by capture, or the workaround is documented.

### B75. A USB overload drop: recovery and indicator built, the real-drop and BT-pad checks are left — open

**Built (measured 2026-10-31 on `.115` with a simulated drop: the hub's `authorized` set to 0 and a `VBUS_ERROR` line written
to `/dev/kmsg`):** `arcamond` reads new `/dev/kmsg` records; a `VBUS_ERROR` line arms a check at 5 s, and if no non-hub
device is then enumerated it latches `MON_FLAG_USBFAULT` and runs `/etc/init.d/usb-host recover` at 5, 15 and 45 s, then
every 120 s with no end, clearing at the first non-hub device (decision: `native_apps/common/usbfault.h`). While latched,
`framebuffer.c` paints a 16×16 red square top-left and 6 px red sidebars, three 200 ms flashes every 10 s. The hub came back
at ~9 s and the indicator cleared. The `$MUSB/mode` file is no detector: it reads `a_idle` with the hub, a USB audio card and
the BT dongle all working. `device-files/bluetooth` `watch()` now sees each new `hci0` (it used `stat`, which BusyBox here
lacks, so it never fired) and retries bonded devices at 5, 15, 45 s and three 120 s rounds, starting over on a newer instance.
**Not measured:** a real overcurrent drop; a BT pad actually reconnecting after one; the indicator at 16 bpp (ScummVM, VNC)
and in the retro cores; the sidebars flash only when the app presents (the launcher ~1 Hz).
**Defaults chosen with the operator away, to overrule:** no letter in the square; 6 px bars, 3 × 200 ms per 10 s; retries
never stop and the indicator stays; a recover does not wait for the launcher (the hub is already gone); no Control Panel
"powered hub" line; the BT window ~7 min.
**Done when** on `.115`, after a real drop, the hub returns without a replug, square and bars show during the fault and
vanish after, and a previously connected BT pad reconnects without a power cycle.
**Operator test 2026-10-31 (`.115`, a real overload):** recover brought the port back at once and the load
tripped it again in a loop; a BT pad reconnected ~20 s after the load was pulled. **Built since:** 3 errors in
30 s → `usb-host off` + 120 s retries, red LED flashing while latched (simulated on `.115`, not yet a real overload).

### B76. A plain `reboot` can hang in the shutdown scripts, and `provision.sh` reboots that way — open, seen once on `.115`

**Measured 2026-10-31 on `.115`, n=1:** two `reboot` calls over SSH left the unit up; `/etc/init.d/rc 0` was asleep in
`wait_woken` with fd 0–2 on `/dev/console` (a VT, 100×30, not the serial port), so the K scripts never ran (`sshd` still up).
**Inferred:** a write to the console blocked. `sync; reboot -f` booted. `provision.sh` ends with a plain `ssh … reboot`,
so an update can silently not take effect. **Next:** reproduce, read which K script or `rc` line writes, and why a VT write
blocks (console scrolled-lock? `fbcon` blanked?). **Done when** a plain `reboot` reboots `.115` five times in a row from the
launcher state, or `provision.sh` detects a reboot that did not happen. **Not reproduced** in 4 tries the same day (idle, just
booted, during `rc 5`, two `reboot`s at once). Next capture before `reboot -f`: `/proc/<rc pid>/stack`, `stty -F /dev/console -a`
and `stty -F /dev/ttyO1 -a`; `rc` echoes to the console (serial `ttyO1` + `tty0`) before each K script [inferred first write].

## Features

Userspace except F101, which is the image build.

---

### F101. Build our own kernel image, rebased onto 4.14.336 — open

**The deliverable is a `uImage` we compiled, staged on p1 beside the vendor's, on 4.14.336 — the final 4.14 release
(4.14 has been EOL since Jan 2024, so no newer one exists; a mainline kernel is unevaluated, see the kernel notes).** It brings every
stable fix, including **FragmentSmack CVE-2018-5391** (a large `inet_frag` rework, not patched on .52; the stop-gap
`ipfrag_high_thresh=262144` / `ipfrag_low_thresh=196608` is not applied). **Measured:** all 9 of our patches dry-run
clean on 4.14.336 (one offset); the 12 `tcp-*` patches are redundant there; 3 of the 5 `patches-modules` (KNOB, HIDP
length, `remote_efs`) are in .336, the two RTL8761CU ones still apply (`btusb` needs fuzz 2, refresh);
`CPU_SPECTRE`/`HARDEN_BRANCH_PREDICTOR` exist there and .336 prints the IBE state at boot. **Risks (measured diffs):**
`omap2430.c` drops `.set_vbus` from `omap2430_ops` (changes the SESSREQ/port-power VBUS path and the IRQ branch of our
set-vbus-report patch), and `musbhsdma.c` has a TX-completion rework (our audio DMA path). `4.14.52` is pinned in
`kernel/build-image.sh:18`, `build-modules.sh:15,21`, `build-bt-modules.sh:17,83`, `usb_host/build-kernel-modules.sh:16`,
`usb_host/build-and-deploy.sh:56`, `bluetooth/build-and-deploy.sh:43`, `device-files/bluetooth:29`, `touch-module:8`,
`usb-audio-modules:18`, `xpad-modules:8-10` (use `uname -r`), `provision-rules.conf:118,128` and the `release.sh` licence
text. **Rebase steps, stepwise:** (1) generalise the module paths to `uname -r`; (2) build .336 with our 9 patches and
diff `dropped-symbols.txt`; (3) boot it on `.188`; (4) re-run the USB and audio checks; (5) retire the redundant
patches. With vendor-kernel support dropped (see the cleanup item of that name) no second `.52` module set is needed.
**Effort [inferred]:** about 1 h for the image, 0.5-1 day for the scripts, 1-2 operator sessions.

**The image work:** the policy and the
standing costs are [§7](SYSTEM_ANALYSIS.md#7-kernel-policy); this entry is the work. The image boots, takes
DHCP and answers SSH, and the panel works ([`kernel/README.md`](kernel/README.md) holds the build, the patches
and the p1 files). What is left is building the touch driver into the image, the fbcon cursor and boot
console, and one unexplained dmesg line.

**What the image is for — the payoff is deployment stability, not speed.** A kernel compiled here ships
with its own corresponding source and can go in a release, which gives back the free undo both bring-up
paths lost. The vendor-kernel byte-patch path is deleted (tag `last-vendor-kernel`), so the release
supports only our image. ⚠️ **That is not delivered until an image we built is the one a unit is
deployed on** — panel and touch working.

**What to fold in, so the image is built once with everything wanted in it:**

| Wanted | Change | Note |
|---|---|---|
| Touch | finish `kernel/drivers/cy8ctmg120_ts/` — single- and multi-touch work on our image as a `.ko` from `kernel/build-modules.sh`, loaded at boot by `device-files/touch-module` ([`kernel/README.md`](kernel/README.md) has its state), handshake in [§3.3](SYSTEM_ANALYSIS.md#33-touch) | Open: **(iii) pressure** — test a profile peak-height sum against a light/firm press, the columns and rows being mapped ([§3.3](SYSTEM_ANALYSIS.md#33-touch)); **(iv) calibration accuracy on our driver** — an operator check of the corners; it is unchecked beyond "taps land on tiles" |
| fbcon cursor | `vt.global_cursor_default=0` via `CONFIG_CMDLINE_EXTEND` | permanently off. U-Boot's bootargs stay untouched — they cannot be persisted |
| Boot messages on the panel | append `console=tty0` **last** in the same `CONFIG_CMDLINE_EXTEND`, so the panel is `/dev/console` (operator's choice) | ⚠️ **Resolve the hazard first — measured by code search:** no app sets `KD_GRAPHICS` or touches the VT, and apps `mmap` `/dev/fb0` directly, so once `tty0` is a console any printk at the default console loglevel — the known USB printk loop, say — draws over a running game. **The fix is `KDSETMODE KD_GRAPHICS` in `fb_init()` in `native_apps/common/framebuffer.c`** (operator agreed; every shipped fb program goes through it, so redeploy all three components): open `/dev/tty0` explicitly (apps have no controlling tty), set it unconditionally on every init so a crashed or `kill -9`ed predecessor is repaired, and do **not** restore `KD_TEXT` in `fb_close()` — the launcher closes and re-inits around each child, so that would flash the console; restore it only in the init script's `stop`, via a small helper. A `loglevel=` stays as a second line of defence. The serial getty on `ttyO1` comes from `inittab`, so it is unaffected **[inferred]** |
| Scheduling | `PREEMPT`, `HZ=250` | config-only, and never measured to limit anything — include it, but do not justify the image with it |
| USB gadget mode | `CONFIG_USB_GADGET` | config-only: the micro-B socket is already the one physical port |
| Boot logo | the Arca cuckoo replaces Tux: render `distro/arca-logo.svg` to a ≤224-colour plain PPM and have a `kernel/patches/` entry replace `drivers/video/logo/logo_linux_clut224.ppm` (`LOGO_LINUX_CLUT224` is already enabled in `kernel/config-changes`) | 4.14 draws the logo at the top-left with no centring (`drivers/video/fbdev/core/fbmem.c:503`, `image.dx = 0`), so the bezel can cover its corner. Pad the PPM: a margin, or a full 800×480 frame with the logo centred. The full frame costs ~375 KB of image [inferred: one byte per pixel of `clut224` data]. A U-Boot splash of the logo is possible (`drivers/video/omap3_dss.c` is in v2026.10, but no board uses it) and would come after this row |

**The order to do it in, cheapest first.**

1. **The touch driver, the fbcon cursor and the boot console** — the three rows above, which share one
   image build and one p1 write; the operator has allowed a reboot and a p1 write of `.188` for them. ⚠️ Until the
   module is loaded, `app_launcher` still exits after boot on the missing `/dev/input/touchscreen0` and
   the respawn loop clears fb0 every ~30 s — stop the init script before judging a panel frame.

**Which boot channel — writing an image needs no console.** On our U-Boot an alternate filename needs only a
`kernel=` line in p1's `uEnv.txt`, with `uImage-fallback` behind it (measured 2026-10-15, `.188`); overwriting `uImage-system` needs nothing, and recovery for that is a
card pull plus copying a backup back onto p1
([`#4-boot-chain-and-recovery`](SYSTEM_ANALYSIS.md#4-boot-chain-and-recovery)). The operator accepts the
overwrite ("I can re-flash easily"), which does not retire
[§1](SYSTEM_ANALYSIS.md#1-read-this-first) rule 3: **take a verified p1 backup before the write**, of the
*running* kernel and not merely the pristine vendor one, because they can be different files. The serial console (`P4`, fitted; RS-232 behind `U27`, a MAX3232 breakout ordered for
the operator's TTL cables — [`HARDWARE.md#4-unpopulated-and-expansion`](HARDWARE.md#4-unpopulated-and-expansion))
is off the critical path now that SSH answers, and remains the only channel for an image that does not.
⚠️ **Do not repoint `ctrlblock.bin` at a bootstrap image**: it overwrites two protected files and destroys
the vendor recovery image.

### F135. Boot the newest LTS kernel with omapfb on a spare card — open, operator request 2026-10-05, step 1 only

Mainline still carries `omapfb`, so a newer kernel is unevaluated rather than ruled out; the evidence, the
unproven list and the gains and costs are in `kernel/README.md` → *Mainline and omapfb* and are not repeated
here. **Step 1 only:** on a **separate card**, so `.188`'s card is untouched, boot the newest LTS with omapfb
and a DT panel node, and answer two questions: does the panel light, and does the runtime 32/16bpp switch work
(`fbset`, `/sys/class/graphics/fb0/bits_per_pixel`). Anything past that (touch, Bluetooth, modules) waits on
a yes to both.

### F137. Users other than root — open, operator wish 2026-10-09, not designed

The operator wants root not to be the default user on our root filesystem. Operator's answer, 2026-10-10: a normal user and
root; normal operation runs as the user; a change such as the SSH settings needs root (type the root password) and then runs as
root, like `sudo`. That touches `/dev/fb0`, `/dev/input`, `/dev/dsp`, `/dev/watchdog` and the `arca-app` respawn loop. If it
is too much effort it goes last. The terminal app (`native_apps/terminal/`) runs `/bin/login`, so it grants exactly what the accounts grant. **Done when** it is designed together with the operator and built.

### F145. Update a unit from a bundle — open, operator request 2026-10-10, not designed

Two package kinds: a ready-to-use generic card image (`.img`) and a bundle update. The bundle update installs only what changed
(per component), and the upstream it fetches from is configurable (today `deploy-all.sh --from-release` fixes one). A bundle-based
update normally avoids reflashing, so the unit keeps its host keys. **Done when** a unit updates from a bundle with only the changed
components installed, from a configured upstream.

### F147. Immersive games with no on-screen menu or close button — open, operator wish 2026-10-12, not designed

The operator wants games that show no menu or close button at all. Open design question: how a touch-only player pauses and exits (pad and keyboard keep their keys). The games do not carry the shared top bar, so their own exit X is part of the same question. **Done when** the question is answered with the operator and one game follows the answer.

### F157. Physical keyboard and mouse input in the on-screen keyboard — open, operator request 2026-10-22

`keyboard_enter` (`common/keyboard.c`) now accepts typed letters from a physical keyboard (`kb_apply_key`, `gamepad_text_entry` /
`gamepad_take_keys`), but it is **host-tested only**. **Panel result, 2026-10-22:** with the operator's Bluetooth keyboard that has an
integrated touchpad, the arrows move the on-screen focus but typed letters are not picked up. **Hypothesis, unverified:** the combo
device is classified as a mouse/pointer (`mouse_is_keyboard` in `gamepad.c` decides this), so its letter keys never reach the
typed-key queue; check the device's evdev nodes and which node the letters arrive on before changing code. The on-screen keyboard
also has **no mouse support** (no pointer click on a key). **Done when** typing on that Bluetooth keyboard and on a plain USB
keyboard enters letters, and a mouse click presses an on-screen key, at the panel.

### F160. Does BT/USB audio need the speaker's `>>1` ceiling? — open, operator decision 2026-10-24

`AUDIO_MASTER_SHIFT` (`common/audio_gen.h`, set in `common/audio.c`) halves every output: it is the internal speaker's ceiling,
but it is spent on BT and USB sinks too, which is 6 dB of headroom they may not need. The system volume's top step (+12 dB,
soft-limited) already sounds right on a BT speaker [operator, by ear]. **Next:** an A/B on one BT and one USB sink, shift 1 vs 0
at the same volume step, listening for clipping and limiter pumping. **Done when** the shift per sink class is decided and, if it
differs, set by the output route rather than the app.

### F156. Log page: a Follow setting — open, operator request 2026-10-22, not designed

The Control Panel's Log page (`control_panel/log_page.c`) shows the selected log read-only. Add a **Follow** setting, true or
false, for the selected log: on, the view keeps showing the newest lines as the file grows; off, it stays where the reader
left it. Open design questions: whether the setting is per log or one for the page, and whether it persists across visits.
**Done when** the page follows a growing log with the setting on and holds still with it off, at the panel.

### F106. Support BeagleBone Black boards — open, operator idea 2026-10-01, future

The operator inherited many BeagleBone Black boards (photos: `beaglebone/image/beaglebone-black-board-{1,2}.jpg`). **Measured from the
photos:** TI AM3358 (Cortex-A8, 1 GHz — also no hardware integer divide), TPS65217C PMIC, 512 MB DDR3,
eMMC, SMSC LAN8710A PHY, an NXP HDMI framer to micro-HDMI, USB-A host, mini-USB, microSD, P8/P9 headers.

**Inferred, not built:** our apps need only `/dev/fb0`, evdev and ALSA, so the differences are board-specific —
no touch panel, other LED/backlight/watchdog paths, no speaker GPIO or bezel, 5 V power. That points to a
**board-profile layer** under `common/hardware.c`, `common/touch_input.c` and `provision.sh` rather than
`#ifdef`s. Our binaries are soft-float against the Arca rootfs while stock BBB Debian is hard-float, so
this ties to our own root filesystem (`rootfs/`; one rootfs for both boards). A BBB wants a mainline kernel with DRM fbdev emulation; the
Steelcase RoomWizard stays on 4.14.52 omapfb. **Done when** the first question is answered: which board-specific
paths a launcher plus one game actually touch, listed from a BBB boot.

**Future scope, operator-agreed, not scheduled:** mouse-only operation of the launcher, Control Panel and games, for a unit with no touch and no keyboard or pad. The operator has a RoomWizard whose touch is broken, and a BeagleBone has no touch. The mouse pointer in the launcher and the Control Panel exists and is operator-verified; the games still take no mouse pointer.

### Potential ideas (not planned)

Recorded so they are not lost; none has an owner or a design.

- Meeting or home dashboard: clock, weather and a calendar (ICS) feed, with the red and green LEDs as busy and free, the device's original role.
- Home Assistant or MQTT control panel: buttons on the touch screen, state shown on the LEDs. Operator decision: Alexa integration is not a goal (no public local API).
- Notification display: an HTTP push (for example ntfy) shown on screen with an LED flash.
- Touch drawing board or whiteboard.
- Ambient sound or white-noise generator on `common/audio_gen.c`.

## Structural and cleanup

### C10. Make a deep game state reachable without playing to it — open

`brick_breaker`'s indestructible bricks only exist from **level 5 up**, so verifying them costs a full
play session of somebody's time. A `--level N` argument or a debug entry in the pause dialog turns it into
one launch, and would serve any future level-dependent bug; generalise to the other games where a state is
expensive to reach. **Operator ruling: a test-only command-line switch is acceptable** — the launcher
passes no arguments, so such a flag is reachable over SSH and deliberately not from the panel, which is
what a test entry point wants. `brick_breaker` already has both halves (`--test` and a pause toggle), so
its level-5 problem is open on the level number, not on the mechanism. A pause-dialog toggle (the shape
Office Runner's TRAINING toggle in `platformer.c` uses) is reachable by `arca_tap` taps, so a mode
with no CLI entry is still script-checkable; a CLI switch only makes the deep state cheaper to reach.

### C15. The bare plan-ID scan collides with function-key names — open, measured 2026-09-03

`bare_sites()` in `tests/doc_check.sh` matches an `F`-numbered ID in parentheses or after `see`/`is`/
`was`, and F1-F12 are key names as well as plan IDs here — a ScummVM key-table row such as
`Save/load dialog (F5)` counts as a citation, and it turns into a dangling one the moment an entry of that
number closes. **So the reported citation count carries false positives, and the next F-numbered entry to
close can fail the gate on unrelated documentation.** Narrow the scan rather than excluding a file: a hit
on a line that also carries a key-binding marker (`Ctrl+`, `Alt+`, `Shift+`), or one inside a two-column
key table, is not a citation. ⚠️ Needs a control in both directions — a real bare citation must still fire,
and it must fire in a file of the same kind, or the scan goes blind where it used to see.

---

## Out of Scope

Recorded so the decision is not re-litigated. Anything needing a connector, a populated device or a wire
is out under [`#8-hardware-policy`](SYSTEM_ANALYSIS.md#8-hardware-policy); a kernel config symbol on its own
blocks nothing, since we build the image (F101). Requesting GPL source from Steelcase stays ruled out and
is needed for nothing ([`#7-kernel-policy`](SYSTEM_ANALYSIS.md#7-kernel-policy)).

| Item | Blocked by | Detail |
|------|---|---|
| Enable the two EHCI USB host ports | **Nowhere to plug in** — no second USB connector and no unpopulated footprint on the board, so `CONFIG_USB_EHCI_HCD` is not what blocks this and an image we build gains nothing here | [`#36-usb`](SYSTEM_ANALYSIS.md#36-usb) |
| SPI | Four controllers `okay` in the DT and `CONFIG_SPI` unset, but **nothing is on the bus** — no children are declared, and putting a device there needs a wire | [`#314-what-is-not-present`](SYSTEM_ANALYSIS.md#314-what-is-not-present) |
| Piezo buzzer on TWL4030 PWM | **Needs a wire**, and all 3 dmtimer PWMs are taken; `CONFIG_PWM_TWL` is the cheap half | [`#39-i2c`](SYSTEM_ANALYSIS.md#39-i2c) |
| Mainline 5.x/6.x port | **Standing operator decision to stay on our 4.14 build, not a closed technical verdict:** `omapdrm` would break the runtime bpp switching and the DSS overlay sysfs, but mainline still carries `omapfb` (upstream source, 2026-10-05), which is unevaluated on this device. Building 4.14.52 ourselves (F101) keeps all three intact | [`#7-kernel-policy`](SYSTEM_ANALYSIS.md#7-kernel-policy) |
| Ambient-light sensor / auto-backlight | **No such hardware.** The teardown found no sensor and, decisively, no aperture, window or light pipe anywhere in the enclosure — a sensor would have nothing to sense even if fitted. ⚠️ Do **not** probe for it: `pv02_app 5` can hang I2C bus 1, which carries the PMIC. *Time-of-day* dimming needs no sensor and is still available. | [`#39-i2c`](SYSTEM_ANALYSIS.md#39-i2c) |
| Serial console | Located and pinned out (`P4`), then declined: the recovery loop is *pull the card, reimage, DHCP, SSH*, and since NAND and U-Boot stay untouched the card **is** the entire failure surface. Serial would add boot visibility, not recovery capability. Revisit only if NAND or U-Boot ever get written — or once we are iterating on our own images (F101), where serial is the only channel that shows *why* one failed to boot, though fitting `P4` is itself a board change. | [`#312-serial-ports`](SYSTEM_ANALYSIS.md#312-serial-ports) |

**Note:** enabling **UART3** as a `ttyO2` is *not* in this table — it may be reachable by patching the
appended DTB, which needs no kernel source ([`#312-serial-ports`](SYSTEM_ANALYSIS.md#312-serial-ports)).

⚠️ **Operator ruling, 2026-09-06: no further USB work beyond USB audio.** Enumeration-at-probe is closed
and that is a result rather than a gap — three mechanisms read out of the MUSB driver were each applied
and **refuted on hardware**, and the answer that ships is the one-tap RESCAN, verified on a panel. Before
anyone proposes a fourth theory, read the refuted table in `usb_host/README.md`, which names the one
never-attempted candidate and the question any candidate must answer first. **The operator ruling of 2026-10-15 lifts this
for USB storage** (the file manager and the Backup page); the enumeration-at-probe closure above stands.

**Bundles hold built artifacts only — settled**, because the one consumer that installs device scripts runs
from a clone and has `device-files/` beside it either way.
