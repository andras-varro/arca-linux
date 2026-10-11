# RoomWizard Commissioning

How a RoomWizard gets our software, and how it stays up to date. A unit runs **only our own root
filesystem** (the Buildroot tree in [`rootfs/`](rootfs/CLAUDE.md), marker `/etc/roomwizard-rootfs`);
nothing here commissions or cleans the Steelcase one. Anyone holding a stock unit can check out the tag
`last-vendor-rootfs`, the last tree that could.

## Overview

Two paths, and a third step that follows either:

| Path | Script | Connection | When |
|---|---|---|---|
| **Image a card** | `rootfs/make-card-image.sh` (`--bundle` runs `commissioning/commission-offline.sh`) | Offline, on the dev host | Delivering a unit, or starting one from nothing |
| **Update a unit** | `commissioning/provision.sh <ip>` | SSH | A unit already running our root |
| **Deploy apps** | `deploy-all.sh` | SSH | Per deploy |

**A menu over all of it is [`roomwizard.sh`](roomwizard.sh)**, which implements nothing of its own and
shells out to the scripts above: build a card image, update a unit, back up, restore, deploy, and *First
boot of an imaged card* (boot, wait for SSH, deploy). It polls **SSH, not ping**, because ping answers
while `sshd` is still starting. Every script stays callable directly and non-interactively; `--help` on
each is current.

## Imaging a card

The delivery path. One image, written to a card, one boot; no network and no reachable device are needed
to produce it.

```bash
./release.sh --stage-only                                    # build host, once: build/release
rootfs/make-card-image.sh --bundle build/release <partsdir> <rootfs.tar> <out.img>   # as root, in WSL
```

`<partsdir>` comes from `rootfs/fetch-card-parts.sh`, `<rootfs.tar>` from the Buildroot build, and the
kernel modules from `kernel/build-modules.sh --out <dir>`; inputs, geometry and why the image is built on
the native WSL filesystem are in [`rootfs/CLAUDE.md`](rootfs/CLAUDE.md) and the script header. Write the
finished image to the card from Windows.

**A card with no unit state:** `rootfs/make-card-image.sh --generic` (see `--help`) reads no `state.tar`, refuses any SSH key
material, and the unit names itself on first boot. Run as root, the module directory defaults to `~/rw-kmods` of the invoking user
(`$SUDO_USER` under `sudo`); a root shell without `SUDO_USER` (`wsl.exe -u root`) looks in root's own home (read from the script, not run), so pass it. The recipe used on `.188`:

```bash
bash rootfs/build-rootfs.sh; bash release.sh --stage-only --skip-tests      # WSL, as the normal user
rootfs/make-card-image.sh --generic --modules /home/z001x91h/rw-kmods --bundle build/release <partsdir> ~/br-rw-out/rootfs.tar <out.img>   # as root
```

⚠️ **After reflashing a card, remove the unit's old host key in both shells** (the keys live on p2, which an image creates empty, so
the unit gets new ones; Git Bash and WSL keep separate `known_hosts`). A bundle-based update normally avoids reflashing.

```bash
ssh-keygen -R <ip>                                  # in Git Bash
wsl.exe -e bash -lc "ssh-keygen -R <ip>"            # the same, in WSL
```

`--bundle` mounts p6, p2 and p3 as `<base>/{root,data,log}` and runs
[`commissioning/commission-offline.sh`](commissioning/commission-offline.sh) over them. It asks nothing,
cleans nothing and mounts nothing else: the boot scripts and links from
[`device-files/provision-rules.conf`](device-files/provision-rules.conf) and every component's files go in,
and the script's own verify pass runs before you boot the card. `--dry-run` resolves and prints every
action without writing.

**What it verifies on the image.** md5 of every installed file against the bundle manifest; `+x` on
everything that needs it (a real measurement on ext4, impossible on `/mnt/c`);
`native_apps/check-arm-safe.sh` over the downloaded binaries; every `.app`'s `exec=` and `icon=`, and that
`default-app` names one of them; `dash -n` on every `/bin/sh` script it wrote.

p1 holds `mlo`, `u-boot.bin`, `ctrlblock.bin` and `uImage-system` and is never mounted or written by the
bring-up scripts; our own kernel image is installed there by hand ([`kernel/README.md`](kernel/README.md)).

Regression: [`tests/commission_offline_test.sh`](tests/commission_offline_test.sh) — every check with a
sabotage case; needs root and a staged bundle.

### First boot

Connect Ethernet and power on. SSH answers after roughly half a minute, and the unit also answers to
`<hostname>.local` once mDNS is up. sshd policy is the overlay's
`rootfs/board/roomwizard/overlay/etc/ssh/sshd_config` (key-only), the only one there is. SSH host keys are
generated on first boot, so a fresh p2 means a slow first boot. Then deploy: `./deploy-all.sh <ip>`, or the
menu item *First boot of an imaged card*.

A card larger than the image's fixed geometry needs p6 grown:
[`commissioning/clone-to-32gb.sh`](commissioning/clone-to-32gb.sh) (`--help` lists its modes and guards).

## Finding the card

⚠️ **A partition is identified by position and content, never by filesystem UUID.** A UUID is
generated at mkfs time, so it names one *card*, not a model: two RoomWizards running the identical
firmware build share **none** of their UUIDs. A hardcoded UUID therefore recognises only the unit its
constant was copied from and rejects every other one. Nothing on the device consumes a UUID either:
U-Boot passes `root=/dev/mmcblk0p6` and `/etc/fstab` names partitions by position.

[`lib/rw-identify.sh`](lib/rw-identify.sh) holds the two checks:

| Function | Question | How |
|---|---|---|
| `rw_is_rootfs` | is this mounted tree our RoomWizard root? | the marker file `/etc/roomwizard-rootfs`. No vendor root is recognised. |
| `rw_is_card_disk` | is this disk a RoomWizard card? | the partition table: start and size of the fixed partitions, which are byte-identical on every unit. The partitions that absorb the difference in physical card size are **not** pinned. |

Partition roles are root (p6), data (p2) and log (p3). p5 stays in the partition table, unused and never
mounted. `rw_offline_path` and `rw_offline_base_ok` map a device-absolute path onto the right mounted
partition and check that a `<base>` is usable. `/` is never a candidate: selecting it would rewrite this
host's own `/etc/shadow`.

Regression: [`tests/rw_identify_test.sh`](tests/rw_identify_test.sh) — host-only, no card, no root. It
builds synthetic rootfs trees and partition tables (`sfdisk` on sparse files), so the positive and
negative controls are self-contained.

```bash
./tests/rw_identify_test.sh
```

## Updating a unit

[`commissioning/provision.sh`](commissioning/provision.sh) is the **online** path for a unit that already
runs our root. It deletes no software and never writes p1.

Order: SSH gate, compile the plan, `--dry-run` prints it and exits, then
[`commissioning/backup.sh`](commissioning/backup.sh) (**the run aborts if it fails**), install the plan,
apply sysctl, reboot.

The plan is [`device-files/provision-rules.conf`](device-files/provision-rules.conf), the same data file
`commission-offline.sh` reads, so the online and offline passes cannot drift. Its verbs are `install`,
`link`, `link-opt` and `unlink`; stale `rc*.d` links from an earlier install are removed first.

```bash
./commissioning/provision.sh <target>                    # backup, install the plan, reboot
./commissioning/provision.sh <target> --dry-run          # print the plan; change nothing
./commissioning/provision.sh <target> --no-usb           # skip a group: mdns usb bluetooth
./commissioning/provision.sh <target> --status           # report only, no changes
./commissioning/provision.sh <target> --hostname rw09    # set the host name only. NO reboot.
```

`<target>` is an IPv4 address **or** a host name. `--status` also md5s the deployed scripts against the
repo's and reports `matches repo` or `DRIFTED` per file. Every retired flag is refused: `--ssh-auth`/`--sshd-only` by name, the rest as unknown options.

### The host name, and mDNS

Give each unit a **unique single label** (`rw09`, not `rw09.local` — mDNS appends `.local` itself). The
plan links `/etc/init.d/avahi-daemon` into `rc5.d`, so after the reboot the unit answers to
`<hostname>.local`:

```bash
ssh root@rw09.local
./commissioning/provision.sh rw09.local --status
```

[`commissioning/set-hostname.sh`](commissioning/set-hostname.sh) writes `/etc/hostname` **and**
`/etc/hosts` (loopback only: `127.0.0.1 localhost`, `127.0.0.1 <name>`), so anything on the device that
resolves its own name gets the right answer. `provision.sh --hostname NAME` is targeted and does **not**
reboot, which matters for a unit in service as a live display. Two units claiming one name make avahi
rename the second to `NAME-2.local`.

## Backup and restore

**Before an update or a reflash, back the unit up; after it, restore:**
`./commissioning/backup.sh <ip>` then, once the unit is back, `./commissioning/restore.sh <ip> <archive>`
(`--dry-run` lists what it would write). `provision.sh` runs the backup itself. The archive holds secrets
(VNC password, SSH host keys): keep it private. `rootfs/make-card-image.sh` refuses a `state.tar` made
before the image allowlist existed; re-run `rootfs/fetch-card-parts.sh` to make a current one.

⚠️ **Restoring a card from a whole-card capture:** a same-release restore by `dd` is measured working; the
restored unit inherits the donor's touch calibration and needs recalibrating. ⚠️ **Only p1 may be restored
file-by-file** (FAT32, regular files); an ext partition must go back with `dd` — a file copy of a live
rootfs carries no symlinks and leaves no `/bin/sh`, on hardware with no serial console.

## The dev host: what must be installed, and in which shell

Everything that compiles or decodes lives in **WSL**. The dev host is Windows, the repo lives on
`c:\work\roomwizard`, and WSL reaches the same tree at `/mnt/c/work/roomwizard` — so every build and
every framebuffer decode is invoked through WSL:

```bash
wsl.exe -e bash -lc "cd /mnt/c/work/roomwizard/<component> && ./build-and-deploy.sh <ip>"
```

**`./setup-build-env.sh` is the one place the host package set is written down.** It probes what is
missing, prints the exact `apt` line before running it, and installs only on `--install-deps` or a TTY
confirmation; `--scummvm` also clones the upstream ScummVM tree, which is gitignored and therefore absent
from a fresh clone. The component build scripts still check their own prerequisites — they are meant to
run standalone — but they report a missing tool and point here rather than each reciting a package list.

**Present in this WSL, all verified 2026-09-06:** `shellcheck` 0.7.0, `gh`, `sfdisk`, `cmake`, `bc`,
`bison`, `flex`, `git-lfs`, and `python3` with `PIL` 10.4.0, plus both cross-compilers — `arm-linux-gnueabi-*`
(soft-float, dynamic: native apps and ScummVM) and `arm-linux-gnueabihf-*` (`vnc_client`), each 9.4.0
(re-measured 2026-09-29).
ScummVM additionally needs WSL Ubuntu 20.04+.

⚠️ **None of it is in Git Bash** — not `gcc`, not either `arm-linux-*` toolchain, and not
`sfdisk`, `gh`, `shellcheck` or `strings` either. A `command -v` sweep run in that shell therefore
reports a host with no toolchain at all, and that reading has been mistaken for a hard blocker on all
building. **State which shell a prerequisite claim was measured in**, and measure with
`wsl.exe -e bash -lc`.

⚠️ **`command -v python3` succeeds in Git Bash and the interpreter does not exist.** It resolves to
the Windows App Execution Alias — a real file that prints *"Python was not found"* and fails. Test a
prerequisite by running it (`python3 --version`), not by looking it up.

### What the host cannot do for you

**Touch calibration is per-unit and always needs a boot.** The curve is a property of the panel in
front of you: it cannot be derived on the host, and it cannot be copied from another unit.

**One increment per boot.** Make one change, boot, look at the panel, and only then make the next
one. A failed boot yields no diagnostics at all — so if two changes went out together, nothing on the
unit can tell you which of them is responsible.
There is no serial console ([§3.12](SYSTEM_ANALYSIS.md#312-serial-ports)), so the only post-mortem is
mounting p3 offline and reading `messages` — which helps only if the boot got as far as syslog.

## Deploy apps

After imaging or updating, deploy apps to the device.

### All at once (recommended)
```bash
./deploy-all.sh <ip>              # build + deploy all components
./deploy-all.sh --list            # show discovered components
```

### Individually
```bash
cd native_apps       && ./build-and-deploy.sh <ip> set-default
cd vnc_client        && ./build-and-deploy.sh <ip>
cd scummvm-roomwizard && ./build-and-deploy.sh <ip>
```

The `set-default` flag makes that app start on boot.
After deploying, reboot: `ssh root@<ip> reboot`

### From a bundle, with no toolchain

⚠️ **Everything above BUILDS.** `deploy-all.sh <ip>` and every `build-and-deploy.sh` need
a cross-compiler (`arm-linux-gnueabi-gcc`; `-gnueabihf-` for `vnc_client`), and ScummVM needs WSL and
`arm-linux-gnueabi-g++` too. Someone who has been handed a device has none of that, which is what a release bundle is for:

```bash
./release.sh --stage-only                                  # on a build host, once
./deploy-all.sh --from-bundle build/release <ip>            # anywhere, no compiler
./deploy-all.sh --from-bundle roomwizard-<tag>.tar.gz <ip>  # or from the tarball
```

It stops the running app, installs, md5-verifies every file against the bundle's manifest, asserts `+x`
on every entry declared executable, sets `default-app` and restarts the launcher. Modes come from the
manifest, never from the transfer — see [`CLAUDE.md`](CLAUDE.md) → *Bundles*.

### USB host mode: it travels in the bundle and the provision plan

⚠️ **A bundle can deliver USB host mode**, by two independent mechanisms:

| Mechanism | Delivers | Lives on | Delivered by |
|---|---|---|---|
| `/dev/mem` patch of `omap2430_ops.dma_init`/`.dma_exit` + a MUSB rebind | **USB host mode itself** | nothing on disk — re-applied at every boot by `/etc/init.d/usb-host` | the `usb` group of [`device-files/provision-rules.conf`](device-files/provision-rules.conf) |
| `xpad.ko` / `joydev.ko` / `ff-memless.ko`, force-loaded | the controller as `/dev/input/event*` | `/lib/modules/4.14.52/extra` (p6) | the release bundle, md5-verified |

So a unit set up by either path — `commissioning/provision.sh <ip>` or
`commissioning/commission-offline.sh` — comes up with USB host mode by default. The controller power
budget is whatever our kernel's device tree says (`kernel/README.md`). The device scripts and the
`rc5.d` links are ordinary provision records; the built artifacts (`devmem_write` plus the `.ko`s) travel
in the bundle and add **no** `TAKEN ON TRUST` entries, because all of them are unstripped.

⚠️ **A bundle install alone does NOT give you USB.** `./deploy-all.sh --from-bundle <b> <ip>` installs
whatever the manifests name, so the artifacts arrive — but nothing installs the device scripts. Those come
from a provision plan. To add USB to a running unit from a machine with the ARM toolchain:

```bash
cd usb_host && ./build-and-deploy.sh <ip>      # needs bc libssl-dev bison flex python3
```

`--no-usb` opts out of the whole group on both paths.

### Switching Apps

To switch which app starts on boot, just set a different default:
```bash
# Switch to VNC client
ssh root@<ip> 'echo /opt/vnc_client/vnc_client > /opt/roomwizard/default-app'

# Switch to the native launcher
ssh root@<ip> 'echo /opt/roomwizard/app_launcher > /opt/roomwizard/default-app'

# Check current default
ssh root@<ip> 'cat /opt/roomwizard/default-app'
```

Then reboot or restart the service: `ssh root@<ip> /etc/init.d/roomwizard-app restart`

## Architecture

**One data file holds every install decision; the scripts are executors over it.** Neither the SSH pass
nor the offline pass decides what to install, which is what makes "the result is the same either way" a
fact rather than an intention.

```
device-files/provision-rules.conf  WHAT IS INSTALLED   <type> <group> <mode> <target> <source> <reason>

lib/rw-provision.sh   parses provision-rules.conf -> a plan, plus BOTH executors
lib/rw-bundle.sh      the bundle layout, plus the SSH bundle installer
lib/rw-identify.sh    which card, which partition — by content and POSITION, never by UUID

roomwizard.sh                          Front door: a menu over everything below
rootfs/make-card-image.sh              Whole-card image; --bundle runs the next one over it
commissioning/commission-offline.sh    The image step: install + verify, offline, no prompts
commissioning/provision.sh             Online update: backup, install, reboot
commissioning/backup.sh, restore.sh    Per-unit state off and back onto a unit
deploy-all.sh                          Build + deploy everything
deploy-all.sh --from-bundle            Install a release bundle with NO toolchain
release.sh                             Build all components + stage one offline bundle
device-files/roomwizard-app            Device payload: installed as /etc/init.d/roomwizard-app
device-files/{enable-usb-host.sh,usb-host,xpad-modules}   Device payload: the usb group
*/build-and-deploy.sh                  One per component
```

### On-device layout
```
/opt/roomwizard/
├── apps/*.app                   Launcher manifests (INI: name=, exec=, icon=, args=)
├── icons/*.ppm                  Tile icons, PPM P6
└── default-app                  One line: path to executable (e.g. /opt/roomwizard/app_launcher)

/etc/init.d/
├── roomwizard-app               Generic app launcher (S99 in rc2-5.d)
├── audio-enable                 Speaker amplifier setup (S29)
├── time-sync                    rdate at boot (S28) — the RTC has no battery
├── xpad-modules                 insmod -f ff-memless, joydev, xpad (S89)
└── usb-host                     Re-applies the MUSB host-mode patch (S90, after S89)

/usr/local/bin/enable-usb-host.sh  The /dev/mem patch itself, run by usb-host
/usr/local/bin/devmem_write        Its tool
/lib/modules/4.14.52/extra/        ff-memless.ko, joydev.ko, xpad.ko
/etc/sysctl.conf                 Kernel hardening; no iptables here
/opt/games/                      Native games + tools
/opt/vnc_client/                 VNC client binary + config
/opt/scummvm/                    ScummVM, where installed
```

⚠️ **p1 is not in the list above and never will be.** `mlo`, `u-boot.bin` and `ctrlblock.bin` are unreachable from every function in `lib/rw-identify.sh` —
`RW_PART_ROLES` does not contain p1, and a test asserts its absence.

## Troubleshooting

### "No RoomWizard rootfs is mounted"

Tools that look for a card print the diagnosis themselves — if a disk with the RoomWizard partition
layout is present they name it. If none is found, the card is not visible to Linux at all; on WSL it must
first be attached from Windows:

```bash
wsl --mount \\.\PHYSICALDRIVEn --bare
lsblk -o NAME,FSTYPE,SIZE,MOUNTPOINT | grep -v loop
```

Then **re-run the script**. ⚠️ Do not work out a partition number by hand: whoever is holding a card
cannot see them, `lib/rw-identify.sh` exists so that nobody has to, and `${dev}6` is the wrong name on an
`mmcblk` reader anyway (`rw_part_dev` inserts the `p`). **Do not go looking for a particular UUID** — see
[*Finding the card*](#finding-the-card).

### No app starts after reboot
No default app configured. Set one:
```bash
ssh root@<ip> 'echo /opt/roomwizard/app_launcher > /opt/roomwizard/default-app'
ssh root@<ip> reboot
```

## Related Documentation

- [SYSTEM_ANALYSIS.md](SYSTEM_ANALYSIS.md) — Hardware specs, boot chain, subsystems
- [native_apps/README.md](native_apps/README.md) — Native apps development docs
- [vnc_client/README.md](vnc_client/README.md) — VNC client docs
- [scummvm-roomwizard/README.md](scummvm-roomwizard/README.md) — ScummVM backend docs
