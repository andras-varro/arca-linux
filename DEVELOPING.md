# Developing Ludbox Linux

> **Developer guide: how to build, deploy and release Ludbox Linux (codename Arca) for the Steelcase RoomWizard.** If you only want to flash a card and use the unit, read [README.md](README.md) instead.

Arca is the project's codename; you will see it in paths and file names.

## Documentation map

| Doc | What it owns | Read it when |
|---|---|---|
| [README.md](README.md) | The user guide: flash a card, use the apps, configure, troubleshoot | You own a unit and want to use it |
| [CLAUDE.md](CLAUDE.md) | How to build, deploy, and not break things. The trap list. | Before writing any code |
| [SYSTEM_ANALYSIS.md](SYSTEM_ANALYSIS.md) | Authoritative device facts: SoC, boot chain, display, audio, GPIO, unused hardware | Before touching anything hardware-related |
| [HARDWARE.md](HARDWARE.md) | The board itself: parts, connectors, headers, the unpopulated XBee socket, the enclosure — with the teardown photos | Opening a unit, or probing a header |
| [IMPROVEMENT_PLAN.md](IMPROVEMENT_PLAN.md) | The single backlog — bugs and features with `file:line` | Before starting work, so you don't rediscover a known bug |

Each component directory also has a `CLAUDE.md` (authoring guidance for that component) and a
`README.md` (what it is and how to use it):
[native_apps](native_apps/CLAUDE.md) · [scummvm-arca](scummvm-arca/CLAUDE.md) ·
[vnc_client](vnc_client/CLAUDE.md).

**The rule:** component docs describe their own code only. Device facts belong in
SYSTEM_ANALYSIS.md, open work belongs in IMPROVEMENT_PLAN.md. Link, don't copy.

### The device in one line

TI **OMAP3503** (ARM Cortex-A8 @ 600 MHz, **no GPU, no DSP**), 234 MB RAM, 800×480 LCD via
legacy `omapfb`/`omapdss` (no DRM/KMS), projected-capacitive touch, Linux 4.14.52, SysVinit.
All code is cross-compiled on the dev host and deployed over SSH — there is no local app to run.

### Projects
- **[Native Apps](native_apps/)** — High-performance C apps with direct framebuffer rendering
- **[Browser Games](browser_games/)** — HTML5 games with LED control
- **[ScummVM Backend](scummvm-arca/)** — Classic adventure games port
- **[VNC Client](vnc_client/)** — Remote desktop viewer for Raspberry Pi displays

---

## Quick Start

> **Cloning:** the teardown photos in [HardwarePhotos/](HardwarePhotos/) are stored in
> **Git LFS**. Run `git lfs install` once before cloning, or those files arrive as
> one-line pointer stubs (`git lfs pull` fixes an existing clone). Nothing in the
> build or deploy path depends on them — they are documentation only.

### Start here — `./arca.sh`

One menu over every path below. A card image written once produces a working unit at next
boot. It has **no logic of its own**: each item execs one of the scripts documented further down, and
every one of those stays non-interactive when called directly.

```bash
./arca.sh
```

```
  2) Update a booted device      ssh; backup, update, reboot
  3) Deploy apps                 ssh; source, bundle or release
  5) First boot of an imaged card  boot, wait for ssh, deploy

  6) Build a card image          offline; our root + bundle, one boot (deliver a unit)

  4) Device status               read-only
  7) Host build prerequisites    this machine; no device, no card
```

Which item you want depends on whether you are **delivering** a unit (item 6, then 5) or **developing**
on one (items 2 and 3); they differ in how many boots and how much toolchain they need.

### Before anything builds — `./setup-build-env.sh`

The one home for the **host** package set: the cross toolchain, `build-essential`, `cmake`, `python3`
with `PIL` for framebuffer decodes, the kernel-module deps `usb_host` needs, and `shellcheck`. It probes
what is missing, prints the exact `apt` line before running it, and installs only on `--install-deps` or
a TTY confirmation. `--scummvm` also clones the upstream ScummVM tree, which is gitignored and so is
absent from a fresh clone. ⚠️ **Run it from WSL** — every tool it looks for is absent from Git Bash, so
it refuses to run there rather than report a host that cannot build anything. The *cross-compiled*
dependencies are not its job and install themselves.

Measured in this project's WSL (2026-09-06; the cross-compilers re-measured 2026-09-29): `shellcheck` 0.7.0, `gh`, `sfdisk`, `cmake`, `bc`, `bison`,
`flex`, `git-lfs`, `python3` with `PIL` 10.4.0, and both cross-compilers, `arm-linux-gnueabi-*` (soft-float, dynamic: native apps and ScummVM) and
`arm-linux-gnueabihf-*` (`vnc_client`), 9.4.0 each. None of it is in Git Bash (`CLAUDE.md`, *Working from this host*).

```bash
./setup-build-env.sh --install-deps --scummvm
```

### Before anything deploys — `./tests/run-all.sh`

The host gate: every test that needs no device, in one command. It runs the shell suites in `tests/`, the
host-gcc regressions under `native_apps/tests/`, and `shellcheck` over every tracked script. `deploy-all.sh` and
`release.sh` **run it first and refuse to build if it fails**; `--skip-tests` on either is the override. `--list`
shows what it will run, `--self-test` runs its own negative controls, and exit **2** means it could not judge rather
than that a test failed. One suite needs root and reports itself SKIPPED without it, which the summary names — a skip
is never counted as a pass. Authoring rules: `tests/CLAUDE.md`.

The device half: `native_apps/smoke-first-screen.sh <ip> [binary ...]` (`--list`, `--out DIR`) stops the app loop, launches each
native app, and gives its first frame one verdict (pass / did-not-start / started-died / black-screen / could-not-tell).

```bash
./tests/run-all.sh                  # everything
./tests/run-all.sh --scope=deploy   # skip the documentation checks
```

### Delivering a unit — build a card image, one boot

A card is one whole-card image built by `rootfs/make-card-image.sh` (root, WSL): our boot files on p1 (`MLO`,
`u-boot.img` and the kernel), our root filesystem, the kernel modules and — with `--bundle` — every component, installed by
`rootfs/commission-offline.sh`. The unit works at first boot and never needs to be reachable.

```bash
sudo ./rootfs/make-card-image.sh --mlo <MLO> --uboot <u-boot.img> --kernel <uImage> --bundle <tar.gz|dir> <rootfs.tar> <out.img>
```

`MLO` and `u-boot.img` come from `uboot/build.sh`, `<uImage>` from `kernel/build-image.sh`, the kernel modules from
`kernel/build-modules.sh --out <dir>` (`--modules`; `--help` has the default), `<rootfs.tar>` from `rootfs/build-rootfs.sh`. The partition table is
generated at fixed positions, so no donor card and no vendor file is an input; write the finished image to the card from Windows. Make a bundle with
`./release.sh --stage-only`, or publish one with `./release.sh --tag <tag> --source-offer`. A larger card needs nothing extra: `S07growroot`
grows p3 to the end of the card at the first boot.

Every image is the same and carries no unit state: the unit names itself (`arca-<4 hex>`) and generates its SSH host key at the first boot, after
which `./deploy-all.sh <ip>` works (or the menu item *First boot of an imaged card*). ⚠️ **After reflashing a card, remove the unit's old host key in
both shells** (Git Bash and WSL keep separate `known_hosts`): `ssh-keygen -R <ip>` and `wsl.exe -e bash -lc "ssh-keygen -R <ip>"`. ⚠️ **A reflash also
wipes `/home/root/data`** (settings, scores, touch calibration, ScummVM saves) — `./backup.sh <ip>` first (below).

A publish is refused without its source offer: `./release.sh --tag <tag> --source-offer [--image <img>]... [--source-out <dir>]` publishes the bundle,
`ludbox-source-<tag>.tar`, its `.sha256` and the images under one tag. `--image` needs it too. The offer (`rootfs/make-source-offer.sh`) is
assembled after the host gate and before the component builds; `--stage-only --source-offer` makes a development offer (`--allow-dev`: dirty tree or unpushed commit accepted; the name gets `-dev` only on a dirty tree).

### Installing from a published release — no cross-compiler

Every other mode builds, so every other mode needs the ARM cross-compilers. Someone handed a device
has no toolchain, so a published release is installable directly over SSH:

```bash
./deploy-all.sh --from-release latest <ip>       # over SSH, to a set-up unit
./deploy-all.sh --from-release v1.0.0 <ip>       # a specific tag
```

The tarball's sha256 is checked against the digest GitHub publishes, and it is cached under
`build/release-cache/<tag>/`, so a second unit costs no second download. Digest mismatch refuses and
deletes the partial file. `v1.0.0` is the first published release (~125 MiB, one asset); the fetch
rules are in [lib/CLAUDE.md](lib/CLAUDE.md).

`--from-bundle <tar.gz|dir>` is the same install with a local bundle instead of a download.

### Getting SSH access to a generic card

A generic card carries no key and refuses SSH until the panel opens it; the steps are the user's, in [README.md → *Reaching the unit over SSH*](README.md#5-reaching-the-unit-over-ssh-optional) (measured on `.188`, 2026-10-11, including `KEY ONLY` after `ssh-copy-id` and `OFF` across a reboot).

### Developing on a unit — update, then deploy

This is the build/deploy loop, and what you want while writing code. The unit already runs our own root
(from an imaged card, above) and is reachable by key-only SSH.

```bash
# Update the system setup: backs the unit's per-unit state up first, installs, reboots
./provision.sh <ip>

# Build + deploy everything (native_apps first — it provides the launcher)
./deploy-all.sh <ip>
./deploy-all.sh <ip> vnc_client      # or one component;  --list  to see them
```

⚠️ **Only our own kernel image is supported**; installing it is a manual step ([kernel/README.md](kernel/README.md)),
and no script writes p1. `provision.sh` deletes no software. Its other flags:

```bash
./provision.sh <ip> --dry-run        # print the plan; change nothing
./provision.sh <ip> --no-usb         # no USB host mode at all
./provision.sh <ip> --status         # report current state; changes nothing
```

`./backup.sh <ip> [<out.tar.gz>]` copies the unit's per-unit state (host keys,
highscores, VNC config, ScummVM config and saves, calibration) to `backups/`; it is read-only on the unit and the archive holds
secrets. `provision.sh` runs it first. Nothing writes the archive back; it is a plain tarball to unpack by hand.

## Architecture

```
arca-linux/
├── arca.sh                      # Front door: a menu over everything below
├── deploy-all.sh                # Build + deploy all components
├── release.sh                   # Build + stage a bundle; --tag also publishes it
├── rootfs/                      # Our own root filesystem: Buildroot tree, make-card-image.sh (whole-card image), commission-offline.sh (its bundle installer), the set-hostname overlay file
├── provision.sh                 # Update a unit over SSH: backup, install the plan, reboot
├── backup.sh                    # A unit's per-unit state to a host tarball (read-only on the unit)
├── lib/                         # Sourced, never executed
│   ├── arca-identify.sh         # Is this our root; which partition, by content/position (root, log)
│   ├── arca-provision.sh        # provision-rules.conf -> a plan
│   ├── arca-ssh.sh              # The one "can I reach this device" gate
│   ├── arca-release.sh          # Resolve + fetch + verify a published release
│   └── arca-bundle.sh           # The release-bundle layout, both directions
├── device-files/                # Installed onto the device verbatim
│   ├── arca-app                 # App respawn loop; arcamond = init for the history daemon
│   ├── enable-usb-host.sh       # The /dev/mem MUSB host-mode patch; usb-host runs it (S90)
│   ├── xpad-modules             # insmod -f the three controller modules (S89)
│   └── provision-rules.conf     # What the device ends up with
├── LICENSE.md                   # MIT, plus the third-party enumeration
├── SYSTEM_ANALYSIS.md           # Hardware analysis
├── native_apps/                 # C apps (games, launcher, tools)
├── browser_games/               # HTML5 games + LED control
├── scummvm-arca/                # ScummVM backend
├── usb_host/                    # USB host mode + Xbox controller modules
├── bluetooth/                   # BlueZ + btmon, BT modules (incl. rfcomm), RTL8761CU firmware, boot start; BlueALSA (a2dp-source, hfp-ag)
├── uboot/                       # Our own mainline U-Boot v2026.10 board port (rw20-board.patch), build.sh and BUILD.md; host build only
└── vnc_client/                  # VNC remote desktop viewer
```

### Separation of Concerns

| Layer | Script | Runs |
|-------|--------|------|
| **Front door** | `arca.sh` | Whenever you'd rather not remember the flags (composition only) |
| **Card image** | `rootfs/make-card-image.sh` | Once per card (offline, WSL root); `--bundle` runs `rootfs/commission-offline.sh` over it |
| **Update a unit** | `provision.sh` | Per update (SSH; backs up first, reboots) |
| **Deploy all** | `deploy-all.sh` | After setup (builds + deploys everything) |
| **Build + stage + publish** | `release.sh` | Per release |
| **App launcher** | `device-files/arca-app` | Every boot (respawn loop, reads `/opt/arca/default-app`) |
| **Project deploy** | `*/build-and-deploy.sh` | Per project (build + deploy + app manifests) |

Each project's `build-and-deploy.sh` handles building, deploying binaries, and installing
`.app` manifests to `/opt/arca/apps/` for the visual launcher.
System setup is done once by `provision.sh` — no duplication across projects.

## Projects

1. **Native Apps** — Direct framebuffer C apps (Snake, Tetris, Pong, App Launcher, Hardware Test). USB input fully supported: keyboard, mouse, and Xbox 360 controller via unified evdev polling.
2. **Browser Games** — HTML5 brick breaker with LED feedback
3. **ScummVM** — Custom backend for classic point-and-click adventures (OPL/AdLib music, touch controls, virtual keyboard). Independent evdev input: keyboard, mouse, and gamepad.
4. **VNC Client** — Lightweight VNC viewer for Raspberry Pi remote desktop (weather/clock) with keyboard/mouse forwarding (~5% CPU, bilinear scaling)

The **App Launcher** is a visual grid shell deployed by `native_apps/build-and-deploy.sh`.
It scans manifest files from all projects and displays them as touch-friendly icon tiles.
The init script respawns it automatically when an app exits. Keyboard and Xbox pad drive the launcher and the Control Panel (arrows, Enter/Space, Esc/Backspace back); Up from a top row reaches the exit X. The Input page's keyboard, pad and mouse testers exit by holding Esc / pad Select or Start / both mouse buttons for 1.5 s.

For hardware specs, see **[Subsystems](SYSTEM_ANALYSIS.md#3-subsystems)** in the device reference.


---

## Project Status

See **[IMPROVEMENT_PLAN.md](IMPROVEMENT_PLAN.md)** for the current backlog. Highlights:

- Known bugs are catalogued there with `file:line` references.
- **We build our own 4.14.52 image; a mainline port stays out of scope** — built from the vanilla tree
  as it stands the image has no built-in touch driver (it ships as a loadable `.ko`; `kernel/README.md`), and a mainline port would additionally break the
  runtime bpp switching ScummVM and the VNC client depend on. See [Kernel policy](SYSTEM_ANALYSIS.md#7-kernel-policy).

## A note on secrets

`vnc_client/vnc_client.conf` is **gitignored** because it holds a plaintext VNC password.
Copy the template to create your own:

```bash
cp vnc_client/vnc_client.conf.example vnc_client/vnc_client.conf
# then edit host/password
```

## Licence, and one no-warranty note

This project's own code is **MIT**. Two things it distributes are not, and both carry obligations that
MIT does not — the `scummvm` binary and its data (GPL-3.0-or-later), and the three controller kernel
modules (GPL-2.0-only, written source offer). Everything is enumerated in
[LICENSE.md](LICENSE.md), which is also where MIT's *scope* is stated: it governs the source and does not
decide the licence of a binary it is linked into.

⚠️ **No warranty, meant literally.** Our kernel replaces the vendor's on the boot partition, and
recovering a unit that will not boot means reaching the card — which means opening the case. That is
feasible and it takes experience; an inexperienced attempt can break the enclosure.

Steelcase and RoomWizard are trademarks of their respective owner. This project is unaffiliated with,
and not endorsed by, Steelcase.
