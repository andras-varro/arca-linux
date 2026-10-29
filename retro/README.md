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
| SNES | snes9x2002 (Snes9x 1.39 base, ARM asm CPU) | Snes9x licence: non-commercial (`src/copyright.h`: freeware for personal use, commercial users must seek permission; free redistribution in source and binary form without fee for non-commercial purposes) | not GPL: a **named non-commercial exception** like MAME 2003-Plus. Bundles no ROM. Read 2026-10-27 from `src/copyright.h` at `6ffbf9e` |
| SNES | snes9x2005 (Snes9x 1.43 base) | Snes9x non-commercial licence, with ndssfc parts under GPL-2.0-or-later (`copyright` file) | same exception; bundles no ROM. Read 2026-10-27 at `a79dfe9` |
| Mega Drive, 32X, Master System, Game Gear | PicoDrive (`picodrive_libretro`, Cyclone 68K and DrZ80 in ARM asm, SH2/SVP recompilers) | MAME-style non-commercial licence (`COPYING:5`: "Redistributions may not be sold, nor may they be used in a commercial product or activity") | a **named non-commercial exception**, chosen for its ARM asm CPU cores; the GPL Mega Drive cores (BlastEm GPL-3.0, ClownMDEmu AGPL-3.0) were not benchmarked here. Bundles no ROM and no BIOS. Read 2026-10-29 at `1890c29` |
| PlayStation | PCSX-ReARMed (`pcsx_rearmed`, ari64 ARM dynarec, NEON GPU and GTE) | GPL-2.0 (`COPYING`) | bundles no BIOS and no game. Uses its built-in HLE BIOS unless the user drops a real `scph*.bin` in the system dir; read 2026-10-27 at `c881679` |
| Amiga | uae4arm (`uae4arm_libretro`, from UAE; 68k interpreter, JIT code compiled in) with the built-in AROS Kickstart replacement | GPL-2.0-or-later by the UAE file headers (27 source files; the repository has no `COPYING`); AROS ROM image `src/aros.rom.cpp` under the AROS Public License (MPL-derived) | bundles no Kickstart and no game. AROS Public License section 3.2/3.6: executable form may be distributed if the covered source stays available under the APL and a notice says where; AROS source is `github.com/aros-development-team/AROS` (**inferred**: the ROM embedded here is the upstream core's build, its exact AROS revision was not identified). Read 2026-10-27 at `26f444f` |
| DOS | DOSBox Pure (`dosbox_pure_libretro`, DOSBox SVN fork with the ARMv7 `core_dynrec` recompiler) | GPL-2.0-or-later (`LICENSE`, GPL-2 text; README: "version 2 or later") | bundles no game and no operating system. Upstream moved to `codeberg.org/schelling/dosbox-pure` (the GitHub repository holds only a README). Read 2026-10-28 at `d9c1356`. The alternatives, DOSBox SVN libretro (`libretro/dosbox-svn`) and DOSBox-X, are GPL-2.0 as well |

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
  computer-keyboard overlay would be new code. **The first version assumes a physical keyboard and a controller
  are attached** (operator, 2026-10-25), so it has no on-screen keyboard and no touch play.

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
- **SNES (measured 2026-10-27, `.115`, PAL `(E)` ROMs, 900 frames of title/attract, no pad input, frontend `core` ms, `-O3` as upstream builds):** `snes9x2002` (C only, shipped in `systems/snes.sys`) 4.1-14.6 ms over 12 ROMs (worst Super Bomberman 14.6, Sim City 14.0, Donkey Kong Country 14.1), no crash; `snes9x2005` 5.3-15.8 ms (worst Super Bomberman 15.8, Sim City 15.4). Both fit the 20 ms PAL budget; only about 2 ms under the 16.64 ms NTSC budget at the worst. 2002 built with `ARM_ASM=1 ASM_CPU=1 ASM_SPC700=1` is 25-35% faster (Donkey Kong Country 9.4 ms, `retro_bench` 8.3 vs 12.7) but **segfaults on Super Mario Kart** (frontend exit 139 and `retro_bench`; C-only and 2005 run it), and needs `.include` case fixes (`*.s` to `*.S`) plus `-z noexecstack` to build at all, so it is not shipped. Audio underruns were counted over the first 17 stat lines only and are scattered (title load), not sustained. Gameplay was not measured (no pad).
- **PlayStation (`pcsx_rearmed`, built 2026-10-27, `c881679`, `Makefile.libretro platform=unix` with the section 7 flags):** the Makefile's own detection picks `DYNAREC=ari64`, `BUILTIN_GPU=neon`, `NEON_ASM=1` from `-mfpu=neon` (softfp is fine for the NEON asm), `-Ofast`, 1.3 MB stripped. `sdiv`/`udiv`: none in the objects (gate PASS) and none in the dynarec source: `assem_arm.c` emits PS1 `DIV`/`DIVU` as a shift-subtract loop, and `HAVE_ARMV7` there only gates `movw`/`movt`/`ubfx`-style encodings; the `lightrec` and `lightning` code, which does have idiv, is not built for `ari64`. Not measured: the code the dynarec emits at run time, with a game. On `.115` it loads, maps its dynarec memory and runs under the HLE BIOS (`retro_bench` with a hand-made 4 KB PS-X EXE that jumps to itself: 600 frames, no crash, 735 audio frames per video frame); `retro_load_game` refuses no content. **No PS1 game was on the unit, so ms/frame is unmeasured** (budget 16.64 ms NTSC, 20 ms PAL).
- **PlayStation BIOS and discs:** `systems/psx.sys` lists `m3u,cue,chd,pbp,iso,mdf,exe` (the core opens a `.mdf` directly and reads its `.mds` beside it when there is one; `mds` itself is not listed), with no `bin`/`img` on purpose: the picker hides only files named by an `.m3u`, so a listed `.bin` would show beside its `.cue`. The picker does not look inside subfolders of a game folder, so a game whose discs sit in separate folders needs an `.m3u` in the game folder naming each disc by relative path (the core resolves it against the playlist directory; disc swapping is the frontend disk control, not tied to the image format). Put the `.cue` and its `.bin` in `roms/psx/` (a `.bin` with no `.cue` is not offered; make an `.m3u` or a `.cue`). The core option `pcsx_rearmed_bios=Auto` (default) uses a real BIOS from the system dir `/home/root/data/retro/system` (a file such as `scph5501.bin`, `scph1001.bin`, `scph5500.bin`, `scph5502.bin`, dropped there by hand) and otherwise falls back to HLE ("expect compatibility problems"). Nothing from Sony is shipped.
- **Amiga (`uae4arm`, built 2026-10-27, `26f444f`, `patches/uae4arm.patch`):** chosen over libretro-uae (PUAE, `7387937`, plain `platform=unix` + `-lm`, 20 MB). Both boot the built-in AROS with a blank 880 KB `.adf` (the only content; nothing copyrighted) on `.115`, `retro_bench` 600 frames, no input: **uae4arm 16.0 ms/frame** (15.8 with `uae4arm_leds_on_screen=off`), PUAE **53.9 ms** (49.5 with lores/single-line/fast-CPU/16-bit options; it has no ARM JIT), so only uae4arm can fit the 20 ms PAL budget, and its 16 ms is for an idle screen: **games, measured 2026-10-28 on `.115`, `rb2` 600 frames after 1500 warm-up (before, with the default 768x270 hires: Lemmings 36.0, Dyna Blaster 33.0, Giana 31.8, Rod-Land 26.2, Pinball Dreams 24.9, Another World 24.3): `uae4arm_resolution=320x240` (lores, shipped in `amiga.sys`) alone gives Lemmings 30.0 (a cracktro, not gameplay), Pinball Dreams 19.1; the fast-copper option is already on (off: Lemmings 61.7); `uae4arm_leds_on_screen=off` changes nothing; the patch adds `uae4arm_jit`, `uae4arm_cpu` (68020) and `uae4arm_cpu_speed` options, **off by default**: the JIT runs only for a 68020 or later (`newcpu.cpp`), so it needs `uae4arm_cpu=68020`, and with lores it gives Pinball 16.0, Another World 16.6, Lemmings 28.9, a 2 to 3 ms gain that changes the emulated CPU (Rick Dangerous produced no output with it); `cpu_speed=max` is slower (Lemmings 40.8). With lores: Dyna Blaster 25.8, Giana 26.3, Rod-Land 22.3 still over the 20 ms budget. Golden Axe and Lost Vikings stay on a grey AROS screen (not booting under the built-in AROS, so their 18 ms is idle) and Rick Dangerous is garbled stripes. Audio frames per video frame do not track speed (Lemmings 784 at 36 and at 29 ms), so the shortfall is not a slowness symptom. 6.2 MB stripped. The patch adds `codec_stubs.c` (the core calls FLAC and mpg123 directly for CD32/CDTV audio tracks and the unit has neither library; the stubs fail, `new()` returns NULL), links `-z noexecstack` (asm helpers otherwise make `dlopen` fail) and drops `-lz -lFLAC -lmpg123` (zlib is in-tree). `capsimg` (IPF) is not built, so no `.ipf`. The Makefile's NEON branch passes `-DARM_HAS_DIV`, which makes the JIT emit `sdiv`/`udiv`; the build sets the variables itself and omits it (gate PASS, 0 `sdiv`/`udiv`). The JIT is compiled in; the core never set `cachesize`, so the patch adds an option for it (JIT code emits no `sdiv`/`udiv` without `ARM_HAS_DIV`: the eight sites in `compemu_midfunc_arm2.cpp` are all under that `#ifdef`, else VFP division; read). **Kickstart:** `memory.cpp` falls back to the built-in AROS when the ROM file is missing (read); the user's own ROM goes in the system dir `/home/root/data/retro/system` as `kick34005.A500` (Amiga 500), `kick40063.A600`, `kick40068.A1200` (names and MD5s: the core README). `systems/amiga.sys` lists `adf,dms,zip,hdf,lha,m3u,uae`; disk swapping is the frontend disk control with an `.m3u`. Put images in `roms/amiga/`.
- **DOS (`dosbox-pure`, built 2026-10-28, `d9c1356`, plain `make platform=unix` with the section 7 flags, `STRIPCMD=true` because the Makefile strips with the host `strip`):** 2.7 MB stripped, no patch, gate PASS (113 objects, 0 `sdiv`/`udiv`; 0 in the linked `.so` by `objdump`). **Why this core (read, not timed):** `include/config.h` selects `C_DYNREC` with `C_TARGETCPU ARMV7LE` for `__arm__` with `__ARM_ARCH >= 7`, so the recompiler runs on this CPU; `libretro/dosbox-svn` has the same `core_dynrec` ARMv7 selection in `libretro/dynarec.h` but is a plain port with no zip/disc-image start menu; **DOSBox-X has no libretro target** in its tree at `997f616` (no file with `libretro` in its name, no mention in `BUILD.md`; its SDL builds are far larger and need SDL), so it was not built. **Dynarec divide, read:** no `sdiv`/`udiv` mnemonic or encoding in `src/cpu/core_dynrec/risc_armv4le*.h` (the ARM emitter); `ARMV7LE` only enables `movw`/`movt` forms and unaligned access (`C_UNALIGNED_MEMORY`, **not verified on the unit**). Not measured: the code the dynarec emits at run time, and any speed (**no ms/frame**, no unit was touched); core option `dosbox_pure_cpu_core` selects dynamic or normal, and `dosbox_pure_memory_size` defaults to 16 MB of emulated RAM (that much resident when a program uses it). A 386 real-mode game is the expected fit; Windows 3.1 and anything protected-mode heavy is **unverified**.
- **DOS content and Windows 3.1:** `systems/dos.sys` lists `zip,dosz,exe,com,bat,iso,chd,cue,ins,img,ima,vhd,jrc,m3u,m3u8,conf` (the core's own `valid_extensions`, minus directories; no `bin`, for the reason given under PlayStation). Put a game as a `.zip` (or `.dosz`), a folder, or a `.exe`/`.com`/`.bat` in `roms/dos/`; changes a program writes go to a separate `<name>.pure.zip` in the frontend save directory, not into the zip. **Windows 3.1 from the user's own media (core README, not tried here):** load an install CD or floppy image together with it, choose `[ Boot and Install New Operating System ]` in the start menu, pick a disk size, run the installer; afterwards every content offers `[ Run Installed Operating System ]`. Or install once into a `.dosz` placed in the system dir `/home/root/data/retro/system`, which the start menu lists under `[ Run System Shell ]` and underlays beneath each game zip. Nothing from Microsoft is shipped.
- **Yoshi's Island (Super FX 2, PAL, 50.32 fps, 19.9 ms budget; measured 2026-10-28, `.115`, scripted pad, savestate in the first level):** C-only `snes9x2002` 17.2 ms (17-19.7 over the level, so the frontend overran), `snes9x2005` 17.7-19.7. `RETRO_PROF`: PPU tile drawing (`DrawTile16`, `S9xUpdateScreen`, `DrawBackground_16`) about 35%, `fx_run` 6%: the chip is not the cost. `ARM_ASM=1 ASM_CPU=0 ASM_SPC700=0` (the ARM tile and mode 7 renderers; shipped in `build-cores.sh`) runs the same frames at 12.8 ms, 8-17% faster on other titles, no crash on Super Mario Kart (6000 frames of play). Not pixel-identical: about 44 of 57344 pixels differ at the edge of a translucent box.
- **FCEUmm and Prince of Persia (measured 2026-10-27, `.115`):** the audio break-up was the core. Gameplay costs 16.7 ms/frame (budget 16.64; 534 underruns in 30 s) while Super Mario Bros. 3 is 9.4 ms; the first 18 s (title) is 8.5 ms. `RETRO_PROF`: `RefreshLine` 48% of samples, because upstream `ac0d00a` rebuilds a 256-entry background palette table on every call and PoP calls it many times a line ($2002 polling). `patches/fceumm.patch` rebuilds only when `PALRAM[0..15]` changed: 10.7 ms in `retro_bench` (video and audio hashes identical), 0 underruns in the frontend. `-O3` 10.55 ms (1%), `-Os` 12.8 ms: left at `-O2`. PGO not pursued (36% headroom).
- **Profile-guided VICE:** `retro/pgo-vice.sh <ip> --pair CONTENT:STATE[:OPTS] [--pair ...] [--frames 3000]` (WSL) builds an
  instrumented core, trains it on the unit with `retro_bench` (both paths of a pair are on the unit, e.g. a game and its
  saved `states/c64/<game>.s1`; nothing copyrighted reaches the host), stores the `.gcda` profile in
  `~/retro-build/pgo/vice` (`$RETRO_PGO_DIR`) and rebuilds vice with it. It refuses to run if `retro` is running on the
  unit. Afterwards `retro/build-and-deploy.sh <ip>` uses the profile by itself; `--no-pgo` forces plain `-O2`.
  A pair with STATE `-` has no state: it boots and loads the content itself, which trains the drive CPU (OPTS
  `vice_drive_true_emulation=enabled,...`). Measured with a profile that adds P47 and Last Ninja 2 loads (TDE) to the
  Impossible Mission state: loading with TDE 24.5 -> 22.0 ms/frame (P47), 22.9 -> 21.0 (Last Ninja 2); the TDE-off
  fixture 13.22 -> 13.57 ms/frame.
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
| VICE x64 3.10, defaults | none (`READY.`) | 1500 | 68.6 fps | 1.37× of 50.125 | 14.57 |
| VICE, `vice_sid_engine=FastSID` | none | 1500 | 83.8 fps | 1.67× | 11.93 |
| VICE, `vice_sound_sample_rate=22050` | none | 1500 | 69.0 fps | 1.38× | 14.49 |
| VICE, `vice_drive_true_emulation=disabled` | none | 1500 | 77.4 fps | 1.54× | 12.93 |
| VICE, all three above | none | 1500 | 99.4 fps | **1.98×** | 10.06 |
| VICE, defaults | Impossible Mission `.zip` (operator's), autostart, loading from disk | 1500 | 32.2 fps | **0.64×** | 31.04 |
| VICE, FastSID + no true drive emulation | same | 1500 | 80.9 fps | 1.61× | 12.36 |
| ep128emu-core | none; operator's ROM set in `sys/ep128emu/roms/` (`core/core.cpp:59`) | 1000 | **11.45 fps** | **0.23× of 50** | 87.3 |
| ep128emu, `ep128emu_wait=5` | same | 500 | 20.0 fps | 0.40× | 49.9 |
| ep128emu, `wait=5` + `sdhq=0`, `patches/ep128emu-core.patch` | same | 500 | 51.1 fps | **1.02×** | 19.6 |
| MAME 2003-Plus | Hard Hat (mamedev.org free romset) | 1200 | 53.5 fps | **0.94× of 57.0** | 18.7 |

Rows measured 2026-10-25; each speed excludes the first `retro_run`, timed apart (`RETRO_WARMUP`, default 1 frame).
`RETRO_OPTS=key=value,…` overrides a core option and `RETRO_LIST_OPTS=1` lists every option with its choices.

⚠️ **VICE runs its reSID init inside the first `retro_run`: 19.5 s on the device** (FastSID: 0.05 s). The first
session's VICE figures (0.59× idle, 0.45× loading) counted it, so they measured the bench and not the core. A frontend must
show something during that frame, or choose FastSID. The other cores' first frames take 0.01–0.08 s, so their rows
stand (re-measured: ep128emu 0.23×, Fuse 5.16×, Hard Hat 0.89× with the log printed and filtered).

- **VICE is real time with its defaults when idle, but not while loading with true drive emulation** (0.64×): the
  1541's own 6502 runs during a load. The FastSID + no-TDE row is **not** the same workload, because without TDE the
  load is trapped and finishes almost at once, so it most likely times the game rather than the load (**inferred**:
  no frame dump was taken). The sample rate costs nothing measurable; reSID over FastSID costs 2.6 ms/frame and TDE
  1.6 ms/frame idle.
- **ep128emu's 0.23× was the core's settings and its frontend glue, not the Z80** (measured 2026-10-25, `RETRO_PROF`
  sampling, 2438 samples): `AudioConverterHighQuality::sendInputSignal` took **45.5%** of all CPU, the display
  thread's per-pixel `LibretroDisplay::draw` 26.9%, the Z80 about 4%, Nick and Dave about 6%. Three fixes, each
  measured alone, take it to **1.02×** (CPU-bound, not paced: user + sys 10.05 s of 10.38 s real):
  - `ep128emu_wait=5`: the default `wait=0` makes `retro_run` busy-spin on the VM thread (`core/core.cpp` `run_for`);
    86 → 50 ms/frame.
  - `ep128emu_sdhq=0` did nothing as shipped: `core/core.cpp:455` creates the core with `highQuality = true`, and
    `check_variables` (`core/main.cpp:234`) only acts when the option differs from its cached copy, which was already
    updated before the core existed. `patches/ep128emu-core.patch` compares against the core's config; 50 → 34 ms.
  - The same patch adds a fast path to `LibretroDisplay::draw` for frames with no border scan and no interlace (one
    palette lookup per pixel, then a `memcpy` for the doubled line); 34 → 19.6 ms, frame 500 byte-identical (md5).
    The run is half-frame (768×288), so the `memcpy` branch and the interlaced and border-scan frames are **not
    exercised** by it.

  Building without the `unix` platform's `-fno-inline-functions` (Makefile:504) changed nothing (34.1 vs 34.2 ms).
  Left after the fixes: the low-quality resampler 13.9%, `draw` 16.5%, Dave + VM loop 17%, Z80 6%, libc ~26%.
  The patch is GPL-2.0-or-later like the core, against upstream `6af5d03`.

Every core passes `RETRO_ENVIRONMENT` 17 (`GET_VARIABLE_UPDATE`), which `retro_bench` refuses.

- **ep128emu advances only by the time `SET_FRAME_TIME_CALLBACK` reports** (`core/main.cpp:600`, `run_for(curr_frame_time, …)`):
  a frontend that refuses that command gets 0 µs per frame, a blank frame and no audio at ~400 "fps". `retro_bench` now
  serves it (nominal period). With it, the frame dump is the ENTERPRISE boot screen and audio is 881 of 882 samples per
  frame (speed: the bullet above).
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
not measured. Fuse at 50 Hz (20 ms): 3.8 + 8.4 = 12.2 ms with software scaling. VICE (384×272, 20 ms) with defaults: 14.6 + 8.6 = 23.2 ms in software, 14.9 ms via the
overlay; with FastSID + 22 kHz + no TDE: 18.7 / 10.4 ms.

**Decision rule (proposed, not yet decided):** a core goes ahead on a display path if core ms/frame + that path's
ms/frame ≤ 80% of the frame period (13.3 ms at 60 Hz, 16 ms at 50 Hz), leaving the rest for audio and input.
By that rule today: Fuse passes both paths; FCEUmm and VICE (idle, either option set) pass only via the overlay;
VICE while loading with TDE and MAME 2003-Plus (Hard Hat) pass neither, since each misses real time before any display
cost; patched ep128emu is real time (19.6 of 20 ms) but over the 16 ms line before any display cost.

### Multi-disk games (.m3u) and per-game options

**Disks.** The frontend implements libretro disk control (`SET_DISK_CONTROL_INTERFACE`, `..._EXT_INTERFACE`,
`GET_DISK_CONTROL_INTERFACE_VERSION`). A core that reports more than one image gets a `< Disk n/m >` line in the
pause menu (Left/Right or OK: eject, select, insert). The grouping is data, not code: the operator writes a plain
`.m3u` playlist next to the images in `roms/<system>/`, one file name per line (`#` lines ignored), e.g.
`Last Ninja 2.m3u` listing its three `.d64`. The picker lists the playlist as one game and **hides the files any
`.m3u` in that folder names**; `m3u` is in `exts=` of `c64.sys` (add it to any system whose core loads playlists:
Amiga, PS1). Measured: `vice_x64_libretro` loads `.m3u` natively and reports `Disk 1/3`.
Playlists name copyrighted content, so they live on the device only.

**Per-game core options.** `<content path>.opt`, beside the content in `roms/<system>/` (for example
`P47 Thunderbolt (...).d64.opt`, `Last Ninja 2.m3u.opt`): `key=value` lines, `#` comments; layered LAST, so it wins
over `<core>.opt`, the `.sys` `opts=` and the built-in defaults. Device data, never in the repo.
Known: `vice_joyport=1` (Ghostbusters wants port 1), `vice_drive_true_emulation=enabled` (P47, Last Ninja 2).
Warning: a state saved under one true-drive setting does not load under the other.
Measured on `.115`: true drive emulation costs about 29-30 ms/frame (33 fps of 50.1, budget 19.95 ms) on P47 and
Last Ninja 2, against about 9 ms with it off.
**Measured later (`.115`):** the 29-30 ms is the load phase only; once a TDE game is running the core is 12.9-14 ms and 51 fps
(Last Ninja 2 in-game, Street Rod parts shop). Loading stays at 34-36 fps: the core asks for fast-forward
(`vice_autoloadwarp=disk`, `SET_FASTFORWARDING_OVERRIDE`), which the frontend honours (unthrottled, a picture every 100 ms, audio
dropped), but warp does not make a frame cheaper, so it gains about 10% and only silences the load noise. With true drive
emulation on: Last Ninja 2 and Creatures run, Street Rod gets past the copy-protection check to the parts shop (TDE off
it ends in DISK ERROR). The drive idle method is a VICE resource with no libretro option; not measured.

## 10. Open questions

- Pin-to-launcher: launcher extension or generated wrapper (argv is fixed today).
- Commodore ROMs for VICE: ship nothing and link out, or find an open replacement?
- Enterprise 128 ROMs: licence status unknown.
- Audio lead: is about 139 ms acceptable for action games?
- Is the overlay filtering acceptable per system (pixel art versus computer text)?
- Tearing without vsync, if the flip path measures badly.
