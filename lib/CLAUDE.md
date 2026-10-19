# lib/CLAUDE.md

The sourced-not-executed shell libraries, and the repo-root script that drives them
(`provision.sh`; last section). Loaded when you work in `lib/`.

These live at the top level because the component build scripts
source `arca-bundle.sh` on the *write* side while the image installer (`rootfs/commission-offline.sh`) reads it, and every SSH-using script sources
`arca-ssh.sh`. Device facts are in `SYSTEM_ANALYSIS.md`; open work in
`IMPROVEMENT_PLAN.md`; how to author the rules file these libraries parse is in
`device-files/CLAUDE.md`.

| file | job |
|---|---|
| `arca-identify.sh` | the card layout (`ARCA_LAYOUT`), which disk is a card, which partition holds which tree, and the offline path guards (`arca_offline_path`, `arca_offline_base_ok`) |
| `arca-provision.sh` | compile `provision-rules.conf` to an install plan, and generate the online executor |
| `arca-bundle.sh` | the release-bundle layout |
| `arca-release.sh` | fetch a published release — **the one library here that opens a socket** |
| `arca-ssh.sh` | the one answer to "can I reach this device" |
| `arca-state.sh` | the SSH-key deny list the card-image builder applies; `rootfs/` sources it, `tests/arca_state_test.sh` covers it |
| `product.sh` | `PRODUCT_NAME` / `PRODUCT_STEM` for banners, release titles and download names, read from `rootfs/board/arca/os-release`; no fallback, it refuses instead |
| `arca-kernel-src.sh` | `arca_check_kernel_tarball`: the sha256 check `kernel/build-image.sh` and `usb_host/build-kernel-modules.sh` run before `tar xf`; the pin is read from `rootfs/package-hashes/linux-headers.hash` and nowhere else, and a missing pin refuses |

## One SSH gate, and BatchMode stays on it

**`arca-ssh.sh` is the only implementation of "can I reach this device".** The scripts that source it
are listed by `grep -rln 'arca-ssh.sh' -- provision.sh deploy-all.sh arca.sh */build-and-deploy.sh`
and every call goes through `arca_ssh_gate`. They each keep their own
gate *call*, because a component script must run standalone; what they must not keep is their own
probe. There used to be eight, already drifted into three wordings of one message.

- ⚠️ **`BatchMode=yes` stays on the probe.** Only the *probes* set it; the `ssh`/`scp` calls behind
  them do not. So dropping it "works" and then prompts for a password **once per call** — and
  `native_apps/build-and-deploy.sh` makes dozens. The goal is never a password-driven deploy; it is
  to get a key installed once.
- **"Down" and "up but refusing us" are different answers, and only the second has a remedy.**
  `arca_ssh_classify` decides, and it matches `Permission denied` — ⚠️ **never the parenthetical method
  list.** That list is the *server's*: our key-only root says `(publickey)`,
  and a plain `sshd` says
  `(publickey,keyboard-interactive)`. Keying on one server's list passes against that device and
  calls every other server "down" — which *suppresses* the offer, so nothing looks broken. An
  unrecognised error classifies as `down` on purpose.
- ⚠️ **`arca_ssh_probe` reports the state twice — printed AND in `ARCA_SSH_LAST_STATE`/`_LAST_STDERR` —
  and the gate must use the globals.** `state="$(arca_ssh_probe …)"` runs it in a *subshell*, so the
  stderr global is discarded and every message shows a blank line where ssh's own complaint belongs.
  That was the first version of the file; it passed every wording assertion, because they grepped for
  the surrounding text rather than for what ssh said.
- **Interactive help is `[ -t 0 ]`-gated.** `release.sh` and `deploy-all.sh` drive component scripts
  as a batch, so a blocking `read` there would hang a run nobody is watching. A non-TTY caller gets
  the diagnosis plus the exact commands.
- **No stored password, and no `sshpass`.** `ssh-copy-id` asks once and stores nothing;
  `arca_ssh_keygen` makes an ed25519 key with no passphrase if the operator has none. `release.sh`
  already refuses to publish config precisely because one shipped file carries a plaintext password —
  a second one moves toward the thing that check guards. (`sshpass` *is* installed in this WSL,
  measured 2026-08-07; it is rejected on the merits, not for absence.)
- ⚠️ **A key generated under `sudo` must be chowned back.** A script run as root (for example under
  `sudo`) that calls `ssh-keygen` writes the key root-owned *inside the operator's
  home* — where `ssh` then needs `sudo` forever. `arca_ssh_key_owner` takes the euid as an **argument**
  so both branches are reachable from a non-root test.

## Partitions: position, never UUID

⚠️ **Never identify a partition by filesystem UUID.** A UUID is assigned at mkfs time, so it names one
*card*: units are mkfs'd independently at the factory and two Steelcase RoomWizards on identical firmware share
**none** of their four UUIDs. Nothing on the device consumes one either — `root=/dev/mmcblk0p3` and
the partition numbers in `/etc/fstab` are both by position.

`arca-identify.sh` is the one implementation: **content** for a mounted rootfs (`arca_is_rootfs`), the
**partition table** for a disk (`arca_is_card_disk`), and **position** for which partition holds which
tree (`ARCA_PART_ROLES` = p3 root, p2 log; `/home/root/data` is a plain directory on p3, so there is no data role). It requires our marker
(`/etc/arca-rootfs`) of a rootfs; no other tree counts. `arca_offline_base_ok` refuses `/` (and an empty
base, or one with `..`) on purpose — a base that selected the dev host's root would rewrite this host's
`/etc/shadow`.

⚠️ **p1 is deliberately absent from `ARCA_PART_ROLES`, and a test asserts its absence.** Nothing can
reach `mlo`, `u-boot.bin` or `ctrlblock.bin` through those functions — a stronger guarantee than every
caller remembering not to. The layout itself is `ARCA_LAYOUT` (`ARCA_LAYOUT_GROW=3` marks the one partition whose size is not pinned; `arca_uboot_root_ok` refuses a `u-boot.img` whose `root=` is not the root role); `tests/card_layout_test.sh` is its host test. No function in this directory mounts p1 at all.

⚠️ **A rootfs mounted offline shows `/home/root/log` as an EMPTY directory** — it is the mount
point for p2. An offline tool that mounts only p3 sees none of the logs, and would report success having touched
them. Use `arca_check_card_mounts`; its negative half —
"a rootfs where `log` was expected means the partitions are in the wrong order" — is the half that
catches the mistake that makes every later path resolve under the wrong tree.

Measurements: `SYSTEM_ANALYSIS.md#42-partitions`.

## p1: nothing scripted writes it

There is **no boot-time MD5 check** of the kernel and no signing — the only gate on `uImage-system` is
its uImage header + data CRC, which `mkimage` writes when `kernel/build-image.sh` packages our image.
U-Boot has no `saveenv`, so the environment cannot be persisted or corrupted.
**No script here writes `/dev/mtd*`, `mlo`, `u-boot.bin` or `ctrlblock.bin` of a unit** — a scripted write lands on every unit it
runs on; by hand, each is allowed, and its undo cost is `SYSTEM_ANALYSIS.md#what-a-boot-path-write-costs-to-undo`.
The one exception is `rootfs/make-card-image.sh`, which formats and fills the p1 of an image **file** it created itself (it refuses a
block device), with our own `MLO`, `u-boot.img` and kernel; no vendor file is ever copied.
stage experimental kernels under a *new* filename.

**Only our own kernel image is supported**, and no library or script here writes `uImage-system`:
installing it is a manual operator step (`kernel/README.md`), preceded by a verified backup of the
*running* image. The vendor-kernel byte patch that used to be this file's one writer is deleted; tag
`last-vendor-kernel` is the last tree that carried it.

Observe all of the above and JTAG never comes up. Detail and recovery procedure:
`SYSTEM_ANALYSIS.md#4-boot-chain-and-recovery`.

## The plan compiler

`arca-provision.sh` parses the tab-separated `provision-rules.conf`, compiles it to a plan, and lets each
consumer keep its own **executor** — because `/` is the correct prefix on a device and a refused one
offline.

- **Order is emitted by the compiler, not read from the file**: unlink → install → link. Unlink before
  link (a glob would eat the link just made), install before link (a link to a not-yet-written file
  dangles on a card).
- ⚠️ **The online executor is generated, not written twice.** `arca_provision_online_script` emits a
  POSIX `sh` interpreter that `provision.sh` pipes to the device; `install` is the one
  verb it cannot do alone, because the source bytes are on the host, so the caller `scp`s them first
  and the interpreter only sets the declared mode. Check it with `dash -n`, not `bash -n` — it runs
  under BusyBox ash.
- ⚠️ **That `scp` step is `arca_provision_push_installs`, and it reads the plan on fd 3.** `ssh` reads
  its own stdin and forwards it to the remote command, so a `while read … done < "$PLAN"` loop with an
  `ssh` in the body loses the whole rest of the plan to the *first* `ssh`: **one file installed of
  eight**, the `link` records then dangling, and the executor correctly refusing on the seven — which
  read as an executor bug. `ssh -n` fixes one body and not the next; fd 3 is a property of the loop.
  The function also **counts** — install records in versus files copied out, refusing on a mismatch —
  because the old shape was silent exactly where the copying happened. It lives here with
  `$ARCA_SSH`/`$ARCA_SCP` indirection because `provision.sh` and
  `usb_host/build-and-deploy.sh` each had a verbatim copy of the loop, i.e. the defect twice, and
  because indirection is what makes the copy step reachable from a test with no device. **Never inline
  it again.**
- **A plan-summary line is computed, never hand-rolled.** `arca_provision_plan_summary` counts every
  record type present, including one its ordered list does not know about. Hand-rolled arithmetic in
  the callers left whole record types out of the breakdown.
- ⚠️ **`usb` is a provision group, and a component script compiles it through
  `arca_provision_plan_component`, not through its own `scp`/`ln -sf`.** `arca_provision_plan_component
  FILE GROUP` compiles one optional group's records for `usb_host/build-and-deploy.sh` (`usb`) `bluetooth/build-and-deploy.sh` (`bluetooth`) and `native_apps/build-and-deploy.sh` (`backup`) — a separate
  entry point rather than a flag on `arca_provision_plan`, and it refuses `base`, so an install
  path cannot reach a base-less plan by mistyping a group list.

## Bundles: one layout, declared modes, no configs

`release.sh` exists so that putting apps on a device does not require reproducing the toolchain.
It calls `build-and-deploy.sh --bundle <dir>` on all five components. The
layout lives in **`arca-bundle.sh`** and nowhere else: `<dir>/root/<device-path>` plus
`<dir>/manifest.d/<component>.{list,md5}`.

- ⚠️ **Modes are *declared* by the caller, never read off disk.** `/mnt/c` reports every file 0777 and
  discards `chmod`, so `stat -c %a` here is a constant, not a measurement. `arca_bundle_add` takes the
  mode as an argument and `.list` is the authority.
- **`arca_bundle_check` asserts both directions** — no manifest entry without a staged file, *and* no
  staged file without an entry. The second is the one that catches a file added by hand that nothing
  will ever `chmod`.
- **`release.sh` greps the staged manifest and refuses to publish config** (`*.conf`, `/etc/hosts`,
  `/etc/hostname`, `arca_config`, `touch_calibration`, `input_config`). Not a rule each component is
  trusted to remember — the negative control for the one that forgets. Device config carries
  the per-unit host name and
  `vnc_client`'s plaintext VNC password.
- ⚠️ **And it refuses to publish vendor firmware** — any entry whose basename is `uImage*`, `mlo`,
  `u-boot*` or `ctrlblock*`. Matched on the basename, not a path, because p1 is not a bundle path at
  all: a staged copy would arrive at some invented location. `uImage-system` is a 5.2 MB Steelcase
  binary and this repo is meant to be published. `usb_host`'s `--bundle` carries its *built*
  artifacts only.
- ⚠️ **A new staged file is a licence decision, and `LICENSE.md` is where it is recorded.** Ask whether
  the file is *ours*: `scummremastered.zip`, `gui-icons.dat` and `vkeybd_arca.zip` are all
  GPL-3.0+ ScummVM data and were being published with no licence line until someone looked, and
  `vkeybd_arca.zip`, the Realtek firmware and `device-files/bluetooth.conf` are the non-MIT files committed here. `LICENSE.md` is the
  repo-level half of `release.sh`'s per-release `NOTICE`; **the two must agree**, and MIT governs our
  *source* — it does not decide the licence of a binary it links into (`scummvm` is GPL-3.0+ as a
  whole, `vnc_client` GPL-2.0+). Measure a dependency's licence *version* rather than carrying it
  forward: `NOTICE` claimed GPLv2+ for ScummVM and the tree's `COPYING` is GPLv3.
- **Both halves have now run end to end.** `--stage-only` produces a tarball that is a first-class input
  to the offline installer, and `--tag` published `v1.0.0` — verified by comparing a local `sha256sum`
  against the digest GitHub reports. `gh` 2.86.0 is installed in WSL (from the release `.deb`: focal's
  apt has no `gh`, and the snap links against a glibc newer than 2.31).

## `arca-release.sh` fetches, and it is the only file here that opens a socket

The fetch is thin by construction: `deploy-all.sh --from-bundle` and `rootfs/commission-offline.sh --bundle`
both already accept a tarball *or* an unpacked directory, so it resolves, downloads, verifies and hands
over a path. It installs nothing and never touches a device. `arca-bundle.sh` stays network-free so that
"does this code reach the network" is answerable from the source list alone.

- ⚠️ **There is ONE owner/repo derivation, `arca_release_repo`, and `release.sh` sources it.** A fork must
  fetch from the fork it published to. `gh` cannot resolve this repository from its remote at all —
  `origin` is an SSH host **alias**, and `gh` refuses the repository outright with "none of the git
  remotes configured for this repository point to a known GitHub host" — so `--repo` is mandatory on
  every `gh` call. ⚠️ **It reads the remote URL and nothing else from git**: `remote get-url` is local,
  while any *network* git operation dies under WSL with "Could not resolve hostname
  github.com-personal", because the alias lives in the Windows ssh config.
- ⚠️ **sha256, never md5.** The bundle's `manifest.d/*.md5` proves internal consistency once unpacked and
  says nothing about whether the bytes that arrived are the bytes GitHub served. Nothing is signed, so
  the one authenticity check available is `.assets[].digest`, which GitHub reports as `sha256:…` — on
  `gh release view --json assets` **and** on the unauthenticated REST API. Comparing the manifest's md5
  against that digest compares two different algorithms and can never agree, and a release publishing
  **no** digest is refused outright: an asset that cannot be verified is not a safer install than none.
- ⚠️ **`gh release download` prints NOTHING — zero bytes on stdout and stderr for a 125 MB asset.** The
  bundle is ~126 MB, most of it uncompressed mono music beds, so on an ordinary link that is minutes of
  dead terminal, which reads as a hang and gets Ctrl-C'd. `_rw_release_progress` polls the partial file
  against the size the resolve step already knows; it wraps `curl` too, so both transports look alike.
- **Cached by digest, and the download lands on a `.part` first.** A cached file whose sha256 matches is
  the same bytes GitHub would serve, so a second unit costs no second download; one that does not match
  is re-downloaded rather than trusted. The move into place happens only after verification, so an
  interrupted run leaves nothing for the next one to find and a mismatch cannot poison the cache — `gh`
  refuses an existing target without `--clobber` anyway.
- **`jq` is not installed here**, so the `gh` path uses gh's built-in `--jq` and the `curl` fallback
  parses with `grep`/`sed`. ⚠️ **It splits on commas, not braces**: every asset object nests an
  `uploader` object, and a brace split cuts one asset in half, separating its name from its digest. With
  more than one tarball asset that fallback **refuses** rather than pairing a digest by guesswork.
- ⚠️ **stdout carries the path and nothing else.** Callers do `p="$(arca_release_fetch …)"`, which is a
  subshell — the same trap as `arca_ssh_probe`, avoided here by having no globals to lose. Every
  diagnostic and every progress line goes to stderr.
- **A fetch under `sudo` hands the cached file back to `$SUDO_UID`**, for the reason
  `arca_ssh_key_owner` exists: otherwise the operator needs `sudo` to touch their own `build/`.

## `provision.sh`: the developer's update of a running unit

A user flashes an image; this is for a unit already running our root. `./provision.sh <ip|host>`:
SSH gate, plan compiled from `device-files/provision-rules.conf`, `--dry-run` prints it and exits, then
step 0 (below; **a failed backup aborts the run**), install, sysctl, reboot. It deletes no software and never
writes p1. `--status` (md5s the deployed scripts against the repo's: `matches repo` or `DRIFTED` per file) and
`--hostname NAME` (stages and runs the overlay's `set-hostname` over SSH) never back up or reboot; the second
is what makes renaming a unit in service safe. `--no-<group>` skips one provision group. An unknown flag is
refused, and `--ssh-auth` / `--sshd-only` by name: the overlay's key-only `sshd_config` is the only sshd
policy. ⚠️ **Boot service and install live only in `provision.sh` and `rootfs/commission-offline.sh`** —
`deploy-all.sh` and the `*/build-and-deploy.sh` must not duplicate any of it. Regression:
`tests/provision_online_test.sh` (fake `ssh`/`scp`; no device).

**Step 0 runs the repo's `device-files/arca-backup` and every `*/backup.d/*.item` from a scratch dir on the unit** (`ARCA_BACKUP_ITEMS`),
exports every item, secrets included, and copies `ludbox-backup-<host>-<stamp>.tar.gz` to `backups/` (mode 600, refuses to overwrite);
any failure aborts with nothing written. The item format is in `DEVELOPING.md#plug-in-file-formats`;
users export and import from Control Panel → Backup.

## Regressions

Host-only, no device, no root: `tests/arca_ssh_test.sh`, `tests/arca_provision_test.sh`,
`tests/provision_online_test.sh`, `tests/arca_identify_test.sh`, `tests/arca_state_test.sh`, plus the
`tests/measure_*_sabotage.sh` harnesses that re-measure them. What each one can and cannot see, and
the traps in extending them, are in `tests/CLAUDE.md`.
