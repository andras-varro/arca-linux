# device-files/CLAUDE.md

What is installed onto a device **verbatim**, plus the data file that decides what a bring-up
installs. Loaded when you work in `device-files/`.

Anything installed by more than one path lives here, never in a heredoc: `arca-app` (the boot
init script — it carries the name it is *deployed* as rather than a `.sh` one), `audio-enable`,
`time-sync`, `touch-module`, `arcamond`, `sysctl.conf`, the four USB scripts `enable-usb-host.sh` /
`usb-host` / `xpad-modules` / `usb-audio-modules`, the Bluetooth files `bluetooth` / `bluetooth.conf` /
`bluetooth-main.conf` / `bluetooth-input.conf` / `bluealsa.conf` / `20-bluealsa.conf`, `avahi-daemon.conf`,
plus `provision-rules.conf`. Both `provision.sh` (over SSH) and
`rootfs/commission-offline.sh` (onto a mounted card) install those same bytes, and **neither
decides what to install — both read the rules.**
⚠️ **`.gitattributes` pins `device-files/**` to `eol=lf`.** The init scripts have no `.sh` extension —
`/etc/init.d/audio-enable` is the name the `rc5.d` link points at — and a CRLF shebang is rejected by
BusyBox as a misleading "no such file or directory". Never bulk-edit a file here with a python script;
it rewrites the whole file's line endings. Use `Edit`, or `sed -i`.

## `provision-rules.conf` — `<type> <group> <mode> <target> <source> <reason>`

The one data file: it says what the device ends up *with*. The reason is mandatory and last, a line
with fewer fields than declared is an error, and `lib/arca-provision.sh` compiles it to a plan (see
`lib/CLAUDE.md`) that is executed twice — once with `/` over SSH, once under `$BASE/root` offline.

Types `install` / `link` / `link-opt` / `unlink`. Every column means one thing for every type; `-` is
the explicit not-applicable and an *empty* field is an error.

- **Every `install` record has a `device-files/` source** — that is the check that a new file is in the
  right place.
- ⚠️ **A `link` source must be RELATIVE** — `../init.d/time-sync`, never `/etc/init.d/time-sync`. An
  absolute symlink target is correct on a running device and *dangling on a mounted card*, and a
  dangling `rc5.d` link is skipped in silence at boot. It is the one defect this file could introduce
  that nothing downstream would catch, so validation rejects it.
- **`unlink` is how a file an older image carried is retired** — a unit imaged earlier keeps whatever
  that image installed, and nothing else sweeps `/etc/init.d`.
- **`usb` and `bluetooth` are optional groups**, compiled by `usb_host/build-and-deploy.sh` and
  `bluetooth/build-and-deploy.sh` through `arca_provision_plan_component`. The four USB device scripts and
  the three `rc5.d` links are ordinary `usb`-group records, and nothing in the group touches p1.

Modes are **declared** in this file, never read off disk — `/mnt/c` reports every file 0777 and
discards `chmod`, so `stat -c %a` here is a constant, not a measurement.

## `arca-app` — stopping what is running

`/etc/init.d/arca-app` is the app respawn loop, and **its `stop` is the only implementation of
"stop what is running".** The three component scripts call it and **must not carry a `killall` of their
own.** The reason is not tidiness: a name-based rule cannot see the app that `app_launcher` *started*,
and that grandchild is normally the process holding `/dev/fb0` — its basename appears in no config
file. `app_pids()` walks `/proc/*/exe` against the three deploy directories instead, because the exe
link is the only identity neither chosen by the process nor limited to the configured app.

Two consequences: **`provision.sh <ip>` is what pushes that script**, so a `do_stop()`
change does not reach a device until it is re-run; and to see what is running use
`/etc/init.d/arca-app status`, because `ps w` on this busybox lists only processes with a TTY.
See `SYSTEM_ANALYSIS.md#53-app-launcher-and-manifests`.

## Redeploying anything here

Changing a file in this directory does **not** go out with a component deploy. Only
`./provision.sh <ip>` (which ends in a reboot) or, for a new card image,
`rootfs/commission-offline.sh` (run by `rootfs/make-card-image.sh --bundle`) installs it.
The exceptions are the four **`usb`-group** scripts and the **`bluetooth`-group** files, which
`cd usb_host` / `cd bluetooth` `&& ./build-and-deploy.sh <ip>` also installs, and which need no reboot.

## Regressions

`tests/arca_provision_test.sh` and `tests/provision_online_test.sh` (both host-only, no card, no root), plus
their `tests/measure_*_sabotage.sh` harnesses. What those suites structurally cannot see is in
`tests/CLAUDE.md` — read it before trusting a green run over a change here.
