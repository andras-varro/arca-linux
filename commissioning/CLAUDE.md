# commissioning/CLAUDE.md

The bring-up scripts. Loaded when you work in `commissioning/`.

None of these is an answer to "what do I run" except through `arca.sh` at the repo root. The
libraries they drive are documented in `lib/CLAUDE.md`; the rules file they read in
`device-files/CLAUDE.md`; the operator-facing walkthrough and the argument for the phase split in
`COMMISSIONING.md`.

Every unit runs **our own root filesystem** (`rootfs/`, marker `/etc/arca-rootfs`). Nothing here
cleans, removes or edits a vendor stack, and neither installer mounts or writes p1.

| script | when |
|---|---|
| `provision.sh <ip>` | the online UPDATE of a unit already running our root: backup, install the provision plan, reboot |
| `commission-offline.sh --bundle <tar.gz\|dir> --base <dir>` | the image step: installs a release bundle and the provision plan into a card's partitions mounted by the caller — `rootfs/make-card-image.sh --bundle` is the only caller |
| `set-hostname.sh` | the one writer of the name files |
| `clone-to-32gb.sh` | grows p6 onto a larger card (`SD_CARD_UPGRADE.md`) |
| `backup.sh <ip> [<out>]` | before an update or reflash: the unit's BACKUP set (`lib/arca-state.sh`) to `backups/` — read-only on the unit, archive holds secrets |
| `restore.sh <ip> <tar.gz> [--dry-run]` | after: refuses unsafe or non-BACKUP paths, `arca-app stop`, `tar -o` over `/`, SSH modes, `start` |

## `provision.sh` updates; it deletes nothing

The SSH gate (`lib/arca-ssh.sh`), then the plan compiled from `device-files/provision-rules.conf`.
`--dry-run` prints the plan and exits without touching the unit; `--status` and `--hostname` are the
other non-installing modes and never back up or reboot. Before the **first write** it runs `backup.sh`,
and a failed backup aborts the run. An unknown flag is refused, and `--ssh-auth` / `--sshd-only` are
refused by name: the overlay's key-only `sshd_config` is the only sshd policy and this script never
edits it. Boot service and install live **only** here (and in the offline installer) — never in a
component script. `deploy-all.sh` and the `*/build-and-deploy.sh` must not duplicate any of it.

Regression: `tests/provision_online_test.sh` (fake `ssh`/`scp`; no device). Rules: `tests/CLAUDE.md`.

## `commission-offline.sh` has no prompt, no clean and no disk scan

It reads no terminal, needs no root of its own and mounts nothing: `--base` is required and names a
directory already holding `{root,data,log}`, and `lib/arca-identify.sh` refuses a `root` that lacks our
marker. The retired flags (`--no-clean`, `--keep-<group>`, `--delete-factory`, `--unattended`, `--disk`,
`--release`, `--ssh-auth=*`, `--sshd-only`) are **refused by name with a non-zero exit** — a flag
accepted and ignored reads as a choice honoured. Host name, password and ssh identity are not set here;
they are whatever the caller's p6 already carries.

## `commission-offline.sh` verifies what it installed, on the card

It md5s every **installed** file against the bundle manifest, asserts `+x` (real ext4 honours it, so
that check is a measurement offline and cannot be one on `/mnt/c`), runs `check-arm-safe.sh` over the
**downloaded** binaries, asserts every `.app`'s `exec=`/`icon=` and that `default-app` names one of
them, and `dash -n`s every `/bin/sh` script it wrote.

⚠️ Three ways that verification can lie, all guarded: `dash -n` misses bashisms (`[[` parses as a
command name); a missing `arm-linux-gnueabihf-objdump` is a **refusal**, not a pass, so the caller
counts the ELF candidates itself and says loudly what it did not check (`--arm-check=skip` is the
deliberate override); and a **stripped** binary cannot be gated at all, so the checker returns 2
("could not judge") and the installer proceeds with a loud block naming the count. `scummvm` and
`vnc_client` ship stripped, so **every full bundle** takes that path — the sound verdict is the
build-time one, on the unstripped artifact. Detail: `tests/CLAUDE.md`, and `IMPROVEMENT_PLAN.md`.

## Host name: `set-hostname.sh` is the one writer

It writes `/etc/hostname` and rewrites `/etc/hosts` so the device's own name never sits on a
non-loopback line (a unit would otherwise resolve its own name to a wrong address). It keys the
`/etc/hosts` removal on the name it reads from `/etc/hostname`, never a hardcoded one, and has a
negative control: it refuses to write an `/etc/hosts` that lost `localhost`. The unit's `S04firstboot-hostname` (first boot of a card image,
which carries no name of its own) calls it as `/usr/sbin/set-hostname`, and `provision.sh --hostname` over SSH, so they cannot drift; `--hostname` does
**not** reboot, which is what makes it usable on a unit in service as a live display. `/etc/hostname`
is the only home of the name.

## A device can run an older copy than the repo's

`init.d/arca-app` reaches a unit **only** through `provision.sh` (or the offline installer).
`./commissioning/provision.sh <ip> --status` md5s the deployed copy against the repo's and says
`matches repo` or `DRIFTED` (read-only, no reboot). **Check that before reproducing anything against
a device**, or you will draw conclusions about code the device is not running. The software watchdog
that this once also covered is not on our root; the hardware watchdog is fed by `/sbin/watchdog`
(`rcS.d/S06watchdog`): [`SYSTEM_ANALYSIS.md` §3.13](../SYSTEM_ANALYSIS.md#313-watchdogs).

## Regressions

`tests/provision_online_test.sh` (host-only, fake `ssh`/`scp`), `tests/arca_provision_test.sh` (the plan
compiler and offline executor) and `tests/commission_offline_test.sh` (needs root and a staged
bundle), plus `tests/measure_provision_sabotage.sh`. Rules for all: `tests/CLAUDE.md`.
