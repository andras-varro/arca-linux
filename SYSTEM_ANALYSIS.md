# Steelcase RoomWizard Device Reference

Everything known to be true about the Steelcase RoomWizard as a hardware and software platform:
the silicon, the board, the boot chain, the OS, and the traps. Verified on real units — where
something is inferred rather than measured, it says so.

**This document answers "what is true?".** It does not argue for changes. Open work, bugs and
feature proposals live in [`IMPROVEMENT_PLAN.md`](IMPROVEMENT_PLAN.md); the short must-know set
loaded into every session lives in [`CLAUDE.md`](CLAUDE.md). One fact, one home — if you find
something stated in two places, the other copy is the stale one.

Each subsystem below follows the same shape, so you can skim to the part you need:

| Field | Contains |
|---|---|
| **What's there** | parts, addresses, device nodes |
| **How to drive it** | the path, API or command that works today |
| **Gotchas** | what bites — stated next to the thing, not collected in a warnings chapter |
| **As shipped** | stock Steelcase behaviour, *only* where it explains the present |

**Provenance.** Software facts were read off live units (primarily RW09, `192.168.50.73`), plus
on-device measurements across two further units. The hardware facts here inherit the teardown and the
continuity measurements recorded in [`HARDWARE.md`](HARDWARE.md), which also holds the photos.

---

## Contents

1. [Read this first](#1-read-this-first)
2. [The board](#2-the-board) — the hardware itself is [`HARDWARE.md`](HARDWARE.md)
3. [Subsystems](#3-subsystems)
4. [Boot chain and recovery](#4-boot-chain-and-recovery)
5. [Software stack](#5-software-stack)
6. [Building for this device](#6-building-for-this-device)
7. [Kernel policy](#7-kernel-policy)
8. [Hardware policy](#8-hardware-policy) — the board is used as delivered

---

## 1. Read this first

### What this device is

A wall-mounted meeting-room display repurposed as a games/apps platform. TI **OMAP3503** ARM
Cortex-A8 at 600 MHz, 234 MB RAM, 800×480 LCD, projected-capacitive touch, Linux 4.14.52 with
SysVinit, booting from a removable microSD card and powered over Ethernet. **No GPU and no DSP** —
all rendering is software. There is no local app to run: everything is cross-compiled on a host and
deployed over SSH.

### What a boot-path write costs to undo

Every write here is allowed; what differs is the price of getting it wrong. Know it before the write.

1. **`/dev/mtd*`** — the 12 KB NAND redirector in `mtd0` is the only non-SD part of the boot path, and a bad
   one is undone only by JTAG. Dump it (`cat /dev/mtd0`, read-only) before any write; reading is free.
2. **`mlo`, `u-boot.bin`, `u-boot-sd.bin`, `ctrlblock.bin` on p1** — a bad one leaves the unit dark, undone
   by editing p1 in a card reader (measured 2026-10-11: four failed `mlo`s on `.188`, each restored by
   renaming the vendor copy back). Keep the vendor file beside the replacement under a new name.
3. **`uImage-system`** — `bootcmd` is hardcoded to it; stage an experimental kernel under another name and a
   power cycle is a free undo.
4. **A known-good SD image** is the entire recovery story for everything except item 1.
5. **The watchdog**: an app owning the screen for long must keep `/dev/watchdog` fed (60 s) or the device
   hard-resets.

Full detail:
[Boot chain and recovery](#4-boot-chain-and-recovery).

### Physical safety

- **The unit is PoE-powered.** "Unplug it" means pulling the Ethernet cable — there is no other
  power lead and no barrel jack.
- **Up to 57 V sits on the RJ45 (`J3`) magnetics centre taps when powered.** Keep probes out of
  that corner. Everything downstream of the flyback is isolated and safe.
- **Releasing the bezel tears the touch flex if you don't disconnect `J2` first.** The touch glass
  bonds to the bezel; its flex lands on the board. They are tethered with no slack. This has
  already cost one unit its touchscreen.
- The board is 3.3 V logic — **no 5 V TTL adapters**. Note that the `P4` console header is behind a
  MAX3232 and is therefore at **RS-232** levels, not TTL. See [Serial ports](#312-serial-ports).

### How to read a claim in this document

**Every statement here is measured on this hardware unless it carries a tag**, and there are three:
**`[inferred]`** — derived from a datasheet, a driver source or another measurement, but not itself
observed (a good pedigree is not a measurement: two inferences read straight out of the kernel source
were refuted on hardware in one afternoon); **`[unverified]`** — nobody has tried it, no pedigree at all;
**`[n=1]`** — measured once, or on one unit, so the *shape* usually generalises and the digits do not.
⚠️ **Anything needing a test to resolve is an open item in `IMPROVEMENT_PLAN.md`, not a hedge here.** A
tagged claim is one this document is content to carry untested; a hedge is work nobody has filed.

---

## 2. The board

The board as a physical object — the parts inventory with markings, every connector and what it goes
to, the unpopulated `J5`/`J6` XBee socket with its measured pinout, the `P4` RS-232 console and `P3`
JTAG headers, the enclosure and how it comes apart, and the teardown photos placed beside the parts
they show — is [`HARDWARE.md`](HARDWARE.md).

---

## 3. Subsystems

### 3.1 SoC, memory and storage

**What's there.** TI **OMAP3503 ES3.1.2**, GP (general-purpose, non-secure) silicon — the ARM-only
member of the OMAP34xx family. **No SGX530 GPU, no IVA2 DSP.** The DSS overlay hardware is the only
graphics acceleration that exists on this part.

```
$ cat /sys/devices/soc0/{family,machine,revision,type}
OMAP3 / OMAP3503 / ES3.1.2 / GP
$ dmesg | head -1
OMAP3503 ES3.1.2 (l2cache neon isp)
```

| | |
|---|---|
| CPU | ARM Cortex-A8 (part `0xc08`), 300 MHz base, **600 MHz** with cpufreq scaling active |
| BogoMIPS | 594.73 |
| Features | `half thumb fastmult vfp edsp thumbee neon vfpv3 tls vfpd32` |
| RAM | 239,904 KB (**234 MB**) usable of 256 MB; ~182 MB free in game mode |
| Swap | 258 MB, unused |
| Storage | microSD, 3.7 GB usable (SanDisk "Video Buffer" 4 GB, SDHC, UHS-I, **U3**) |
| NAND | 256 MB, **effectively unused** — see [Boot chain](#4-boot-chain-and-recovery) |

**Gotcha — this is an OMAP3, not an AM335x.** Worth stating because the two get confused and the
addresses differ throughout: GPMC `6e000000`; I2C1/2/3 `48070000`/`48072000`/`48060000`; UART2
`4806c000`; McBSP2 `49022000`; six GPIO banks (AM335x has four); MUSB glue `omap2430`; serial
naming `ttyO*` not `ttyS*`; companion PMIC TWL4030 (AM335x uses TPS65217).

**Gotcha — Cortex-A8 has no hardware integer divide.** This shapes every static build. See
[Building for this device](#6-building-for-this-device).

**The SD card is the entire failure surface**, and therefore the entire recovery story. "Video
Buffer" is SanDisk's OEM high-endurance line (dashcams, surveillance) — a sensible choice for
hardware that gets hard power-cycled, and worth matching when cloning to a larger card.

**Sequential read is ~11.4 MB/s; sequential write is ~0.87 MB/s — a 13× asymmetry, measured on `.188`
2026-08-20 and 2026-08-22** (read: one 3,898,628 B file whole; write: 9.8 MB by `scp`, 4.6 s plus 6.7 s of
`sync` — count the flush or the card reads twice as fast as it is). Read settles whether an asset can be
*streamed* rather than loaded: 44.1 kHz 16-bit mono needs 88.2 KB/s, **0.77 %** of it. ⚠️ **Both are
throughput, not per-read LATENCY** — the half a render loop feels, absorbed by a read-ahead refilled when dry.

### 3.2 Display

**What's there.** Sharp **`LQ070Y3LG4A`** — 7" WVGA TFT, 800×480, LVDS input, LED backlight. The
vendor DTS `compatible = "sharp,lq070y3lg4a"` is the literal part number. Signal path:

```
OMAP3 DSS (24-bit RGB888 DPI)
  -> U16 SN65LVDS83B  (parallel RGB -> LVDS)
    -> 40-pin FFC -> discrete twisted-pair harness
      -> JAE ~30-position connector (only ~14-16 positions wired: 4 data pairs + clock + power)
        -> Sharp T-CON board "K5784TP"
             60153F00B0 timing controller  +  JRC A770 gamma buffer
          -> panel
```

The panel is a **separate, field-replaceable module** and Sharp was still producing it in 2018 — a
cracked screen is a repair, not a write-off.

**How to drive it.** `/dev/fb0`, memory-mapped, double-buffered. Legacy **omapfb + omapdss**:

```
$ cat /proc/fb            -> 0 omapfb / 1 omapfb   (CONFIG_FB_OMAP2_NUM_FBS=2; we only use fb0)
$ ls /dev/dri /sys/class/drm
(neither exists - there is no DRM/KMS on this device at all)
```

Backlight: `/sys/class/leds/backlight/brightness`, 0–100.

**Gotcha — bpp is per-app and set at runtime.** `/dev/fb0`'s format is **global mutable state**:
whatever the running app last selected via `FBIOPUT_VSCREENINFO`.

| App | bpp |
|---|---|
| every native app — games, launcher, tools, diagnostics | **32bpp XRGB8888**, pinned via `fb_set_bpp(dev, 32)` before `fb_init()`. `app_launcher` also re-asserts it after every child exits |
| ScummVM, VNC remote session | **16bpp RGB565**, to halve write bandwidth |

`native_apps/common/framebuffer.c` is **bpp-aware**: its primitives dispatch on `bytes_per_pixel`
through `fb_pack565`/`fb_unpack565`/`fb_store`/`fb_load`, so the public API takes RGB888 at either
depth and no depth corrupts the back buffer. `fb_init()` accepts 16 and 32; on any other depth it
forces 32bpp and reports why on stderr. So the reason an app pins the depth is **determinism and
appearance** — 16bpp bands every gradient, and how an app looks must not depend on which app ran
before it — not memory safety.

When screenshotting, decode at the bpp of whatever was running: `ssh root@<ip> cat /dev/fb0 > fb.raw`
then `fb565_to_png.py fb.raw fb.png` (defaults to 32bpp; `--bpp 16` for ScummVM/VNC). One 32bpp
frame is 800×480×4 = 1,536,000 bytes — coincidentally the same size as two 16bpp pages, which is
why a wrong-bpp decode looks size-correct while producing garbage. **Run `fbset | grep geometry`
and believe it** rather than inferring the depth from which app you launched: an app killed
mid-session leaves the panel in a mode nothing running asked for. RGB565 read as 32bpp has a
recognisable signature — R == B with a low G, e.g. (64,8,64), (96,16,96).

**Visible area.** The bezel hides **10–15 px on the top and bottom edges only**, and effectively
nothing left or right. Measured on two devices.

Apps never deal with this. `fb_init()` shrinks the drawing surface to the visible rectangle and
`fb_swap()` places it on the panel at the viewport origin, leaving the hidden bands black — so
`fb.width`/`fb.height` and `SCREEN_VISIBLE_*` are the **logical** (fully visible) screen, 800×450 at
the shipped 15/15/0/0 margins. (`SCREEN_SAFE_*` is a *different* rectangle — visible ∩ touchable;
see [§3.3](#33-touch).) Margins come from `/etc/touch_calibration.conf` line 2, default
`FB_BEZEL_*_DEFAULT` = 15/15/0/0 (`native_apps/common/framebuffer.h`), and are set on-device from
**Control Panel → Display → SCREEN EDGES**.

> **The constraint that actually binds interactive layout is digitizer reach, not the bezel** —
> and on this panel the two do not coincide. Touch is not reported in a band at the top and bottom
> of the *logical* surface, so keep interactive targets out of it; decoration can go to the edge.
> The measured figures live in [§3.3 Touch](#33-touch); do not restate them here.

**Panel timings — recovered, and worth preserving.** The device tree contains no timing block; the
timings were compiled into a vendor panel driver whose source does not exist in any tree we have.
omapdss exposes them at runtime, so they were read off a live device before anything could change:

```
$ cat /sys/devices/platform/omapdss/display0/timings
33230770,800/40/88/128,480/9/26/9
```

| Parameter | Value |
|---|---|
| Pixel clock | **33,230,770 Hz** |
| Horizontal | 800 active / 40 front / 88 back / 128 sync → **htotal 1056** |
| Vertical | 480 active / 9 front / 26 back / 9 sync → **vtotal 524** |
| Refresh | 33230770 / (1056 × 524) = **60.05 Hz** |
| Bus format | 24-bit RGB888 DPI (`data-lines = <24>`) |
| Panel GPIOs | pwrdn `gpio1[14]`, lvds `gpio1[15]`, backlight `gpio1[19]` — all active-high |
| Polarity | hsync, vsync **active-low**; DE **active-high**; data on the **rising** pixel-clock edge, sync on the **falling** one — **measured** from the vendor driver's probe immediates (the sysfs string omits it) |
| Power sequence | enable: DPI on, `msleep(50)`, then pwrdn → lvds → backlight high; disable: all three low, `msleep(100)`, DPI off — **measured** from the vendor `sharp_lq_enable`/`_disable` disassembly |

With these numbers the panel needs no custom driver on any kernel: it reduces to a stock
`panel-dpi` / `panel-simple` node with a `panel-timing {}` block. This was the single hardest
blocker to any future kernel work, and it now lives here rather than only inside a binary on a
2018 SD card.

**DSS overlay planes — three overlays, and the hardware scaler is proven but not used.** ⚠️ **Operator ruling
2026-10-04: low return, so nothing ships on it.** Native apps draw 800×480 into `fb0` and need no scaling;
ScummVM's software nearest-neighbour is sharp and already cheap (below), so the hardware path saves a few
percent of a CPU that is not the limit; and sharp output needs an unbuilt, untested kernel patch. A HUD
plane, colour-key sprites and YUV video go with it. The rest of this section is kept as measured fact. Two overlays scale:

```
$ ls /sys/devices/platform/omapdss/
display0  manager0  manager1  overlay0  overlay1  overlay2

overlay0: name=gfx   enabled=1  manager=lcd  input_size=800,480  output_size=800,480  zorder=0  global_alpha=255
overlay1: name=vid1  enabled=0
overlay2: name=vid2  enabled=0
manager0: lcd  trans_key_enabled=0  alpha_blending_enabled=0
manager1: tv   display=<none>
/dev/video0 = omap_vout (V4L2 output)
```

Each overlay exposes `output_size`, `position`, `zorder`, `global_alpha` and `pre_mult_alpha`;
`input_size` and `screen_width` are **read-only** — both `OVERLAY_ATTR`s pass `NULL` for the store
method (`dss/overlay-sysfs.c:375`) and `overlay_attr_store()` answers `-ENOENT` when there is none
(`:429-430`), so a write *fails* — and they track the bound framebuffer's mode, so input geometry is
set with `fbset -fb /dev/fb1` and never by writing sysfs. ⚠️ **`screen_width` is the SOURCE row stride
in pixels, not a destination width**: it is `line_length / (bits_per_pixel >> 3)`
(`omapfb-main.c:897`, stored into the overlay info at `:909`, shown at `dss/overlay-sysfs.c:126-133`)
and is consumed only as the DMA row stride (`dss/dispc.c:1917-1918,2012,2135`). So `screen_width 320`
under a 320×200 RGB565 `fb1` is **correct and is not a blur cause** — do not go hunting there.

⚠️ **Three overlays enumerate but only TWO framebuffers exist, so `vid2` can never be funded from
userspace.** Measured on `.188` 2026-09-11: `/sys/class/graphics/` holds `fb0` and `fb1` only, against
three `overlay*` directories under `omapdss`, because the running kernel has
`CONFIG_FB_OMAP2_NUM_FBS=2` (`/proc/config.gz`). There is no `fb2` node to bind, so an `overlay2`
plane has no way to get memory and the third plane is out of reach until an image is built with the
value raised — a **config-only** change with no source patch. `vid1` is the only scaling plane
userspace can actually use.

The manager exposes
`trans_key_enabled` (colour keying) and `alpha_blending_enabled`. ⚠️ **`gfx` (`overlay0`) has NO
scaler**, so the input and output sizes are *not* freely independent on the plane the apps use:
`omap3430_dss_overlay_caps[]` omits `OMAP_DSS_OVL_CAP_SCALE` for it (`dss/dss_features.c:341`) and
`dss_ovl_simple_check()` returns `-EINVAL` for any `width != out_width` (`dss/overlay.c:116`). Only
`vid1`/`vid2` scale, and the limits are **per axis and different in each direction — measured in the
vanilla tree**: **downscale 4×**, which is `FEAT_PARAM_DOWNSCALE = {1,4}` (`dss/dss_features.c:436`)
and is downscale-only; **upscale 8×**, refused by `out_width > width * 8` and the matching height test
(`dss/dispc.c:2515` and `:2518`). Two further gates: **maximum single-line width 1024 px**
(`FEAT_PARAM_LINEWIDTH`, `dss/dss_features.c:437`, enforced at `dss/dispc.c:2378` and `:2411-2420`),
and the output must be framed — `pos + out <= res` on each axis or `-EINVAL` (`dss/overlay.c:174-186`).
**[inferred]** a *pure* upscale is the cheapest case for the core-clock gate: `calc_core_clk_34xx()`
returns plain `pclk` unless an axis shrinks (`dss/dispc.c:2259-2272`).

**Measured on `.188`: `vid1` upscaling 400×240 → 800×480 full-screen, over the app, with no reboot,
no boot parameter and no kernel work.** `fb1` is the second framebuffer
(`CONFIG_FB_OMAP2_NUM_FBS=2`, see above) and is *already* bound to `overlay1` — a 1:1 default at probe
(`omapfb-main.c:1924`), readable as `/sys/class/graphics/fb1/overlays`. The one thing missing is
memory: **omapfb allocates vram for `fb0` only** (`omapfb-main.c:1551`), so `fb1` starts `size=0`, and
an overlay whose `paddr` is 0 is refused (`dss/dispc.c:2620`) or disabled again on the next apply
(`omapfb-main.c:958`). The whole recipe is four writes, in this order because a bad geometry is
rejected at write time: `echo 384000 > /sys/class/graphics/fb1/size` (allocates from the existing
`vram=12M` pool — `fb0` holds 1,536,000 of it, so most is free), `fbset -fb /dev/fb1 -g 400 240 400
240 32`, `output_size`, then `enabled=1`. Undo is `enabled=0` then `size=0`. `zorder` is **not
writable** on this SoC and does not need to be: the fixed order is GFX < VID1 < VID2, so `vid1`
composites above the app with `alpha_blending_enabled=0`.

⚠️ **`fb1/size` and `overlay1/enabled` both reading `0` is the IDLE state, not a capability limit.**
Measured on `.188` 2026-09-11, with `omap_vout: failed to allocate DMA Channel for video-1` in that boot's
log: funding `fb1` from sysfs worked (128000 written for 320×200 RGB565, **131072 read back** — page
rounding), `enabled` read `1`, the upscale was **visible on the panel**. That error is a V4L2 failure and
bears only on `/dev/video0`.

**⚠️ A scaled overlay is ALWAYS filtered — there is no nearest-neighbour path and no filter-off bit.**
Measured in the vanilla tree. `dispc_ovl_setup_common()` calls `dispc_ovl_set_scaling()` for any plane
carrying `OMAP_DSS_OVL_CAP_SCALE` (`dss/dispc.c:2771-2775`); that function (`dss/dispc.c:1720-1734`)
unconditionally reaches `dispc_ovl_set_scale_param()` (`dss/dispc.c:1597-1599`), which writes all
**8 FIR phases** into the coefficient registers (`dss/dispc.c:654-697`). The only hardware bypass is
the `RESIZEENABLE` attribute bit, and it is set purely on size *inequality*
(`dss/dispc.c:1603-1605`) — so the moment input ≠ output the FIR is in the path, and nothing can ask
for point sampling.

**Which coefficients you get is decided by the scale ratio alone.** `fir_hinc = 1024 * orig_width /
out_width` (`dss/dispc.c:1490-1491`), and `dispc_ovl_get_scale_coef()` buckets on `inc / 128`
(`dss/dispc_coefs.c:289-325`, bucket select at `:320`). Horizontal is hardcoded to the **5-tap** table
(`dss/dispc.c:661` passes `true`); vertical is **3-tap** for any upscale, because
`*five_taps = in_height > out_height` (`dss/dispc.c:2354`) is false whenever the output is taller.

⚠️ **Above 2× upscale the driver DELIBERATELY picks a softer kernel, and says so** — the comment is at
`dss/dispc_coefs.c:310-314`: above 2× the M8 tables show "blockiness and outlines around the image", so
M11/M16/M19 are substituted to hide it. A 320 → 800 upscale gives `fir_hinc` 409, bucket 3, hence
`coef5_M11` phase 0 `{-5,23,92,23,-5}` (`dss/dispc_coefs.c:190-199`) and `coef3_M11` phase 0
`{0,14,100,14,0}` (`:58-67`). Neither is an identity kernel, so **pixel-art text is smeared at exactly
the ratio a 320×200 engine wants.**

**Exact 2× is the sharpest ratio reachable, and it rings.** `inc = 512` lands in bucket 4, which is
`coef5_M8` / `coef3_M8`, and phase 0 of both is the identity `{0,0,128,0,0}`
(`dss/dispc_coefs.c:26,158`). **[inferred]** at exactly 2× the FIR accumulator alternates phase 0 and
phase 4, so half the output pixels pass through untouched — that step is register arithmetic beyond
what the source states, while the coefficient values themselves are measured. But horizontal phase 4 is
`{0,-14,78,78,-14}` — **negative lobes, i.e. a sharpening kernel that overshoots at a high-contrast
edge** — where vertical phase 4 is a clean `{0,64,64,0,0}`. So the other half of the columns ring.

**None of it is tunable from userspace at any ratio.** All 24 tables are file-scope `static const`
(`dss/dispc_coefs.c:25-287`), the sole selector is `dispc_ovl_get_scale_coef()`, and it is called only
from `dss/dispc.c:661-662`. The overlay's whole sysfs attribute set is listed at
`dss/overlay-sysfs.c:372-403` — the writable members are the ones named above — and contains nothing
coefficient-related. ⚠️ **No sysfs attribute and no ioctl in this tree reaches the filter**: the only
choice a caller has is the ratio. ⚠️ **And a kernel *module* cannot reach the tables either** — measured
on `.188` from `/proc/config.gz`, `CONFIG_FB_OMAP2_DSS=y` and `CONFIG_FB_OMAP2=y` are **built in**, no
DSS module is loaded and no `omapfb`/`omapdss` module file exists under `/lib/modules`. Changing a
coefficient table means a full image, which inherits the standing rebuild blocker in
[Kernel policy](#7-kernel-policy).

⚠️ **A framebuffer screenshot cannot see an overlay.** `cat /dev/fb0` returns the gfx plane's own
memory, not the composited panel — so overlay work is the one screen check in this repo that no
`fb565_to_png.py` decode can perform, and it is verified by looking at the display.

**Userspace hardware upscale: measured cheap, and rejected on image quality.** ⚠️ **Software
nearest-neighbour stays the ScummVM path; no hardware configuration wins on pixel-art text from
userspace at either reachable ratio, and none is selectable.** *Cost, measured on `.188`, 200 frames each:*
rendering the same scene into a 400×240 surface on `vid1` costs **5,510 µs CPU per frame against 26,075 µs
at 800×480 on `fb0`**, and enabling the overlay costs nothing measurable; a funded 800×480 `fb1` as device
control came within 0.4 % of `fb0`, so the win is the pixel count and not the node.
The scene was fractions of the surface and printed its own pixel count; two runs whose counts are not in the
ratio of their areas are not an A/B.
Against that, ScummVM's NEON nearest-neighbour resample spends 6.4–12.9 ms of a 33 ms frame where the
hardware path spends 0.1–0.2 ms, but *Full Throttle* plays at 12–13 % CPU, so nothing is short. *Quality,
operator at the panel, `.188`:* a 320×200 → 800×480 hardware arm is blurry and the blur **followed the
hardware arm when the two halves were exchanged**, so viewing angle is excluded; on a real *King's Quest II*
frame the hardware arm was *"very bad"*, its text *"barely readable"*, the software arm *"very sharp"* —
a SCUMM dialogue box is a 1-px-stroke bitmap font, the worst case for any filter. Exact 2× (`output_size`
`640,400`, `position` `80,40`, poked live while `vid1` held the plane) was *not* an improvement: ringing
from the phase-4 negative lobes above, on a picture smaller than the 672×420 ScummVM produces itself. A
nearest-neighbour hardware path would need an unbuilt, untested kernel patch (the **[inferred]**
all-identity 8-phase table in `dss/dispc_coefs.c`; it would also need `NUM_FBS=3` for a third plane). The A/B drew a soft, a hard and a split arm with a `--swap` viewing-angle control and a
real frame by PPM; `fb_to_game_ppm.py` inverts the software upscale off an `fb0` grab. The `vid1` state before any
funding is `enabled=0`, `fb1/size` 0, and `overlay1` has no `trans_key*` attribute of its own.

`/dev/video0` (`omap_vout`) is the V4L2 *output* path, which accepts **YUV with hardware colour-space
conversion**, and is untried. **[inferred]** it is what would make a video player conceivable on a part
that could never software-decode one, and the DMA-channel error above may be what blocks it.

> ⚠️ This is a **legacy omapdss sysfs** interface. It does not exist under `omapdrm`. Anything
> built on it is cheap today and would need rewriting as DRM atomic plane programming after a
> **mainline** port (a 4.14.52 rebuild leaves it intact) — see [Kernel policy](#7-kernel-policy).

**As shipped.** X11/Xorg (`Xorg -br -nolisten tcp -nocursor -pn -dpms vt8 :0`, started by
`/etc/init.d/x11`) hosting a WebKit browser. All removed in game mode; apps render straight to the
framebuffer.

### 3.3 Touch

**What's there.** A **projected-capacitive** panel on I2C bus 2 at address `0x03`. The controller
silicon is a **Cypress `CY8CTMG120-56LTXI`** — a PSoC **TrueTouch Multi-Touch Gesture** chip, `IC1`
on an orange flex marked `EDT REV.A 40-0016-2` (56-QFN, date code `1043`).

> **"Panjit" is the touch-module vendor, not the chip vendor**, despite the kernel driver being
> called `panjit_ts`. This matters: the CY8CTMG120's I2C register map is published Cypress
> documentation, so reaching the controller directly is *implementing a documented protocol*, not
> reverse-engineering an unknown one.

**How to drive it.** evdev at `/dev/input/touchscreen0` (also `/dev/input/event0`); the vendor's driver
is `panjit_ts`, ours is `kernel/drivers/cy8ctmg120_ts/`. DT node `tsc_panjit@03`: reg `0x03`, IRQ
`gpio1[23]` (the DT cell says falling; the vendor driver requests **level-low**), reset `gpio1[16]`
active-high. The symlink comes from the device's `/etc/udev/rules.d/local.rules`, keyed on
`ENV{ID_INPUT_TOUCHSCREEN}=="1"` — udev's classification, not the driver name.

**The controller needs a host handshake, and the vendor driver is the only record of it.** Read from
the vendor kernel's disassembly (probe `@c05e8e10`, work fn `poscheck` `@c05e8b48`): pulse reset high
~10–20 ms, release, wait 100 ms; `reg0=0x00`; write `reg1=0x00` until it reads 0 (10 tries); request
the IRQ `IRQF_TRIGGER_LOW`; `reg0=0x08`. The ISR masks the IRQ and queues work that polls every 10 ms
while a finger is down, writing `reg1=0x00` after **every** read (`X2`/`Y2` are read at count 2 and
discarded); on lift `reg0=0x08`, `reg1=0x00`, release, IRQ re-enabled. **Measured on our image**
(2026-09-23, `.188`): with nothing bound the part NAKs (reset held); released it reads `08 81 00 ff…`;
without the per-read ack it reports one touch then stalls in ~2.7 s cycles of all-zero bursts with
`reg1` back at `0x81`; `cy8ctmg110_ts.c`'s `0x10` to `reg0` wedges it until reset. With the handshake,
level IRQs track touch and I2C errors stay at zero.

**Gotcha — capture coordinates BEFORE the press event.** The event order is:

```
ABS_X  ->  ABS_Y  ->  BTN_TOUCH  ->  SYN_REPORT
```

Read the coordinates on `ABS_X`/`ABS_Y` and latch them; if you wait for `BTN_TOUCH` you get stale
values. Reference implementation: `native_apps/common/touch_input.c`.

**Coordinate model — two independent stages.** A raw reading becomes an app coordinate in two
steps, each owned by a different concern and configured on its own line of the config file:

| Stage | Maps | Owner | Config |
|---|---|---|---|
| 1. Calibration | raw digitiser → **panel** pixel | `scale_coordinates()` in `touch_input.c` | line 1 |
| 2. Bezel viewport | panel pixel → **logical** pixel | the same function, from the framebuffer globals | line 2 |

Stage 1 is per-axis **piecewise linear** over the whole 800×480 panel, bezel included: three
segments joined at two knots fixed at panel `dim/4` and `3*dim/4`. Raw is 12-bit (0–4095), and four
raw values per axis define the curve — the readings at panel `0`, `dim/4`, `3*dim/4` and `dim-1`:

```text
raw_min .. knot_lo   ->  panel       0 .. dim/4       outer segment
knot_lo .. knot_hi   ->  panel   dim/4 .. 3*dim/4     fitted interior
knot_hi .. raw_max   ->  panel 3*dim/4 .. dim-1       outer segment
```

`touch_map_axis_panel()` is the single implementation; a linear map is the `knot_lo == knot_hi == 0`
sentinel. There is **no affine transform, no rotation, no shear and no bilinear corner
correction** — each axis remains an independent monotone 1-D curve.

**Why the format has three segments — and why the curve is currently a straight line.** The model
consistent with every measurement is **linear across the panel, hard-clipped at raw 0 and 4095**. The
clipping starts *inside* the panel: on Y, raw 4095 is first emitted around panel 450 and raw 0 around
panel 30 (see **Reach** below). A single line fitted to the interior therefore extrapolates outside
the emittable range — the reference capture's Y fit is `-279..4382` against a hardware range of
`0..4095` — and **that overshoot is the correct answer, not an error.** With the endpoints stored
unclamped, the three-segment curve reduces to exactly that line; the host regression asserts *zero*
deviation.

The format is kept anyway for one practical reason: changing line 1's field count is the single edit
that silently breaks a component linking a stale `touch_input.o` (the old 4-number `sscanf` returns 4
on an 8-number line and accepts it as a legacy config). Three segments also leave room to model a
genuinely non-linear panel if one ever turns up.

> ⚠️ **Outer-band slope compression is NOT established, and nothing may be built on it.** One run's
> per-segment slopes suggested a ~12 % steeper interior; residuals against the interior line are
> **±80 raw** (≈±8 px of finger placement) with no consistent sign, which over ~100 px baselines accounts
> for ±8 % of slope on its own. One run cannot distinguish 12 % from noise. Settling it needs `touch_raw`'s
> `TARGETS` mode repeated — three or more runs on one panel, compared *across* runs, not within one.

Stage 2 subtracts the viewport origin (`screen_view_x`/`screen_view_y`), which is exactly the
offset `fb_swap()` draws at. A touch on the bezel therefore clamps to the nearest logical edge, and
drawing and touch always share one coordinate system.

Keep the stages apart: calibration must be fitted against **panel** coordinates. Fitting it against
logical coordinates bakes the bezel into line 1, and stage 2 then subtracts it a second time.

**`/etc/touch_calibration.conf`**, loaded automatically by `touch_init()` and `fb_init()`:

- Line 1: `x0 x_knot_lo x_knot_hi x1  y0 y_knot_lo y_knot_hi y1` — the raw→panel curve, per axis the
  raw readings at panel `0`, `dim/4`, `3*dim/4`, `dim-1`. Endpoints may legitimately fall outside
  `0..4095`; that is the measurement.
  A legacy **4-number** line (`min_x max_x min_y max_y`) is still accepted and migrated on load:
  the knots go onto the line it described and the endpoints are left exactly where that line puts
  them, so the legacy mapping is reproduced rather than "improved". A legacy file carries no edge
  measurement, so there is no basis for moving its endpoints — if it described a map with a dead band,
  the migration keeps the dead band and `TOUCHABLE:` reports it. The migration is logged.
- Line 2: `bezel top bottom left right` — panel pixels hidden by the bezel. Omit the line to get the
  compiled defaults (15/15/0/0).
- Line 3 (optional, keyword-tagged): `reach x_lo x_hi y_lo y_hi` — the raw values the **physical**
  edges actually emit, from the wizard's `REACH` sweep. Absent means "assume the `EVIOCGABS` limits",
  which yields a zero inset. Tagged and trailing on purpose: an old parser reads lines 1–2
  positionally and ignores what it does not recognise, so the file stayed back-compatible.

**Control Panel → Input → CALIBRATE** (and Display → SCREEN EDGES) owns both lines, through **one wizard** — `touch_wizard_run()` in
`native_apps/control_panel/touch_wizard.c`. Everything it does runs with the bezel zeroed
(`fb_set_bezel(fb,0,0,0,0)`) on the full 800×480 panel, so a drawn pixel *is* a panel pixel and both
lines are measured against the same premise:

| Step | Measures | How |
|---|---|---|
| `TAP` | line 1 | 11 targets × 3 taps, median; least-squares per axis from **interior targets only** |
| `CHECK` | — | the derived curve, per-axis dead-band verdict, edge-probe residuals; ACCEPT / REDO / RESET |
| `EDGES` | line 2 | numbered 2 px ladders at each panel edge; raise each margin until its line clears the plastic |
| `REACH` | line 3 | slide a finger along each of the four edges; 16 coverage cells per edge, all four live at once. An unswept edge falls back to the hardware limit; a *completed* sweep that fell short widens the reported band |
| `REPORT` | — | visible rectangle vs touch-safe rectangle and the per-side inset in px, with the reminder that the band is still drawable. Amber on **magnitude** (`DISP_INSET_SUSPECT`, 24 px), not on "non-zero" |
| `CONFIRM` | — | goes live on the new mapping with a 20 s countdown; reverts unless you press KEEP |

`CALIBRATE` runs the whole thing, Display's `SCREEN EDGES` jumps to the `EDGES` step for a margins-only
tweak, and `RESET` restores the hardware `EVIOCGABS` range and the default margins. ⚠️ **Nothing is
written until CONFIRM, and the wizard hit-tests its own buttons through the *entry* calibration until
then** — so a bad fit can never leave you unable to press the button that rejects it.
`touch_calib_backup()` copies the old file to `.bakN` first.

> **One flow and one fit, deliberately.** This was two flows plus a third copy of the same defective fit
> in a standalone `unified_calibrate`, and they disagreed. ⚠️ **An edge adjuster that draws its reference
> frame on the *logical* edge is measuring the bezel through the bezel**, and a crosshair inset only
> 40 px sits inside the band where raw compresses, so its fit comes out shallow.

**One implementation of the fit, in `native_apps/common/touch_calib.c`** — the target set, the per-axis
interior masks (≥100 px from each end on X, ≥80 px on Y), the least-squares, the unclamped endpoints, the
per-axis dead-band verdict, the reach→inset calculation, the edge-sweep accumulator, the sanity gate and
the `.bakN` backup. Both the wizard and `touch_raw` link it, so the diagnostic validates the very code
the wizard calibrates with. ⚠️ **The fitted endpoints must never be clamped into `0..4095`** — a correct
fit on this panel legitimately extrapolates outside it, and the clamp that used to do it asserted raw
4095 is emitted at panel 479 when it is emitted at panel ~450, running the reported position **ahead of
the finger by up to +19 px across the bottom quarter**. The library rules, the deleted legacy-migration
clamp and the sanity gate's actual criterion are `native_apps/CLAUDE.md` → *Touch model*.

**`touch_raw`** (`native_apps/tests/touch_raw.c`, deployed to `/opt/games/`, hidden from the
launcher; reachable from Control Panel → Input → `DIAGNOSTIC`) is the diagnostic that settled
reach, and the only tool that shows the panel with **no calibration and no bezel**: it resets the raw
range to the `EVIOCGABS` values and calls `fb_set_bezel(fb,0,0,0,0)`, so the dot is
`raw × 799 / 4095`. It logs to `/tmp/touch_raw.tsv` with a monotonic millisecond column and can write
the interior fit to line 1 after a `.bakN` backup. Four modes, and the split between the middle two is
the whole point:

| Mode | Question it answers | Method |
|---|---|---|
| `LIVE` | free tracking | crosshair, trail, session extremes, pin flag when raw sticks at a hardware limit |
| `SWEEP` | *what raw does the physical edge emit?* | slide a finger along each edge; per-bucket extreme along the edge, so a corner reading differently from the middle shows up instead of averaging away |
| `INSET` | *where does raw first reach that value?* | tap a bar walked inward from each edge at 0/10/20/35/55 px; the answer localises where the flat band starts |
| `TARGETS` | the interior line | 11 targets × 3 taps, then a hard press on each bezel; fits interior-only vs all-points and reports what each predicts at the edges, per axis |

⚠️ **Only `INSET` decides the curve.** `SWEEP` alone cannot: a finger sliding along an edge yields no
position information, so it reads identically whether clipping begins at the edge or 30 px inside it,
and conflating the two questions kept the endpoint bug alive across three sessions. Nor may a slope come
from two adjacent `INSET` bars — that rule and its numbers are `native_apps/CLAUDE.md` → *Touch model*.

**ScummVM and `vnc_client` each link their own copy of `touch_input.o`** — rebuild and redeploy both
after changing that file or its touch silently goes stale (see *Reach* below for how silently).

Accuracy: ~3 px at centre, 14–27 px error at the corners before calibration.

**Reach at the border — every edge drives raw to its limit, but not at the edge.** Settled on
2026-08-01 with `touch_raw`'s `SWEEP` and `INSET` modes, with the calibration and bezel zeroed so a
drawn pixel is a panel pixel. Raw capture:
[`touch_raw-2026-08-01-rw09.tsv`](touch_raw-2026-08-01-rw09.tsv), 16:53.

> **Every number below is the reference capture, not the live calibration of any unit** — the fit is
> re-run per unit and per wizard run (see *Provenance* at the end of this section for what RW09 carries).
> What generalises is the *shape*: linear interior, a saturated band inside each Y edge, a much smaller
> one on X. Not the digits. The host regression replays this capture's medians rather than reading a
> device, which is what makes it a regression and not a snapshot.

`SWEEP` first: **all 16 buckets on all four edges** drive raw to `0`/`4095`, uniformly, with no
corner-vs-middle variation. So the electrode array is not short.

`INSET` then asks where that limit is *first* reached, and this is the part a bezel press cannot see:

| Axis end | raw limit first emitted at panel | panel extent | flat (saturated) band |
|---|---|---|---|
| X left | ~0–12 | 0 | ~0–12 px |
| X right | ~790–799 | 799 | ~0–9 px |
| **Y top** | **~30** | 0 | **~30 px** |
| **Y bottom** | **~450** | 479 | **~29 px** |

Confirmed independently: the 11-target interior fit (`X 10..4076`, `Y -296..4376`), which never sees
an edge sample, predicts a bezel press landing at panel **30 / 450** — the same numbers from a
different method. Whether the cause is a short electrode array or controller clipping is not
distinguished by this data and does not matter for calibration. (Two fits of that panel are quoted in
this section — the one above and `X 17..4084 / Y -279..4382`, which the host regression replays. They
differ by ≤7 raw, an order below the ±80 raw tap residual.)

**Consequence: drawable ≠ pressable, and a dead band is a fact about this panel rather than a bug.** The
band at each end of Y is visible and fully drawable but cannot be pressed, so two rectangles exist —
`SCREEN_VISIBLE_*` (the full logical screen) and `SCREEN_SAFE_*` (visible ∩ touchable). On the reference
capture at bezel T=11 B=14 the inset is **~17 px top / ~16 px bottom** with X ≈ 0; RW09's live
calibration gives 19/16 **and ~6 px on each side of X** (see *Provenance*). **X's band is much smaller
than Y's and can be zero, but a non-zero X inset is not a fault** — it arises by exactly the same
mechanism as Y, from fitted X endpoints falling outside `0..4095`. Treat every one of these numbers as
per-unit and per-calibration.

The inset is **measured at runtime, never hardcoded** — the four raw edge extremes are pushed through the
production mapping, so it is correct in portrait and under any bezel and is `0` until an edge sweep has
been recorded. Read it from Control Panel → Display → `TOUCHABLE:`, the wizard's `REPORT` screen, or the
display test's `SAFE AREA` page (red rect = visible, green = touchable). Which rectangle a call site
wants, the cap on the inset and the drawing policy are `native_apps/CLAUDE.md` → *Screen edges*.

**The mapping lives in the config file, so deploying code never fixes a bad stored curve.** Line 1 of
`/etc/touch_calibration.conf` is what `touch_init()` uses; a unit whose line 1 was written by older
code keeps that behaviour, symptom intact, across any number of correct deploys until the wizard is
re-run. Corollary for handovers: "the fix is deployed" and "the device behaves correctly" are separate
claims.

**Provenance — what a live unit carries.** The figures above are the 16:53 diagnostic run; RW09's stored
config is a *different* fit of the same panel, from the wizard run that was kept at 18:50 the same day:

```text
line 1  -33 1007 3087 4122   -296 875 3217 4379
line 2  11 14 0 0
line 3  reach 0 4095 0 4095    -> published inset: X 6..793  Y 19..438  of 800x455
```

`reach 0 4095 0 4095` means the sweep found the hardware limits on all four edges — the "assume the
hardware limit" case, contributing *nothing* — so this unit's entire published inset comes from the fit's
endpoint overshoot (`-33`/`4122` on X, `-296`/`4379` on Y against a `0..4095` hardware range), which is
the model working as designed. ⚠️ **The inset and the ~30/29 px flat band are therefore not the same
measurement and must not be reconciled:** the inset is *logical rows the current curve cannot address*,
the flat band is *panel pixels over which the sensor's reading is saturated*, measured by `INSET` with no
calibration in the path. Both are true at once, of different things. No `touch_raw` capture exists for
the 18:50 calibration — the wizard writes the config, only the diagnostic writes `/tmp/touch_raw.tsv`.

⚠️ **A bezel press cannot locate where clipping starts** — raw hits `4095` whether that is panel 479 or
panel 450 — and nothing inferred *through* a calibration can measure that calibration. Every dead-band
figure this document carried before `touch_raw`'s `INSET` mode existed was wrong for one of those two
reasons, twice in *opposite* directions (a "~11 mm shorter electrode array" computed by mapping bezel
presses through the fit under test; then "every visible pixel is touchable, so a dead band is a bug",
whose clamp made the bottom edge measurably worse). **Design the measurement to answer one question and
state what it cannot answer.**

Two secondary effects, measured at the same time:

- **Finger-centroid scatter is real, and it shifts the intercept rather than the slope.** Nine hard
  presses on the top bezel returned raw `22, 76, 105, 111, 93, 79, 90, 118, 47` — pressing flat near the
  saturation zone gives a tall contact patch whose centroid is pushed inward, and it scatters widely.
  Prefer target taps for anything quantitative; use a bezel press only for the yes/no question "does raw
  reach its limit here?".
- **A stale binary misparses the config instead of failing.** A `vnc_client` built before the 8-number
  line 1 read `0 1020 3074 4095  0 874 3215 4095` as `X [0..1020] Y [3074..4095]`, confining touch to the
  left quarter and the bottom strip — presenting as "touch is broken in vnc_client" rather than as a
  version mismatch. ⚠️ **The discriminator is `(piecewise)` in the binary's `Calibration loaded from:`
  line**, which only 8-number-aware code prints. It is *not* the presence of
  `Touch raw range set (linear):` — that string is live in `touch_set_raw_range()`, the `EVIOCGABS`/RESET
  path, and a current binary prints it whenever the hardware range is set.

Every touch measurement in this section is **`[n=1]`** — the digits are RW09's. Touch itself is
confirmed working on two or more units; no second panel has been *swept* and recorded.

**Two points are a bounding box, not two fingers — measured 2026-09-23/24 on `.188`.** Point 1 is (min X, min
Y), point 2 (max X, max Y): the diagonal is lost, `X2` is the marker below in every two-finger frame, and `Y2`
can hold one stale in-range value for 2 s while both fingers move. **The fingers survive in the profiles — one
delta byte per electrode, 27 columns at `0x13..0x2d` and 15 rows at `0x2e..0x3c`** (self-capacitance; X ≈
156.5·col, Y ≈ 288.7·row, rms 64/110 raw over 1748 one-finger frames): two peaks per axis. A firm thumb peaks
~2× a light fingertip on **both** axes, which pairs them; equal fingers carry no pairing. Burst and profiles
each rewrite every **60 ms, 30 ms out of phase** `[n=1]`. The vendor `panjit_ts` reports only `ABS_X`/`ABS_Y`/
`BTN_TOUCH`; on the vendor kernel the chip is reachable only via `/dev/i2c-2`, no `unbind` ([Kernel policy](#7-kernel-policy)).

**A nine-byte burst from register 3 at address `0x03` is the entire protocol**, read against a live
finger on 2026-09-21: `X1` at bytes 3–4, `Y1` at 5–6, `X2` at 7–8, `Y2` at 9–10 and the finger count at
11, every coordinate **big-endian 16-bit**. That is byte-for-byte the map in
`drivers/input/touchscreen/cy8ctmg110_ts.c:41-50`, so the 110→120 delta is closed — same family, same
map. The count reads **0, 1 and 2**. Two corrections to that driver, both measured here:
**coordinates are 12-bit `0..4095`, not its hardcoded `759x465`** (observed `X1` 272..3832, `Y1`
4..4081, `Y2` max 4090); and ⚠️ **`X2 = 0x408E` exactly is an "unresolved" marker, not a coordinate** —
masked it becomes 142, a dot on the left. After a two-finger touch ends, **every later one reads it until the
chip is reset**; single touches do not cause it; near the right edge it can persist a whole touch. **Quiescent, registers 0..12 read
`08 00 00 ff ff ff ff ff ff ff ff 00 00`**: coordinates park at `0xffff` and the count at 0, identically
on both units — so "no finger" is distinguishable from a failed read with no previous sample to compare
against, which is what lets a reader of this map validate itself.

**`ABS_PRESSURE` carries no measurement.** The vendor driver declares it `0..255` (`capabilities/abs =
1000003` → bits 0, 1, 24) and reports a constant 255 while down and 0 on release — read from disassembly.
`touch_input.c` discards it.

**As shipped.** The stock stack used `xinput_calibrator` and `/etc/pointercal.xinput`. Both belong
to the removed X11 stack and are **not** used by anything current.

### 3.4 Audio

**What's there.** TWL4030 (`U14` TPS65930) HiFi codec driving a **single** metal-can speaker
(`SPKR1`, ~20 mm) through the HandsfreeL/R class-D bridge.

**There is no microphone, no 3.5 mm jack, and no jack footprint.** Confirmed by teardown. The
codec registers a capture PCM and exposes 62 mixer controls including `Analog Left Main Mic`,
`Analog Left Headset Mic`, `AUXL/AUXR`, `TX1`, `TX2` and digital loopback — and the stereo
`Headset` output path is distinct from the `PreDriv` path that drives the speaker. **All of it goes
nowhere.** Mono, one small speaker, is a hardware fact and not a driver limitation.

**How to drive it.** ALSA card `rw20`, `hw:0,0` (`twl4030-hifi` ↔ `49022000.mcbsp`). OSS shim at
`/dev/dsp`, `/dev/audio`, `/dev/mixer`. **GPIO12 must be driven HIGH to unmute the amplifier.**

```
DAC1 (app rate -> 48000 SRC) -> HandsfreeL Mux (AudioL1) -> HandsfreeL Switch -\
                              -> HandsfreeR Mux (AudioR1) -> HandsfreeR Switch --> SPKR1
```

| Control | Range | Use |
|---|---|---|
| `DAC1 Digital Fine Playback Volume` | 0..63 | 63 |
| `DAC1 Digital Coarse Playback Volume` | 0..2 | 0 (0 dB) |
| `PreDriv Playback Volume` | 0..3 | 0 = mute, 3 = +6 dB |

Boot setup is `device-files/audio-enable` (→ `rc5.d/S29audio-enable`): GPIO12, the Handsfree
controls above, **and the Predrive path** — `PreDriv Playback Volume` 3, the `PredriveL/R Mixer`
inputs on, `DAC2` analog 4 / coarse 2. ⚠️ **Without the Predrive settings the speaker is silent, and the
codec boots with them muted** — measured on `.188` 2026-10-11: a card with an empty `/var/lib/alsa/`
played nothing under either bootloader; loading a playing card's mixer state, then the script alone,
each restored sound by ear. ⚠️ **Our root filesystem restores no mixer state** — it has no
`alsa-state` init script (measured, `/etc/init.d` on `.188`), so `asound.state` is read only by a
manual `alsactl restore`, and every setting the speaker needs must be in `audio-enable`.

Native rate is 48000 Hz; `plughw` and the OSS shim both sample-rate-convert automatically. ScummVM runs 22050 Hz
(`[inferred]` halves OPL synthesis cost — arithmetic, never measured), native games 44100 Hz.

⚠️ **A native ALSA client works through the device's own dynamic `libasound`, on both cards.** Kernel:
`CONFIG_SND`, `SND_PCM`, `SND_SOC`, `SND_OMAP_SOC`, `SND_OMAP_SOC_MCBSP`, `SND_SOC_TWL4030` all `=y`
(`usb_host/device_config:2711`, `:2713`, `:2757`, `:2778-2779`, `:2855`); OSS is `SND_PCM_OSS` emulation
(`:2718-2720`) over the same cards. Userspace ships `libasound.so.2.0.0` (**alsa-lib 1.2.1.2**), `aplay`,
`amixer`, `alsactl`, 238 files under `/usr/share/alsa` — which **`libasound` itself needs**: every
`snd_pcm_open` loads `alsa.conf`, and `plughw`/`plug` are defined only there [inferred from alsa-lib] — but no headers — a same-version alsa-lib build
supplies them (§6.3); git tag `static-only-last` is the last commit before that dynamic build. Measured 2026-09-27 on our kernel: a soft-float client played on `plughw:0,0` and
`plughw:1,0`. **Native apps and ScummVM are such clients, and ALSA is their only output** — `audio_out.c`
opens `plughw:N,0` for `/dev/dspN`; on `.188` 2026-09-28 native apps and ScummVM held `/dev/snd/pcmC1D0p`, and ScummVM's 22050 Hz
1 ch request was granted 1 ch, so `plughw` converts. Dynamic, not raw ioctls: `bluez-alsa` is a plugin.

- ⚠️ **`CONFIG_SND_SEQUENCER` is not set** (`:2731`), so ScummVM's `--enable-alsa` is a trap: it is
  MIDI/sequencer support, not PCM output. PCM is hand-written, which is why `oss-mixer.cpp` exists.
- ⚠️ **After a USB audio card is removed, `snd_pcm_avail_update()` lies and `snd_pcm_avail()` tells the
  truth.** Measured 2026-10-01 on `.188` (alsa-lib 1.2.x, ARM, no mmap'd status page, so every query is
  `SYNC_PTR`): `avail_update` returned a frozen positive count with no error for the 25 s watched, while
  `avail` returned `-ENODEV` at once and `snd_pcm_writei` `-EBADFD`. On a healthy stream `avail_update`
  also lags `avail` by up to ~1.8k frames (under one period). `native_apps/common/audio_out.c`
  `alsa_space()` therefore asks `snd_pcm_avail`; a stream that only polls `avail_update` never notices a
  second unplug. Host regression: `native_apps/tests/audio_alsa_lost_test.c` over a stub ALSA header.

**What `hw:0,0` actually grants — measured on `.188`, 2026-08-14**, with
`native_apps/tests/alsa_probe.sh`. That probe needs nothing cross-compiled: the vendor's `aplay`
has `--dump-hw-params`, which is the same `SNDRV_PCM_IOCTL_HW_REFINE` any client calls.

| Parameter | Range | Consequence |
|---|---|---|
| `CHANNELS` | **2, exactly** | ⚠️ stereo-only, see below |
| `RATE` | `[8000 96000]`, continuous | 8000/11025/22050/32000/44100/48000/96000 all granted **exactly** (`exact rate 22050 (22050/1)`), so no userspace resampling is ever needed |
| `FORMAT` | `S16_LE`, `S32_LE` | `S16_LE`, as today |
| `PERIOD_SIZE` | `[4 16384]` frames | ⚠️ the shim always takes 2048, see gotcha 1 |
| `BUFFER_SIZE` | `[640 32768]` frames | 13 ms … 683 ms at 48000 |
| `PERIODS` | `[2 255]` | |
| `ACCESS` | `MMAP_INTERLEAVED`, `RW_INTERLEAVED` | a plain `write()` loop is fine; no mmap needed |

⚠️ **`hw:0,0` is stereo-only. The hardware is mono; the interface is not.** `CHANNELS: 2` is a point,
not a range, and `aplay -c 1` is **refused at every one of seven rates**. **The speaker sums L and R —
measured, see below** — so mono remains the right *source* model, but the device boundary must hand the
kernel **interleaved stereo frames** with the sample duplicated into both. The interleave arithmetic is
*confined to that one place* rather than removed. Frame/byte confusion is the bug to guard against
(`bytes == frames * 2ch * 2B`), not the interleaving itself.

**`SPKR1` sees L + R, not L − R — measured on `.188`, 2026-08-14.** Three 48 kHz stereo tones identical
but for channel phase (`native_apps/tests/audio_phase_gen.py`, played by `audio_phase_test.sh`):

| Tone | L, R | Heard |
|---|---|---|
| `left` | `s`, `0` | audible — reference |
| `dup` | `s`, `s` | **slightly louder than `left`** |
| `anti` | `s`, `−s` | **inaudible** |

Complete cancellation on anti-phase is possible only if the speaker is driven by the **sum**. So
**duplicating a mono sample into both channels is correct, and is the loudest of the three options.**
⚠️ **"HandsfreeL/R class-D bridge" above means each amp is internally bridge-tied — it does NOT mean
`SPKR1` bridges L against R.** That ambiguity is worth spelling out because the wrong reading predicts
silence for exactly the write a mono backend performs, and the two readings are indistinguishable by
playing music: real stereo content never has `L == R`. The harness self-checks (equal peaks, and
`R==0` / `R==L` / `R==-L` in every loud frame) so that a loudness comparison cannot be measuring
differing content instead of phase.

- ⚠️ **The tones must be *self-identifying*.** The first attempt played all three back to back and the
  operator reported *"I think I heard only two"* — which cannot distinguish "one was silent" from "I lost
  count". `audio_phase_test.sh` precedes tone *N* with *N* marker clicks, which made the answer decisive.
- **Consequence for streamed stereo content**: a stereo file plays as an analogue `L+R` downmix. So
  storing music **mono on disk and duplicating at playback halves the file and the SD read bandwidth
  at no audible cost** — a mono `(L+R)/2` source, duplicated, reproduces what the speaker already does.
- ⚠️ **A pure sine at peak 18000 does NOT reproduce cleanly, and the ceiling is FREQUENCY-DEPENDENT — the
  driver is excursion-limited, not amplitude-limited.** Measured `.188` 2026-08-16/17 **[n=1, by ear against
  a signal generator]** through the vendor's `aplay`, so none of this repo's audio code is in the path. A
  peak-6000 *square* is harsh and audibly **louder** than the sine (3 dB more RMS at equal peak — the
  control showing the listener was discriminating). At 440 Hz the ladder is monotonic: 6000 clean, 8000
  *"woodwind"*, 12000 *"brass", harsher*, 18000 distorted. At a **constant** 18000 it instead **falls with
  pitch**: 220 Hz *"clearly a square wave"*, 440 Hz noisy, 1320 Hz *"cleaner"*. So **≈55 % is not a safe
  acoustic ceiling**, the overdrive is amp-or-cone below every digital stage, and ⚠️ **one global peak is
  the wrong shape of limit** — it over-quietens the nearly-clean band and still leaves 220 Hz square.
- ⚠️ **The usable BAND is narrow, and low pitches fail on LEVEL rather than on reproduction.** Sharp
  rolloff below ~700 Hz; below ~300 Hz inaudible at viewing distance, audible only with an ear at the
  panel; below 500 Hz hard to hear at the shipped level. A continuous glide is what measures this — one
  `aplay` per tone restarts the stream and cannot. **An effect centred below ~700 Hz is unusable on this
  hardware whatever its shape**, and everything audible in the tree today is ≥ 880 Hz.
- ⚠️ **TWO sustained sines INTERMODULATE, and no level law fixes it — the fix is source material.** Measured
  `.188` 2026-08-20 the same way, a **pre-mixed** two-sine WAV through the vendor's `aplay` with no mixer,
  generator or pump in the path: *"same distortion as on the device"*. 440 + 880 (peak 12287, RMS 6143) is
  far worse than a lone 440 (peak 16383, RMS 11585), so it is not level in either sense — what the ear gets
  is |m·f1 ± n·f2|, inharmonic to both inputs unless they are octaves. ⚠️ **Two BROADBAND streams at the
  same level are CLEAN** (`officerunner1-mono` + `officerunner2-mono` overlapping in full), so **effects must be
  broadband, not sustained tones, whatever their source**; per-frequency budgeting is comfort, not the fix.
- ⚠️ **The +12 dB of apparently unused analog gain is NOT headroom — spending it distorts.** `DAC1 Analog
  Playback Volume` (numid=6) sits at **12 of 18** on a −24 … +12 dB scale and `DAC1 Digital Fine` (numid=2)
  is already at 0 dB, so the codec looks able to pay for a lower digital peak. It cannot: measured `.188`
  2026-08-17 **[n=1, by ear]**, peak 6000 is clean at 0 dB analog and *"louder but distorted"* at +12 dB —
  **identical samples**, so the limit is **acoustic**, downstream of every gain stage the codec exposes.
- ⚠️ **The kernel will NOT mix — a second concurrent stream is refused.** One card, **one playback
  subdevice** (`/proc/asound/pcm`), and `default` is `plug` over raw `hw:0,0` (`aplay -v`; `/etc/asound.conf`
  is an empty comment), so a second `aplay` during a first dies `Device or resource busy` — measured `.188`
  2026-08-17. `aplay -D plug:dmix` **does** take two streams, but only via `libasound`, which ships
  **shared**-only. **Mixing two sounds is userspace work by construction, not an optimisation.**
- ⚠️ **Audio on this device cannot be measured acoustically — there is no microphone, and the codec's
  loopbacks run the wrong way.** `arecord` works and the card really does have a capture subdevice
  (`/proc/asound/pcm`), so this looks possible and is not: with `TX1 Capture Route` on `Analog`, both main-mic
  and AUXL capture switches on and gain at maximum, a recording is **railed 50 Hz mains hum**, and the 440 Hz
  bin is *identical* between a silent capture and one taken during a loud 440 Hz playback — measured `.188`
  2026-08-17, so there is no acoustic and no electrical coupling to find. Nor can the codec be turned into a
  loop: **both loopback families route capture → playback** (mic-to-speaker), not playback → capture
  (`../usb_host/linux-4.14.52/sound/soc/codecs/twl4030.c:1553-1579`). ⚠️ **So every timbre and loudness
  finding in this section is necessarily `[n=1, by ear]`, and no future session can promote one by
  instrumenting the device.** What *can* be measured objectively is the byte stream we send it
  (`native_apps/tests/oss_play.c --dump`), which is a different claim.
- ⚠️ **There is no AVLS, AGC or output limiter — and no external amplifier.** `SPKR1` is driven from the
  TPS65930 itself (`../HARDWARE.md`), and the driver's only playback-side dynamics are an **anti-pop ramp**
  on output enable (`handsfree_ramp()`, `headset_ramp()`, `twl4030.c:593-720`, ramping *up*) plus a one-time
  offset cancellation at init. So a *"starts distorted, then settles"* onset cannot be an AVLS.
- ⚠️ **Every audible click is a stream transition, not the samples, and there are TWO** — `.188` 2026-08-17,
  operator at the panel, each prediction stated before the listen, `[n=1, by ear]`. **(a)** A lone click ~5 s
  after the last sound is the ASoC power-down: `/sys/devices/platform/sound/TWL4030 HiFi/pmdown_time` reads
  **5000**, and writing a large value removes it — an 8 s gap then stays silent. **(b)** A click still ends
  every sound with the power-down held off, and it survives **400 ms of trailing silence appended to the
  file**, so it is the stream *stop* (DAI teardown, which `pmdown_time` does not guard), not a truncated tail
  — `oss_play.c:336` already drains with `SNDCTL_DSP_SYNC`. Heard as *"an old CB radio push-to-talk"*: a step
  through the class-D bridge. **Native ALSA clicks too** (*"clinking then a klack then a beep"*, below), so
  no userspace path avoids it — only not stopping the stream does. ⚠️ **(b)'s scope is one stop in
  isolation, and it does not yet reconcile with repeat presses** — a diagnostic screen pressed again
  inside the default `pmdown_time` was reported inaudible (`.188` 2026-09-05, `[n=1, by ear]`), whereas (b)
  was measured with the power-down held *off*. The two have never been listened to together, so neither
  bounds the other; do that before promising any screen is click-free.
- **The OSS shim adds no conversion when the parameters already match — it cannot reshape a waveform.**
  Every plugin in `snd_pcm_plug_format_plugins()` is gated on a mismatch (`sound/core/oss/pcm_plugin.c:414`
  onward: mu-law, channel reduction, resample, format), and with `S16_LE`/2/44100 granted at **both** layers
  — `SOUND_PCM_READ_*` on our side, `/proc/asound/card0/pcm0p/sub0/hw_params` on the slave's — none is
  built, so the write is a plain copy into the same ring `aplay` fills. `direct` is 0 here, but that only
  runs the *builder*, which then adds nothing. **[source-read, with both endpoint states measured]**
- ⚠️ **The production OSS buffer is 743 ms in 46.4 ms periods, and the shim grants 2048 frames and 16
  periods at EVERY rate and channel count tested** — measured `.188` 2026-08-17 and again 2026-08-18
  (`native_apps/tests/oss_geom.c`, both client configurations) with no `SNDCTL_DSP_SETFRAGMENT`:
  `period_size` **2048** frames, `buffer_size` **32768**, `fragsize` scaling with the frame size alone
  (8192 B stereo, 4096 B mono), against `aplay`'s 5512 / 27560. So at 22050 mono that same 2048 frames is
  **92 ms** and the ring **1486 ms**. It is what any OSS client gets.
- **The two consumers attenuate differently:** the native synth pins a peak of **18000**
  (`AUDIO_PEAK` in `native_apps/common/audio_gen.h`, a constant rather than a shift); ScummVM does `>>1`
  post-mix. The summing is why ~50 % looked about right, but the ladder above puts 18000 past clean at
  every pitch a canned sound uses, and no mixer control refunds the loudness that lowering it costs.
- ⚠️ **Digital attenuation inside the codec does not fix a distortion introduced upstream of it, and that
  is what makes it a probe.** Cutting `DAC1 Digital Fine Playback Volume` by 20 dB with `amixer` (numid=2,
  63 → 43) made the app's tone quieter with the timbre **unchanged** — measured `.188` 2026-08-16 — which
  localises a corruption **above** that control, where a cleaned-up timbre would localise it below.
  ⚠️ **On its own it localises nothing**, because a controlled comparison then found no corruption to
  localise. Restore with `amixer cset numid=2 63,63`.

⚠️ **A `-c 1` probe fails on CHANNELS at every rate, which reads as "no rate is accepted"** — that is how
`alsa_probe.sh`'s first run produced seven REFUSED lines from one mistake; the number was the harness. Same
cause: the shipped `/opt/sound/*.wav` are mono, so `aplay -D hw:0,0 asl_click.wav` fails and **that is not
a broken audio path** — use `plughw:0,0`, or generate stereo (`speaker-test -c 2`).

**Native ALSA is audible on hardware — confirmed by the operator at `.188`, 2026-08-14.** Both paths
of `alsa_probe.sh` step 7 exited 0 *and* were heard: the three mono WAVs through `plughw:0,0`, then a
440 Hz sine **straight at `hw:0,0`** — no plug, no conversion.
Reported as *"some clinking then a klack then a beep"*. So the native path is proven end to end
independently of anything this project writes.

- The **"klack"** falls between the two, i.e. at a stream open — and a start-of-stream pop is now the
  *measured* explanation of the ~60 ms minimum-tone rule rather than a plausible one: see gotcha **6**
  below, where keeping a stream continuously fed drops the audible floor to 5 ms.

⚠️ **Native ALSA's latency win over the shim is ~2× at the period, not the ~24× a 506 ms period would
imply.** `period_size=1024, buffer_size=4096` is **granted exactly** at 48000 (`period_time: 21333` µs)
and at 22050; vanilla `omap-pcm.c:49-52` (`period_bytes_min = 32`, `periods_min = 2`,
`buffer_bytes_max = 128 * 1024`) puts the floor at 8 frames and the ceiling at 32768. **A client that
does not ask for a small period gets a big one, and the shim cannot ask because
`SNDCTL_DSP_SETFRAGMENT` is ignored** — but it settles for 2048 frames, so it is 21 ms against 46 ms.

**Gotcha — the OSS shim is buggy, in six distinct ways.** All of these are in `snd-pcm-oss`
emulation, not the hardware. ALSA itself works correctly.

1. **A blocking `write()` stalls for hundreds of ms — the ~506 ms figure is that STALL, not a period.**
   The period is 2048 frames (46 ms) and the ring 32768 (743 ms), both measured twice (above), so a
   blocking write waits on the RING draining rather than on a period; the exact 506 ms is unaccounted
   for, and the ~22,317-frame "period" it was read as reproduces in **no** configuration `oss_geom.c`
   tested. Measured effect: 185 ms of audio, 321 ms of silence, repeating — the "bru-bru-bru-KLICK"
   artifact, diagnosed with `native_apps/tests/oss_diag.c`. **Always open `/dev/dsp` with `O_NONBLOCK`**
   and handle `EAGAIN` with a ~5 ms sleep.
2. **Speaker distortion at full scale.** Apply ~50 % software attenuation (`>>1` on int16) before
   writing. ScummVM does this post-mix.
3. **ioctls reset each other.** `SNDCTL_DSP_STEREO` is **silently ignored** (returns `rc=0,
   stereo=1` while the device stays mono — verified with `native_apps/tests/ch_test.c`);
   ⚠️ note the PCM underneath is **stereo-only** (measured above), so what stays mono is the *shim's*
   view of it, with `SND_PCM_OSS_PLUGINS` converting below — *inference from the two measurements, not
   itself measured.*
   `SNDCTL_DSP_SPEED` may reset format and/or channels; `SNDCTL_DSP_SETFMT` may reset speed; and
   set-ioctl output values may not reflect actual device state. **Workaround:** set SPEED → FMT →
   CHANNELS in that order, then read back the truth with `SOUND_PCM_READ_RATE`,
   `SOUND_PCM_READ_BITS`, `SOUND_PCM_READ_CHANNELS`, and use the read-back rate **and the read-back
   channel count**. *Evidence:* at
   22050 Hz music played at half speed; at 48000 Hz it got proportionally worse (~4×), consistent
   with `_outputRate` not matching the real device rate. No client in this tree opens the shim —
   `native_apps/common/audio_out.c` (which ScummVM also links) is ALSA-only.
4. **32-bit `time_t` overflow.** `sizeof(long) == 4`. Never compute
   `(now.tv_sec - epoch_0) * 1000000L` — baseline timers to *current* time, not epoch zero.
5. ⚠️ **The shim only hands ALSA WHOLE PERIODS, and an underrun DISCARDS what it was staging.**
   Measured on `.188` 2026-08-15 with `native_apps/tests/audio_mix_test` holding `/dev/dsp` open, from
   `/proc/asound/card0/pcm0p/sub0/`: our `O_NONBLOCK` open negotiates **`period_size` 2048 frames
   (46 ms) / `buffer_size` 32768 (743 ms) / 16 fragments of 8192 B**, and `appl_ptr` steps
   2048 → 4096 → 6144 and **never lands between two periods** — while `SNDCTL_DSP_GETOSPACE` counts the
   partial period the shim is still accumulating. So **a write smaller than one period is invisible to
   ALSA until the period completes**, and a writer that keeps only 1.7 periods queued leaves ALSA one
   playable period: it drains that, `state` goes **`XRUN`**, and the recovery throws the staged bytes
   away. Polled 14 × 40 ms, the cycle is `RUNNING → XRUN → RUNNING` about every **120 ms**, which is
   also the crack rate an operator counted by ear in a 3 s tone.
   **Consequence for any incremental writer here: measure the period and queue a whole number of them,
   at least three.** `native_apps/common/audio_gen.c`'s `audio_pump_lead_frames()` is that rule.
   ⚠️ **`GETOSPACE` over-reports what is really buffered by ~1.3 periods (~60 ms), so a nominal lead is
   worth that much less in real audio.** Measured `.188` 2026-08-18 by `native_apps/tests/oss_keepalive.c`,
   reading `GETOSPACE` and `/proc/asound/card0/pcm0p/sub0/status` at the same instant: `in_flight` stands
   **2650–2670 frames** above the kernel's own `buffer_size − avail` on every `RUNNING` row. A 3-period
   139 ms target therefore holds ~79 ms, falling to **44 ms** at its shallowest under a 33 ms service loop.
   ⚠️ **Hence a never-reset stream must be serviced every ~66 ms or better: 33 and 66 ms produce ZERO dry
   windows, 100 ms empties the ring on 15 of 59 services, and 150 and 200 ms on every one** (one run, one
   continuous stream, swept clean-to-dirty; 150/200 are the probe's own positive control, since both
   exceed the lead arithmetically). ⚠️ **An underrun here is otherwise INVISIBLE** — the shim swallows it,
   and `CONFIG_SND_PCM_XRUN_DEBUG` is unset (`usb_host/device_config:2730`) so neither `xrun_debug` nor a
   `dmesg` trace exists. The two `/proc` witnesses, and the way each of them lies, are in that probe's header. **Known limit, measured `.188` 2026-09-28:** the Mix Bus Test cracks from about 6 simultaneous 440 Hz voices while the screen is fully redrawn (7th with only the readout band repainted), not from clipping (`clip=0 lim=0`); the mechanism is an underrun at `snd_pcm_writei` hidden by `alsa_recover()` **[inferred]**, and the per-sample `sin()` in `audio_gen.c` is the suspected cost. Accepted, not fixed.
6. ⚠️ **The minimum audible tone length is a property of RESTARTING the stream, not of
   `SNDCTL_DSP_RESET`** — removing the reset does not change it. Measured on `.188` 2026-08-15: with the
   ring allowed to empty between sounds, 5–40 ms is inaudible, 60 ms partial, 100 ms clean; with the
   stream **continuously fed** (silence written between sounds; repeated 2026-08-18 on the never-reset
   stream), **5 ms is audible and 20 ms recognisable**. Any claim about
   a minimum tone length must say which regime it was measured under; nothing in the tree clamps it.

**A USB DAC is a second card, and it is the only route to stereo and to a headphone jack.** Four
modules — `snd-hwdep`, `snd-rawmidi`, `snd-usbmidi-lib`, `snd-usb-audio` — load with plain `insmod`, no
`-f` and no unresolved symbols, from `/etc/init.d/usb-audio-modules`, whose header holds the OSS minor
and why it is deterministic. `/proc/asound/cards` then gains `1 [Device]: USB-Audio` and the shim appears
at `/dev/dsp1`. Which card an app opens is the `audio_device` key — `onboard` | `usb` | `auto`, default
`auto`; [`native_apps/CLAUDE.md`](native_apps/CLAUDE.md) holds the one-home rule for resolving it.

⚠️ **A USB card advertises a different parameter set, so the `hw:0,0` table above cannot speak for it.**
`[n=1]` on a C-Media `0d8c:0014`, and the same on `.188`'s dongle 2026-09-28: `/proc/asound/card1/stream0`
offers `S16_LE`, `Channels: 2` and rates **48000 and 44100** only — no 22050, no mono; 44100 needs no SRC.

⚠️ **But the OSS shim reports the client's REQUEST back as the grant, on either card, so
`SOUND_PCM_READ_*` is not a capability probe.** A mixer asking 22050/1 is answered `granted 22050 Hz
16-bit 1 ch` on `/dev/dsp1` — the card just described, which advertises neither — and identically on
`/dev/dsp`, an A/B whose only variable was the `audio_device` key. Reading the grant back is still
correct, because it is what the shim will deliver; it simply cannot reveal what the card underneath
would have refused. **So a per-device rate preference has nothing to prefer** — the conversion is the
shim's either way, and moving it into a caller would double that caller's mixing and synthesis cost to
remove it.

**PIO costs about 0.6 pp of the one core, so bandwidth is not the objection to USB audio.** Busy ticks
over a wall-clock denominator, three interleaved 20 s runs each proving the stream still alive 2 s before
the end: onboard 41/35/34 ticks ≈ **1.8 %**, USB 51/50/41 ≈ **2.4 %** `[n=1]`. The figures are `aplay`,
not our mix bus, and carry no valid process-versus-IRQ split. ⚠️ **`/proc/stat`'s *total* column is not a
usable denominator on this kernel** — `NO_HZ_IDLE` with `TICK_CPU_ACCOUNTING` never samples idle ticks,
so it under-counts: three nominal-15 s windows reported totals of 1434, 1450 and 1179. Busy ticks are
honest and `CONFIG_HZ=100`, so the denominator is wall seconds × 100.

**As shipped.** The vendor's `init_amixer.sh` never unmutes any mic — corroborating that nothing
was ever wired to the capture path.

### 3.5 Network and power

**Ethernet.** 10/100 Mbps via `J3` (TE MagJack `1-6605834-1`) and `U15` **SMSC LAN9221** — a
**MAC+PHY in one package** on the GPMC bus, so there is no separate PHY chip. MAC seen on RW09:
`00:07:B0:0D:30:53`. **The MAC lives in the LAN9221's own EEPROM, so no rootfs sets it** — measured
2026-10-08 on `.188`: `ethtool -e eth0` reads `a5` (the valid-EEPROM signature) then `00 07 b0 0d 3c 5e`,
which is what `smsc911x` prints at probe (3.17 s, before any init script); `addr_assign_type` is `0`, and
neither `/proc/cmdline` nor the DT carries one (`smsc911x_init_mac_address` falls back to the registers the
EEPROM loaded).

**Power: 802.3af PoE only. There is no barrel jack.** The whole PD front end is on the main board:

| Ref | Part | Role |
|---|---|---|
| `J3` | TE MagJack `1-6605834-1` | RJ45 with integrated magnetics and centre taps |
| `U1` | TI **TPS23750** | 802.3af PD interface **+** DC/DC controller |
| — | Coilcraft **POE13F-12L** | isolated flyback transformer (Coilcraft's TPS23750 reference part) |
| `Q1` / `D1` / `U2` | `4848 5BD` / `NHSTQW 3406` / `MT1107` | flyback FET / rectifier / isolated feedback |
| `U4` | TPS54325-class | secondary-rail buck |

- **A PoE injector or PoE switch port is the only way to power the unit**, on the bench as much as
  on the wall. Budget for one before planning any out-of-case session.
- **A shut-down unit turns on when a micro-USB OTG adapter is plugged in; USB power coming in does not
  start it.** Measured 2026-10-13 on `.188` by the operator, unit fully dark after a shutdown: two different
  adapters (one empty) and an unpowered hub on its own micro-USB plug each started it at plug-in, a pull
  did nothing, and a plain micro-USB cable did nothing with or without a charger on its USB-A end. So the
  start event is the ID pin going to ground, which the TWL4030 watches from an always-powered register
  ([§3.6](#36-usb)). The cause is in our power-off, read from source: our DT's `ti,system-power-controller`
  (`kernel/dts/rw20.dts`) makes `poweroff` call `twl4030_power_off()`, which clears `STARTON_VBUS` and
  `STARTON_CHG` in `CFG_P1_TRANSITION` before `DEVOFF` (`drivers/mfd/twl4030-power.c:667`) and leaves
  `STARTON_USB` (start on USB host) as it found it. **[inferred]** that bit's value has not been read on a unit.
- **Class budget is 12.95 W at the PD**, enforced by the TPS23750's class signature. Everything
  added inside the case draws from it — an XBee at ~50 mA is nothing, a bus-powered USB hard disk
  is not going to work.
- The RJ45 is isolated (magnetics in `J3`, transformer isolation in the flyback), so the Ethernet
  side is safe — but **`J3`'s centre taps carry up to 57 V while powered**.

**Invisible to software.** `/sys/class/power_supply/` shows only `twl4030_ac` and `twl4030_usb`,
both reading 0 (not connected). Nothing reports PoE state.

**No wireless of any kind is fitted.** No WiFi, no Bluetooth. The 802.15.4 socket is empty — see
[Serial ports](#312-serial-ports).

**Host name and resolution — the image ships a broken one.** `/etc/hostname` on the vendor rootfs
is `RW09`, and `/etc/hosts` carries an extra *non-loopback* line mapping that same name to an
external address that is unreachable from anywhere these units are used:

```text
127.0.0.1 localhost
<external address> RW09.<external domain> RW09
```

Two defects in one file. The name is **baked into the image** rather than generated, so every unit
cloned from it claims `RW09` — which is also where this repo's name for the reference unit came
from. And on a stock image **the device's own name resolves to a dead address**, so anything that
resolves its own hostname gets the wrong answer. `set-hostname` (the overlay's `/usr/sbin/set-hostname`) fixes both files together, to
loopback-only (`provision.sh --hostname` calls it over SSH, and the unit's own
`S04firstboot-hostname` calls it at the first boot of a card image).

**⚠️ The vendor regenerates all four network files on every boot, so editing them is not the last
word.** `/opt/sbin/networkmanager` — a 24,894-byte shell script, started by `/etc/init.d/networkmanager`
and **byte-identical on both captured cards** — rewrites these from `/home/root/data/websign/net.*`:

| File | Written by | From |
|---|---|---|
| `/etc/hosts` | `set_manual()`: truncated to `# Generated by PV networkmanager` + `127.0.0.1 localhost`, then `<ip> <name>` appended | `net.ipaddress`, `net.hostname` |
| `/etc/hostname` | `set_manual()` and `set_dhcp()` | `net.hostname` |
| `/etc/resolv.conf` | truncated and rewritten | `net.dnsserver` |
| `/etc/dhclient.conf` → `send host-name` | `set_dhcp()` — **this is the name a DHCP server, and therefore a router's device list, sees** | `net.hostname` |

- **`websign/net.mode` selects the branch, and `manual` makes the unit unreachable.** `dhcp` runs
  `dhclient eth0`; `manual` runs `ifconfig eth0 <net.ipaddress> netmask <net.netmask> up` and **never
  sends a DHCP request** — so the unit appears in no lease list and answers only on whatever subnet its
  previous owner used. A stock card can ship either way: one captured unit is `manual`, with a static
  RFC-1918 address on its previous owner's subnet and `net.hostname = null`, and its own syslog records
  `Manual IP Mode detected.` / `Vaild host name found: null` / `status: manual-bound`.
- **Deleting `/home/root/data/websign` makes the script inert for the host name.** Both writers live
  *inside* `set_manual()`/`set_dhcp()`; with `net.mode` unreadable neither branch runs, so `/etc/hosts`
  and `/etc/hostname` are never touched again.
- ⚠️ **The vendor's own validator rejects hyphens.** `net.hostname` is filtered by an awk regex that
  accepts `RW09`, `RW20`, `rwtest` and `null` but **rejects `RW-Test` and `rw-test`**; a rejected name
  logs `Invalid host name detected.` and the DHCP client then announces the hardcoded fallback
  `rwtwenty`. Prefer a hyphen-free name on any unit that still has the vendor stack.
- **There are two dhclient scripts, and only the vendor's rewrites `/etc/hosts`.** `/etc/dhclient-script`
  (vendor, 10,370 bytes) has a `# PV02 Addition` block that on every `BOUND` event writes
  `net.hostname` into `/etc/hostname`, truncates `/etc/hosts` to `127.0.0.1 localhost` and appends
  `<leased-ip> <name>`. `/sbin/dhclient-script` (the stock ifupdown one, 16,772 bytes) contains
  **zero** references to `/etc/hosts`. Which one runs depends on who starts `dhclient`: the vendor
  `networkmanager` passes `-sf /etc/dhclient-script`, while `S40networking` +
  `auto eth0 / iface eth0 inet dhcp` uses the default. So once the `networkmanager` boot link is gone
  **nothing regenerates either file**, which is what makes an offline-set name stick — measured on a
  unit in service, five months of daily `/var/lib/dhcp/dhclient.leases` updates with `/etc/hosts` and
  `/etc/hostname` untouched.
- ⚠️ **`/etc/dhclient.conf`'s `send host-name` is a third copy of the name**, and it is the copy a
  router's device list shows: the vendor image ships `send host-name "RW09";`, and a unit renamed
  months earlier still announced `RW09`. `set-hostname` writes this file too.

**mDNS is present but not started.** `/usr/sbin/avahi-daemon` (109 KB) and a complete
`/etc/init.d/avahi-daemon` are both on the vendor image — there is simply **no `rc5.d` link**, so it
never runs. One symlink enables it (`provision.sh` now adds `S30avahi-daemon`), after which the
unit answers to `<name>.local` and neither SSH nor the deploy scripts need a DHCP-lease hunt. Its
`Required-Start` is `$remote_fs dbus`, so dbus must be running too.
The shipped `enable-wide-area=yes` and
`publish-workstation=no` do not affect `.local` resolution. **Listeners:** the vendor image had UDP 514 (syslogd, no
arguments), UDP 5353 plus a random avahi port, TCP 22, UDP 68; `SYSLOGD="-s -s"` in `/etc/default/syslogd` and avahi
`use-ipv6=no`, `allow-interfaces=eth0`, `enable-wide-area=no`, `enable-reflector=no` leave UDP 68, 5353 and TCP 22 only [measured, `.188`].
⚠️ This is only useful **after** each unit gets a unique name: with the stock image every unit is
`RW09`, and avahi resolves the collision by renaming to `RW09-2.local`, `RW09-3.local` and so on.
**Cost measured on RW09 2026-08-03 — cheap:** ~3.9 MB RSS total (2424 kB `avahi-daemon` + a 1512 kB
chroot helper) of 234 MB, and it does **not** start dbus — `dbus-daemon` was already running at a
lower PID and costs its own 1692 kB regardless.
⚠️ **`.local` resolves from Windows but not from WSL**, whose `nsswitch.conf` is `hosts: files dns`
with no mDNS module — so `ssh root@rw09.local` works in PowerShell while the WSL-based build and
deploy scripts cannot resolve it until `libnss-mdns` is installed there.

### 3.6 USB

**One connector: `J4`, micro-USB, MUSB OTG in host mode.** Working today for keyboards, mice,
touchpads and game controllers. A micro-USB OTG adapter is required, and a powered hub is
recommended (the port may not supply enough VBUS).

**There is no second USB port and no unpopulated footprint for one.** The device tree declares two
EHCI high-speed host ports with their own PHYs and VBUS regulators, and `hsusb2_phy` even carries a
board-specific reset GPIO (`gpio1[13]`) implying deliberate wiring — but nothing was ever brought
out to a connector on board revision `550-0204-03`. `CONFIG_USB_EHCI_HCD` and
`CONFIG_USB_OHCI_HCD` are both unset and `dmesg | grep ehci` is empty. **Even with a rebuilt kernel,
enabling EHCI would gain nothing — there is nowhere to plug in.** Recorded so the decision is not
re-litigated.

**Three problems stand between the vendor kernel and host mode. Hacks 1 and 2 still ship; the third
is not solved:**

**Hack 1 — MUSB forced to IRQ-driven PIO, patched at runtime through `/dev/mem`.** The OEM kernel has
*both* `CONFIG_USB_INVENTRA_DMA` and `CONFIG_MUSB_PIO_ONLY` unset, so MUSB init always fails with
`DMA controller not set` (`-ENODEV`). This is a build defect, not a version problem. The fix writes
noop stub function pointers into the `dma_init`/`dma_exit` fields of the `omap2430_ops` struct in
live kernel memory, forcing PIO fallback; the driver then rebinds successfully. Re-applied every
boot by `/etc/init.d/usb-host` (S90).

> ⚠️ **State this the right way round: the patch makes MUSB *give up* on DMA.** It does not fix or enable
> DMA — it installs stubs so the core falls back to interrupt-driven programmed I/O. **DMA remains
> unavailable on this device**, and every USB transfer costs CPU. That is fine for input devices and a
> Bluetooth dongle (tens of KB/s) and starts to matter for uncompressed USB audio (~190 KB/s).
> ⚠️ `CONFIG_DMADEVICES=y` and `CONFIG_TI_EDMA=y` *are* set and are a **red herring** — that is the
> **system** EDMA via dmaengine, not the Inventra engine inside the MUSB block that OMAP3 uses.
> Our own image sets `CONFIG_USB_INVENTRA_DMA` and DMA works there; the config symbol is the safe route [inferred] — a force-loaded module could not safely supply the controller, since a misbehaving DMA engine scribbles into RAM where today's stubs fall back to PIO ([kernel/README.md](kernel/README.md)).

**Hack 2 — three cross-compiled kernel modules for Xbox controllers.** `CONFIG_INPUT_JOYSTICK`,
`CONFIG_INPUT_JOYDEV` and `CONFIG_INPUT_FF_MEMLESS` are all unset, and the Xbox 360 pad
(`045e:028e`) uses vendor-specific class `ff` rather than HID class `03`, so `usbhid` ignores it.
Three modules built from matching 4.14.52 source and loaded in order by `/etc/init.d/S89xpad-modules`:

| Module | Size | Purpose |
|---|---|---|
| `ff-memless.ko` | 8.4 KB | force-feedback support (xpad dependency) |
| `joydev.ko` | 19.5 KB | `/dev/input/jsX` interface |
| `xpad.ko` | 36 KB | Xbox gamepad driver |

Loadable modules work because `CONFIG_MODULES=y`, `CONFIG_MODULE_FORCE_LOAD=y`, and
`CONFIG_MODULE_SIG` is unset.

**Problem 3 — the USB power budget is 100 mA, and nothing raises it.** The DTB appended to
`uImage-system` sets the MUSB `power` property to `0x32` (50 → **100 mA**), so anything drawing more —
an Xbox pad wants 500 mA — is rejected with `rejected 1 configuration due to insufficient available bus
power` when connected directly; a powered hub is the remedy. Read out of the 4.14.52 source 2026-08-08:

| Where | What happens |
|---|---|
| `drivers/usb/musb/omap2430.c:452` | `of_property_read_u32(np, "power", (u32 *)&pdata->power)` — read from the device tree **at driver probe** |
| `drivers/usb/musb/musb_core.c:2369`/`:2381` | passed on as `musb_host_setup(musb, plat->power)` |
| `drivers/usb/musb/musb_host.c:2797` | `hcd->power_budget = 2 * (power_budget ? : 250);` |

The driver reads it before any init script exists, so the value lives only in the image's DTB; our
image keeps `0x32` ([kernel/README.md](kernel/README.md)). ⚠️ **Note the `? : 250`: an absent or zero
property would already give 500 mA** — the vendor set 100 mA over the kernel default, a vendor choice and
not a hardware limit. The vendor `uImage-system` is byte-identical across units (`edc637ac14f90e0187b1ed65ffedf6d7`,
5,225,796 bytes; measured 2026-08-08 on five sources from three units). The byte patch of that image which
raised the budget is deleted with vendor-kernel support; tag `last-vendor-kernel` carries it.

**Supported device types:**

| Device | Driver | Works out of the box |
|---|---|---|
| Keyboard, mouse, touchpad, hub | `usbhid` / `hub` (built in) | ✅ |
| HID gamepad (generic) | `usbhid` | ✅ if HID-compliant |
| Xbox 360 / One controller | `xpad` (module) | ❌ needs the three modules |
| **Bluetooth dongle** | `btusb` — ⚠️ **module, loaded by hand** | ❌ `# CONFIG_BT is not set` in the image; see below |
| **USB audio class DAC** | `snd-usb-audio` (module) | ❌ needs four modules, which ship — [§3.4](#34-audio) |

**A Bluetooth dongle is the only route to a wireless peripheral.** There is no radio on the board at all
([`HARDWARE.md` §4](HARDWARE.md#4-unpopulated-and-expansion)), so BT means a dongle in this single
connector. `# CONFIG_BT is not set` in the image, but Bluetooth builds as loadable modules against it with
no p1 write (`kernel/build-bt-modules.sh`; measured: the relinked `vmlinux` is byte-identical and every
imported CRC matches). ⚠️ **`lmp_subver` `0x8761` does not identify an RTL8761 variant** — A, B and CU all
report it and differ by `hci_rev`, which 4.14's `btrtl` does not look at. Dongle state and next steps are open
work in [`IMPROVEMENT_PLAN.md`](IMPROVEMENT_PLAN.md); the *wired* USB DAC ships ([§3.4](#34-audio)).

⚠️ **The 8BitDo Pro 2 works over Bluetooth in X mode (recommended) and in A mode; its radio shows three classic
identities and one LE** (measured 2026-10-01, `.188`, RTL8761CU, BlueZ 5.66). **X mode, hold pair ~3 s:** it
answers classic inquiry as "8BitDo Pro 2" (`E4:17:D8:40:EB:AE`), Just Works pairing with a `NoInputNoOutput`
agent succeeded first try, and it presents as an Xbox One S controller (`045e:02e0`, version `0903`) bound by
`hid-generic` — `event0` + `js0` + a kbd handler, ten keys `0x130`..`0x139` (`BTN_SOUTH`..`BTN_TR2`) plus `KEY_MENU` and **no `BTN_SELECT`/`BTN_START`**, `ABS` 0-5
and `HAT0X/Y`. ⚠️ **Same hardware, different codes: View/Menu arrive as `0x136`/`0x137`, where `xpad` sends `BTN_SELECT`/`BTN_START`** (pad-tester numbers, code minus `0x130`, measured by the operator: 8BitDo bottom 0, right 1, left 2, top 3, Select 6, Start 7; Xbox 360 `xpad` bottom 0, right 1, left 3, top 4, Back 10, Start 11), and the order is positional (A,B,X,Y,LB,RB,View,Menu,LS,RS), not the 8BitDo's Nintendo labels. The operator verified it in the Control Panel pad tester
and Office Runner, and with a USB `xpad` pad live at the same time (the tester attributes events per source). **A
mode** answers as "Wireless Controller" (`E4:17:D8:13:49:41`, class `0x002508`, modalias `usb:v054Cp05C4d0100`, a
DualShock 4); its first pairing attempt gave `AuthenticationCanceled` (cause not investigated), the second
succeeded, and `hid-generic` makes one node with `BTN_SOUTH`..`BTN_THUMBR`. In S mode only the LE identity was
seen. That one, `8ap` (`E4:17:D8:42:BD:FF`, LE public), is connectable `ADV_IND` with AD flags `0x02` (no "BR/EDR
not supported" bit); its GATT has **no HID service (`0x1812`)**, so connecting yields no input node **[inferred:
8BitDo's companion-app channel]**; `pair` pages it over BR/EDR and fails with Page Timeout. ⚠️ **Witness an
advertisement by btmon's event matched on the address — not by `bluetoothctl`'s `NEW` line nor by btmon's
abbreviated title** (`btmon -T` prints `Inquiry Result` for an Extended one): X mode was first recorded as silent
from a busybox `grep` of `bluetoothctl` output, while btmon had logged 96 EIRs. ⚠️ **A scripted `bluetoothctl` needs ~2 s between starting and `agent` or the registration fails** (measured 2026-08-08), and a classic HID keyboard ("BT Keyboard 5.1", class `0x002540`) paired Just Works with no PIN, `hidp` → `hid-generic` making one node for keys and touchpad that `app_launcher` hot-plugged.

⚠️ **BlueZ 5.66 starts no BR/EDR connection on adapter power-up or re-add** (measured from source: `plugins/policy.c` reconnects only after a link loss, and `src/device.c` `device_set_auto_connect` is LE-only), so a bonded headset or pad stays off after a panel reboot or a hub reset until something calls `Device1.Connect`; `device-files/bluetooth` does that from its `watch` subcommand (measured 2026-10-05, `.188`, reboot: `reconnect: hci0 appeared`, then both bonded devices connected ~13 s and ~20 s later). Non-interactive `bluetoothctl --timeout N connect` exits 0 on failure and always runs to its timeout (measured), so judge a connect by `Device1` `Connected`. The device has udevd 243, no mdev and no `/sys/class/rfkill` (measured).

Hubs work, including combo devices with a built-in hub; multiple simultaneous devices are fine.

⚠️ **On the vendor kernel a stale `is_active` arms a `printk` loop that survives unplugging and ends in
a hardware-watchdog reset ~46 min later — run `dmesg | grep -c musb_bus_suspend` before trusting any
on-device measurement, and discard it if non-zero.** A clean unplug hits the `default:` arm of the
`MUSB_INTR_DISCONNECT` switch (`musb_core.c:897-900`, `unhandled DISCONNECT transition (a_idle)`), which
leaves `musb->is_active` set; once the child is gone, root-hub autosuspend calls `musb_bus_suspend()`
(`musb_host.c:2588`), which fails `-EBUSY` through an unratelimited `WARNING()` (`musb_debug.h:38-41`),
and `hcd_bus_suspend()` retries with no backoff: ≈2000 lines/s, the CPU starved, `FAT-fs … not properly
unmounted` next boot. It needs **both** the stale flag and the child gone — measured apart, neither
storms. Babble is neither necessary nor sufficient (`.188`'s `/home/root/log/messages` holds storms
without it; `omap2430_ops` has no `.recover`). A driver unbind+bind ends a live storm in ~20 s. Our
image patches the switch ([`kernel/README.md`](kernel/README.md#what-we-patch-and-why)).
Touchpad-plus-keyboard combos create two event nodes.

**Verified working:**

```
musb-hdrc musb-hdrc.0.auto: MUSB HDRC host driver
hub 1-0:1.0: USB hub found
input: HID 04d9:a088 as .../input3   (keyboard)
input: HID 04d9:a088 as .../input4   (mouse)
input: Microsoft X-Box 360 pad as .../input5
```

⚠️ **On the vendor kernel — and on ours without the ID-ground patch — a device is enumerated only if it
is attached when the MUSB driver probes.** Boot with nothing plugged in and the port is dead for the rest
of that boot — anything inserted afterwards is never even powered. Measured on `.188` 2026-08-13,
reproducible with a driver unbind/bind, and the operator reports it on every unit: *"USB only worked if
it was connected at boot."* Control Panel → USB → **RESCAN** revives a dead port in one tap, ~5 s, and
once a port is live, replug works at any gap. **On our image an adapter plug starts the session itself**
(the mailbox guard below; measurement in [`kernel/README.md`](kernel/README.md#what-we-patch-and-why)).
Where `$MUSB` = `/sys/devices/platform/68000000.ocp/480ab000.usb_otg_hs/musb-hdrc.0.auto`:

| Action | Result |
|---|---|
| device attached as the driver probes (boot, or a rebind) | **works.** `Vbus on`, enumerates ~0.5 s later |
| driver probes with **nothing** attached | `Vbus on` for a few seconds, then **`Vbus off`** and the port is dead. The OTG adapter alone does not prevent this — measured with the adapter in and no device in it |
| plug a device into that dead port | **nothing at all.** No log line, no VBUS, the device's own LED stays dark |
| unplug a working device, wait 95 s, replug | **works.** `Vbus on` throughout; `dmesg` shows `unhandled DISCONNECT transition (a_idle)` and the disconnect is not processed until the device returns — ⚠️ **not benign: that unprocessed disconnect is the stale `is_active` above**, and it is what arms the `printk` loop |
| driver unbind + bind with the device attached | **the only userspace recovery found.** Enumerates ~0.5 s later |

**`$MUSB/vbus` is the diagnostic — `Vbus on` live, `Vbus off` dead.** Its on/off half is a real read of
the DEVCTL VBUS comparator, not cached state.

**Why, read out of the 4.14.52 source** in [`usb_host/linux-4.14.52/`](usb_host/linux-4.14.52/) — vanilla
upstream, and authoritative for this code because none of it is vendor-patched:

- VBUS here is driven **solely by the DEVCTL `SESSION` bit**. `twl4030` registers no `set_vbus` op, so
  `otg_set_vbus()` returns `-ENOTSUPP` and `omap2430_musb_set_vbus()` does nothing.
- **The OTG ID pin is watched by the TWL4030 PMIC, not by MUSB** — its own interrupt line
  (`phy-twl4030-usb.c:747`) reading an always-powered `PM_MASTER` register, so it fires with the PHY
  asleep and VBUS off, and an adapter plug reaches the glue as `MUSB_ID_GROUND` (a pull as
  `MUSB_VBUS_OFF`; measured by kprobe on `omap2430_musb_mailbox`). ⚠️ **`omap_musb_set_mailbox()`
  (`omap2430.c:171-181`) then writes `SESSION` only `if (musb->gadget_driver)` — always NULL with
  `# CONFIG_USB_GADGET is not set` — so the plug is dropped and a cold port stays cold.** Nor does
  `omap2430_musb_enable()` set it (`:330-333`, UTMI only; this board is ULPI). Our image drops the guard.
- **Without that patch a session is never torn down.** `omap2430_ops` has no `.try_idle`, so
  `musb_platform_try_idle()` is a no-op — which is why a live port stays live across an unplug. With the
  patch an adapter pull clears `SESSION` (`set_vbus(0)`, state `B_IDLE`). With nothing connected,
  `musb_pm_runtime_check_session()` matches `MUSB_QUIRK_A_DISCONNECT_19` and after 3×1000 ms polls drops
  its pm_runtime reference.

⚠️ **A hub or adapter left permanently attached does NOT fix a dead port** — the teardown path's reading
assumes a session already exists. A passive hub on a dead port reads `Vbus off` for minutes and a device
plugged into it enumerates nothing; re-seating an adapter revives only a port that has had a session.

⚠️ **Plugging a USB pad re-enumerates the whole tree, so the Bluetooth dongle's `hci0` is recreated and every BT
link drops** (measured 2026-10-01, `.188`: hub `1-1` re-found, dongle `1-1.3`). Bonds in `/var/lib/bluetooth`
survived and the pad reconnected on its Home button once `hci0` was powered; without `/etc/bluetooth/main.conf`
BlueZ 5.66 leaves the new adapter `Powered: no`. We ship `AutoEnable=true` ([`device-files/bluetooth-main.conf`](device-files/bluetooth-main.conf)); **measured 2026-10-01, `.188`:** after `/etc/init.d/usb-host recover`, `btusb` reloaded the `rtl8761cu` firmware and `bluetoothctl show` read `Powered: yes` with no manual step. **Boot start also works** (measured, same unit): after a reboot `/etc/init.d/bluetooth` (S91) started `bluetoothd`, the modules loaded, `hci0` was `Powered: yes`, the bonds survived, and the 8BitDo Pro 2 (X mode) reconnected on a Home press, drove `app_launcher` (which holds its event node by hot-plug) and stayed connected 4 m 39 s. ⚠️ **The operator's hub is effectively unpowered — it claims a supply and has none — and that is why a wired pad drops everything** (measured 2026-10-05, `.188`): plugging an Xbox 360 pad disconnects `usb 1-1` (the hub) and everything behind it, the BT dongle included; all re-enumerate within ~1.5 s, and the `watch` reconnect in `device-files/bluetooth` re-connects the bonded devices about a minute later. A low-power 2.4G receiver did not drop it; musb logs a VBUS error only at `KERN_DEBUG`. **Use a hub with its own working supply.**

⚠️ **BlueALSA (and any GIO/GDBus program) fails on this device unless `DBUS_SYSTEM_BUS_ADDRESS=unix:path=/run/dbus/system_bus_socket` is set.** `/var/run` is not a symlink to `/run` here; the vendor libdbus compiles in `/run/dbus/system_bus_socket` (the real socket) but the vendor libgio 2.62.6 compiles in `unix:path=/var/run/dbus/system_bus_socket`, which does not exist, so the daemon dies with `Couldn't obtain D-Bus connection: No such file or directory` (measured 2026-10-01, `.188`: `grep -a` on both libraries plus `ls`). The vendor libasound's external-plugin directory is `/usr/lib/alsa-lib` (compiled in, absent on stock), and its `alsa.conf` load hook reads `/etc/alsa/conf.d`, not `/usr/share/alsa/alsa.conf.d` (same measurement).

⚠️ **`plug:bluealsa` with no `DEV` opens the most recently connected sink; with none connected the open fails `-19` ENODEV** (measured 2026-10-01, `.188`). Its `hw_params` ranges are period 480..262144 and buffer 960..206158430 frames, so **a client that leaves the buffer at max gets a ~5 s first period and silence** — set both; the A2DP SBC grant was S16_LE stereo 48000 Hz. A2DP adds ~150-250 ms latency [inferred]; the Sony WI-C310 reported 180 ms (measured, same unit). BlueALSA starts at boot from `/etc/init.d/bluetooth` as `bluealsa -S -p a2dp-source -p hfp-ag` after `bluetoothd`, with the D-Bus address above exported. **`DEV=<addr>` of an absent sink fails at once** (`ENODEV` "PCM not found", 60 ms, measured 2026-10-02, `.188`).

⚠️ **BlueZ 5.66 clears the kernel bondable flag while no agent is registered, unless `main.conf` `[General]` has `AlwaysPairable=true`** (`src/adapter.c` `adapter_set_io_capability`; measured 2026-10-01, `.188`): without it `bluetoothctl pair` gave `Paired: yes` / `Bonded: no`, stored no LinkKey, and the headset fell into pairing mode at each power-on. We ship it in `device-files/bluetooth-main.conf`, and it must stay: `device-files/bluetooth-input.conf` sets `ClassicBondedOnly=true` (BlueZ 5.66 reads it, measured from source; 5.71 only flipped the default), which rejects an unbonded pad. **BlueZ 5.66 does not expand `%h` in `Name=`** (measured `.188`: the adapter advertised a literal `%h`), so `device-files/bluetooth` starts `bluetoothd -f` on a tmpfs copy of `main.conf` with `Name=$(hostname)`. **A bonded headset (Sony WI-C310) powered on pages back only a host that offers Handsfree Audio Gateway** (measured, same unit, n=1 device): with A2DP alone `btmon` showed no Connect Request; with `-p hfp-ag` it connected within seconds, as it does to a phone. HFP needs `rfcomm.ko` (`BT_RFCOMM`, built by `kernel/build-bt-modules.sh`). ⚠️ **`btmon -w` read with `btmon -r` while still running can lack the newest events** (not yet flushed): a reconnect looked absent and was there a minute later. `btmon` ships at `/usr/bin/btmon`.

**After pairing an audio device, if `bluetoothctl info` lists only the PnP UUID, `disconnect` then `connect`.** The first pairing of a Sony WI-C310 left SDP incomplete, so `connect` failed `br-connection-profile-unavailable`; the reconnect re-ran SDP (Audio Sink, Headset, Handsfree, AVRCP) and A2DP connected (measured 2026-10-01, `.188`, n=1 device).

⚠️ **Five readings that look diagnostic and are not** — three were believed and written down before being
refuted, one of them in this document.

| Reading | Actually |
|---|---|
| `echo host > $MUSB/mode` | **silent no-op** — `omap2430_ops` has no `.set_mode`, so the store reports success having done nothing |
| `$MUSB/vbus`'s `timeout 1100 msec` | **inert** — nothing on omap2430 reads `musb->a_wait_bcon`; writing it changes the printed number and nothing else |
| `power/control = on` (forbidding runtime PM) | **does not prevent the drop** — measured with `runtime_status` reading `active` throughout |
| `$MUSB/mode` as a *live-port* reading | **not diagnostic of a live port** — reads `a_idle` with a pad enumerated, `js0` present and the game responding (unpatched; our patched image reads `a_host`). ⚠️ **It IS the one observable of a dead port after `VBUS_ERROR`** (measured 2026-10-01, `.188`, our image): the kernel logged `VBUS_ERROR in a_idle`, `mode` then read `b_idle`, every device stopped working, yet no disconnect was logged and `/sys/bus/usb/devices` still listed them all (stale); `usb-host recover` fixed it on attempt 1 (seen once; operator's inference: port overload, ~1100 mA declared against 500 mA, no recurrence in a day of heavy use after the load was cut). A `b_*` reading means dead whatever the device list says (`usb_port_dead()`, `native_apps/control_panel/usb_bus.c`) |
| `twl4030-usb/vbus` | **not a port-state reading at all** — 0444, reports `vbus_supplied` (somebody feeding *us*), so it reads `off` in the working state **and** the dead one |
| `lsmod` → `xpad … 0` | a refcount of module *users*, not bound devices — reads `0` with a pad bound and `event1`/`js0` present |

**The one real userspace trigger besides a rebind is debugfs `softconnect`** (`musb_debugfs.c:301-343`) —
and it sets `SESSION` **only** in `OTG_STATE_A_WAIT_BCON`, so it cannot revive a port sitting in `a_idle`.

⚠️ **Three source-derived mechanisms were applied and refuted on hardware before the mailbox guard was
found**, the last the DTB `mode` 3 → 1 patch: the **booted tree** read back `mode = <1>`, and a pad plugged
in after an empty-socket boot stayed dark while `usb-host recover` brought it up on attempt 1. None of the
three explained how a cold port obtains a session; the guard does.

**Reading the live device tree.** `/sys/firmware/devicetree/base/` is the unflattened tree as the running
kernel holds it, and `/sys/firmware/fdt` the raw blob.
⚠️ **`find /proc/device-tree -name X` silently finds nothing**: `/proc/device-tree` is a *symlink* to the
sysfs path and `find` does not follow it. Use the `/sys/firmware/devicetree/base` path — that is how the
booted `mode` value was confirmed against the running kernel rather than against the decompiled
`usb_host/original.dts`.

**`/etc/init.d/usb-host recover`** does the rebind — unbind, settle `RECOVER_SETTLE` (2 s) so VBUS can
decay below VBusValid, bind — and retries up to `RECOVER_TRIES` (3), stopping the moment a **non-hub**
device appears and exiting non-zero on exhaustion. Plug the device in **first**. Reachable from the panel
as Control Panel → USB → **RESCAN**, which forks it when a scan finds nothing **or `mode` reads `b_*`**, and by opening that page with an
empty or `b_*` port (one attempt, measured 6.0 s on `.188`); measured on `.188`
2026-08-14 at ~5 s from one tap, leaving `Vbus on`, `1-1`, `event1` + `js0` and the pad playable. ⚠️ It is
deliberately not on a timer, and the reason is not merely wasted rebinds: **unpatched, nothing in software can
distinguish "nothing is plugged in" from "a pad is plugged into an unpowered port"** — VBUS is off either
way and no connect interrupt can arrive in either — so an operator who has just plugged something in
holds the one bit no poll can obtain.
Full technical detail, including MUSB memory addresses, the `omap2430_ops` struct layout, why
`mmap()` works where `write()` does not, and the approaches that failed:
[`usb_host/README.md`](usb_host/README.md).

**Application-side input.** All input is evdev (`/dev/input/event0`–`event31`). Native apps use
`native_apps/common/gamepad.c`, which abstracts touch + USB keyboard + USB mouse + Xbox pad behind
abstract button IDs (`BTN_ID_UP` … `BTN_ID_BACK`) and is configurable via `/etc/input_config.conf`.
**New apps should use it rather than reading evdev directly.** ScummVM has an independent evdev
implementation carrying the same fixes. Analogue stick centre is computed as
`(axis_min + axis_max) / 2` rather than trusting the kernel-reported value; default dead zone 25 %.

### 3.7 LEDs, backlight and PWM

**What's there.** A **bi-colour red/green** status LED and the panel backlight, all three on true
`leds-pwm` channels backed by dedicated dmtimers:

```
pwmchip0 - dmtimer-pwm@9      pwmchip1 - dmtimer-pwm@11      pwmchip2 - dmtimer-pwm@10
```

```bash
/sys/class/leds/red_led/brightness     # 0-100, real duty cycle
/sys/class/leds/green_led/brightness   # 0-100
/sys/class/leds/backlight/brightness   # 0-100
```

C implementation: `native_apps/common/hardware.c`. Vendor scripts that still exist:
`/opt/sbin/backlight/setbacklight.sh`, `/opt/sbin/brightness.sh`, `/opt/sbin/conc_leds.sh`.

**The backlight ceiling is 100, and a unit without the vendor stack is already sitting on it.** `max_brightness` is 100
and a write above it does not raise the duty — `echo 150` reads back `100`. Measured on `rwtest`
2026-08-06 before `sshd` and long before `arca-app`: a freshly booted unit with the vendor stack removed reads
**100 of 100** with nothing in our stack having written it — `app_launcher` makes no `hw_set_backlight()`
call at all. The vendor's own mechanism (`adjustbklight.sh` → `setbacklight.sh` / `backlight.sh -1`)
writes this one node from `websign/brightness.conf` and **defaults to 100** when that file is missing,
which is the state without the vendor stack. So a boot-time setter would write 100 over 100.

⚠️ **"The panel looks dim" is therefore not a software question — at a fixed duty cycle, perceived
brightness follows what is *drawn*.** The launcher grid measures **19.2 % mean luminance**, 87.5 % of
its pixels in the darkest quarter (32bpp capture, 2026-08-06); the vendor's browser filled the same
panel with a near-white page. Judge a brightness claim with **identical content on both panels**, or it
measures the UI's palette rather than the hardware.

**There is no third colour and no light bar on the main indicator** — but driving red and green
together gives amber, so the effective palette is red / amber / green with smooth crossfade.

Separately, `J7`/`J8` feed **side LED status bars** in the left and right case edges (driven by
`U22`/`U23`), and `LED1`–`LED5` are discrete SMD indicators on the board itself.

**Gotcha — all three PWM channels are consumed.** There is no spare PWM for a buzzer.

### 3.8 GPIO and pinmux

Six SoC banks of 32, plus the PMIC's and the GPMC's:

```
gpiochip0    GPIO   0-31    48310000.gpio
gpiochip32   GPIO  32-63    49050000.gpio
gpiochip64   GPIO  64-95    49052000.gpio
gpiochip96   GPIO  96-127   49054000.gpio
gpiochip128  GPIO 128-159   49056000.gpio
gpiochip160  GPIO 160-191   49058000.gpio
gpiochip490  TWL4030 GPIO 490-507   (18 pins)
gpiochip508  GPMC GPIO 508+         (4 pins)
```

**Known assignments** (all in bank 1, `gpiochip0`):

| GPIO | Function | Ours? |
|---|---|---|
| 12 | Speaker amplifier enable (out, high) | **Yes** — audio unmute |
| 13 | HSUSB2 PHY reset | No (port is dead) |
| 14 | LCD panel power-down | No (driver-owned) |
| 15 | LVDS enable | No (driver-owned) |
| 16 | Touch controller reset | No (driver-owned) |
| 17 | Ethernet (SMSC) reset | No (driver-owned) |
| 19 | LCD backlight enable | No (driver-owned) |
| 23 | Touch interrupt (active low) | No (driver-owned) |
| twl | SD card-detect | No |

Currently exported: `gpio12` only.

> ⚠️ **Unclaimed ≠ usable.** The pinmux node configures only ten function groups — `backlight`,
> `dss_dpi`, `gpmc`, `green_led`, `red_led`, `hsusb_otg`, `i2c1`, `i2c2`, `mmc1_cd`, `uart2`. Every
> other SoC ball sits at ROM default and may not reach a pad at all. **The 18 TWL4030 GPIOs are the
> safer expansion target** — they are guaranteed real chip pins.

**`debugfs` is not mounted**, which is why `/sys/kernel/debug/gpio` is unavailable. Mounting it
gives the definitive pin-by-pin label/direction/value dump and should be step one of any GPIO work.

### 3.9 I2C

| Bus | Address | Device |
|---|---|---|
| 1 (`48070000.i2c`) | `0x48` | **TWL4030 PMIC** (multi-function) |
| 1 | `0x49`–`0x4b` | dummy placeholders |
| 2 (`48072000.i2c`) | `0x03` | **Touch controller** (Cypress CY8CTMG120) — and the **only** responder on the bus, by a read-only bare-read scan of `0x03`–`0x77`. A 1-byte read with no register pointer returns `0x08`, i.e. register 0. Register map: [Touch](#33-touch) |
| 3 (`48060000.i2c`) | — | `status = "disabled"` |

**Gotcha — do not scan bus 1.** The vendor's own factory-test wrapper warns its light-sensor probe
can hang the bus, and bus 1 carries the PMIC. There is nothing to find there anyway (see
[Not present](#314-what-is-not-present)).

**TWL4030 subsystems**, all on the one chip: audio codec (`twl4030-codec`), battery-charger
interface (`twl4030-bci`), 18-pin GPIO expander, RTC (`twl_rtc`), USB transceiver
(`twl4030-usb`), and the MADC.

**TWL4030 blocks declared in the DT but with no driver compiled:** two general PWMs, `pwmled`
(LEDA/LEDB), a matrix keypad controller, `pwrbutton`, `bci`, and its own watchdog. The
`pwm`/`pwmled` outputs are the natural place to hang a piezo buzzer — but that needs both a kernel
option (blocked) and a wire.

### 3.10 RTC and hold-up

```
Device:  /dev/rtc0
Driver:  twl_rtc (TWL4030 integrated)
```

Working correctly — `hwclock -r` matches `date`, and `provision.sh` already does `hwclock -w`
after `rdate`. `/etc/localtime` and `/etc/timezone` are written by the Control Panel only (System page APPLY, atomically); nothing in `device-files/`, `rootfs/board/` or `provision.sh` writes them [measured by grep].

**Hold-up is a supercapacitor, not a battery.** `U17` is a Panasonic/Matsushita **"Gold Cap"
5.5 V 0.47 F** supercap (`GC5.5V0.47F`). MADC channel 9 reads **3184 mV**, meaning the cap is
charged — not that a cell is healthy.

- **Nothing to replace on a schedule and no leakage risk** — there is no chemistry to exhaust.
  Supercaps age (ESR climbs, capacitance falls) over decades, not years. One less worry on
  eight-year-old hardware.
- **But hold-up is hours-to-days, not months.** **Expect the clock to be wrong after any extended
  unplugged period.** That is why `provision.sh` does time-sync at boot, and it means anything
  trusting the RTC across a shelf-storage gap needs a sanity check rather than blind faith.

### 3.11 ADC and temperature (TWL4030 MADC)

Present and working; the Monitor page reads `in_voltage9` (the RTC cell) and nothing else here.

```
/sys/bus/iio/devices/iio:device0 -> 48070000.i2c:twl@48:madc

in_temp1_input   = 56          # NOT the SoC: ADCIN1, the battery thermistor, raw 0 -> table top
in_voltage0..15_{raw,mean_raw,input}
  ch0,ch11 = 0; ch2-5,7,13-15 < 105 mV; ch6 = 26-29 mV with an Xbox 360 pad on the USB port, 344-364 mV once the BT dongle replaced it [n=1, uncontrolled, cause unresolved]
  ch8 = 5.03-5.10 V VBUS; ch9 = 3.20 V VBKP (RTC supercap); ch12 = 3.28 V VBAT; in_current10 = -750
```

`CONFIG_TWL4030_MADC=y` and the driver probes cleanly at boot. `in_voltage*_mean_raw` gives free
hardware averaging. ⚠️ **The six general-purpose channels are out of scope, not merely awaiting work** —
reaching one needs a test point physically wired to it, which [Hardware policy](#8-hardware-policy)
rules out. `in_voltage9` needs no wire (the Monitor page's `RTC CELL` row, found by iio name `madc`; 3.19 V on `.188`). The only MADC device is `iio:device0`, no hwmon; sampled idle, under a busy loop and at backlight 0/100 on `.188` 2026-10-06, only `thermal_zone0` moves. ⚠️ **`in_temp1_input` is not a die temperature, measured `.188`:** `in_temp1_raw` and `in_temp1_mean_raw` are 0 (ADCIN1, the TWL4030 PMIC battery-thermistor input), and raw 0 maps to the top of the -3..55 °C lookup table (59-3 = 56), so it reads 56 at any temperature. The SoC sensor is `thermal_zone0` (`cpu_thermal`, `step_wise`, no trip points), which reads once `CONFIG_OMAP3_THERMAL=y` (DT already carries `bandgap@48002524` and the zone): ~63 °C idle, 73-75 °C at 85-90 % CPU load with full backlight, and the SoC ran in the 60s with the backlight at minimum against ~70 °C at full, so the backlight measurably heats the SoC area [measured on the panel, `.188` 2026-10-06, no active cooling]. Upstream marks the OMAP3 data `TI_BANDGAP_FEATURE_UNRELIABLE` and Kconfig says it is neither accurate nor near hotspots; a 7-bit ADC with 1-2 °C table steps makes it a trend, not a calibrated temperature; the over-temperature indicator therefore latches at 85 °C and clears at 80 °C as accepted defaults, not datasheet limits. **`ti-soc-thermal … Non-trimmed BGAP, Temp not accurate` prints after a power-on and not after a warm reset, under the vendor bootloader and ours alike unless the SPL has run a conversion** [measured `.188` and `.115`, 2026-10-11]: `bgap_efuse` is 0 in `omap3-thermal-data.c`, so the driver's check reads the sensor's own data register at `CONTROL_TEMP_SENSOR` 0x48002524 (0x41/0x43 after a warm reset), which a power-on clears [inferred]. `rw20_ts_convert()` (`uboot/rw20-board.patch`, SPL stage 1) runs one conversion, and with it a cold power-on prints no such line [measured `.188` and `.115`, 2026-10-12, `dmesg | grep -ic bgap` = 0].

### 3.12 Serial ports

| DT node | Linux | Address | Status |
|---|---|---|---|
| `serial@4806a000` (UART1) | — | `0x4806a000` | **disabled** — do not probe |
| `serial@4806c000` (UART2) | `/dev/ttyO1` | `0x4806c000` | **okay** — the console |
| `serial@49020000` (UART3) | — | `0x49020000` | **disabled** — the radio port |

**`ttyO1` is the console:** 115200 8N1, kernel console output plus a **root login shell already
running** (`/etc/inittab`: `O1:12345:respawn:/bin/start_getty 115200 ttyO1 vt102`, and `ttyO1` is
in `/etc/securetty`). U-Boot prints there too with `bootdelay=1` — a one-second window to reach the
`arca #` prompt (`rw20 #` under the vendor U-Boot). Physically it comes out at **`P4`** at RS-232 levels — pinout in
[Unpopulated and expansion](HARDWARE.md#4-unpopulated-and-expansion).

> **The serial console is not on the recovery path, but the header is now fitted.** Recovery is: pull
> the SD card, reflash the card image, DHCP, SSH. Since the rules in §1 keep NAND
> and U-Boot untouched, the card *is* the entire failure surface — serial adds boot-time *visibility*,
> never recovery capability. What it does buy is the only channel that says *why* an image we built
> failed, and the only way to boot an alternate filename at all ([Kernel policy](#7-kernel-policy)).
> ⚠️ It is RS-232 behind `U27`, so a 3.3 V USB-TTL cable will not work
> ([Unpopulated and expansion](HARDWARE.md#4-unpopulated-and-expansion)).

**UART3 is the 802.15.4 / XBee port, and it is dark.** Two independent vendor sources agree:

```
opt/sbin/RoomWizard-zbgatewayd/readme.txt:
  ./zbgatewayd /dev/ttyS2 --baud 57600 --stdout --nodaemon --config gateway.conf
gateway.conf: channelmask 0x07fff800                          # channels 11-26, 2.4 GHz 802.15.4
              tclinkkey 5a6967426565416c6c69616e63653039      # "ZigBeeAlliance09"

opt/pv02/pv02_app  - full XBee AT-command implementation on /dev/ttyS2:
  ATID (PAN ID)  ATCH (channel 0x0B-0x1A)  ATMY (source addr)  ATDL (dest addr)
  "Start an XBee Loopback Test ... Latency = %iusec"
conc_xbeespam.sh   - runs `pv02_app 8 spam` as a burn-in test
```

Legacy `/dev/ttyS2` under the vendor's old 2.6 kernel = OMAP **UART3** = `serial@49020000` = the
`serial2` alias, so enabling it yields `ttyO2`; `/dev/ttyS0..3` are stale nodes with nothing bound.
⚠️ **The pads are already muxed for UART3, so no pinmux node is needed** — measured on a running unit,
`0x4800219e` reads `0x118` (`PIN_INPUT_PULLUP | MUX_MODE0`) and `0x480021a0` reads `0x000`
(`PIN_OUTPUT | MUX_MODE0`), which is what the upstream OMAP3 boards prescribe; vendor U-Boot sets them
and this project leaves U-Boot alone. Check with `mount -t debugfs none /sys/kernel/debug` then
`grep -E '4800219e|480021a0' /sys/kernel/debug/pinctrl/48002030.pinmux/pins`. The node is otherwise
identical to the working console node, so `status = "disabled"` is all that stands between it and a
`ttyO2`, and a 5-byte in-place edit clears it. Socket pinout and the measured
3.3 V rail: [Unpopulated and expansion](HARDWARE.md#4-unpopulated-and-expansion).

⚠️ **A `ttyO2` is not needed to reach the radio, and `disabled` is the reason: with no driver bound,
nothing contends for UART3, so userspace may own it outright.** Measured on a running unit — the module
is out of reset and already half-clocked, so the whole cost is one bit. `uart3_ick` (`0x48005010`
bit 11) is already set; `uart3_fck` (`CM_FCLKEN_PER`, `0x48005000`, bit 11) is not. Take both from the
vanilla tree's `omap3xxx-clocks.dtsi` (`reg` `0x1000`/`0x1010`, `ti,bit-shift = 11`), not a remembered
layout. With `fclk` on, `0x49020000` reads `MVR` `0x46`, `SYSS` `0x01` (RESETDONE), `MDR1` `0x07` — the
reset value, so nothing has ever driven it — and `LSR` `0x60`; parent `per_48m_fck` is a fixed 48 MHz,
so the 16x divisor is 48e6/(16 × baud) and 57600 wants 52 (`0x34`). ⚠️ **Validate any such probe with
the UART's own internal loopback before believing a silent radio** (`MCR` bit 4, TX→RX inside the
module — not the vendor *XBee* loopback test above): without it a dead radio and a wrong register
sequence produce identical output. `probes/xbee_probe.sh` does that and restores what it writes.

**The vendor assumed a Series 1 module, and the modules on hand are Series 1** — they are
`XB24-ACI-001 revC` (MaxStream, 802.15.4), read off the label, which is why the vendor's settable `ATMY`
and `ATCH` are the right command set. ⚠️ **`AP = 1` is the trap, not the series**: a module in API mode
answers no `AT` at any rate whatever it is, so a framed `ATVR` must be swept before silence is read as a
wiring fault. `probes/xbee_probe.sh` sweeps both, and all eight `BD` rates.

### 3.13 Watchdogs

**Two independent watchdogs. Keep one, disable the other.**

**Hardware — OMAP WDT2. Keep it.**

```
/dev/watchdog, /dev/watchdog0 (OMAP WDT, rev 0x31), /dev/watchdog1
Timeout:  60 seconds
Daemon:   /usr/sbin/watchdog, started by /etc/init.d/watchdog
Config:   /etc/watchdog.conf  (feeds the timer only; test-binary and repair-binary commented out)
Enable:   /etc/default/watchdog -> run_watchdog=1
Feed:     ~1 s (daemon default)
```

Low-overhead and it prevents hard resets. **Once opened it must be fed continuously** — any app
that owns the screen for long periods must keep feeding it.

**Steelcase software watchdog — cron-based. MUST DISABLE.**

```
*/5 * * * * /opt/sbin/watchdog/watchdog.sh
```

`watchdog.sh` → `watchdog_test.sh` checks whether HSQLDB, Jetty and the browser are running and
whether the browser log is fresh. Any failure exits 100–112, `watchdog_repair.sh` runs, and when
repair fails it calls `/sbin/reboot`. There is a grace period of roughly 65 minutes of "repeat
failure in grace period" before it gives up.

**In game mode those services are all absent, so this reboots the device every ~70 minutes.**

**Our own root carries no cron and none of this.** `/opt/sbin/watchdog` is a *directory* of three scripts
(`watchdog.sh`, `watchdog_repair.sh`, `watchdog_test.sh`) that exists only on the vendor root.
`watchdog_test.sh` skips every check unless `/var/watchdog_test` (or `_checkmem`) exists. The hardware
watchdog above is a separate mechanism and stays.

### 3.14 What is not present

**Confirmed absent:**

- ❌ **WiFi / Bluetooth** — no radio of any kind fitted. A USB Bluetooth dongle is the only route, and
  `CONFIG_BT` is unset ([§3.6](#36-usb), [`IMPROVEMENT_PLAN.md`](IMPROVEMENT_PLAN.md)).
- ❌ **Ambient light sensor** — and none is possible. The teardown found no sensor part **and no
  aperture, window or light pipe anywhere in the enclosure**: the case is light-tight. The vendor
  factory test does have a light-sensor step (`functionaltest.sh` → `pv02_app 5`, on `/dev/i2c-1`) —
  it is shared firmware across a product family. **Do not probe bus 1 looking for it.**
- ❌ **Microphone, and any audio input path to the outside** — no MEMS mic, no electret, no
  acoustic port. See [Audio](#34-audio).
- ❌ **3.5 mm jack**, and no unpopulated footprint for one.
- ❌ **Second USB port**, and no footprint. See [USB](#36-usb).
- ❌ **Occupancy / PIR / proximity sensor** — a grep of the entire vendor rootfs for
  `rfid|nfc|badge.?reader|PIR|occupancy|proximity|motion.?sensor` returns nothing. RoomWizard 2.0
  detects presence by people tapping the screen.
- ❌ **Badge / NFC / RFID reader** — not on this model.
- ❌ **Camera** — the ISP block is enabled in the DT but no sensor is declared and no driver module
  exists on disk.
- ❌ **Accelerometer.**
- ❌ **Second SD slot** — `mmc@480ad000` and `mmc@480b4000` are both `status = "disabled"`.
- ❌ **Video output** — no HDMI or VGA. A composite/CVBS encoder exists in the SoC and `manager1:
  tv` is present in omapdss, but no connector is known to be routed. Unverified.
- ❌ **Hardware RNG** — `/dev/hwrng` exists but `rng_current = none` (not bound on GP silicon). Use
  `/dev/urandom`.

**Present but unusable without a config change, so gated on the image we build** — see
[Kernel policy](#7-kernel-policy):

- ⚠️ **SPI** — four controllers (`spi@48098000`, `@4809a000`, `@480b8000`, `@480ba000`) are all
  `status = "okay"` in the DT, but **`CONFIG_SPI` is not set**, so `/sys/bus/spi/` does not exist.
  No children declared.
- ⚠️ **EHCI USB host** — see [USB](#36-usb). Doubly dead: no kernel support *and* no connector.
- ⚠️ **USB gadget mode** (presenting the device *as* a USB keyboard, serial port or ethernet
  adapter) — **`CONFIG_USB_GADGET` is not set**. The MUSB controller is dual-role capable and the
  micro-B port is the one physical socket, so this is a config-only block, but still a rebuild.

---

## 4. Boot chain and recovery

### 4.1 The chain

**One ROM and one NAND shim start two possible boot chains; which one runs is decided by what is on p1 of the card.**
If you know the Raspberry Pi: the SoC's mask ROM (the Pi's GPU boot ROM) reads a small first-stage file from the FAT
partition (`MLO` here, `bootcode.bin` on the Pi 3 and earlier) whose only job is to wake SDRAM; a bigger loader follows (`u-boot.img`, where
the Pi has `start.elf`), then the kernel; and U-Boot reads an optional text file of settings (`uEnv.txt`, roughly the Pi's
`cmdline.txt` plus `config.txt`). Two things have no Pi counterpart: the OMAP ROM looks in NAND first **[inferred]**, so a
12 KB program there exists only to send it to the card, and the kernel is a U-Boot `uImage` (64-byte header) with the
device tree appended.

```
OMAP3 ROM (in the SoC, cannot be changed)
 └─> NAND /dev/mtd0 "boot" @0x0 (+ mirror @0x20000): "Nuvation NAND-Boot Redirector Ver 1.0"
       TI GP header, 12,548 B, load addr 0x40200800 (SRAM). Prints a banner on UART2 (115200).
       Writes a software boot configuration "MMC1" (0x480029C0, pointer at 0x48002910), then
       warm-resets via PRM_RSTCTRL. It loads nothing itself; the ROM loads MLO without any check by it.
 └─> OMAP3 ROM again: loads MLO from FAT p1 of the card (bootable, 70.6 MB) to 0x40200800
 └─> SPL (the MLO file): clocks, pinmux, SDRAM, then the next stage from FAT p1
 └─> U-Boot proper  ──>  kernel uImage-system (appended DTB)  ──>  Linux 4.14.52, root=/dev/mmcblk0p3 (ours; the vendor's is p6)
```

The redirector was disassembled from a dump of `mtd0` (measured). The two chains differ from the SPL's file name on:

| Stage | Vendor chain | Our chain |
|---|---|---|
| SPL | `mlo`, U-Boot SPL 2015.07 | `MLO` (FAT is case-blind: the same directory entry), mainline SPL from `uboot/rw20-board.patch` |
| SPL loads | `u-boot.bin` (raw) | `u-boot.img` (legacy uImage, load/entry `0x80100000`) — the mainline SPL does **not** load `u-boot.bin` |
| U-Boot | 2015.07 Gumstix-Overo derivative, board `rw20`, prompt `rw20 # `; build date varies per unit | v2026.10, prompt `arca # `, no persistent environment; build notes `uboot/BUILD.md` |
| Before booting | `bootcmd`: `mmc dev 0; mmc rescan; cb_getinfo 0x82000000` — the custom command reads `ctrlblock.bin` ([§4.5](#45-control-block-and-boot_tracker)) | `preboot` imports `uEnv.txt` if present (whitelist `bootargs`, `optargs`, `kernel`, `fallback`, `bootdelay`), then the `bootdelay` (1 s) |
| Chooses the kernel | `cb_boot_mode` `system`: `uImage-system`; `bootstrap`: `uImage-bootstrap` + `ramfilesys.gz` | `uImage-system`, else `uImage-fallback` (names overridable by `uEnv.txt`); no `cb_getinfo`, so `ctrlblock.bin` is never read |
| Boots it | `sysargs` then `bootm` ([§4.4](#44-the-u-boot-environment-cannot-be-persisted)) | `fatload` then `bootm`, ATAGs + the DTB appended in the image; `bootm` returning (bad image) falls through to the fallback |

**The two chains program the SoC identically** — measured with `uboot/tools/dump-boot-regs.sh`, 251 registers (padconf, SDRC, CM,
PRM, GPMC), the vendor's chain against ours. `.188` 2026-10-11 and `.115` on a warm reboot: byte-identical. `.115` 2026-10-12, booted
from a generated card, both dumps with the backlight at 0: 249 identical, 0 faults, **two differ** — `CM_FCLKEN1_CORE` (0x48004A00)
0x4000 against 0xC000 (bit 15, `EN_I2C1`) and `CM_IDLEST_CKGEN` (0x48004D20) 0xA0B against 0xA0F (bit 2, the 96 MHz clock, I2C's
functional clock); a repeat dump gave the same. **[inferred]** Kernel runtime state, not bootloader setup: measured on that card
`i2c1_fck` enable count 1, `48070000.i2c` `runtime_status` active, `twl4030-usb` (the only TWL child with runtime PM) active, musb mode
`a_host`; and the same U-Boot build gave 0xC000/0xA0F on `.188` and 0x4000/0xA0B on `.115`'s old card. **Take both dumps with the
backlight in the same state:** `CM_FCLKEN_PER` (0x48005000) bit 10 (`EN_GPT9`) follows it, and with it off GPT9's registers bus-fault
**[inferred]**: GPT9 is the backlight PWM; seen on both units. **Not covered by the comparison:** the PMIC (TWL4030), the GPIO banks,
McBSP, `CONTROL_DEVCONF*`, `CM_CLKOUT_CTRL`. A card reader undoes a bad `MLO` or `u-boot.img`, JTAG a bad `mtd0` ([§1](#what-a-boot-path-write-costs-to-undo)).

**The vendor U-Boot is a Gumstix Overo derivative** (its strings carry Overo's "Unrecognized expansion board" and
`rw20-storm`). **The SDRAM setup it leaves behind**, read with `devmem` on `.188` 2026-10-09 and found as the same words in one
table in all three vendor SPLs: 256 MB on CS0 only (`SDRC_MCFG_0` `0x03588099`, CS1 disabled), `ACTIM_CTRLA_0`
`0x7AE1B4C6`, `ACTIM_CTRLB_0` `0x00024217`, `RFR_CTRL_0` `0x0005E601`, `MR_0` `0x32`, `CS_CFG` `0x2`, `SHARING` `0x100`,
`DLLA_CTRL` `0xA`. Clocks: 13 MHz sys_clk, core 332 MHz, L3 166 MHz, MPU 600 MHz. The refresh word is the 200 MHz table's,
so at 166 MHz refresh runs about every 9.1 µs [inferred from the decode].

**p1, file by file** — md5 and `grep -a 'U-Boot'` on the p1 of four units, 2026-10-09; the last three rows are ours:

| File | What it is | Read by | Notes |
|---|---|---|---|
| `mlo` | the SPL | the ROM, after the redirector's reset | **differs per unit**: three vendor builds, 2017-08-06 (53,260 B), 2021-04-06 and 2021-12-13 (50,336 B); any one boots on any unit (operator, cross-booted cards). Ours replaces it in place |
| `mlo.vendor` | a copy of the vendor's `mlo` kept beside ours on the test unit | nothing | the undo: copy it back over `mlo` |
| `u-boot.bin` | vendor U-Boot, with `cb_getinfo` | the vendor SPL | the same build as that unit's `mlo` |
| `u-boot-sd.bin` | **byte-identical to `u-boot.bin`** on all four (md5 `34550b9c…`) | neither chain | — |
| `ctrlblock.bin` | the 28-byte control block, [§4.5](#45-control-block-and-boot_tracker) | vendor `cb_getinfo`; userspace `/opt/sbin/ctrlblk` | **identical** on all four (md5 `e9ec9adb…`) |
| `uImage-bootstrap`, `ramfilesys.gz` | the vendor's recovery/upgrade kernel and ramdisk | vendor U-Boot in `bootstrap` mode | deleted from our card |
| `uImage-system` | the kernel: 64 B header + zImage + appended DTB (`CONFIG_ARM_APPENDED_DTB=y`; no `.dtb` file exists). The vendor's original is 5,225,796 B (zImage 5,158,728 B, DTB 67,004 B @ `0x4eb788`) | both U-Boots | ours: `kernel/README.md` |
| `u-boot.img` | our U-Boot proper | our SPL | placed by hand |
| `uImage-fallback` | a kernel tried when `uImage-system` fails to load or boot | our U-Boot | placed by hand |
| `uEnv.txt` | optional settings, template `uboot/uEnv.txt` | our U-Boot (`preboot`) | missing or empty = the built-in defaults |

A card from `rootfs/make-card-image.sh` carries none of the vendor files: its p1 holds our `MLO`, `u-boot.img`, `uImage-system`,
`uImage-fallback` (a second copy of the same kernel) and `uEnv.txt` ([§4.2](#42-partitions)). On a unit that still has the vendor's p1,
`mlo.vendor` and the last three rows are placed by hand.

**The kernel command line** has three sources: U-Boot's string (vendor: `sysargs` plus words U-Boot appends; ours: the built-in default, or
`bootargs` from `uEnv.txt`, with `optargs` appended), passed in the ATAGs (neither the vendor DTB, `usb_host/original.dts`, nor `kernel/dts/`
sets `bootargs`); then the words built into the kernel by `CMDLINE_EXTEND`, appended after it (`kernel/config-changes`, `set-str CMDLINE`). The line
on `.188` under our chain (`/proc/cmdline`, 2026-10-11) is U-Boot's words up to `rootwait`, then the kernel's own:

| Word | Comes from | Read by |
|---|---|---|
| `console=ttyO1,115200n8` | U-Boot (`${console}`) | the kernel console: UART2 gets every printk ([§3.12](#312-serial-ports)) |
| `vram=12M` | U-Boot | **nothing**: the `omapfb` parameter is `omapfb.vram` (`module_param_named` in `omapfb-main.c`, built in), so this bare word ends in init's environment — measured, `/proc/1/environ` on `.188`. The value `12M` is `[inferred]` from the vendor's unexpanded `${vram}` |
| `initcall_blacklist=fb_logo_late_init` | U-Boot | `init/main.c`: skips `fb_logo_late_init` (`drivers/video/logo/logo.c`), which marks the boot logos freed; skipped, they stay usable `[inferred]` |
| `consoleblank=0` | U-Boot | `drivers/tty/vt/vt.c` (`core_param`): the text console never blanks |
| `mpurate=auto` | U-Boot | **nothing**: no occurrence in the 4.14.52 tree, and it is in init's environment (measured, as `vram`). Inherited from the Overo lineage `[inferred]` |
| `root=/dev/mmcblk0p3 rw rootfstype=ext4 rootwait` | U-Boot | `init/do_mounts.c`; by position, never by UUID. Built-in default of `uboot/rw20-board.patch`; the capture above was taken on the earlier layout (`p6`), and p3 is built, **not yet booted** |
| `vt.global_cursor_default=0` | kernel `CMDLINE_EXTEND` | `drivers/tty/vt/vt.c`: the console cursor is off by default |
| `usbcore.autosuspend=-1` | kernel `CMDLINE_EXTEND` | `drivers/usb/core/usb.c`: USB autosuspend off; the reason is in `kernel/config-changes` |
| `console=tty0` | kernel `CMDLINE_EXTEND` | the kernel console: the panel; last, so it is `/dev/console` while `ttyO1` keeps every printk |

**Under the vendor U-Boot the line also carries `curBoot1`, `curBoot2`, `prmRstSt`, `coldBoot1`, `coldBoot2`** (boot-ROM status words, appended by the vendor U-Boot `[inferred]`: the recovered `sysargs` lacks them). Nothing in our
tree reads them; the vendor rootfs's `/opt/sbin/getBootStatus.sh` parses them out of `/proc/cmdline` into `/var/volatile/tmp/bootROMStatus`, and the `/opt/sbin/ctrlblk` binary names that script
(`grep -a`; whether it runs it is `[unverified]`). Our U-Boot appends none of them.

### 4.2 Partitions

**Our card** (`rootfs/make-card-image.sh`; the layout is `ARCA_LAYOUT` in `lib/arca-identify.sh`): MBR, three primary partitions, measured on the built image
(`~/rw-card-out/f148.img`, 3293184 sectors = 1608 MiB, so it fits a nominal 2 GB card **[inferred]**). **Host-verified (`tests/card_layout_test.sh`) and booted
on one unit (`.188`, 2026-10-13/14, `[n=1]`): the ROM and SPL load from p1 with this table, `root=/dev/mmcblk0p3`, p2 is mounted on `/home/root/log` and receives logs, and the
SSH host key under `/home/root/data/ssh` kept its fingerprint over two reboots. Not looked at: the Control Panel log and monitor pages reading p2 `[unverified]`.**

| Partition | Type | Start, size | Mount | Contents |
|---|---|---|---|---|
| p1 | FAT32, label `ARCABOOT`, type `0c`, bootable | 63, 144522 sectors (70.6 MB), byte-identical to the vendor's | none | Boot files, listed in [§4.1](#41-the-chain). **The DTB is inside `uImage-system`** — no separate `.dtb` file exists. |
| p2 | ext4, label `arca-log` | 147456, 2097152 sectors = 1 GiB, fixed | `/home/root/log` | System logs |
| p3 | ext4, label `arca-root` | 2244608, 1048576 sectors = 512 MiB, **last so it can grow** | `/` | Root filesystem; 193,880 KiB used with the full bundle. `/home/root/data` is a **plain directory on it**: settings, scores, calibration, the SSH host key and ScummVM saves live here, so a reflash wipes them (`backup.sh` is the only protection) |

`resize2fs` offline from 512 MiB to 30 GiB ran clean (measured); an online grow of the mounted root ran clean on `.188` (`[n=1]`, 512 MiB to 6520 MiB in 17 s, no reboot; kernel log "resized filesystem to 1669120"). `rootfs/board/arca/overlay/etc/init.d/growroot` (`S07growroot`) does it at every boot when p3 has a tail: `resizepart` for the kernel, the four size bytes of the MBR entry, then `resize2fs`; a freshly flashed `f149.img` did it by itself at its first boot on `.188` (`[n=1]`, 8 GB card; syslog "resizing filesystem from 131072 to 1669120 blocks").
U-Boot's built-in root is `/dev/mmcblk0p3` (`uboot/rw20-board.patch`); `make-card-image.sh` refuses an older `u-boot.img` (`root=` p6) via `arca_uboot_root_ok`.

**The vendor's card** (measured on RW09 and a second unit):

| Partition | Type | Size | Mount | Contents |
|---|---|---|---|---|
| p1 | FAT32 | 70.6 MB | `/var/volatile/boot` | Boot files, listed in [§4.1](#41-the-chain). |
| p2 | ext3 | 256 MB | `/home/root/data` | Application data |
| p3 | ext3 | 250 MB | `/home/root/log` | System logs |
| p5 | ext3 | 1500 MB | `/home/root/backup` | Firmware backup, incl. `factory/` upgrade images + `.md5` files |
| p6 | ext3 | ~981 MB | `/` | Root filesystem |

p1's files, their roles and readers: [§4.1](#41-the-chain). The vendor's p1 is FAT32 made by `mkdosfs` with 1-sector clusters
(measured on two units), and sectors 1..62 between the MBR and p1 are zero on both. ⚠️ **The four vendor files cannot go into a
published image:** U-Boot is GPL-2.0, these builds are modified, and no source is published, so a redistributor cannot offer the
corresponding source. Operator ruling 2026-10-12: a card image carries nothing of the vendor's. `rootfs/make-card-image.sh` writes
the partition table itself at the positions above, formats p1 the same way, and puts only our `MLO` (first), `u-boot.img`,
`uImage-system`, `uImage-fallback` and `uEnv.txt` on it.

Windows may add a `System Volume Information` directory to p1 when it writes a card. **It is ignored**: no boot stage reads it
and nothing scripted writes p1 (`lib/CLAUDE.md`, *p1: nothing scripted writes it*).

> **Gotcha — the vendor's p6 is ext3, mounted by the ext4 driver.** U-Boot passes `rootfstype=ext4` and the ext4
> driver happily mounts the ext3 filesystem. **Do not reformat a vendor root as ext4 in place.** The converse also fails: `mke2fs -t ext3 -O none` makes a journal-less (ext2) filesystem that the kernel refuses as ext3 with `EINVAL`.

Live usage (vendor card): root 47 % used (474 MB free), data 40 %, log 4 %.

**What actually occupies each partition of the vendor card** — measured 2026-08-05 on the second captured card, stock and
uncommissioned. `/home/root/{data,log,backup}` are **mount points**, so a rootfs mounted offline shows
all three empty; the vendor's config and logs are on the other partitions.

| Partition | Used | Largest items |
|---|---|---|
| p6 `/` | 379 MB | `/usr` 223 MB (`/usr/lib` 138, `/usr/share` 60) · `/opt` 142 MB (`openjre-8` 93, `jetty-9-4-11` 43). **Everything outside `/usr` and `/opt` totals 15 MB** |
| p2 `/home/root/data` | 144 MB | `cron/` **131 MB** — a *log*, not the spool · `test.hex` 10 MB · `websign/` 220 KB, the network config of [§3.5](#35-network-and-power) |
| p3 `/home/root/log` | 31 MB | `jetty_logs` 18 MB · `browser.err` 8 MB · `messages` 4 MB |
| p5 `/home/root/backup` | 492 MB | `factory/` **472 MB** — vendor upgrade/restore images plus `.md5` files · `websigns/` 15 MB |

**`/usr/lib`'s 138 MB is a kiosk-browser stack that nothing in this project uses** — `libwebkit2gtk`,
`libicudata`, `libjavascriptcoregtk`, `libgtk-3`, the X11 and gstreamer trees, a spell checker — served by
`/etc/init.d/browser`, with `Xorg.0.log` and p3's `browser.err` as its output. Every component here draws
straight to `/dev/fb0`, so none of it is linked or loaded. `/usr/lib` also holds `libpython3.8`, `perl5/`
and `ts/` (tslib), which are worth keeping.

**The vendor's upgrade machinery is on disk and its payload is p5's `factory/`:**
`/etc/init.d/startautoupgrade`, `/opt/sbin/upgrade_logger.sh`, `IsUpgradeRunning` on p5, and the
litter of `upgradeProgressListener_upgradeStatus=*` files dropped in `/` show it has run.
**[unverified]** whether it can still fire unattended — nobody has established it either way.

**Vendor p5 content, measured on `.188` (2026-10-09):** `factory/nand_boot_redirect_oob.bin` (135168 B, plus
`.md5`), `factory/uImage-system-original` (5225796 B), `factory/upgrade.conf`, `serialno` and `pointercal`.
The full-card backup holds them.

**The vendor layout is the identity there; the UUIDs are not.** Measured across two units of the same firmware
build (`/etc/version` `20180309123456`), the partition table is byte-identical — same start sector
and same size for p1, p2, p3, p5 and p6:

```text
p1 : start=      63, size= 144522, type=c, bootable
p2 : start=  144585, size= 514080, type=83
p3 : start=  658665, size= 498015, type=83
p4 : start= 1156680,               type=5    (extended container)
p5 : start= 1156743, size=2939832, type=83
p6 : start= 4096638, size=2008062, type=83
p7 : start= 6104763,               type=82   (swap)
```

p4 and p7 sizes differ between units — they absorb the difference in physical card size (two nominal
4 GB cards measured 3.71 and 3.86 GiB), so **do not pin them**.

⚠️ **Every filesystem UUID differs per unit — all four of them.** A UUID is assigned at mkfs time and
units are mkfs'd independently at the factory, so two RoomWizards on identical firmware share none:

| | p2 | p3 | p5 | p6 (`/`) |
|---|---|---|---|---|
| RW09 | `d5758df8` | `da4cda60` | `26a7a226` | `108a1490` |
| a second unit | `2932baf1` | `d4d5ddbd` | `0f17045a` | `8acefb02` |

**Nothing on the device consumes a UUID.** U-Boot passes `root=` by device name (compiled in: p6 on the vendor card, p3 on ours, and
there is no `saveenv` — §4.4), and the vendor `/etc/fstab` names `/dev/mmcblk0p{2,3,5,7}`. Both are by position,
so the root partition *is* the rootfs and cannot be renumbered. Host tooling identifies a card by layout and a rootfs
by content — `lib/arca-identify.sh`, `lib/CLAUDE.md`. A hardcoded UUID
recognises exactly one unit, and assigning it to a second card so it "matches" produces two cards
claiming one UUID.

### 4.3 NAND is effectively unused

There **is** NAND (`U13`, Micron MT29F2G16ABBEAHC, 256 MiB SLC, 16-bit, BCH8), but only `mtd0`
holds anything. Everything else reads blank (`ff ff ff ff ...`):

| Partition | Size | Contents |
|---|---|---|
| `mtd0` boot | — | 12 KB Nuvation redirector (the u-boot slot @0x80000 is blank) |
| `mtd1` nandkernel | 11 M | **blank** |
| `mtd2` sdkernel | 11 M | **blank** |
| `mtd3` bootstrap | 92 M | **blank** |
| `mtd4` scratch | 11 M | **blank** — 11 MB of free space that survives an SD reflash |
| `mtd5` controlblock | 4 M | **blank** |

**This is a pure SD-boot device with a 12 KB NAND shim.** That shim is the only irreplaceable
non-SD component, and there is no reason ever to write to it. (Writing `/dev/mtd4` is safe;
`mtd0` is not.)

⚠️ **Bare `blkid` probes `mtdblock0` and the kernel prints `ecc unrecoverable error`,
`print_req_error: I/O error, dev mtdblock0, sector 0` and `Buffer I/O error` on the console, i.e. the
panel.** Measured on `.188` on our own root: read-only, and the unit boots through that redirector, so it is
intact. Why the kernel's ECC rejects it is `[inferred]` (a different ECC layout than the ROM's), not
measured. Probe block devices by name, never bare `blkid`.

### 4.4 The U-Boot environment cannot be persisted

**This section is the vendor U-Boot's.** Ours has no persistent environment either; its variables and `uEnv.txt` are in [§4.1](#41-the-chain).

`u-boot.bin` contains `setenv`, `printenv`, `editenv` and `showvar` but **no `saveenv`** — verified,
zero occurrences of the string. It is built `CONFIG_ENV_IS_NOWHERE`, consistent with the absence of
`fw_printenv`/`fw_setenv` on the device, no `/etc/fw_env.config`, and no `environ` MTD partition.

> **You cannot brick this device through the bootloader environment.** Every reset restores the
> compiled-in defaults verbatim. Anything typed at the `rw20 #` prompt is a one-shot experiment.

Recovered defaults:

```
bootdelay=1                      # 1 second to interrupt into the "rw20 # " prompt
console=ttyO1,115200n8
mmcdev=0
loadaddr=0x82000000
sysroot=/dev/mmcblk0p6 rw
sysrootfstype=ext4 rootwait
sysargs=setenv bootargs console=${console} vram=${vram} initcall_blacklist=fb_logo_late_init \
        consoleblank=0 mpurate=${mpurate} root=${sysroot} rootfstype=${sysrootfstype}
loadsysimage=fatload mmc ${mmcdev} ${loadaddr} uImage-system
sysboot=echo Booting from mmc ...; run sysargs; bootm ${loadaddr}
loadbootscript=fatload mmc ${mmcdev} ${loadaddr} boot.scr
bootscript=echo Running bootscript from mmc ...; source ${loadaddr}
```

Available commands include `ext4load`, `tftpboot`, `usb`, `i2c`, `mmc`, `nboot`, plus the custom
`cb_getinfo`.

> **Gotcha — `boot.scr` is a trap.** Support is compiled in but **`bootcmd` never calls it**.
> Dropping a `boot.scr` on p1 does *not* override the boot. Overrides require the serial prompt or
> replacing `uImage-system` itself.

### 4.5 Control block and `boot_tracker`

**Binary:** `/opt/sbin/ctrlblk` · **Storage:** `ctrlblock.bin`, 28 bytes, on FAT32 p1 — not in
NAND, not in the U-Boot environment.

| Field | Values |
|---|---|
| `boot_from` | `bootstrap` \| `system` |
| `upgrade_type` | `factory` \| `field` |
| `boot_tracker` | 0–2 (failure counter) |
| `fwversion` | firmware version string |

Verified behaviour:

- U-Boot's `cb_getinfo` **only reads** it, setting `cb_boot_mode`, `cb_upgrade_type`,
  `cb_boot_tracker`. It never writes.
- It is incremented or reset only by **userspace** `/opt/sbin/ctrlblk`, driven by
  `/etc/init.d/ctrlblk` (`S40ctrlblk`), which acts only when `BOOTMODE=system && TRACKER==1` —
  i.e. the single boot immediately after a firmware upgrade.
- **`/opt/sbin/fail.sh` does not exist on this device.** The automatic recovery path that older
  notes referred to is already dismantled.

> **Therefore a kernel that fails to boot does not increment `boot_tracker` and does not trigger
> recovery mode.** U-Boot prints `Failure to load system kernel image`, `bootcmd` falls through,
> and you land at the `rw20 #` prompt. Nothing has changed state.

### 4.6 Integrity checking — what is and is not verified

**Nothing is cryptographically signed. There is no secure boot.**

- **No boot-time MD5 verification of the kernel.** The only integrity gate on `uImage-system` is
  its uImage header CRC + data CRC, which `mkimage` writes. ⚠️ The data CRC must be computed *before*
  the header CRC — the header carries the data CRC, so the other order signs a header that is already
  stale, and these two CRCs are the only thing standing between a bad write and a unit that does not
  come up with no serial console to say why.
- **No `.md5` files exist on p1.**
- The MD5 scheme that *does* exist guards the **upgrade package** on p5
  (`/home/root/backup/factory/`), and is checked by scripts inside the bootstrap ramdisk, in three
  layers: the whole package (`upgrade.cpio.gz.md5`); each partition image
  (`sd_rootfs_part.img.md5`, `sd_boot_archive.tar.gz.md5`, `sd_data_part.img.md5`,
  `sd_log_part.img.md5`); and a post-write read-back comparison after each `dd`, with up to 3
  retries per partition and exit code 6 on final failure. Modify anything in that tree and every
  `.md5` beside it has to be regenerated.

**Packaging an image we build** — the recipe, the `-C none` rule and the round-trip proof against the vendor header are in `kernel/README.md`; `kernel/build-image.sh` is the one implementation.

### 4.7 Recovery

**Three independent layers. JTAG is not one of them.**

**Layer 1 — the untouched-kernel trick (zero-cost rollback).** The vendor `bootcmd` is hardcoded to
`fatload ... uImage-system`. Stage experiments under a *different filename* and leave
`uImage-system` alone; a failed experiment is undone by a power cycle.

⚠️ **One deliberate exception, and it costs Layer 1 on that unit: our own kernel image.** `uImage-system`
is the only file `bootcmd` will load and U-Boot has no `saveenv`, so a unit that boots our image by itself
requires replacing that name in place — a manual operator step with a verified backup of the running
image first ([kernel/README.md](kernel/README.md)). Copying that backup back is the undo while SSH
answers; otherwise it is Layer 2.

**Layer 2 — pull the card.** The whole system is on removable microSD (`mmcblk0`, root
`mmcblk0p3` on our card, `p6` on the vendor's). `dd` a known-good backup back, roughly 10 minutes. **This is the working recovery
loop for this project:** pop the card, reimage, set up DHCP, SSH back in. ⚠️ It is also the *only*
recovery from a `uImage-system` that does not boot (short of copying the backup back in a card reader)
— nothing runs before the kernel loads, so there is no SSH and no serial console to fix it from.

**Layer 3 — the serial console**, if you ever wire it up. `bootdelay=1` gives a one-second window
to the U-Boot prompt (`arca #` ours, `rw20 #` the vendor's); a root shell is already running there. See
[Serial ports](#312-serial-ports). The loop it enables, with a card backup taken first: stage the
experiment under a **new** filename on p1, interrupt into the prompt, `fatload mmc 0 0x82000000 uImage-test;
run sysargs; bootm 0x82000000`. If it panics, power-cycle and do **not** interrupt — `bootcmd` loads the
untouched `uImage-system` and nothing has changed state. Promote only after several clean boots, keeping
the old image under another name.

**Reading a failed boot by the two side LEDs.** With no SSH to ask, the LEDs and the serial console are the only witnesses; the states below are
**our** U-Boot's (the vendor chain lights them differently), and the line of the U-Boot build that drives each is in `uboot/BUILD.md`. A good cold boot, from
power-on (seconds off two videos, approximate): green at 0; yellow (both lit), then red, at 1; green at 2; off at 3 (`ledoff`: the card answered); `redx3` and
`greenx3` at 3 to 4, which the eye reads as **two** blinks each because the third pulse stays lit — red until `greenx3`, green until Linux takes the pads at
about 5; dark and a black screen to 8; at 8 the screen comes on, the LED turns yellow (Linux has taken the LEDs over **[inferred]**) and kernel messages start
(operator, 2026-10-15). **The last LED state shows where it stopped** (steady = working on that stage, or hung there; **blinking is a failure the code detected**):

| LED | Meaning |
|---|---|
| dark from the start | the SPL never reached its first stage: `MLO` missing or unreadable, or the redirector's reset did not happen **[inferred]** |
| red, then red + green (both lit) | the SPL got through SDRAM; stays red = the SDRAM test failed |
| red + green steady | the SPL is stuck before handing over: loading `u-boot.img` (missing, unreadable), or an SPL exception |
| red blinking, 2 Hz | a detected failure in the SPL or in U-Boot; the serial console has the reason |
| green, then red, then stuck | U-Boot started and is stuck early (relocation, timer, memory, serial) |
| red + green (U-Boot's) | U-Boot is stuck before MMC and environment init |
| green | U-Boot is waiting out its `bootdelay`, or has no `bootcmd`, or the card does not answer |
| dark | the card answered; loading the kernel (or the fallback) failed or hung (also dark for the seconds a 5 MB load takes) |
| three red flashes | the kernel file was loaded; the command line is being built |
| three green flashes | `bootm` was reached. Still green = `bootm` hung or the kernel died before touching the pads; dark = Linux took the pads **[inferred]**; dark then red flashes again = the first image was bad and the fallback ran |
| yellow blinking, 2 Hz | U-Boot ran to its end and nothing booted: neither `uImage-system` nor `uImage-fallback` loaded. Ctrl-C on the serial console stops the blinking at the prompt (steady yellow) |

A flash sequence that freezes part-way means U-Boot's timer is dead **[inferred]**. The red blink in `hang()` and the yellow blink at the end have not run on
hardware **[unverified]**. Make one change per boot and look at the panel before the next: a failed boot leaves no other diagnostic, so two changes at once
cannot be told apart; the post-mortem after that is mounting p2 offline and reading `messages`, which helps only if the boot got as far as syslog.

> **JTAG is required only if you damage the 12 KB NAND redirector (`mtd0`)** — a bad `mlo`/`u-boot.bin`
> on p1 is undone in a card reader ([§1](#what-a-boot-path-write-costs-to-undo)). `P3` is a TI-14 JTAG header
> **[inferred]** if it ever does.

**Logs** live in `/var/log/` (system), `/home/root/log/` (application),
`/var/log/browser.{out,err}` and `/var/log/jettystart`.

---

## 5. Software stack

### 5.1 As shipped

```
Linux (SysVinit)   4.14.52 on firmware 4.13.1.0 — the release decides the kernel, see below
  -> X11 / Xorg
    -> WebKit browser (Epiphany), home page http://localhost/frontpanel/pages/index.html
      -> Jetty 9.4.11  (/opt/jetty-9-4-11/, init script /etc/init.d/webserver)
        -> OpenJRE 8   (/opt/openjre-8/)
          -> Steelcase room-booking application
            -> HSQLDB 2.0.0  (/home/root/data/rwdb/)
            -> Interbase     (/opt/interbase/data/websign.gdb, legacy)
```

This entire stack is removed or disabled in game mode. Java 8 still exists at `/opt/openjre-8/` if
something needs it; Python requires cross-compiled ARM binaries.

**⚠️ The firmware release is per unit and it decides the kernel — do not assume the unit in front of you
is the one this document was measured on.** `/etc/firmware.version` names it, `/etc/issue` the Yocto
version, `/lib/modules/` the kernel. `4.13.1.0` and `4.12.0.0` are both Yocto 3.1.4 on **4.14.52** and
the reference unit is `4.13.1.0`, so that is what was measured here; `4.7.1.0` is Yocto 1.8 on
**4.1.32** (two cards). So `uImage-system` is per-release rather than a constant — different md5, tree at
a different offset — and anything gated on that file's *identity* is gated on the release.

### 5.2 As we run it — game mode

Native apps render straight to the framebuffer, so X11, the browser, Jetty, Java and the databases
are all unnecessary. Our own Buildroot root does not carry them: it starts only the services listed in
`rootfs/CLAUDE.md` (our boot links), and the Steelcase stack, cron and its software watchdog are absent
from it ([§3.13](#313-watchdogs) for what that watchdog does on a stock unit).

**mDNS on the stock image:** `S30avahi-daemon` is absent while `/usr/sbin/avahi-daemon`, `/etc/avahi/` and
`/etc/init.d/avahi-daemon` are present, so the daemon never runs there.

⚠️ **On our own Buildroot root, OpenSSH `sshd` does not listen until the kernel's crng is initialised, and with
no seed that takes ~2 min after boot.** Measured twice on `.188`: 09:20:34 to 09:22:42, and rc 5 at 09:43:44
to sshd listening at 09:45:50. BusyBox `seedrng` (`rootfs/board/arca/overlay/etc/init.d/seedrng`, link
`S01seedrng`) cut it to 1 s (rc 5 10:02:17, sshd 10:02:18). The first boot of a new card has no seed and stays slow.

⚠️ **A first boot with no seed prints a handful of "uninitialized urandom read" lines; the second boot prints none, bar one different line.** Measured on `.188`, 2026-10-12, card `arca-card-20261012b`: the first boot's dmesg held 7 lines (`seedrng` 256 B, `udevd` 16 B twice, `tr` 8192 B twice, `avahi-daemon` 4 B, plus "4 callbacks suppressed"), and `/var/lib/seedrng` held `seed.no-credit`. The second boot, read at 1 min uptime with `udevd` and `avahi-daemon` running, printed 0: `seed.credit`, "fast init done", "crng init done". One line remains on it: `random: get_random_u32 called from bucket_table_alloc+0xf8/0x24c with crng_init=0`. **[inferred]** The first-boot lines cannot be avoided: a fresh card has no seed, and a seed baked into the image would be shared by every card made from it. **Operator-observed:** none of these lines shows on the console or in the Control Panel log viewer.

⚠️ **`/var/log` is a symlink to `/home/root/log`, i.e. p2 on our card (p3 on the vendor's), and `syslogd` holds three files there
open.** Measured from `/proc/<pid>/fd` on a unit in service, 2026-08-05: `messages` (its target per
`/etc/syslog.conf`), `upgrade.log` and `concurrent.log`. **Unlinking any of them on a running device
leaves `syslogd` writing to an unlinked inode and logging silently stops until reboot** — truncate them
instead, and the same applies to `wtmp` and `lastlog`, which `sshd` and `login` hold open. Offline the
symlink dangles, so a path under `/var/log/` reaches nothing at all; name the log-partition path.

⚠️ **On the stock image the root crontab lives on p2, reached through a symlink.** `/var/cron/tabs/root` is a
symlink to `/home/root/data/cron/tabs/root` (measured on the unit in service, symlink dated Jan 2022).
`rm -rf /home/root/data/cron` destroys the crontab and cron's spool root, not just the 131 MB `log` file
inside it. Offline, the crontab is on a different partition from `/var`: a tool that mounts only p6 sees
`/var/cron/tabs/root` as a dangling symlink and `/home/root/data` as an empty mount point, so it can
neither read nor write the crontab. The mounts of [§4.2](#42-partitions) are not optional.

### 5.3 App launcher and manifests

`/etc/init.d/arca-app` reads `/opt/arca/default-app` and **respawns that binary
whenever it exits** — which is how quitting a game returns you to the launcher.

`app_launcher` scans `/opt/arca/apps/*.app` manifests and renders them as a touch grid.
Manifest format is INI: `name=`, `exec=`, `icon=` (PPM P6), `args=` (one of `fb,touch` / `fb` /
`touch` / `none`). **Each component's deploy script writes its own manifests and icons** — that is
how projects plug into the launcher with no central registry.

Device paths worth knowing:

```
/opt/games/                       native binaries
/opt/arca/apps/             .app manifests
/opt/arca/icons/            PPM icons
/opt/arca/default-app       boot target
```

**Seeing what is running: use `ps`, never `ps w`.** This busybox (v1.31.1) treats `ps w` as "processes
with a controlling TTY" — on RW09 **3 lines against plain `ps`'s 51**, the two gettys and the header. An
app started at boot or by the launcher has no TTY, so `ps w` shows nothing and the process looks absent.

| Want | Command |
|---|---|
| everything | `ps` (shows `comm`, i.e. the **executable's** basename) |
| the Arca apps, with exe and cmdline | `/etc/init.d/arca-app status` |
| find one by name | `pidof <basename>` — matches `comm` **or** `basename(argv[0])` |
| what a PID really is | `readlink /proc/<pid>/exe` |

`comm` comes from the file being executed and **not** from `argv[0]`, so it is trustworthy even when a
parent sets a display title: `killall`/`pidof` match a process whose `cmdline` reads `VNC Client`.
`/proc/<pid>/cmdline` is the one that can lie about identity.

---

## 6. Building for this device

Everything cross-compiles on the host and deploys over SSH. **There is no CI**; what does run before a
deploy, and what is an interactive diagnostic rather than a test, is in `tests/CLAUDE.md`.

Host toolchain and build prerequisites: `CLAUDE.md`, and `DEVELOPING.md` → *Before anything builds*.

### 6.1 Cortex-A8 has no hardware integer divide

The core does **not** implement `sdiv`/`udiv` (those need ARMv7ve, Cortex-A15 and later).
Executing one raises **SIGILL** — the binary dies instantly with exit code 132, no output, no log
files. `dmesg` may not even show the trap on 4.14.52.

The conventional advice is to build with `-mcpu=cortex-a8 -mfpu=neon`. **On this toolchain that
makes no difference to the emitted code** — app-level 32-bit `int` division already compiles to a
call to the software helper `__aeabi_idiv`, and the output is byte-identical with and without the
flags (verified). Keep them for explicitness, but they are not what saves you.

| Component | Flags actually used |
|---|---|
| Native apps, ScummVM, their ARM dependency libraries | `arm-linux-gnueabi` `-march=armv7-a -mtune=cortex-a8 -mfpu=neon -mfloat-abi=softfp`, dynamic (§6.3) |
| `vnc_client` | `arm-linux-gnueabihf` `-march=armv7-a -mfpu=neon -static` ([`vnc_client/Makefile`](vnc_client/Makefile)) |

**`-march=armv7-a` has no idiv, so it is safe; `armv7ve`, or a `-mcpu` of a core that has idiv
(Cortex-A7/A15), is what would break it.** The softfp builds need an explicit `-march` because
`arm-linux-gnueabi` otherwise targets armv5.

**Checking a binary — the expected count is a hard zero.** Use
[`native_apps/check-arm-safe.sh`](native_apps/check-arm-safe.sh), which all three component build scripts
run before every deploy and on build-only runs too. It reports zero across every ARM artifact, skips
anything whose `objdump -f` architecture is not ARM, and says how many it skipped.

Two ways to get a wrong answer out of it, both measured on this repo:

> ⚠️ **Match the tab-delimited mnemonic field, not the line.** A bare `grep 'sdiv\|udiv'` matches the
> *substring* `udiv` inside the **names** of the software-divide helpers — `__udivsi3`, `__udivmoddi4`
> and their call sites. Those are symbol names and branch targets, and their presence is positive
> evidence that division is being done in software. `libgcc.a` on this toolchain contains **zero**
> hardware `sdiv`/`udiv`, so there is nothing to allowlist and any correctly-matched hit is real.
>
> ⚠️ **Gate the *unstripped* artifact.** `objdump` needs the symbol table to tell **Thumb-2 from ARM**,
> and these binaries are Thumb-2; stripped, it re-reads the same `.text` bytes as 32-bit ARM words and
> manufactures divides that are not in the file — 9 phantom hits on ScummVM, 1 on `vnc_client`. The
> phantom operands are not reliably invalid, so eyeballing them is not triage. The gate therefore
> **refuses to judge** a stripped target: **0** clean, **1** a real hit, **2** could not be judged.
> ScummVM's gate sits inside `strip_binary` *before* the `strip` call for this reason — the strip is
> in-place, so that is the only moment an unstripped artifact exists. Harness rules, including why the
> status must not be read through `xargs`: `tests/CLAUDE.md`.

The check itself:

```bash
arm-linux-gnueabihf-objdump -d BIN | awk '/\t(sdiv|udiv)(\.w)?\t/ {print}'
# Expected output: nothing.
```

What must hold is: **no `sdiv`/`udiv` instruction anywhere in the binary.**

It applies to the code we compile and to static `libgcc.a`, linked static or dynamic; the device's own
shared libraries are the vendor's and run.

### 6.2 Never use `--whole-archive` with `-lpthread`

**Measured (March 2026):** a static hard-float ScummVM build died with **SIGSEGV before `main()`**,
no output and no log; `dmesg` showed `PC is at 0x40` with `r0 : ffffffda` (capture:
`scummvm-arca/SCUMMVM_DEV.md`). Its link line was not recorded beyond `--whole-archive -lpthread`.
**Hypothesis, unproven:** glibc 2.31's pthread init calls `clock_gettime64` (syscall 403, kernel 5.1+),
gets `-ENOSYS` on 4.14.52, then dereferences a NULL VDSO pointer. Against it: glibc falls back to
syscall 263 on `-ENOSYS` (inferred), and `vnc_client` links `-static` with plain `-lpthread`, calls
`pthread_create` and ships. The script kept `--whole-archive` until 2026-09-01, so the "fix" was not
applied then. Nothing links with it now; the rule is to use plain `-lpthread`.

### 6.3 Cross-compiled dependencies must be built from source

Ubuntu Focal under WSL cannot do armhf multiarch — `dpkg --add-architecture armhf` fails and the
standard mirrors carry no armhf. This is why ScummVM and the VNC client each build their own zlib,
libpng and libjpeg into a local prefix.

⚠️ **The device's userspace is soft-float ABI, so a dynamic binary needs `arm-linux-gnueabi`.** Vendor
binaries read `soft-float ABI`, VFPv3, interpreter `/lib/ld-linux.so.3`; a `gnueabihf` binary asks for
`/lib/ld-linux-armhf.so.3`, which is absent, and could not call a softfp libc anyway. Measured 2026-09-27:
`arm-linux-gnueabi-gcc`/`g++` 9.4 (glibc 2.31, libstdc++ 6.0.28 — the device's own) with `-march=armv7-a
-mtune=cortex-a8 -mfpu=neon -mfloat-abi=softfp` built snake, ScummVM and an alsa-lib 1.2.1.2 client that run
on the device's libraries; without `-march` it targets armv5. On the rootfs: libz 1.2.11, libpng16 1.6.37,
libdbus-1 1.12, glib 2.62, `dbus-daemon`; absent: BlueZ, libsbc. Check with `readelf -hl <bin>`.

**libpng needs `-DPNG_ARM_NEON_OPT=0`.** With `-mfpu=neon`, libpng's build system detects NEON and
enables NEON code paths in the C source — but the actual NEON assembly files
(`png_do_expand_palette_rgba8_neon`, `png_riffle_palette_neon`,
`png_do_expand_palette_rgb8_neon`, `png_init_filter_functions_neon`) are not compiled by our manual
build, producing undefined-reference link errors. Disabling the paths costs nothing measurable for
the small PNGs in use.

### 6.4 Verify artifacts on disk, not config flags

Generated `config.mk` / `config.h` files go stale. A leftover `USE_PNG = 1` once made the build
compile `image/png.cpp` with no `libpng.a` present. **Test for the `.a`, not the flag.**

### 6.5 Software rendering techniques that paid off

ScummVM went from 80 % to 32 % CPU using these; the VNC client independently reused the same set:

- precomputed palette LUTs
- a precomputed source-column table plus row-pointer lifting, to remove per-pixel division
- border-only clearing, and skipping `fb_swap` entirely on unchanged frames
- a rotated (portrait) copy writes the **uncached framebuffer sequentially** and reads **strided from the cached back buffer**: the earlier order cost 63 % CPU against 54 % for Frogger in portrait on `.188` (landscape 51 %; `/proc/<pid>/stat` over 10 s, 2026-10-02; the old order's exact access pattern was not re-read, so the mechanism is **[inferred]**) (`framebuffer.c` `fb_swap`, byte-identical by `fb_rotate_test`)
- 16bpp RGB565 to halve write bandwidth
- NEON `vst1q_u16` 8-pixel blits
- row deduplication via an L1-resident temp row (~57 % of scaled rows are duplicates)

---

## 7. Kernel policy

**Build our own 4.14.52 image from `usb_host/linux-4.14.52/`. A mainline 5.x/6.x port stays ruled out.**
The two are separate questions and the case against the second does not carry to the first. Asking
Steelcase for their source is also ruled out, and is needed for neither.

**The ruling, so this is not re-litigated: an image of ours boots, and what is left is work rather than
unknowns — measured 2026-09-23 on `.188`.** Built from this tree plus the one source patch in the table
below and written over `uImage-system`, it takes DHCP and answers SSH ~50 s after power-on. It comes up
exactly as the table predicts: no `/dev/fb0` (omapfb `failed to setup` — no panel driver), no touch evdev,
and the out-of-tree `xpad`/`joydev`/`snd_usb_audio` load. USB host needs one config symbol the vendor
config lacks — why, and what DMA needs: [kernel/README.md](kernel/README.md). One dmesg line is
**unexplained**: `omap2_set_init_voltage: unable to find boot up OPP` for `vdd_mpu_iva`/`vdd_core` (not
yet compared with the vendor kernel's dmesg). **The tree in the repo is a working build tree.** `usb_host/linux-4.14.52/` is vanilla upstream 4.14.52,
not Steelcase source — but `build-kernel-modules.sh` configures it from the device's own `/proc/config.gz`
plus `olddefconfig`, and the `.ko`s in `usb_host/modules/` are **measured** building from it and loading
there (`vermagic=4.14.52`); the image is packaged by the recipe in
[Boot chain and recovery](#4-boot-chain-and-recovery). ⚠️ **The device is not running vanilla 4.14.52,
though.** Its config carries `CONFIG_CPU_SPECTRE` and `CONFIG_HARDEN_BRANCH_PREDICTOR`, absent for 32-bit
ARM in .52, so the vendor built with post-.52 stable backports (gcc 9.3.0 under Yocto — the 2018 build
date is not the build year). **Find a vendor change by function size, not by config — measured
2026-09-23:** of 31114 functions common to the vendor's `/proc/kallsyms` and our `System.map`, 55 differ in
size, and the boot-relevant one was `smsc911x_drv_probe` (2788 vs 2692 B); vendor-only symbols are just
`panjit_ts_*`, `sharp_*`, the rw20 logo, `__mach_desc_RW20` and the Spectre set (`cpu_ca17_*`). Disassemble
the decompressed vendor `Image` and resolve its calls through that `kallsyms`. What an image must supply:

| `/proc/config.gz` symbol | Absent from vanilla 4.14.52 | Cost |
|---|---|---|
| `CONFIG_SMSC911X=y` (survives `olddefconfig`) | the vendor's reset pulse in `smsc911x_drv_probe` | ⚠️ **no network at all, so an unpatched image answers nothing — measured 2026-09-23.** The vendor DT's `smsc,lan9221` node (GPMC CS5, `0x2c000000`) flags `reset-gpios = <&gpio1 17 0>` active-high; vanilla requests it `GPIOD_OUT_LOW` and never touches it again (`drivers/net/ethernet/smsc/smsc911x.c:453-455`), so the pin sits low, while the vendor probe drives it 0, sleeps 100 ms, drives it 1 (**measured** by disassembly), and the vendor kernel's `/sys/kernel/debug/gpio` shows `gpio-17 (reset) out hi`. That the LAN9221's reset is active-low is **[inferred]**, confirmed by outcome: with `kernel/patches/smsc911x-reset-pulse.patch` reproducing the pulse, `eth0` probes and takes DHCP. No config diff and no compatible-string check can see this — the node is claimed and the driver built in |
| `CONFIG_FB_OMAP2_PANEL_SHARP_LQ070Y3LG4A=y` | no `panel-sharp-lq070y3lg4a.c`, ever | **a blank panel, and not fixable by config** — the vendor DTB's `/display` is `compatible = "sharp,lq070y3lg4a"` alone, vanilla's `panel-dpi` matches only `omapdss,panel-dpi` (`displays/panel-dpi.c`, and `omapdss-boot-init.c` prepends the prefix), and a compatible string does not degrade — so nothing in the tree claims that node. `CONFIG_FB_OMAP2_PANEL_DPI=y` is **already set**. **Solved by DT, no driver — measured 2026-09-23 on `.188`**: `kernel/dts/rw20.dts` declares that node for stock `panel-dpi` (the measurement was taken with the same node made by an edit of the vendor DTB; the current DTS differs from that running tree by no hardware property) with the timings from [Display](#32-display) and hogs the other two control lines high from DT (`gpio-hog`, `gpiolib-of.c:244-264`), and the panel lights on our image; state and the remaining defects are in [`kernel/README.md`](kernel/README.md). ⚠️ **measured**: the vendor panel driver exposes no `bind`/`unbind`, so unlike touch this cannot be rehearsed on the running kernel |
| `arch/arm/boot/dts/omap3-rw20.dts` | absent | **low, and measured rather than asserted since 2026-09-21.** Every unique `compatible` string in the vendor DTB was checked against this tree (116 of them): all but twelve are claimed by a driver; nine more appear only in vanilla `.dts` files but are decorative CPU/DSP/bus-identity nodes, or are rescued by a driver-matched second string in the same property exactly as vanilla boards do it (`ti,omap3-l4-core` then `simple-bus`, `smsc,lan9221` then `smsc,lan9115`); one is `status = "disabled"`. **Exactly two enabled nodes have no possible binder — the panel and the touchscreen, both already rows in this table.** ⚠️ A claimed node can still fail at probe, which this check cannot see — the `smsc911x` row is that case. `kernel/build-image.sh` appends a DTB and packages the image with `mkimage`, so the packaging half is solved |
| `CONFIG_TOUCHSCREEN_PANJIT=y` | no `panjit*.c`; vanilla's `TOUCHSCREEN_USB_PANJIT` is an unrelated USB driver | **a dead touchscreen**, from a silent drop. ⚠️ **Not written from specs — adapted, measured 2026-09-21**: the tree already holds a same-family driver, `drivers/input/touchscreen/cy8ctmg110_ts.c` — I2C, `tristate`, register map at `:41-50` — and it bursts nine bytes from reg 3, spanning the second coordinate pair and the finger count, then discards the second point exactly as `panjit_ts` does ([Touch](#33-touch)). It is platform-data-only (`:187-190` returns `-ENODEV` without pdata), so the work is an `of_match_table`, gpiod for its legacy integer GPIO calls, a falling IRQ for its `IRQF_TRIGGER_RISING`, the hardcoded `759x465` range, and slot reporting for the pair it already reads. ⚠️ **The 110→120 delta is closed — measured 2026-09-21**: a nine-byte read from reg 3 on `/dev/i2c-2` matches that register map byte-for-byte, so the adaptation is against a known layout rather than a guessed one, and [Touch](#33-touch) carries the map, the 12-bit `0..4095` range that replaces the hardcoded `759x465`, and the `0x0fff` flag mask a port must apply. ⚠️ **And it is rehearsable before any image exists — measured**: `/sys/bus/i2c/drivers/panjit_ts/` carries `bind` and `unbind` with device `2-0003`, so a replacement module can evict the vendor driver on the running kernel, with a reboot as the undo and no write to p1. ⚠️ **Reading the controller needs not even that**: `I2C_RDWR` carries the target address inside each `i2c_msg` and never consults the busy list that makes `I2C_SLAVE` return `-EBUSY` (`drivers/i2c/i2c-dev.c`), so the map above was read with the vendor driver bound and serving touch normally — nothing to evict and nothing to undo. `input_mt_*` is exported there too, so multi-touch needs no image either |
| `CONFIG_OMAP_PACKAGE_CUS`, `CONFIG_MACH_RW20` | the vendor board file, absent | **nothing at all, and this row is closed — measured 2026-09-21.** Neither symbol can drive a line of code in this tree: `OMAP_PACKAGE_CUS` is a bare unreferenced `bool` (`arch/arm/mach-omap2/Kconfig:180`) and `omap3_mux_init` does not exist anywhere in 4.14.52, the legacy `omap_mux` layer having been deleted upstream before this release — so a vendor board file calling it would not even link. The machine descriptor is moot for the same reason it looked dangerous: DT match scores a descriptor by the **1-based position** of the string it hits in the root `compatible` list (`drivers/of/fdt.c:94-113`), the vendor root is `"ti,omap3-rw20", "ti,omap3"`, and vanilla's `OMAP3_DT` claims the second at score 2 (`board-generic.c:122`) — nothing matches the first, so `OMAP3_DT` wins and the image gets the full generic OMAP3 init (`omap3430_init_early`, then `omap_generic_init`). ⚠️ **And `MACH_RW20` is inert on the vendor kernel too — measured 2026-09-23:** its `__mach_desc_RW20` is field-for-field vanilla's `Generic OMAP3 (Flattened Device Tree)` (the same seven callbacks, `omap_reserve` through `omap3xxx_restart`), differing only in name and compatible, and `debug_ll_addr` is identical code in both (UART2 `0x4806c000`, `ttyO1`). None of that path touches pinmux, which matches a DTB bringing its own: 27 `pinmux` nodes, 11 `_pins` blocks, 13 `pinctrl-0`/`pinctrl-names` consumers including `/display`, and the audio binding (`ti,omap-twl4030`) |
| `CONFIG_LOGO_LINUX_RW20_CLUT224` | a vendor boot logo | **cosmetic** — and the running kernel suppresses it anyway with `initcall_blacklist=fb_logo_late_init` in `bootargs`, so an image we build is free to carry its own |

**Module or image: decide it from the device's own config, not from the subsystem.** A feature whose
symbols are tristate is reachable as an out-of-tree `.ko` built against this tree with no image at all —
the `joydev`/`xpad`/`ff-memless` set is the worked precedent — and this kernel takes them readily
(`CONFIG_MODULE_FORCE_LOAD=y`, `CONFIG_MODVERSIONS=y`, `CONFIG_MODULE_SIG` unset). A feature the vendor
built **in** cannot be reached that way at all: the DSS and MUSB are both `=y`, so an overlay-plane change
or `CONFIG_USB_INVENTRA_DMA` needs the image and nothing cheaper will do. ⚠️ **A `=y` driver can still be
evicted if its bus exposes `unbind`**, which is what decides whether a replacement can be rehearsed on a
running vendor kernel — measured yes for `panjit_ts`, no for the panel.

**Why build one: the image becomes publishable.** A kernel compiled here ships with its own
corresponding source and can go in a release; the vendor's `uImage-system` never can (`LICENSE.md`).
That retired the byte-patch route into p1, which reached the USB power budget by rewriting a firmware
image we may not redistribute; only our own image is supported now. **The argument is deployment
stability, not performance.** The config defects a rebuild also fixes (USB host/DMA, `PREEMPT_NONE`/`HZ=100`) are
real, but none of them limits anything measured.

**Verification is the cost that scales, but the exposure is one step.** A kernel that fails to boot does
not trigger recovery: U-Boot prints `Failure to load system kernel image` and lands at the `rw20 #`
prompt with nothing changed ([Boot chain and recovery](#4-boot-chain-and-recovery)). Booting an alternate
*filename* needs that prompt, so it needs the console; overwriting `uImage-system` does not, and its
recovery is a card pull plus copying a backup back onto p1 (plain FAT). ⚠️ **Assert an image over SSH,
never by the panel**, which stays blank until the panel driver exists; from SSH on, every panel and touch
iteration is a `.ko` copied over, and touch itself still ends at an operator ([Touch](#33-touch)).

**A mainline 5.x/6.x port stays out (standing operator decision); its DRM/KMS reason holds for `omapdrm` only.**
Under `omapdrm`, `/dev/fb0` exists only via `CONFIG_DRM_FBDEV_EMULATION`, a **fixed** pixel format, while this
project switches bpp at runtime in three components ([Display](#32-display)), and the DSS overlay sysfs is
gone. That the emulation would *reject* the switch is **[inferred]**; **measured** is that the current stack
supports it (`/sys/class/graphics/fb0/bits_per_pixel` tracks the running app). **Measured from upstream source
2026-10-05: mainline still carries `omapfb`** (`drivers/video/fbdev/omap2/omapfb/`, own `dss/`), so a newer
kernel is unevaluated, not ruled out: `kernel/README.md` → *Mainline and omapfb*. A 4.14.52 rebuild leaves
omapfb, the switch and the overlay sysfs exactly as they are.

⚠️ **What a rebuild does not buy: enumeration.** "Recompile the kernel and the USB problems go away" is
not true and has been proposed. It buys the config defects above and the *ability* to patch the driver.
It does **not** buy enumeration reliability: a cold port stays dark because VBUS here is driven solely
by the DEVCTL `SESSION` bit and nothing sets that bit on a port that probed with an empty socket
([USB](#36-usb)) — no `.config` option changes which code writes that register, so this needs a
**driver change**, which a rebuild enables rather than performs. The userspace one-tap RESCAN is the
answer until such a change is written and measured; the refuted mechanisms are tabulated in
`usb_host/README.md`.

---

## 8. Hardware policy

**Use the device as delivered: no modification to the board.** Settled by the operator 2026-09-06, and it
is a scope rule rather than a feasibility claim — several of the things it rules out are entirely doable.
⚠️ **The no-soldering half has been spent once, deliberately:** the operator fitted the `P4` console
header 2026-09, for the boot visibility an image we build otherwise has no way to get
([Serial ports](#312-serial-ports)). The rule stands for everything else.

**What it excludes.** Anything whose first step is a wire, a pad or an iron. The two worked examples:

| Ruled out | Why it was tempting | What was actually possible |
|---|---|---|
| The six idle MADC general-purpose channels ([ADC and temperature](#311-adc-and-temperature-twl4030-madc)) | a potentiometer on one channel is a real analogue paddle, and two plus `/dev/dsp` is a whole controller with no USB | nothing in software reaches them — `ADCIN2..ADCIN7` have no populated test point, so an input device needs one physically wired to a channel |
| XBee wireless | the header is on the board | closed on **value**, not on difficulty — a BGA pin could have been patched up |

⚠️ **This is why an entry that reads "needs a reachable pad" is out of scope, not merely expensive** —
and it is a different judgement from [Kernel policy](#7-kernel-policy), where the tree is ours to change
and an image is the deliverable. Here the tree is fine and the *board* is the boundary.

**What it does not exclude**, and these stay good candidates: every reading obtainable through a driver
already probing. `in_voltage9` is the RTC supercap, `cat`-able today with no wire — a "backup cell low" warning is
Control Panel work. The SoC temperature needs the OMAP3 thermal driver first ([ADC and temperature](#311-adc-and-temperature-twl4030-madc)). The rule bars adding a *sensor*, not surfacing one.
