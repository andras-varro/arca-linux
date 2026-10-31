# retro: libretro cores behind our own frontend

The frontend is `native_apps/retro` (the player) with `native_apps/retro_picker` (the game grid, the launcher tile "Retro"). Cores
are built by `retro/build-cores.sh` and deployed with `retro/build-and-deploy.sh <ip>`; an image carries them except three ([Cores not in the image](#cores-not-in-the-image)). `tools/retro_bench.c` is a headless
core runner and `tools/fb_bench.c` a display-path bench (device-only). Every device fact is linked to
[`SYSTEM_ANALYSIS.md`](../SYSTEM_ANALYSIS.md); claims are marked **measured**, **read in source**, **inferred** or **unverified**.

**Operator-tested at the panel (unit `.115`, 2026-10-30):** Mega Drive (`picodrive`) and PlayStation (`pcsx_rearmed`) play well;
DOS (DOSBox Pure) plays, and a mouse works inside the core. The pad chord Select+Start opens the pause menu in every emulator.
The other systems were not part of that session.

**Save states are experimental** (user-facing statement: [`README.md`](../README.md#3-the-apps)).

## 1. Purpose and scope

Retro emulation is done by loading **libretro cores** into a small frontend of our own that reuses `native_apps/common/`
(framebuffer, gamepad, audio_out, config): one implementation per subsystem.

**RetroArch is not shipped.** It has its own UI, settings and input model, which ignore our safe rectangles, the touch
calibration and the settings app. It is used only as a **reference for correct environment-callback behaviour**.

The libretro environment callback (`RETRO_ENVIRONMENT_*`) has a long tail: the frontend implements the commands the shipped
cores call and refuses every other one cleanly (returns false). `retro_bench` lists what a core really calls.

**No further emulators will be added for now** (operator decision, 2026-10-30). The systems in `retro/systems/*.sys` are the
set. `build-cores.sh` also builds `snes9x2005`, which has no `.sys` and is not offered by the picker.

## 2. Library model

Games are entries independent of what runs them; the runner is configuration.

- One launcher tile, "Retro", opens a game grid built by **scanning user content folders**. The file extension gives the
  system, the system's `.sys` gives the core; per-game overrides live in a `.opt` file (section 7).
- No box art is shipped (copyright). Tiles are generated (system colour and title).
- Pinning one game to the launcher is not implemented; `app_launcher.c` passes a fixed argv to a manifest's binary (read in source).
- Content lives in `/home/root/data/retro/roms/<system>/` (on p3) or on a USB stick at `/media/usb0`
  ([storage](../SYSTEM_ANALYSIS.md#31-soc-memory-and-storage), [USB](../SYSTEM_ANALYSIS.md#36-usb)).

## 3. Content and licensing policy (operator rule)

- Ship only **open-source-rooted** software. Non-commercial but freely redistributable licences are acceptable, **named** where used. **GPL preferred** wherever a feasible GPL alternative exists.
- **Never ship proprietary software, ROMs, BIOS images or games.** Provide only links to official legal download pages and hints on where to buy.

Shipped cores. Licences **read from each repository LICENSE/COPYING file** at the commit named; re-read the exact release before shipping.

| System | Core | Licence | ROMs and shippability |
|---|---|---|---|
| NES | FCEUmm | GPL-2.0 | bundles no ROM (FDS BIOS `disksys.rom` is user-supplied) |
| ZX Spectrum | Fuse | GPL-3.0 | Spectrum 48/128 ROMs built into the core. Amstrad's permission (Cliff Lawson, comp.sys.sinclair, 1999-08-31, as quoted at `github.com/mward-sudo/spec_chum/issues/189`) reads "Amstrad are happy for emulator writers to include images of our copyrighted code as long as the (c)opyright messages are not altered", with a request for the note "Amstrad have kindly given their permission for the redistribution of their copyrighted material but retain that copyright". It is addressed to emulator writers, not to a platform that bundles an emulator, and Debian files the ROM package `spectrum-roms` in `non-free` (`packages.debian.org/sid/spectrum-roms`). **Verdict: UNCLEAR**, so by the operator rule Spectrum is left out of any image or release. The core is built and deployed to a unit only for testing. Sources read 2026-10-30; the original post and Debian's copyright file were **not** read; Fedora, Lakka and Batocera practice was **not** checked |
| C64 | VICE x64 | GPL-2.0-or-later | Commodore ROMs are (c) Commodore Business Machines with no redistribution grant: **not shippable**, user supplies them |
| Enterprise 128 | ep128emu-core | GPL-2.0-or-later | EXOS, IS-BASIC and EXDOS ROMs built into the core (and the Spectrum, CPC and TVC ROMs too), rights holder Intelligent Software Ltd collapsed by 1986, no current holder or grant found. **Verdict: UNCLEAR**, so the Enterprise is left out of any image or release; built and deployed to a unit only for testing. Detail in `LICENSE.md` |
| Arcade | MAME 2003-Plus | classic MAME 0.78 non-commercial licence (`LICENSE.md:9`) | `:45` forbids distributing a modified version of the official MAME archive; `:51` allows non-commercial use of the source if full source is made available. Fits the operator rule only as a **named non-commercial exception** |
| SNES | snes9x2002 (Snes9x 1.39 base) | Snes9x licence: non-commercial (`src/copyright.h`; free redistribution in source and binary form without fee for non-commercial purposes). Read at `6ffbf9e` | not GPL: a **named non-commercial exception**. Bundles no ROM |
| Mega Drive, 32X, Master System, Game Gear | PicoDrive (`picodrive_libretro`, Cyclone 68K and DrZ80 in ARM asm) | MAME-style non-commercial licence (`COPYING:5`: "Redistributions may not be sold, nor may they be used in a commercial product or activity"). Read at `1890c29` | a **named non-commercial exception**, chosen for its ARM asm CPU cores. Bundles no ROM and no BIOS |
| PlayStation | PCSX-ReARMed (`pcsx_rearmed`, ari64 ARM dynarec, NEON GPU and GTE) | GPL-2.0 (`COPYING`). Read at `c881679` | bundles no BIOS and no game. Uses its built-in HLE BIOS unless the user drops a real `scph*.bin` in the system dir |
| Amiga | uae4arm (`uae4arm_libretro`, from UAE) with the built-in AROS Kickstart replacement | GPL-2.0-or-later by the UAE file headers (the repository has no `COPYING`); AROS ROM image `src/aros.rom.cpp` under the AROS Public License (MPL-derived). Read at `26f444f` | bundles no Kickstart and no game. APL section 3.2/3.6: executable form may be distributed if the covered source stays available under the APL and a notice says where; AROS source is `github.com/aros-development-team/AROS` (**inferred**: the exact AROS revision embedded was not identified) |
| DOS | DOSBox Pure (`dosbox_pure_libretro`, DOSBox SVN fork with the ARMv7 `core_dynrec` recompiler) | GPL-2.0-or-later (`LICENSE`; README: "version 2 or later"). Read at `d9c1356` | bundles no game and no operating system. Upstream is `codeberg.org/schelling/dosbox-pure` |

`snes9x2005` (Snes9x 1.43 base; Snes9x non-commercial licence, ndssfc parts GPL-2.0-or-later) is built but not shipped.

Legal test content to **link, never ship**: `mamedev.org/roms` (free, non-commercial, distribution on that site only);
MIT homebrew NES built from source: `zorchenhimer/nes-runner`, `xram64/falling-nes`. Out of reach for this CPU: N64, NDS, Dreamcast, PSP.

### Cores not in the image

A fresh card image and a release bundle carry every core in the table above, with its `systems/*.sys` and system-dir files,
**except** Enterprise 128 (`ep128emu-core`), ZX Spectrum (`fuse`) and C64 (`vice`). Their built `.so` files embed ROM images whose
rights holders have granted no redistribution (rows above), so they are left out; the three systems do not appear in the picker of
an installed image. The list is `IMAGE_EXCLUDE` in `retro/build-and-deploy.sh`, and `tests/retro_bundle_test.sh` fails if a bundle
carries any of the three. snes9x2005 is built but belongs to no system, so it is not staged either.

To use them, build from source and deploy over SSH (WSL, a unit reachable with key auth, the cross toolchain from
[`setup-build-env.sh`](../setup-build-env.sh)):

```bash
wsl.exe -e bash -lc "cd /mnt/c/work/roomwizard/retro && ./build-and-deploy.sh <ip> ep128emu-core fuse vice"
```

`<ip>` alone (no core ids) builds and deploys every core, these three included. `--no-build` deploys what is already in
`~/retro-build/out` (or `$RETRO_OUT`); `--no-pgo` builds VICE without a profile (`retro/pgo-vice.sh`). The script copies the cores
to `/home/root/data/retro/cores`, the `.sys` files to `/home/root/data/retro/systems` and everything under `retro/system/` (the
Enterprise configuration among it) to `/home/root/data/retro/system`, then checks md5 on the unit. It never touches `roms/` or
`content/`.

The ROMs of these three cores are compiled in from the pinned upstream clone, so a build needs no ROM file placed anywhere; a
BIOS or firmware a core loads from disk (the FDS BIOS for NES, a real Kickstart or PlayStation BIOS) goes in
`/home/root/data/retro/system` on the unit. Whether a user-supplied ROM can replace the built-in one in these three cores was Content goes in `/home/root/data/retro/roms/<system>/` as for every system.

## 4. Display

An emulator hands over a full frame per `retro_run`. The panel is 800x480 at 60.05 Hz ([display](../SYSTEM_ANALYSIS.md#32-display)).
A system's `.sys` `video=` key picks `sw` (software scale into fb0) or `overlay` (DSS overlay, hardware scaling, always filtered).

Measured on `.115` with `tools/fb_bench.c` (120 frames per line, fb0 mapped write-combined):

| path | 16bpp RGB565 | 32bpp XRGB8888 |
|---|---|---|
| full-frame memcpy RAM to fb | 3.75 ms | 8.36 ms |
| second page (`yres_virtual` 960) | **granted** | **refused** (driver answers `virt=800x480`) |
| `OMAPFB_WAITFORVSYNC` alone | 16.53 ms (60.5 Hz) | - |
| `FBIOPAN_DISPLAY`, no wait | 0.017 ms | - |
| nearest-neighbour upscale 256x240 to 800x480, into RAM then memcpy | 8.22 ms | 16.76 ms |

- **Page flipping works at 16bpp only**: pan is free and the vsync wait tracks the panel. 32bpp gets no second page in the 1,536,000-byte fb0.
- **Scale into RAM, then one memcpy**; scattered writes into the mapped fb are 35% slower at 16bpp.
- **DSS overlay (fb1/vid1):** `OMAPFB_SETUP_MEM` and `OMAPFB_SETUP_PLANE` (scaled to 800x480) succeed at 256x240, 320x240 and 384x272; uploading a native-size RGB565 frame costs 0.19 / 0.23 / 0.35 ms.
- ⚠️ **A VRFB rotation pass left the gfx plane disabled until reboot** (`FIFO UNDERFLOW on gfx, disabling the overlay`, `overlay0/enabled` read 0). That `rotate=1` caused it is **inferred**; `fb_bench` cannot see this failure. Treat VRFB rotation as unusable.
- Vanilla omapfb implements the vsync wait and pan (`omapfb-ioctl.c`, `omapfb-main.c`, read in source).

**Fit modes.** *Fit*: proportional inside the visible, bezel-compensated area. *Fill*: ignore bezel compensation; the bezel rows hidden at top
and bottom (`FB_BEZEL_TOP_DEFAULT` / `FB_BEZEL_BOTTOM_DEFAULT` in `native_apps/common/framebuffer.h`; the real value is per unit) behave like CRT overscan
([touch and rectangles](../SYSTEM_ANALYSIS.md#33-touch)).

**Timing.** PAL systems (Commodore 64, Spectrum, Enterprise) run at 50 Hz on a 60.05 Hz panel and judder unless audio-synced (**inferred**).

## 5. Input

Bluetooth/USB keyboards, pads and audio are solved in the platform; the frontend reads them through `common/gamepad.h`.

- Pads fill RetroPad ports 0 and 1 in the SNES layout; a keyboard doubles as pad 0 when the core never asks for the keyboard (`native_apps/retro/retro_input.c`).

  | RetroPad | Xbox pad | Keyboard as pad |
  |---|---|---|
  | B / A | A / B | `z` / `x` |
  | Y / X | X / Y | `a` / `s` |
  | L / R, L2 / R2 | LB / RB, LT / RT | `q` / `w`, none |
  | Select / Start | Select / Start | Backspace / Enter |
  | D-pad | D-pad | arrow keys |

- **Pause menu:** pad Select+Start, or F12; Esc too, except on a computer core with a keyboard. It offers disk control, save and load state, and quit.
- What a RetroPad button does is the core's: the core's default mapping applies, except Mega Drive (`picodrive`), whose descriptors name RetroPad
  B = B, A = C, Y = A, X = Y, L = X, R = Z, Select = Mode (`platform/libretro/libretro.c`), so Xbox A = B, Xbox B = C, Xbox X = A.
- **Mouse:** a USB mouse (or a Bluetooth keyboard's touchpad) is `RETRO_DEVICE_MOUSE` on port 0 (deltas, left/right/middle, wheel). It works inside a core; it does nothing in the picker or the pause menu (open bug in `IMPROVEMENT_PLAN.md`).
- There is no on-screen keyboard and no touch play: the frontend assumes a physical keyboard and a pad (operator, 2026-10-25).

## 6. Audio

Through `common/audio_out` (ALSA). `plughw` grants standard rates exactly ([audio](../SYSTEM_ANALYSIS.md#34-audio)). One `AudioOut` per
process. The default lead is a nominal 139 ms, about 79 ms of real audio per the header comment in `native_apps/common/audio_out.h`.

## 7. Cores, build notes and options

Toolchain and flags as ScummVM ([building](../SYSTEM_ANALYSIS.md#6-building-for-this-device)): `arm-linux-gnueabi`, `-march=armv7-a -mtune=cortex-a8 -mfpu=neon -mfloat-abi=softfp`,
dynamic, `platform=unix` (the Makefiles' own ARM platforms are hard-float or armv7ve). Cores load with `dlopen` (`/lib/libdl.so.2`). Every
core binary must pass `native_apps/check-arm-safe.sh` (no `sdiv`/`udiv`; [why](../SYSTEM_ANALYSIS.md#61-cortex-a8-has-no-hardware-integer-divide)).
⚠️ Code a dynarec generates at run time is invisible to that gate; the dynarec sources were read for divide emission (none found).

- **Patches** (`retro/patches/`, applied by `build-cores.sh`): `vice`, `ep128emu-core`, `fceumm`, `mame2003-plus`, `pcsx_rearmed`, `uae4arm`.
- ⚠️ **VICE embeds the Commodore ROMs in the `.so`** as built, so that build is device-only test use. VICE runs its reSID init inside the first `retro_run` (19.5 s on the device; FastSID 0.05 s), so a frontend must show something during that frame or choose FastSID. VICE divides by its sample-rate option, so `GET_VARIABLE` must serve the `SET_VARIABLES` defaults.
- **ep128emu** advances only by the time `SET_FRAME_TIME_CALLBACK` reports; its patch (`core/core.cpp`, `LibretroDisplay::draw`) fixes the high-quality resampler option and adds a draw fast path. With `ep128emu_wait=5` and `ep128emu_sdhq=0` it runs at about real time (19.6 of 20 ms).
- **MAME 2003-Plus** needs a romset (it segfaults in `retro_load_game` without content) and logs about 30 lines per frame on some drivers, so core log levels are filtered.
- **FCEUmm:** `patches/fceumm.patch` rebuilds the background palette table only when palette RAM changed (upstream rebuilt it on every `RefreshLine`, which broke audio on a heavy title).
- ⚠️ **PCSX-ReARMed:** `gteRTPT_sf1lm0_neon` (`libpcsxcore/gte_neon.S`) wrote q4-q6 without saving the VFP registers AAPCS requires (d8-d15). `patches/pcsx_rearmed.patch` saves them in the core, and `run_core()` in `native_apps/retro/retro_main.c` saves d8-d15 around every `retro_run` of any core.
- **SNES:** `snes9x2002` is shipped C-only plus `ARM_ASM=1 ASM_CPU=0 ASM_SPC700=0` (the ARM tile and mode 7 renderers; 8-17% faster; not pixel-identical, about 44 of 57344 pixels differ at the edge of a translucent box). With `ASM_CPU=1 ASM_SPC700=1` it is faster still but segfaulted on one tested title and needs `.include` case fixes and `-z noexecstack`, so it is not built.
- **Amiga (`uae4arm`):** the idle AROS screen costs 16 ms/frame against the 20 ms PAL budget, and `uae4arm_resolution=320x240` (in `amiga.sys`) is the largest saving on games. Several titles stay over budget even then. The JIT runs only for a 68020 or later (`uae4arm_cpu=68020`), saves 2 to 3 ms and changes the emulated CPU, so the patch adds `uae4arm_jit`, `uae4arm_cpu`, `uae4arm_cpu_speed` options, **off by default**. The patch also adds `codec_stubs.c` (FLAC and mpg123 are absent on the unit), links `-z noexecstack` (asm helpers otherwise make `dlopen` fail) and omits `-DARM_HAS_DIV` (it makes the JIT emit `sdiv`/`udiv`). No `.ipf` support. Without a Kickstart file the core falls back to the built-in AROS (read); a user ROM goes in the system dir as `kick34005.A500`, `kick40063.A600` or `kick40068.A1200`. `amiga.sys` lists `adf,dms,zip,hdf,lha,m3u,uae`.
- **PlayStation content:** `psx.sys` lists `m3u,cue,chd,pbp,iso,mdf,exe`, with no `bin`/`img` on purpose: the picker hides only files named by an `.m3u`, so a listed `.bin` would show beside its `.cue`. The picker does not look inside subfolders of a game folder; a game whose discs sit in separate folders needs an `.m3u` naming each disc by relative path. A real BIOS (`scph5501.bin`, `scph1001.bin`, ...) dropped in `/home/root/data/retro/system` is used with `pcsx_rearmed_bios=Auto`, otherwise the HLE BIOS ("expect compatibility problems").
- **DOS content:** `dos.sys` lists `zip,dosz,exe,com,bat,iso,chd,cue,ins,img,ima,vhd,jrc,m3u,m3u8,conf`. Put a game as a `.zip` (or `.dosz`), a folder, or a `.exe`/`.com`/`.bat` in `roms/dos/`; changes a program writes go to a separate `<name>.pure.zip` in the frontend save directory. Windows 3.1 from the user's own media (core README, not tried here): load an install image with the content, choose `[ Boot and Install New Operating System ]`; or install once into a `.dosz` in the system dir, which the start menu lists under `[ Run System Shell ]`. Nothing from Microsoft is shipped. The core's dynarec is selected by `include/config.h` (`C_DYNREC`, `ARMV7LE`); `C_UNALIGNED_MEMORY` was **not verified on the unit**.
- **Profile-guided VICE:** `retro/pgo-vice.sh <ip> --pair CONTENT:STATE[:OPTS] [--pair ...] [--frames 3000]` (WSL) builds an instrumented core, trains it on the unit with `retro_bench`, stores the `.gcda` profile in `~/retro-build/pgo/vice` (`$RETRO_PGO_DIR`) and rebuilds with it. It refuses to run while `retro` runs on the unit. Afterwards `retro/build-and-deploy.sh <ip>` uses the profile; `--no-pgo` forces plain `-O2`. A pair with STATE `-` boots and loads the content itself, which trains the drive CPU.
- **VICE and multi-disk games:** true drive emulation costs 29-30 ms/frame while loading (budget 19.95 ms) and 13-14 ms once running; the core asks for fast-forward during a disk load (`vice_autoloadwarp=disk`), which the frontend honours. A state saved under one true-drive setting does not load under the other.

**Disks.** The frontend implements libretro disk control. A core that reports more than one image gets a `< Disk n/m >` line in the pause
menu (Left/Right or OK: eject, select, insert). The grouping is data: a plain `.m3u` playlist next to the images in `roms/<system>/`, one file name
per line (`#` lines ignored). The picker lists the playlist as one game and **hides the files any `.m3u` in that folder names**; `m3u` must be in
`exts=` of a system whose core loads playlists. Playlists name copyrighted content, so they live on the device only.

### How core options are applied

`native_apps/retro/retro_main.c` (header comment, `builtin_defaults[]`, `add_ovr`, `ovr_get`) builds one option set per run from these layers, **last wins**:

1. the core's own defaults (what it declares through `SET_VARIABLES`);
2. `builtin_defaults[]` in `retro_main.c`, only for cores that are too slow at their own default;
3. `/home/root/data/retro/options/<core>.opt`, all games of that core;
4. `RETRO_OPTS` (comma-separated `k=v`); the launcher fills it from the `opts=` line of `systems/<system>.sys`;
5. `<content path>.opt` beside the game file in `roms/<system>/`, one game.

**System file keys** (`<data>/systems/<id>.sys`, one INI-like file per system; the header of `native_apps/retro_picker/retro_picker.c` is the authority):

| Key | Does | Default |
|---|---|---|
| `name` | display name in the picker | none (required) |
| `core` | core file: a bare name resolves to `<data>/cores/<name>`, a path with `/` is used as is; a system whose core file is missing is left out | none (required) |
| `exts` | content extensions, comma separated, no dot, case-insensitive; a `.m3u` is one game and hides the files it lists | none (required) |
| `opts` | core options `key=value,key=value`, passed as `RETRO_OPTS` | none |
| `roms` | content directory; a subfolder is one game | `<data>/roms/<id>` |
| `video` | `sw` or `overlay`, passed as `retro -v` | frontend default |
| `nogame` | label of a first list entry that starts the core with no content, for cores that run without one | none |

A `.opt` file takes `key=value` and RetroArch's own `key = "value"` form; `#` starts a comment line. Option files are device data and never go in the repo.
A key the core never sees: `retro_start=PROG.EXE`, for a core that loads by path; the frontend appends it to the content, so DOSBox Pure loads
`GAME.ZIP#PROG.EXE` and skips its program list.

Keys that matter per core (names from each core's own option source in the pinned clone; a `.sys` `opts=` overrides a built-in default):

| Core | Keys |
|---|---|
| VICE (Commodore 64) | built-in default `vice_sid_engine=FastSID` and `vice_sound_sample_rate=22050` (reSID is far too slow); `c64.sys` sets `vice_drive_true_emulation=disabled,vice_sound_sample_rate=11025`, which wins; `vice_joyport` and `vice_drive_true_emulation` per game. `nogame=BASIC` starts it with no content |
| Enterprise 128 | `ep128emu_wait=5`, `ep128emu_sdhq=0` by built-in default and in `ep128.sys`; `nogame=BASIC` |
| NES (`fceumm_`) | `fceumm_sndquality`, `fceumm_sndlowpass`, `fceumm_nospritelimit`, `fceumm_region` |
| ZX Spectrum (`fuse_`) | `fuse_machine`, `fuse_fast_load`, `fuse_load_sound`, `fuse_speaker_type`, `fuse_emulation_speed` |
| SNES (`snes9x2002_`) | `snes9x2002_frameskip`, `snes9x2002_frameskip_threshold`, `snes9x2002_frameskip_interval`, `snes9x2002_transparency` |
| Arcade (`mame2003-plus_`) | `mame2003-plus_frameskip`, `mame2003-plus_sample_rate`, `mame2003-plus_cpu_clock_scale` |
| Mega Drive (`picodrive_`) | `picodrive_renderer` (`accurate` default, `good`, `fast`), `picodrive_sound_rate`, `picodrive_frameskip`, `picodrive_region`, `picodrive_drc` |
| PlayStation (`pcsx_rearmed`) | `pcsx_rearmed_frameskip_type=auto` by built-in default: it skips rendering only when the audio ring runs low |
| Amiga (`uae4arm`) | `uae4arm_resolution=320x240` from `amiga.sys`; `uae4arm_jit`, `uae4arm_cpu`, `uae4arm_cpu_speed` (off by default) |
| DOS (DOSBox Pure) | `dosbox_pure_audiorate=44100` by built-in default (our output rate; the core's 48000 costs a resample); `dosbox_pure_cpu_core`, `dosbox_pure_cycles`; next section |

### Settings for DOS programs (DOSBox Pure)

These are starting points by era, written by the user into `<program>.zip.opt` beside the zip in `roms/dos/`. They are not shipped with anything.
Measured on this CPU (OMAP3, 600 MHz): `dosbox_pure_cpu_core=dynamic` is the main win and the normal core costs far more per frame;
`dosbox_pure_cycles=auto` overruns the 14.3 ms frame of a 70 Hz VGA mode; a 640x400 mode costs more than 320x200 even though the NEON conversion is cheap.

| Program kind | Proposed |
|---|---|
| 8088 era | `dosbox_pure_cycles=315` |
| 286 era | `dosbox_pure_cpu_core=dynamic`, `dosbox_pure_cycles=1320` |
| 386 era | dynamic, 4720; 7800 for heavier ones (higher cycles measured slower on some) |
| 640x400 program bound by the guest CPU | dynamic, 2750 as a compromise between smooth and fast drawing |

**Rule of thumb:** start at the era's CPU, lower the cycles if the sound breaks up, raise them if the program is slow while the sound is clean. **Lowering a
program's resolution made a heavy one playable (operator-tested at the panel, 2026-10-30)**; a heavy protected-mode program can still break up at higher
cycles, and tuning it is the user's. An example `<program>.zip.opt`:

```
# start PROG.EXE directly instead of the DOSBox Pure program list
retro_start=PROG.EXE
dosbox_pure_cpu_core=dynamic
dosbox_pure_cycles=1320
```

### No per-app CPU clock

The unit has no overclock support, so no setting here changes the CPU clock.
