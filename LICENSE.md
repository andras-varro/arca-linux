# Licence

This project's own code is **MIT**. Everything it links against, builds from or redistributes is
enumerated below, because two of those things carry obligations that MIT does not.

**Why MIT and not Apache-2.0.** This repository publishes binaries that are **GPL-2.0-only** — the three
kernel modules under *usb\_host* — and Apache-2.0's patent-termination clause is incompatible with
GPL-2.0-only. MIT is compatible with GPL-2.0, GPL-2.0-or-later and GPL-3.0-or-later, all three of which
appear below. That is the whole reason for the choice.

**What is not here, deliberately.** No Steelcase firmware and no device configuration. `uImage-system` is
a 5.2 MB vendor binary and is never copied, never committed and never published. `release.sh` refuses to
publish either class of file rather than trusting each component to remember.

---

## MIT License

Copyright (c) 2026 Andras Varro

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and
associated documentation files (the "Software"), to deal in the Software without restriction, including
without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the
following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial
portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT
LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO
EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR
THE USE OR OTHER DEALINGS IN THE SOFTWARE.

---

## What MIT covers

Everything committed in this repository **except the files named in the next section**: the C sources
and shared library under `native_apps/`, `vnc_client/`'s client and its renderer/input/settings, the
ScummVM backend port under `scummvm-arca/backend-files/`, `usb_host/`'s `devmem_write.c` and Python
tooling, every shell script (`arca.sh`, `deploy-all.sh`, `release.sh`, `provision.sh`, `lib/`, `rootfs/`'s scripts,
`device-files/`, `tests/`), `kernel/`'s `build-image.sh`, `build-modules.sh` and `tools/*.c`, `uboot/BUILD.md` and `gen-mux.py`, the `.ppm`
launcher icons, and the documentation. `kernel/dts/rw20.dts` is **not** MIT: its SPDX line says GPL-2.0-only and it includes the kernel
tree's own `.dtsi` files; it carries the vendor panel's timing
*numbers* and none of the vendor device tree's text.

⚠️ **The audio assets are NOT MIT and this used to say the opposite.** `native_apps/sounds/fx_*.wav` were
once the output of the committed `native_apps/sounds/fx_gen.c`, which made them MIT by construction; that
generator is **retired** and every shipped effect is now a sourced file. Both the effects and the music
beds are third-party content with their own terms — next section. `fx_gen.c` and `gen-sounds.sh` are still
committed and still MIT, but nothing that ships comes out of them.

⚠️ **MIT covers the *source*; it does not decide the licence of a *binary* it is linked into.** The
backend port under `scummvm-arca/backend-files/` is MIT as source and is compiled into ScummVM, so
the resulting `scummvm` binary is GPL-3.0-or-later as a whole. Same for `vnc_client`, which links
LibVNCClient. Both are listed under *Distributed binaries* below. Taking the backend port's source and
using it elsewhere is governed by MIT alone.

---

## Third-party code committed in this repository

Derived code artifacts and thirteen audio assets. ⚠️ **The audio rows are the only thing in this
repository that is not free for any use**, so read them before packaging a release:

| File | Licence | What it is |
|---|---|---|
| `scummvm-arca/vkeybd_arca.zip` | **GPL-3.0-or-later** | A 2× scaled derivative of ScummVM's own `vkeybd_small.zip` virtual-keyboard pack, produced by the committed `make_vkeybd_scaled.py`. Derived from ScummVM, so it carries ScummVM's licence, not this project's. |
| `kernel/patches/*.patch` | **GPL-2.0-only** | Patches to Linux 4.14.52 source, authored in this project. A patch is a derivative of the file it edits, so it carries the kernel's licence, not this project's. They apply to the **kernel image** only; the three modules below are built from the unpatched tree. |
| `kernel/drivers/*/` | **GPL-2.0-only** | Out-of-tree kernel drivers; `cy8ctmg120_ts.c` is adapted from the kernel's `drivers/input/touchscreen/cy8ctmg110_ts.c`, so it carries the kernel's licence, not this project's. |
| `uboot/rw20-board.patch` | **GPL-2.0-or-later** (the `.dts` hunk is **GPL-2.0-only**, per its SPDX line) | Adds board `rw20` to U-Boot v2026.10, authored in this project. A patch is a derivative of the tree it edits, so it carries U-Boot's licence, not this project's. `uboot/BUILD.md` beside it is our own documentation. |
| `native_apps/common/font_table.inc` | **Public domain** (the source's BDF header: `COPYRIGHT "Public domain font.  Share and enjoy."`) | Generated by `native_apps/gen_font.py`. The Latin-1, Latin Extended-A, Greek, Cyrillic and punctuation glyphs come from the misc-fixed `6x9.bdf` (ISO10646-1) in Markus Kuhn's ucs-fonts package, `https://www.cl.cam.ac.uk/~mgk25/download/ucs-fonts.tar.gz`, described at `https://www.cl.cam.ac.uk/~mgk25/ucs-fonts.html`; its authors are undocumented. The ASCII glyphs are this project's earlier table. Only the generated table is committed, not the BDF. |
| `native_apps/common/stb_image.h` | **Public domain / MIT** (dual-licensed in the file's own footer; Sean Barrett and contributors) | Verbatim copy of v2.30 from `https://github.com/nothings/stb`, last upstream change to the file commit `013ac3beddff3dbffafd5177e7972067cd2b5083` (2024-05-31). Used only by `common/jf_img.c` (JPEG decode of Ludfin album art). Do not edit; update by replacing the file. |
| `native_apps/common/pl_mpeg.h` | **MIT** (`SPDX-License-Identifier: MIT` in the file's own header; Dominic Szablewski) | Verbatim copy from `https://github.com/phoboslab/pl_mpeg` (md5 `b08a46a7476c24eed194bc5bfbfe72b6`; the upstream commit was not recorded **[unchecked]**). MPEG-1 video, MP2 audio and MPEG-PS demuxer, used only by `native_apps/jfvideo/vid_dec.c`. Do not edit; update by replacing the file. |
| `native_apps/data/ca-certificates.crt` | **MPL-2.0** (Mozilla Contributors) | The Mozilla root-certificate bundle, copied unmodified by `native_apps/refresh-ca-bundle.sh` from the WSL Ubuntu `ca-certificates` package, whose `/usr/share/doc/ca-certificates/copyright` declares `Copyright: Mozilla Contributors`, `License: MPL-2.0` (read 2026-10-21). Installed as `/opt/arca/ca-certificates.crt` and read by `common/http.c` for TLS verification; MPL-2.0 file-level terms apply, so the file stays unmodified. |
| `native_apps/common/stb_image_write.h` | **Public domain / MIT** (same footer) | Verbatim copy from the same repository (fetched 2026-10-21). Moved from `native_apps/tests/` when the picture frame needed it: encodes the stored library JPEG in `native_apps/frame/frame_core.c` (a device binary), and still generates the JPEG fixtures of `jf_art_test.c` and `frame_core_test.c`. |
| `native_apps/sounds/fx_*.wav` (eleven files) | **Not for commercial use**, AND **attribution to ElevenLabs (`elevenlabs.io`) is required** — both settled 2026-08-23, see the paragraph below | AI-generated by the project author at `https://elevenlabs.io` on a **free** account (2026-08-21; `fx_gameover` 2026-08-22), then peak-normalised and format-converted with `ffmpeg` by this repository. Not our own synthesis; the prompts are recorded in `native_apps/sounds/prompts.md`. |
| `native_apps/music/<stem><n>-mono.wav` (every bed in that directory) | Royalty-free, commercial use permitted | AI-generated by the project author at `musely.ai` — **all of them, stated by the operator 2026-08-22**; `officerunner{1,2}` came from `musely.ai/tools/platformer-level-music` on 2026-08-14. Downmixed to mono/44100/16-bit with `ffmpeg`, byte-for-byte the mix bus's internal format so nothing resamples at runtime, and `-ac 1` averages rather than sums so the downmix cannot clip on the way down; not our composition. No attribution required per that generator's FAQ. **Two limits on that clearance, and do not widen past them:** the FAQ asserts clearance but **not ownership**, so these cannot be licensed onward as this project's own; and no copy of its text as it stood on the generation dates is archived here, so the claim is only as good as the page. ⚠️ Every one carries the literal string `made with suno` in its WAV `ICMT` metadata (measured with `grep -a` on the committed bytes; `strings` does not exist in Git Bash and returns nothing, which reads as absence) — consistent with `musely.ai` being a Suno front end, and **not** an independent licence claim of its own. Removing any bed is safe: a game whose configured bed is missing plays its effects and carries on. |
| `kernel/3rdparty/realtek/bluetooth/rtl8761cu_{fw,config}.bin` | **Realtek redistributable firmware licence** — binary redistribution **without modification** only, and only with `LICENCE.rtlwifi_firmware.txt` (committed beside them) | Vendor firmware for the RTL8761CU Bluetooth dongle, loaded by `btrtl.ko` from `/lib/firmware/rtl_bt/`. Byte-for-byte from linux-firmware commit `35e542439e8a` — **measured** by git blob ID; full provenance and md5s in that directory's `README.md`. Not our work and never modified here, so it is not MIT. |
| `device-files/bluetooth.conf` | **GPL-2.0-or-later** | BlueZ 5.66's own `src/bluetooth.conf` dbus policy, byte for byte (md5 `440007ba…`), installed to `/etc/dbus-1/system.d/`. BlueZ's tree is GPL-2.0-or-later (SPDX lines; `COPYING` is GPLv2); the file carries no header of its own. Not our work, so not MIT. |
| `device-files/bluealsa.conf`, `device-files/20-bluealsa.conf` | **MIT** (BlueALSA's licence; copyright 2016-2023 Arkadiusz Bokowy) | BlueALSA 4.3.1's own dbus policy (`bluealsa.conf`, → `/etc/dbus-1/system.d/`) and ALSA integration file (`20-bluealsa.conf`, → `/etc/alsa/conf.d/`), byte for byte what its build installs. Measured: neither file carries a header, and the tarball's `LICENSE` is MIT. Same licence as this repository, but the copyright notice is upstream's and is reproduced in `NOTICE`. |
| `HardwarePhotos/*`, `beaglebone/image/*.jpg`, `Screenshots/*` | **The project author's own photographs and captures** — no third-party rights | Taken by the operator (teardown and BeagleBone Black boards) or captured from the device's own panel; stored in Git LFS. Covered by this repository's terms as documentation. |

⚠️ **The eleven `fx_*.wav` carry TWO obligations, and both are now SETTLED** — ElevenLabs answered the
author's enquiry and the operator relayed the answer on 2026-08-23, which closes the ⏳ that stood here:

1. **Not for commercial use.** Audio generated on a free account, which is the account these were
   generated on, is not cleared for commercial use.
2. **Attribution is REQUIRED, and it is required in this file.** The credit is to **ElevenLabs**, given as
   **`elevenlabs.io`**. It is not optional and not satisfied by the provenance column alone, so it is
   stated as an obligation here and reproduced verbatim in a release bundle's `NOTICE`.

**The attribution, in the form the obligation asks for:**

> Sound effects generated with ElevenLabs — `elevenlabs.io`.

So: this repository's *code* is MIT and stays MIT, but a **commercial** redistribution of the repository or
of a release bundle must replace or remove those eleven files, and **any** redistribution must carry that
credit line. The only other committed files under a narrower condition are the Realtek firmware and `device-files/bluetooth.conf` (their rows above). **Do not widen this row from inference** — what is
established is exactly the two obligations above, no more, and the sourced-asset facts that are technical
rather than legal live at `native_apps/CLAUDE.md` → *Sound assets*.

Nothing else third-party is committed. Every other dependency is either **downloaded at build time** or
**built from a tree the developer clones themselves**, and neither is redistributed by this repository.

---

## Built from sources this repository does not contain

Cloned or downloaded by the build scripts, into a local prefix. Listed with the versions the scripts
actually pin, because "current upstream" is not a fact anyone can check later.

| Component | Version | Licence | Fetched by |
|---|---|---|---|
| ScummVM | commit `eaccc4614e88476f73293dd3ad033ffc76c37ab8`, pinned in `scummvm-arca/upstream-commit` (the build refuses any other HEAD) | **GPL-3.0-or-later** | `setup-build-env.sh --scummvm` clones and checks it out into `scummvm/` (gitignored); `scummvm-arca/build-and-deploy.sh` builds it |
| LibVNCServer / LibVNCClient | 0.9.14 (sha256-pinned tarball) | **GPL-2.0-or-later** | `vnc_client/build-deps.sh` |
| libjpeg-turbo | 2.1.5.1 (sha256-pinned tarball) | IJG · BSD-3-Clause · zlib | `vnc_client/build-deps.sh` |
| zlib | 1.2.13 (`vnc_client`), 1.3.1 (ScummVM), both sha256-pinned | zlib licence | both build scripts |
| libpng | 1.6.43 (sha256-pinned tarball) | PNG Reference Library License v2 | `scummvm-arca/build-and-deploy.sh` |
| BlueZ | 5.66 (sha256-pinned tarball) | **GPL-2.0-or-later** (SPDX lines of `src/`, `lib/`, `client/`; `COPYING` is GPLv2) | `bluetooth/build-bluez.sh` |
| BlueALSA (bluez-alsa) | 4.3.1 (sha256-pinned tarball) | **MIT** (`LICENSE`; sources say "licensed under the terms of the MIT license") | `bluetooth/build-bluealsa.sh` |
| sbc (SBC codec library) | 2.0 (sha256-pinned tarball), linked statically into `bluealsa` | **LGPL-2.1-or-later** (library sources `sbc/*.c`, `sbc/*.h`: "version 2.1 of the License, or (at your option) any later version"; `COPYING.LIB` is LGPL 2.1). `COPYING` is GPLv2 and covers only `src/` tools, which are built `--disable-tools` | `bluetooth/build-bluealsa.sh` |
| libvterm | 0.3.3 (sha256-pinned tarball), linked statically into `terminal` | **MIT** (`LICENSE`: Copyright (c) 2008 Paul Evans) | `native_apps/build-libvterm.sh` |
| Linux kernel | 4.14.52 upstream; the image (never the modules) also applies `kernel/patches/` | **GPL-2.0-only** | `usb_host/build-kernel-modules.sh`; the tree lives at `usb_host/linux-4.14.52/` (gitignored) |
| glibc (on the card) | 2.41 (git snapshot `2.41-161-g5dd252cf`) — the card's `/lib/libc.so.6`, from Buildroot's internal toolchain, **soft-float EABI** (`arm-buildroot-linux-gnueabi`, loader `/lib/ld-linux.so.3`) | **LGPL-2.1-or-later** (library); GPL-2.0-or-later (programs), BSD-3-Clause, MIT parts (Buildroot `legal-info`) | `rootfs/build-rootfs.sh` |
| glibc (linked into our binaries) | the host cross toolchain's: `native_apps` build with `arm-linux-gnueabi-gcc` (soft-float, **dynamic** against the card's glibc); `vnc_client` and `devmem_write` are **static, hard-float** (`gnueabihf`) and so embed the host toolchain's glibc, **Ubuntu `2.31-0ubuntu9.9` [unverified]**: the version comes from the plan note the source offer was written from, not from a package query of the toolchain that built the binaries; `rootfs/make-source-offer.sh` pins the `.dsc` of that version (`UBUNTU_GLIBC_DSC_SHA256`) and ships it | LGPL-2.1-or-later | the distribution's cross toolchains |

⚠️ **These are cross-compiled from source on purpose, not vendored.** This host cannot do armhf
multiarch, so each component builds its own copies
([`SYSTEM_ANALYSIS.md` §6](SYSTEM_ANALYSIS.md#6-building-for-this-device)). The consequence for this file
is that *nothing here is redistributed as source by this repository* — the obligations attach to the
**binaries** a release publishes, which is the next section.

**The libretro emulator cores are built and never distributed.** `retro/build-cores.sh` builds each from a
pinned upstream commit, and `retro/build-and-deploy.sh` copies the `.so` files to a unit's data partition;
neither an image nor a release carries a core. Each core's licence, read at its pin, is in the
[candidate cores table](retro/README.md#3-content-and-licensing-policy-operator-rule): GPL, or a non-commercial
licence named there (MAME 2003-Plus, Snes9x, PicoDrive), which this non-commercial project accepts. **uae4arm is
taken as GPL-2.0-or-later, by assumption (operator, 2026-10-29).** The repository has no `COPYING`; 27 of its
source files at `26f444f` carry the GPL-2.0-or-later header (measured by grep) and the rest carry UAE copyright
lines with no grant, which we read as parts of the same GPL work. Its bundled `capsimg` (non-commercial,
"not free software") is not built: the build passes `CAPS=` empty. **Fuse embeds the Spectrum 48/128 ROMs, whose
redistribution grant (Amstrad, 1999) names emulator writers only; Debian files the ROM package in `non-free`. The
status is UNCLEAR (read 2026-10-30), so Spectrum stays out of every image and release** (see the table row).
**ep128emu-core embeds the Enterprise EXOS, IS-BASIC and EXDOS ROMs, and the ZX Spectrum 48/128, Amstrad CPC and
Videoton TVC ROMs too** (`roms/roms.hpp` at its pin, measured by grep); the file's only licence note is the Amstrad
one above. The Enterprise ROMs were written for Intelligent Software Ltd, which collapsed with Enterprise Computers
by 1986; no current rights holder and no redistribution grant was found (web search, 2026-10-30, partial: the
Enterprise Forever forum refused the fetch, and Companies House online lists no 1980s company of that name). **Their
status is UNCLEAR, so the Enterprise stays out of every image and release** too. Both systems stay in the source and
the test build (operator, 2026-10-30).

### The root filesystem (`rootfs/`, Buildroot 2025.02.18)

Every package in the card's p3 comes from one Buildroot build (`rootfs/configs/arca_defconfig`,
pinned and sha256-checked by `rootfs/build-rootfs.sh`). **Measured 2026-10-10** by running Buildroot's
`make -k legal-info` over that tree in WSL: its `manifest.csv` lists the 25 target packages below, and
the licence strings are Buildroot's own `<PKG>_LICENSE` for that version, not ours. Buildroot itself
(GPL-2.0-or-later) and the host build tools are not shipped. Shared libraries pulled in as dependencies
are on the card too, so they have rows.

| Package | Version | Licence (per Buildroot) |
|---|---|---|
| glibc | 2.41 | see the glibc row above |
| gcc runtime (`libgcc_s`, `libstdc++`) | 13.4.0 | **GPL-3.0-or-later with the GCC Runtime Library Exception** |
| BusyBox | 1.37.0 | **GPL-2.0-only**; bzip2-1.0.6 for the bundled bzip2 code |
| bash | 5.2.37 | **GPL-3.0-or-later** |
| readline | 8.2 | **GPL-3.0-or-later** |
| OpenSSH | 9.9p2 | BSD-3-Clause, BSD-2-Clause, public domain |
| OpenSSL (`libopenssl`) | 3.5.8 | Apache-2.0 |
| dbus | 1.14.10 | AFL-2.1 **or** GPL-2.0-or-later (library and tools) |
| avahi | 0.8 | **LGPL-2.1-or-later** |
| libdaemon | 0.14 | **LGPL-2.1-or-later** |
| alsa-lib | 1.2.13 | **LGPL-2.1-or-later** (library); GPL-2.0-or-later (`aserver`) |
| alsa-utils (`aplay`, `amixer`, `alsactl`) | 1.2.13 | **GPL-2.0-only** |
| GLib (`libglib2`) | 2.82.5 | **LGPL-2.1-or-later** |
| libffi | 3.4.6 | MIT |
| PCRE2 | 10.46 | BSD-3-Clause |
| expat | 2.8.4 | MIT |
| zlib (`libzlib`) | 1.3.2 | zlib licence |
| ncurses | 6.4-20230603 | MIT (with the advertising clause) |
| nano | 8.2 | **GPL-3.0-or-later** (a separate program, `/usr/bin/nano`, linked to ncurses only; the Files app starts it through the Terminal app) |
| eudev | 3.2.14 | **GPL-2.0-or-later** (programs), **LGPL-2.1-or-later** (libraries) |
| kmod | 33 | **LGPL-2.1-or-later** (library); the tools are GPL-2.0-or-later (unverified: the manifest names only the library) |
| libxcrypt | 4.4.38 | **LGPL-2.1-or-later** |
| util-linux (+ `util-linux-libs`) | 2.40.4 | **GPL-2.0-or-later**, BSD-4-Clause, LGPL-2.1-or-later (libblkid, libfdisk, libmount), BSD-3-Clause (libuuid), BSD-2-Clause, ISC, MIT |
| ethtool | 6.14 | **GPL-2.0-only** |
| tzdata | 2026b | public domain |

The Linux kernel headers (4.14.52, GPL-2.0 with the syscall exception) are build-time only. ⚠️ Buildroot's
`make legal-info` aborts on them with the stock hash file, which carries the 6.x trees' hashes: a hash file
under `BR2_GLOBAL_PATCH_DIR` does not fix it (it is checked together with the stock one), so
`rootfs/external.mk` overrides `LINUX_HEADERS_HASH_FILES` and `LINUX_HEADERS_LICENSE_FILES` with
`rootfs/package-hashes/linux-headers.hash`, and `rootfs/build-rootfs.sh` runs `legal-info` and fails with it
(measured: the pre-change tree exits 2). **Unverified:** that the manifest is otherwise complete, and the
copyright texts of each package (Buildroot's `licenses/` directory holds them; nothing here copies that
directory onto the card yet).

---

## Distributed binaries, and the obligations they carry

A release bundle (`./release.sh`) publishes ARM binaries. Its `NOTICE` file — generated by `release.sh`
and shipped inside every tarball — is the per-release half of this; **this file is the repo-level half,
and the two must agree.**

⚠️ **Every source obligation here is discharged by *availability*, not by correspondence.** The
repository these releases are published from is public, and the kernel the three modules are built
against is unmodified upstream from kernel.org — so the written offer in `NOTICE` never has to be
exercised: the source sits at the same address as the binaries. That rests on one condition, which is
therefore a licence condition and not a preference: **the repository must stay public for as long as any
release published from it is available**, because the offer is honoured for the lifetime of the release.

| Published artifact | Effective licence of the binary | Obligation |
|---|---|---|
| `scummvm` | **GPL-3.0-or-later** | Corresponding source: upstream ScummVM plus `scummvm-arca/backend-files/`. Written offer in `NOTICE`. |
| `scummremastered.zip`, `gui-icons.dat` | **GPL-3.0-or-later** | ScummVM's own theme and GUI data, staged verbatim from the ScummVM tree. Same offer. |
| `vkeybd_arca.zip` | **GPL-3.0-or-later** | Derivative of ScummVM's `vkeybd_small.zip` (see above). Same offer. |
| `vnc_client` | **GPL-2.0-or-later** | Links LibVNCClient. Corresponding source: LibVNCServer 0.9.14, zlib, libjpeg-turbo, plus this repository. |
| `xpad.ko`, `joydev.ko`, `ff-memless.ko`, `usb-storage.ko` | **GPL-2.0-only** | Kernel modules. ⚠️ **Written source offer required**, and it is in `NOTICE`: unmodified upstream Linux 4.14.52 from kernel.org, plus `usb_host/build-kernel-modules.sh` (the storage module: `kernel/build-bt-modules.sh --set storage`) and the device's own kernel configuration. No kernel source is modified — these are upstream drivers rebuilt for this kernel's config. |
| `bluetoothd`, `bluetoothctl`, `libbluetooth.so.3` | **GPL-2.0-or-later** | BlueZ 5.66. Written offer in `NOTICE`: the upstream tarball plus `bluetooth/build-bluez.sh`. They link the device's own glib, dbus and readline dynamically. |
| `bluealsa`, `bluealsa-aplay`, `libasound_module_{pcm,ctl}_bluealsa.so` | **MIT** (BlueALSA 4.3.1); `bluealsa` additionally contains **LGPL-2.1-or-later** sbc 2.0 | Only `bluealsa` links sbc (`libsbc.a`, static) — measured by `grep -a` for `sbc_encode`/`sbc_decode`: present in `bluealsa`, absent from `bluealsa-aplay` and both plugins. LGPL-2.1 §6 relinking: the sbc tarball, BlueALSA's tarball and `bluetooth/build-bluealsa.sh` reproduce the binary and a relink. Written offer in `NOTICE`; BlueALSA's MIT copyright notice is reproduced there too. |
| the Bluetooth `.ko`s under `/lib/modules/4.14.52/bt` | **GPL-2.0-only** | Kernel modules. ⚠️ **Written source offer required**, in `NOTICE`: upstream Linux 4.14.52 plus `kernel/patches/`, `kernel/patches-modules/` (which modify `btusb.c` and `btrtl.c`) and `kernel/build-bt-modules.sh`. |
| our `uImage-system` (`kernel/build-image.sh`), `cy8ctmg120_ts.ko`, `ca8_ibe.ko` | **GPL-2.0-only** | Not in a bundle; they enter a card image (`make-card-image.sh`). Source: upstream 4.14.52 + `kernel/patches/`, `kernel/dts/`, `kernel/drivers/`, `kernel/config-changes` and the base config `usb_host/device_config` (committed). The offer tar below carries the source; ⚠️ **no written offer text ships with an image yet** (see the gaps below). |
| `rtl8761cu_{fw,config}.bin` | **Realtek redistributable firmware licence** (row above) | Staged unmodified with `LICENCE.rtlwifi_firmware.txt` beside them on the device; the notice is reproduced in `NOTICE`. |
| `devmem_write`, the `native_apps` games and tools, `app_launcher` | MIT. `devmem_write` is statically linked against glibc (LGPL-2.1-or-later); the `native_apps` binaries link the device's own glibc and `libasound` dynamically; `terminal` also links libvterm (MIT) statically, its copyright notice reproduced in `NOTICE` | LGPL-2.1 §6 relinking: the objects and this repository are what a relink needs. |
| `/opt/sound/fx_*.wav` (eleven sound effects) | **Not for commercial use** + **attribution to ElevenLabs (`elevenlabs.io`) required** (free account — see above) | ⚠️ **The one artifact that makes a bundle non-commercial.** A commercial redistribution must remove or replace them; every game falls back to its note tables without them, so removal breaks nothing. Stated in `NOTICE`. |
| `/opt/sound/<stem><n>-mono.wav` (every music bed) | Royalty-free, commercial use permitted (musely.ai — see above) | None. No attribution required. Not this project's composition; stated in `NOTICE` anyway so a bundle carries its own provenance, along with the fact that removing any bed is safe. |

### The source-offer duty for a distributed card image, and where this repository stands

Publishing a **card image** (`rootfs/make-card-image.sh`), as opposed to the bundle above, distributes the
whole root filesystem and the kernel modules in it. The duty, stated as the licences word it and not as
legal advice: whoever distributes **GPL-2.0** binaries (kernel, BusyBox, alsa-utils, ethtool, eudev,
util-linux) must pass on the complete corresponding source, or a written offer for it (GPL-2.0 §3);
**GPL-3.0** programs (bash, readline, the GCC runtime pieces under their exception, ScummVM) carry the
equivalent §6; **LGPL-2.1** libraries (glibc, avahi, alsa-lib, GLib, kmod, libxcrypt, libdaemon, sbc)
additionally require that the recipient be able to relink — shared libraries on the card satisfy that by
construction, static ones need the objects (see the `devmem_write` row). Licence texts and notices must
travel with the binaries (BSD, MIT, Apache-2.0, zlib and the OpenSSH family require the notice).

What the repository **does** today: the kernel image's source is upstream 4.14.52 plus the committed
`kernel/patches/`, `kernel/dts/`, `kernel/config-changes` and the base config `usb_host/device_config`, built by
`kernel/build-image.sh`; the bundle's `NOTICE` carries written offers for the bundle's own binaries (rows
above); `release.sh` refuses to tag what is not on the public remote. **The GPL-2.0 §3(a) mechanism for a card
image is `rootfs/make-source-offer.sh`**: it builds, outside the repo, one tar holding the source of everything
on the image (Buildroot's `legal-info` sources and licences, the pinned kernel tarball, U-Boot at its pinned
commit, ScummVM at `scummvm-arca/upstream-commit`, the `vnc_client` dependency tarballs, the BlueZ family, the
Ubuntu glibc source behind the static binaries, and this repository), each against the sha256 or commit its
build script pins. `rootfs/check-source-offer.sh` verifies the tar and maps every ELF, `.ko` and p1 file on the
image to a source row of `rootfs/source-map.tsv`; `tests/arca_source_offer_test.sh` is its regression. Built
once as a development offer and checked (PASS, no unmapped program).

**Gaps, stated plainly:**

- **The offer ships through `release.sh --source-offer`**, which runs `make-source-offer.sh` under the release's tag and attaches the tar and its `.sha256`;
  `NOTICE` names that source asset **only when `--source-offer` is given** (otherwise it says the bundle was staged without a source file). The image carries
  its own notices: `make-card-image.sh` puts this file, Buildroot's `manifest.csv` and `licenses/`, and `SOURCE-OFFER.txt` in p3 `/usr/share/doc/arca-licenses/`.
  Whether pointing at upstream's mirrors instead would suffice is **unverified** and not assumed.
- **The kernel tarball is not checked before the kernel build.** `kernel/build-image.sh` and
  `usb_host/build-kernel-modules.sh` carry no sha256 for `usb_host/linux-4.14.52.tar.xz`; only Buildroot and the
  offer check it, against `rootfs/package-hashes/linux-headers.hash`.

The three `.ko`s are the reason the choice of licence for this project's own code mattered at all: a
GPL-2.0-**only** binary in the same distribution is what rules Apache-2.0 out.

⚠️ **The vendor's `uImage-system` may never be published, patched or not, and "it is only one byte
different" does not change that.** It is Steelcase's build of Linux, so it is **GPL-2.0-only** and
redistributing the binary obliges **Steelcase's** corresponding source, which this project does not have
and has ruled out requesting
([`SYSTEM_ANALYSIS.md` §7](SYSTEM_ANALYSIS.md#7-kernel-policy)). ⚠️ **That bar is a licence obligation,
not this project's build policy, and nothing about building a kernel here lifts it** — an image compiled
from the vanilla tree would carry its own corresponding source and could ship; the vendor's cannot,
whatever else is decided about kernels. The obligation cannot be met, so the
image cannot ship: not as a release asset, not as a tarball, not as a bundle file. This is already
enforced rather than merely intended — `usb_host/build-and-deploy.sh` stages the `usb` group *without* it
and says why, and `.gitignore` keeps whole-card dumps and their extracted partition trees out of the repo.
The consequence for anyone meeting an unrecognised firmware release is that the patch has to be **derived
on the operator's own card** — the p1 gate knows one firmware release and refuses every other, and
publishing a prebuilt one is not an available shortcut.

---

## No warranty, and one hardware note

The MIT text above disclaims warranty, and that disclaimer is meant literally here. Two specifics:

- **Imaging a card replaces everything on it, and there is no in-place rollback.** Keep the card
  image you wrote; `provision.sh` exports a backup archive before it
  changes a running unit. Neither writes the boot partition.
- **Recovering a unit that will not boot means reaching the SD card, and that means opening the case.**
  It is feasible and it takes experience; an inexperienced attempt can break the enclosure. Nobody
  associated with this project is responsible for a broken case, a broken card or a bricked unit.

Steelcase and RoomWizard are trademarks of their respective owner. This project is unaffiliated with,
and not endorsed by, Steelcase.
