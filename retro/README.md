# retro: libretro cores behind our own frontend (design and feasibility)

**Status: design only. Nothing in this directory runs yet** except two benches: `tools/retro_bench.c`
(headless core runner) and `tools/fb_bench.c` (display path, device-only); both ran on a unit 2026-10-24 (section 9). Working name of the
frontend: `arca_retro`. Every device fact is linked to
[`SYSTEM_ANALYSIS.md`](../SYSTEM_ANALYSIS.md); only the few numbers that justify a design choice are repeated, with their source.
Claims are marked **measured**, **read in source**, **inferred** or **unverified**.

## 1. Purpose and scope

Add retro emulation by loading **libretro cores** into a small frontend of our own that reuses `native_apps/common/`
(framebuffer, gamepad, audio_out, config): one implementation per subsystem.

**RetroArch is not shipped.** It is large, and it has its own UI, settings and input model, which ignore our safe
rectangles, the touch calibration and the settings app. It is used only as a **reference for correct
environment-callback behaviour**.

**Main risk:** the libretro environment callback (`RETRO_ENVIRONMENT_*`) has a long tail. The plan is about 15
commands implemented properly (pixel format, system/save directory, variables, input descriptors, rotation,
log interface, ...) and **every other command refused cleanly** (return false). Which commands each core
really calls is measured with `retro_bench`, not guessed.

## 2. Library model (decided)

Lutris-style: **games are entries independent of what runs them; the runner is configuration.**

- **One launcher tile, "Retro"**, opens a game grid built by **scanning user ROM folders**. File extension gives the
  system, the system gives the core; per-game overrides live in config.
- **"Pin to launcher"** writes a `.app` manifest for one game (manifests are the existing extension point,
  `DEVELOPING.md#plug-in-file-formats`). `app_launcher.c:482-501` passes a **fixed argv** (read in source), so a pinned game
  needs a small launcher extension or a generated wrapper script. **Open question.**
- **No box art shipped** (copyright). Tiles are generated (system colour + title); a user-supplied image per game is optional.
- The ROM picker can reuse `native_apps/file_manager/fm_core.h`.

## 3. Content and licensing policy (operator rule)

- Ship only **open-source-rooted** software. Non-commercial but freely redistributable licences are acceptable,
  **named** where used. **GPL preferred** wherever a feasible GPL alternative exists.
- **Never ship proprietary software, ROMs, BIOS images or games.** Provide only links to official legal download
  pages and hints on where to buy.
- Content lives in `/home/root/data` (on p3) or on a USB stick at `/media/usb0`
  ([storage](../SYSTEM_ANALYSIS.md#31-soc-memory-and-storage), [USB](../SYSTEM_ANALYSIS.md#36-usb)).

Candidate cores. Licences **read 2026-10-20 from each repository LICENSE/COPYING file**; re-read the exact release before shipping.

| System | Core | Licence | ROMs and shippability |
|---|---|---|---|
| NES | FCEUmm | GPL-2.0 | bundles no ROM (FDS BIOS `disksys.rom` is user-supplied) |
| ZX Spectrum | Fuse | GPL-3.0 | Spectrum 48/128 ROMs built into the core; Amstrad's 1999 permission lets emulator writers include the ROM images if the copyright messages stay unaltered (attribution required). Whether that covers a platform bundle is **open** |
| C64 | VICE x64 | GPL-2.0-or-later | Commodore ROMs are (c) Commodore Business Machines with no redistribution grant: **not shippable**, user supplies them (**open decision**) |
| Enterprise 128 | ep128emu-core | GPL-2.0-or-later | has built-in default ROMs and a `roms/` dir, no ROM licence statement found: **not shippable until checked** |
| Arcade | MAME 2003-Plus | classic MAME 0.78 non-commercial licence (`LICENSE.md:9`) | `:45` forbids distributing a modified version of the official MAME archive; `:51` allows non-commercial use of the source if full source is made available. Fits the operator rule only as a **named non-commercial exception**; the least clean of the set |
| Arcade | MAME 2016 | GPL-2.0-or-later overall, over 90% of files BSD-3-Clause, no non-commercial clause | the GPL arcade candidate **if fast enough**: benchmark |
| Arcade | FBNeo | custom non-commercial licence (`src/license.txt`) | not GPL; fallback only |

Legal test content to **link, never ship**: `mamedev.org/roms` (free, non-commercial, distribution on that site only;
e.g. Gridlee, Robby Roto, Super Tank, Alien Arena, Teeter Torture); MIT homebrew NES built from source:
`zorchenhimer/nes-runner`, `xram64/falling-nes`.

## 4. Display design

An emulator hands over a full frame per `retro_run`. The panel is 800x480 and refreshes at 60.05 Hz
([display](../SYSTEM_ANALYSIS.md#32-display)). **Primary candidate: (b) + (c), the DSS overlay with page flipping.**
(a) software scaling is the "Sharp" alternative. All are **to be measured** on a unit.

**(b) DSS overlay (vid1/fb1) at native core resolution, hardware scaling.** The CPU writes about 115 KB/frame for
256x224 at 16bpp, against about 460 KB for software 2x (arithmetic). The overlay **always filters**
([display](../SYSTEM_ANALYSIS.md#32-display)), so the choice becomes a user setting **Sharp (software) / Smooth
(hardware)**. Hybrid: software integer 2x into the overlay, DSS for the small remaining ratio.

**(c) Page flipping.** Vanilla omapfb implements `FBIO_WAITFORVSYNC` / `OMAPFB_WAITFORVSYNC` and `pan_display`
(`usb_host/linux-4.14.52/drivers/video/fbdev/omap2/omapfb/omapfb-ioctl.c:763,774`, `omapfb-main.c:1048`);
**measured working at 16bpp on a unit**, refused at 32bpp (section 9). fb0 holds 1,536,000 bytes, exactly two 16bpp pages, so a 16bpp flip
on fb0 needs no extra memory ([display](../SYSTEM_ANALYSIS.md#32-display)). fb memory is mapped write-combined
(`omapfb-main.c:1116`, read in source).

**Precedent for (b)+(c): OpenPandora emulators (notaz)** (verified from public source, 2026-10-20). They render at core
resolution into the omapfb overlay plane and let the DSS scale:

- `pcsx_rearmed` `frontend/plat_omap.c`: `OMAPFB_SETUP_PLANE` with `pos_x/pos_y/out_width/out_height` (:50-71);
  overlay memory grown to 3 buffers of 1024x512x2 via `OMAPFB_SETUP_MEM` (:56-58).
- `libpicofe` `linux/fbdev.c`: `yres_virtual = h_total * buffer_cnt` (:96), flip = `FBIOPAN_DISPLAY` (:58),
  vsync = `FBIO_WAITFORVSYNC` (:66), 16bpp throughout, old `var` screeninfo restored on exit.
- They accepted the DSS filter: their 1:1 and 2:2 modes were crops, not a software scaler.
- Their kernel was 2.6/3.x omapfb. Our 4.14 omapfb accepts `OMAPFB_SETUP_MEM`, `OMAPFB_SETUP_PLANE`, pan and the
  vsync wait (**measured**, section 9); that the scaled plane is visible is not yet checked by eye.

**(a) Software scale into fb0 at 16bpp RGB565 ("Sharp").** Reuse ScummVM's scaler in
`scummvm-arca/backend-files/arca-graphics.cpp` (precomputed column table, row dedup, NEON 8-pixel blit). Move it into
`native_apps/common/` so one scaler serves both. Pixel-exact, at the full write cost.

**(d) Row diffing.** Compare each core row with the previous frame and write only changed rows; static screens cost almost nothing.

**(e) VRFB hardware rotation** (omapfb rotate/vrfb parameters) could make portrait free, for native apps as well.
**Untested.**

**Fit modes per system.** *Fit*: proportional inside the visible, bezel-compensated area. *Fill*: ignore bezel
compensation; the bezel rows hidden at top and bottom (default 15 px each, `FB_BEZEL_TOP_DEFAULT` /
`FB_BEZEL_BOTTOM_DEFAULT` in `native_apps/common/framebuffer.h`; the real value is per unit) behave like CRT overscan. Portrait view exists in the
framework and is used for vertical arcade games ([touch and rectangles](../SYSTEM_ANALYSIS.md#33-touch)).

**Timing.** PAL systems (Commodore 64, Spectrum, EP128) run at 50 Hz on a 60.05 Hz panel: they need audio-synced timing and
will judder (**inferred**; measure).

## 5. Input

Bluetooth/USB keyboards, pads and audio are solved in the platform; the frontend does not read evdev.

- `native_apps/common/gamepad.h` `ButtonId` exposes only 9 buttons (lines 86-97). **Extend it** with Y, L, R, L2, R2, L3,
  R3 (`input_scan` already reads shoulders). This rebuilds every app, so it is its own change with the full redeploy.
- **Computer emulators need a raw key-event stream** (precedent: `native_apps/terminal/terminal.c` about lines 488-500).
- **Arcade defaults to the game controller.**
- **On-screen keyboard only as a fallback** for special cases. `keyboard.c` is a modal text-entry box; a
  computer-keyboard overlay would be new code.

## 6. Audio

Through `common/audio_out` (ALSA). `plughw` grants standard rates exactly
([audio](../SYSTEM_ANALYSIS.md#34-audio)). One `AudioOut` per process. The default lead is a nominal 139 ms,
which the header comment in `native_apps/common/audio_out.h` puts at about 79 ms of real audio. That may still be
too high for action games: **measure**, and make it a parameter if it is.

## 7. Build

Same toolchain and flags as ScummVM ([building](../SYSTEM_ANALYSIS.md#6-building-for-this-device)):
`arm-linux-gnueabi`, `-march=armv7-a -mtune=cortex-a8 -mfpu=neon -mfloat-abi=softfp`, dynamic, device libstdc++.

- Every core binary must pass `native_apps/check-arm-safe.sh` (no `sdiv`/`udiv`;
  [why](../SYSTEM_ANALYSIS.md#61-cortex-a8-has-no-hardware-integer-divide)).
- **Blind spot:** code generated at run time by a dynarec is invisible to that gate. Pandora-era dynarecs target
  ARMv7 without idiv (**inferred**), so the risk is low but not zero.
- **Loading:** `dlopen` is used nowhere in the tree yet; the device has `/lib/libdl.so.2` and `retro_bench` loads
  cores with it (**measured**, section 9). The fallback, one statically linked `arca_retro_<core>` each, is not needed.

## 8. Bring-up order (decided)

| # | System / core | Why at this position |
|---|---|---|
| 1 | NES / FCEUmm | frontend bring-up: small, GPL, legal homebrew test ROMs |
| 2 | Arcade / MAME 2003-Plus | portrait and rotation, pad defaults, romset-version UX; free ROMs from mamedev.org for testing |
| 3 | C64 / VICE x64 | 50 Hz against 60.05 Hz, audio sync, keyboard |
| 4 | Enterprise 128 / ep128emu-core | technical risk: heaviest per MHz of the list |
| after 3 | ZX Spectrum / Fuse | near free once C64 works |

Later candidates, proven on the same CPU class by Pandora: PicoDrive, snes9x2005, gambatte, gpSP, PCSX-ReARMed
(HLE BIOS), UAE4ARM, DOSBox, TIC-80 (open source; can ship with free carts). **Out of reach:** N64, NDS, Dreamcast, PSP.

Reference hardware is the OpenPandora (OMAP3530, 600 MHz, 800x480, softfp + NEON); its emulators are the existence
proof. **Inferred from that analogy; nothing has been measured on a RoomWizard.**

## 9. Measurement plan

**Offline results so far (measured 2026-10-20, WSL, QEMU 8.2.7 `-cpu cortex-a8`; function only, never timing).**
All five cores cross-build with `platform=unix` and the flags in section 7 (the Makefiles' own ARM platforms are
hard-float or armv7ve, so they are unusable). All are soft-float EABI5 (`readelf -A`: no `Tag_ABI_VFP_args`), with zero
`sdiv`/`udiv`. The core clones, their build logs, the content and the frame dumps live in WSL `~/retro-src` (outside
the repo; `env.sh` there sets the toolchain). `retro_bench` (`retro/tools/retro_bench.c`, built with `-ldl`) dlopens
each core:

| core | `.so` size | no-content run under QEMU |
|---|---|---|
| Fuse 1.6.0 | 1.5 MB | boots to the Sinclair copyright screen; 320×240, 50 fps, 44.1 kHz, RGB565 |
| FCEUmm | 3.0 MB | with `zorchenhimer/nes-runner` (MIT, `e6b5fe2`, built with cc65 via `make -o credits_data.i`): title screen; 256×224 (max 256×240), 60.1 fps, 48 kHz, XRGB8888 |
| VICE x64 3.10 | 3.0 MB | boots to `READY.`; 384×272, 50.125 fps, 48 kHz, RGB565; needs `vice/data` (11 MB) in the system dir |
| ep128emu-core 1.2.13 | 3.5 MB | runs no emulation without `ep128emu/roms/exos21.rom` etc. (768×288, 50 fps) |
| MAME 2003-Plus | 33.5 MB | segfaults in `retro_load_game` with no content; needs a romset |

⚠️ **The VICE build embeds the Commodore KERNAL/BASIC/chargen ROMs in the `.so`**, so shipping it as built ships
Commodore ROMs; it must be built without them. VICE also divides by its sample-rate core option, so a frontend that
answers `GET_VARIABLE` with nothing crashes it (SIGFPE): defaults from `SET_VARIABLES` must be served.

**Offline (no device):**

1. Cross-build the cores and run `check-arm-safe.sh` on each.
2. `tools/retro_bench.c` (headless) under QEMU 8.2 in WSL. This proves **function only, never timing**.
3. `tools/fb_bench.c` (written, builds ARM-safe, unrun): 16/32bpp copy cost, pan + vsync flip, nearest-neighbour
   upscale of 256x240/320x240/384x272, and (`--test overlay` only) fb1 overlay upload and VRFB rotation.

**First device session (measured 2026-10-24 on unit `192.168.50.115`, our rootfs, kernel 4.14.52 built 2026-10-13).**
Both benches built from this tree with the section 7 flags, `check-arm-safe.sh` clean; cores are the 2026-10-20 builds
above, staged on p3 in `/home/root/data/retro-bench` (left there). The launcher was idle (`top`: 83% idle) for
`retro_bench`, and stopped (`/etc/init.d/arca-app stop`) for `fb_bench`.

- **Governor** `performance`, 600 MHz (`scaling_available_frequencies` 125/250/500/550/600 MHz).
- **p3** 115.8 GB free of 118 GB (on that unit's card); RAM 240 MB total, ~169 MB available.
- **`dlopen` works:** `/lib/libdl.so.2` is on the rootfs and `retro_bench` (`-ldl`, `RTLD_NOW`) loaded all four cores it was given.

`retro_bench`, no input, unthrottled (`RETRO_SYS` = the staged `sys/`):

| core | content | frames | unthrottled | × core fps | ms/frame |
|---|---|---|---|---|---|
| FCEUmm | `nes-runner` | 1200 | 111.4 fps | 1.85× of 60.1 | 8.97 |
| Fuse 1.6.0 | none (48K ROM screen) | 1000 | 263.0 fps | 5.26× of 50 | 3.80 |
| VICE x64 3.10 | none (`READY.`) | 1000 | **29.7 fps** | **0.59× of 50.125** | 33.6 |
| VICE x64 3.10 | Impossible Mission `.zip` (operator's), autostart, still `LOADING` at the end | 1500 | **22.7 fps** | **0.45×** | 44.0 |
| ep128emu-core | none; operator's ROM set in `sys/ep128emu/roms/` (`core/core.cpp:59`) | 1000 | **11.45 fps** | **0.23× of 50** | 87.3 |
| MAME 2003-Plus | Hard Hat (mamedev.org free romset) | 1200 | 53.5 fps | **0.94× of 57.0** | 18.7 |

⚠️ **VICE x64 does not reach real time with its default core options** (reSID 6581, filter on, 48 kHz, as its log
reports). `retro_bench` serves only the `SET_VARIABLES` defaults and has no override, so whether a cheaper SID engine
or a lower rate closes the 1.7× gap is **unmeasured**; an option-override (env var) in `retro_bench` is the next step
for that core. Every core passes `RETRO_ENVIRONMENT` 17 (`GET_VARIABLE_UPDATE`), which `retro_bench` refuses.

- **ep128emu advances only by the time `SET_FRAME_TIME_CALLBACK` reports** (`core/main.cpp:600`, `run_for(curr_frame_time, …)`):
  a frontend that refuses that command gets 0 µs per frame, a blank frame and no audio at ~400 "fps". `retro_bench` now
  serves it (nominal period). With it, the frame dump is the ENTERPRISE boot screen and audio is 881 of 882 samples per
  frame, but the core is **4.4× too slow**; it was already flagged the heaviest per MHz (section 8).
- **MAME 2003-Plus / Hard Hat** reaches the "PLEASE STAND BY / HARDHAT GAME" boot screen (frame dump); 0.94× with
  the core's log discarded, 0.88× with it printed (~30 log lines per frame from the Exidy PIA driver), so a frontend must
  filter core log levels.

`fb_bench` (120 frames per line; fb0 mapped write-combined):

| path | 16bpp RGB565 | 32bpp XRGB8888 |
|---|---|---|
| full-frame memcpy RAM→fb | 3.75 ms (195 MB/s; RAM→RAM 191 MB/s) | 8.36 ms (175 MB/s) |
| second page (`yres_virtual` 960) | **granted** | **refused**: driver answers `virt=800x480` |
| `OMAPFB_WAITFORVSYNC` alone | 16.53 ms (60.5 Hz) | — |
| `FBIOPAN_DISPLAY`, no wait | 0.017 ms | — |
| memcpy + wait + pan (frame loop) | 16.65 ms, 60.1 fps | — |
| NN upscale 256×240 → 800×480, into RAM / RAM then memcpy / direct into fb | 4.23 / **8.22** / 11.34 ms | 8.41 / 16.76 / 22.82 ms |
| same, 320×240 | 4.43 / 8.38 / 11.48 ms | 8.43 / 16.69 / 22.64 ms |
| same, 384×272 | 4.57 / 8.59 / 10.73 ms | 8.77 / 16.90 / 20.83 ms |

- **Page flipping works on our 4.14 omapfb at 16bpp only** (measured): pan is free, the vsync wait tracks the panel
  at 60 Hz. 32bpp gets no second page in the 1,536,000-byte fb0, so a flipping frontend runs 16bpp.
- **Scale into RAM, then one memcpy;** never scatter writes into the mapped fb (35% slower at 16bpp). The upscale is
  `fb_bench`'s plain C nearest-neighbour; ScummVM's NEON scaler is **not** measured here and may be faster.
- **DSS overlay (fb1/vid1):** `OMAPFB_SETUP_MEM` + `OMAPFB_SETUP_PLANE` (scaled to 800×480) both succeeded at
  256×240, 320×240 and 384×272; uploading a native-size RGB565 frame costs **0.19 / 0.23 / 0.35 ms**. Nobody watched
  the panel during the run, so that the scaled plane was **visible**, and how its filtering looks, is **unverified**.
- ⚠️ **The VRFB rotation pass left the gfx plane disabled until reboot.** After `--test overlay`, dmesg had
  `overlay 0 vertically not inside the display area (0 + 800 >= 480)`, then `FIFO UNDERFLOW on gfx, disabling the
  overlay`; `overlay0/enabled` read 0 (dark panel), restarting the app did not help, and writing `enabled` = 1
  underflowed again; a reboot cleared it. That `rotate=1` caused it is **inferred** from the 800-vs-480 message.
  `fb_bench` printed `rotate=1 … got 800x480` and normal copy times, so **it cannot see this failure**: it must
  check `overlay0/enabled` and swap `xres`/`yres` for a 90° rotation before its rotation numbers mean anything.

**Frame budget (arithmetic from the two tables; CPU only, no audio, no colour conversion).** At 60 Hz a frame is
16.7 ms. FCEUmm 8.97 ms + Sharp (software 16bpp scale + memcpy, 8.22 ms) = **17.2 ms: does not fit**; with the
overlay instead (0.19 ms) = 9.2 ms, which fits. FCEUmm also emits XRGB8888, so a 16bpp path adds a conversion that is
not measured. Fuse at 50 Hz (20 ms): 3.8 + 8.4 = 12.2 ms with software scaling. VICE fails before the display path.

**Decision rule (proposed, not yet decided):** a core goes ahead on a display path if core ms/frame + that path's
ms/frame ≤ 80% of the frame period (13.3 ms at 60 Hz, 16 ms at 50 Hz), leaving the rest for audio and input.
By that rule today: Fuse passes both paths; FCEUmm passes only via the overlay; VICE, ep128emu and MAME 2003-Plus
(Hard Hat) pass neither, since each misses real time before any display cost.

## 10. Open questions

- Pin-to-launcher: launcher extension or generated wrapper (argv is fixed today).
- Commodore ROMs for VICE: ship nothing and link out, or find an open replacement?
- Enterprise 128 ROMs: licence status unknown.
- Audio lead: is about 139 ms acceptable for action games?
- Is the overlay filtering acceptable per system (pixel art versus computer text)?
- Tearing without vsync, if the flip path measures badly.
