#!/bin/bash
#
# provision.sh — UPDATE a unit that runs our own root filesystem, over SSH
#
# Re-runnable. It installs the files and links device-files/provision-rules.conf
# declares (boot scripts, the app launcher's init service, audio + time-sync, mDNS,
# USB host mode, Bluetooth, sysctl), applies the sysctl settings,
# and reboots. It deletes no software: the unit's root is ours, not the vendor's.
#
# Usage:
#   ./provision.sh <target>                 # backup, update, reboot
#   ./provision.sh <target> --dry-run       # print the provision plan; change nothing
#   ./provision.sh <target> --status        # show device status only (read-only)
#   ./provision.sh <target> --hostname rw09 # set the host name only, no reboot
#
# <target> is an IPv4 address or a host name — `rw09.local` works once mDNS is
# enabled (this script does that) and the unit has a unique name (--hostname).
#
# Prerequisites:
#   - A unit running our root image (rootfs/), reachable by SSH as root
#
# What it does:
#   0. Backs the unit's per-unit state up to this host (the repo's device-files/arca-backup
#      engine, every item, secrets included; backups/) BEFORE the first write; a failed
#      backup aborts the run.
#   1. Installs the provision plan: device-files/arca-app as
#      /etc/init.d/arca-app, the audio-enable + time-sync boot scripts, avahi
#      (mDNS), the USB host-mode scripts and their boot links (--no-usb skips), the
#      Bluetooth boot script and dbus policy (--no-bluetooth skips)
#   2. Applies kernel/sysctl security settings (ASLR, no ip_forward, etc.)
#   3. Reboots the device
#
# NOTE: No iptables firewall — the kernel has CONFIG_NETFILTER=y but no ip_tables
# module was built, and there is no package manager to add one.
# Network security relies on: SSH hardening + sysctl + disabling services.
#
# After this, deploy a project and set it as the default app:
#   cd native_apps && ./build-and-deploy.sh <ip> set-default
#   cd vnc_client   && ./build-and-deploy.sh <ip> set-default

# Source-path directive: resolves the source= hints below against this script's directory.
# shellcheck source-path=SCRIPTDIR
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$SCRIPT_DIR"

# What is INSTALLED is DATA in one file, device-files/provision-rules.conf, shared
# with rootfs/commission-offline.sh so that the live and offline passes cannot
# drift. lib/arca-provision.sh is the parser and the plan compiler; the executors
# differ ("/" is the correct prefix on a device and a refused one offline, and on
# this path the work happens on the far side of an ssh pipe), but there is one
# implementation of each, and arca_provision_online_script is the one this script
# ships to the device.
# shellcheck source=lib/arca-identify.sh
. "$REPO_ROOT/lib/arca-identify.sh"
# shellcheck source=lib/arca-provision.sh
. "$REPO_ROOT/lib/arca-provision.sh"
# shellcheck source=lib/arca-ssh.sh
. "$REPO_ROOT/lib/arca-ssh.sh"
# shellcheck source=lib/product.sh
. "$REPO_ROOT/lib/product.sh"

# ── the non-positional flags are extracted before positional parsing ────────
#
# --no-<g> switches off part of the PROVISION, named after the groups in
# provision-rules.conf so the list is not repeated here. They are pulled out of "$@"
# first because everything below is positional ($1 target, $2 flag) and a --no-usb
# sitting in $3 would otherwise be rejected as an unknown option.
NO_PROV_GROUPS=""
DRY_RUN=""
_ARGS=()
for _a in "$@"; do
    case "$_a" in
        --dry-run)      DRY_RUN="--dry-run" ;;
        --ssh-auth=*|--sshd-only)
            echo "$_a: removed. sshd_config is the root image's overlay alone (key-only); this script never edits it."
            exit 1 ;;
        --no-*)
            _g="${_a#--no-}"
            case " $(arca_provision_optional_groups) " in
                *" $_g "*) NO_PROV_GROUPS="$NO_PROV_GROUPS $_g" ;;
                *) echo "Unknown provision group: $_g"
                   echo "  --no- accepts: $(arca_provision_optional_groups)"
                   exit 1 ;;
            esac
            ;;
        *) _ARGS+=("$_a") ;;
    esac
done
set -- "${_ARGS[@]}"

DEVICE_IP="${1:-}"
FLAG="${2:-}"
DEVICE="root@${DEVICE_IP}"
INIT_SCRIPT="/etc/init.d/arca-app"
# Files installed onto the device verbatim. They live in device-files/ rather
# than in a heredoc here so that the offline installer writes the same bytes —
# two copies of an init script is two things to keep in step, and the one that
# drifts is discovered on a device that boots to a black screen.
DEVICE_FILES="$REPO_ROOT/device-files"

# ── colour helpers ──────────────────────────────────────────────────────────
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; BLUE='\033[0;34m'; NC='\033[0m'
ok()   { echo -e "${GREEN}  ✓ $*${NC}"; }
info() { echo -e "${YELLOW}  → $*${NC}"; }
warn() { echo -e "${BLUE}  ! $*${NC}"; }
err()  { echo -e "${RED}  ✗ $*${NC}"; exit 1; }

# ── usage ───────────────────────────────────────────────────────────────────
usage() {
    echo "Usage: $0 <target> [--status] [--dry-run] [--no-<group>]"
    echo "       $0 <target> --hostname NAME"
    echo ""
    echo "  With no flags this UPDATES a unit that runs our own root: it backs the"
    echo "  unit's per-unit state up to this host (arca-backup, all items, to backups/), installs"
    echo "  the provision plan, applies sysctl and reboots. It deletes no software."
    echo "  p1 is never written."
    echo ""
    echo "  <target>          Device IPv4 address, or a host name (e.g. rw09.local)"
    echo "  --status          Show device status only (no changes, no backup, no reboot)"
    echo "  --dry-run         Print the provision plan this run would apply; change"
    echo "                    nothing, take no backup, do not reboot."
    echo "  --no-<group>      Skip one group of the provision plan. Groups:"
    echo "                    $(arca_provision_optional_groups)"
    echo "                    --no-mdns leaves <name>.local unresolvable;"
    echo "                    --no-usb installs no USB host"
    echo "                    mode; --no-bluetooth installs no BT boot script or dbus"
    echo "                    policy."
    echo "  --hostname NAME   Set the device host name only, and exit. No backup, no"
    echo "                    reboot."
    echo "                    NAME is a single label — 'rw09', not 'rw09.local'."
    exit 1
}

[[ -z "$DEVICE_IP" ]] && usage

# Validate the target before doing anything: every step past here is destructive
# and ends in a reboot.
#
# An IPv4 address OR a DNS name is accepted. The name form is what makes
# `./provision.sh rw09.local` work, which is the whole point of enabling
# avahi below — an IPv4-only gate here silently defeated it. (This used to be
# IPv4-only and there was a SECOND, weaker validator further down that did
# accept a name; the strict one exited first, so the permissive one was dead
# code and the script only *looked* like it took a host name. Both are now this
# one check.)
IPV4_RE='^(25[0-5]|2[0-4][0-9]|1[0-9][0-9]|[1-9]?[0-9])(\.(25[0-5]|2[0-4][0-9]|1[0-9][0-9]|[1-9]?[0-9])){3}$'
# RFC-1123: dot-separated labels, each 1..63 chars, alphanumeric at both ends,
# hyphens allowed inside. No trailing dot.
DNSNAME_RE='^[a-zA-Z0-9]([a-zA-Z0-9-]{0,61}[a-zA-Z0-9])?(\.[a-zA-Z0-9]([a-zA-Z0-9-]{0,61}[a-zA-Z0-9])?)*$'
if [[ "$DEVICE_IP" =~ $IPV4_RE ]]; then
    :   # an address
elif [[ "$DEVICE_IP" =~ ^[0-9.]+$ ]]; then
    # Digits and dots only, but not a valid IPv4 — that is a mistyped address,
    # not a host name. Accepting it as one would turn 192.168.50.999 into a DNS
    # lookup and a confusing timeout instead of an immediate complaint.
    echo "Not a valid IPv4 address: $DEVICE_IP"
    echo ""
    usage
elif [[ "$DEVICE_IP" =~ $DNSNAME_RE ]]; then
    :   # a host name, e.g. rw09.local
else
    echo "Not an IPv4 address or host name: $DEVICE_IP"
    echo ""
    usage
fi

# The init script is the file this script copies whose staleness is silent, and it
# reaches a device ONLY through here — deploy-all.sh does not push it. So a device
# can run an older copy than the repo's with nothing saying so; md5 settles it in
# command.
#
# Byte comparison is valid because .gitattributes pins *.sh to eol=lf, so the
# working tree is LF even on this Windows host and scp copies it unchanged.
report_script_versions() {
    local pairs=(
        "$REPO_ROOT/device-files/arca-app:$INIT_SCRIPT"
    )
    local pair local_path remote_path local_md5 remote_md5 drift=0
    for pair in "${pairs[@]}"; do
        local_path="${pair%%:*}"
        remote_path="${pair#*:}"
        local_md5="$(md5sum "$local_path" | cut -d' ' -f1)"
        remote_md5="$(ssh "$DEVICE" "md5sum $remote_path 2>/dev/null" 2>/dev/null | cut -d' ' -f1 || true)"
        if [[ -z "$remote_md5" ]]; then
            warn "$(basename "$remote_path"): NOT INSTALLED"
            drift=1
        elif [[ "$local_md5" == "$remote_md5" ]]; then
            ok "$(basename "$remote_path"): matches repo (${local_md5:0:8})"
        else
            warn "$(basename "$remote_path"): DRIFTED — device ${remote_md5:0:8}, repo ${local_md5:0:8}"
            drift=1
        fi
    done
    if [[ $drift -ne 0 ]]; then
        warn "Run '$0 $DEVICE_IP' to push the repo's versions (this REBOOTS the device)"
    fi
    return 0
}

# Reject unknown flags rather than silently falling through to a full setup+reboot
case "$FLAG" in
    ""|--status|--hostname) ;;
    *) echo "Unknown option: $FLAG"; echo ""; usage ;;
esac

# --dry-run previews the provision plan, and these two modes install nothing — so the
# combination is a mistake rather than a no-op, and is refused out loud.
if [[ -n "$DRY_RUN" && ( "$FLAG" == "--status" || "$FLAG" == "--hostname" ) ]]; then
    echo "--dry-run does not apply to $FLAG — neither writes anything."
    echo ""
    usage
fi

# NOTE: a stray positional is rejected per-mode below, where the number of
# expected ones is known — --hostname takes a NAME, nothing else takes anything.

# --hostname is the one flag that takes a value, so it consumes $3. Every other
# mode is exhausted by $1 and $2 — --dry-run and the
# --no-<group> options were lifted out of "$@" above, so a leftover positional
# here is a mistake. Say so rather than ignoring it, for the same reason the flag
# case above is exhaustive.
NEW_HOSTNAME=""
if [[ "$FLAG" == "--hostname" ]]; then
    NEW_HOSTNAME="${3:-}"
    if [[ -z "$NEW_HOSTNAME" ]]; then
        echo "--hostname requires a NAME"; echo ""; usage
    fi
    if [[ -n "${4:-}" ]]; then
        echo "Unexpected argument: $4"; echo ""; usage
    fi
    # Validated HERE, at parse time, for two reasons: a typo should not need a
    # reachable device to be caught, and the name is later interpolated into an
    # ssh command string, so nothing unexpected should ever get that far.
    # rootfs/board/arca/overlay/usr/sbin/set-hostname validates again on the device and remains the authority.
    if [[ ! "$NEW_HOSTNAME" =~ ^[a-zA-Z0-9]([a-zA-Z0-9-]{0,61}[a-zA-Z0-9])?$ ]]; then
        echo "Not a valid host name: $NEW_HOSTNAME"
        echo "  RFC-1123, single label: letters, digits and hyphens, alphanumeric at both ends."
        echo "  Set 'rw09', not 'rw09.local' — mDNS appends the .local itself."
        echo ""
        usage
    fi
elif [[ -n "${3:-}" ]]; then
    echo "Unexpected argument: $3"; echo ""; usage
fi

# NOTE: the target was validated at the top of this file, against both an IPv4
# address and a DNS name. A second, weaker check used to sit here; it was
# unreachable, and deleting it is what makes `rw09.local` usable.


# ── SSH check ───────────────────────────────────────────────────────────────
echo ""
echo "════════════════════════════════════════"
echo " $PRODUCT_NAME System Setup"
echo "════════════════════════════════════════"

info "Testing SSH connection to $DEVICE_IP..."
# One gate, in lib/arca-ssh.sh: it tells "down" from "up and refusing us" and, on a
# terminal, offers to generate a key and ssh-copy-id it. This is FIRST contact for
# anyone who did not prep the card, so a bare `check IP and SSH key` — advice about
# a key nothing offered to make — is not enough here.
arca_ssh_gate "$DEVICE" || err "Cannot continue without SSH to $DEVICE"
ok "SSH OK"

# apply_plan PLANFILE
#
# Ship PLANFILE and run it through the online executor. sshd_config is not part of
# any plan: the root's overlay carries the only sshd policy (key-only), so nothing
# here edits it, snapshots it or reloads sshd.
apply_plan() {
    local plan="$1"
    ssh "$DEVICE" "cat > /tmp/arca-provision-plan" < "$plan"
    arca_provision_online_script | ssh "$DEVICE" "cat > /tmp/arca-provision.sh"
    ssh "$DEVICE" "sh /tmp/arca-provision.sh /tmp/arca-provision-plan; rc=\$?; rm -f /tmp/arca-provision.sh /tmp/arca-provision-plan; exit \$rc" \
        || err "the provision step failed on the device"
}

# ── hostname-only mode ──────────────────────────────────────────────────────
# Targeted and reboot-free, so it can be run against an already-commissioned
# unit — including one that is a live display and must not be rebooted, which is
# the case this flag exists for. The work itself is rootfs/board/arca/overlay/usr/sbin/set-hostname, staged
# to /tmp rather than installed: a one-shot that nothing on the device calls again
# (so it stays out of report_script_versions' drift list).
if [[ "$FLAG" == "--hostname" ]]; then
    echo ""
    info "Setting host name to '$NEW_HOSTNAME'..."
    scp -q "$REPO_ROOT/rootfs/board/arca/overlay/usr/sbin/set-hostname" "$DEVICE:/tmp/set-hostname.sh"
    ssh "$DEVICE" "chmod +x /tmp/set-hostname.sh"
    ssh "$DEVICE" "/tmp/set-hostname.sh $NEW_HOSTNAME"
    ssh "$DEVICE" "rm -f /tmp/set-hostname.sh"
    ok "Host name set"
    echo ""
    info "Verifying:"
    ssh "$DEVICE" bash <<'REMOTE'
echo "  hostname:      $(hostname)"
echo "  /etc/hostname: $(cat /etc/hostname)"
echo "  /etc/hosts:"
sed 's/^/    /' /etc/hosts
REMOTE
    echo ""
    if ssh "$DEVICE" "test -L /etc/rc5.d/S30avahi-daemon" 2>/dev/null; then
        ok "mDNS is enabled — after the next reboot, try: ssh root@$NEW_HOSTNAME.local"
    else
        warn "mDNS is NOT enabled on this device, so <name>.local will not resolve."
        warn "Run a full './provision.sh $DEVICE_IP' to install it (that reboots)."
    fi
    echo ""
    exit 0
fi

# ── status-only mode ────────────────────────────────────────────────────────
if [[ "$FLAG" == "--status" ]]; then
    echo ""
    info "Device status:"
    ssh "$DEVICE" bash <<'REMOTE'
echo ""
echo "Disk:    $(df -h / | tail -1 | awk '{print $3 " used, " $4 " free (" $5 " used)"}')"
echo "Memory:  $(free -h | grep Mem | awk '{print $3 " used, " $7 " available"}')"
echo "HW watchdog:      $([ -c /dev/watchdog ] && echo 'active (fed by /usr/sbin/watchdog)' || echo 'n/a')"
echo "Default app:      $(cat /opt/arca/default-app 2>/dev/null || echo '(not set)')"
echo ""
echo "Active cron jobs:"
crontab -l 2>/dev/null | grep -v '^#' | grep -v '^$' | sed 's/^/  /'
echo ""
echo "Init services in rc5.d:"
ls -1 /etc/rc5.d/S* 2>/dev/null | sed 's|.*/||; s/^/  /'
REMOTE
    echo ""
    info "Deployed script versions:"
    report_script_versions
    echo ""
    exit 0
fi

# ── 1. Provision: the boot scripts, the links, the config fix-ups ─────
echo ""
echo "════════════════════════════════════════"
echo " 1. Provision"
echo "════════════════════════════════════════"

# ⚠️ The decisions are NOT here. Every file, link, mode and config edit lives in
# device-files/provision-rules.conf with a reason per entry, read by this script AND
# by rootfs/commission-offline.sh, so the two cannot drift.
#
# What used to be here: five scp calls, an `ssh <<'REMOTE'` block of ln -sf, a second
# one for avahi, a four-command sed block over sshd_config, and a third for the
# sysctl file — every one of them written out a second time in rootfs/commission-offline.sh.
# They HAD drifted: this path deleted stale rc*.d links before relinking and the
# offline path did not.
#
# The plan is compiled HERE, where the parser lives, and shipped as data the device
# only interprets. The interpreter itself comes
# from arca_provision_online_script, so there is one implementation of each verb and
# `--dry-run` on either path prints the same resolved set.
PROV_RULES="$DEVICE_FILES/provision-rules.conf"
[[ -f "$PROV_RULES" ]] || err "missing $PROV_RULES"
if ! PCHECK="$(arca_provision_validate "$PROV_RULES" "$REPO_ROOT")"; then
    echo "$PCHECK"
    err "device-files/provision-rules.conf does not validate — refusing to provision"
fi

PROV_GROUPS="base"
for _g in $(arca_provision_optional_groups); do
    case " $NO_PROV_GROUPS " in
        *" $_g "*) ;;
        *) PROV_GROUPS="$PROV_GROUPS $_g" ;;
    esac
done
[[ -n "$NO_PROV_GROUPS" ]] && info "Skipping:$NO_PROV_GROUPS"

PROV_PLAN=$(mktemp)
arca_provision_plan "$PROV_RULES" "$PROV_GROUPS" > "$PROV_PLAN" \
    || { rm -f "$PROV_PLAN"; err "could not compile the provision plan"; }
info "Provision plan: $(arca_provision_plan_summary "$PROV_PLAN")"

# ── dry-run: print the plan and stop, before anything is written ────────────
#
# The resolved records this run WOULD apply (install / link / unlink / ...), one per
# line, from the same compiler the real run uses. No backup is taken, nothing is
# copied, the device is not rebooted: the only remote call so far is the SSH probe.
if [[ "$DRY_RUN" == "--dry-run" ]]; then
    echo ""
    sed 's/^/    /' "$PROV_PLAN"
    rm -f "$PROV_PLAN"
    echo ""
    info "Dry run only — nothing backed up, nothing written, device not rebooted."
    exit 0
fi

# ── 0. Back the unit up, before the FIRST write ─────────────────────────────
#
# The unit's per-unit state (settings, calibration, pairings, high scores, host
# keys, authorized_keys) goes to backups/ on this host. A failed backup aborts the
# run: an update that cannot be undone from the archive does not start.
echo ""
echo "════════════════════════════════════════"
echo " 0. Backup"
echo "════════════════════════════════════════"
#
# The engine is device-files/arca-backup, and it is run from THIS repo's copy, scp'd to a
# scratch directory on the unit: a unit that predates the engine does not have
# /usr/sbin/arca-backup (this very run installs it), and a copy from the repo is the one
# whose behaviour the host knows. It reads the repo's item files from the same scratch
# directory (ARCA_BACKUP_ITEMS), so nothing is written outside it before the backup exists.
# Every id is exported, secrets included.
BACKUP_FILE=""
backup_unit() {
    local rd="/tmp/arca-prov-backup.$$" bk="$REPO_ROOT/backups" f ids line out rc=0
    local items=("$REPO_ROOT"/*/backup.d/*.item)
    [[ -f "${items[0]}" ]] || { warn "no */backup.d/*.item in the repo"; return 1; }
    command -v gzip > /dev/null || { warn "gzip not installed on this host"; return 1; }
    mkdir -p "$bk" || return 1
    ssh -n "$DEVICE" "rm -rf $rd; mkdir -p $rd/items" || return 1
    scp -q "$DEVICE_FILES/arca-backup" "$DEVICE:$rd/arca-backup" || rc=1
    [[ $rc -eq 0 ]] && { scp -q "${items[@]}" "$DEVICE:$rd/items/" || rc=1; }
    if [[ $rc -eq 0 ]]; then
        line="$(ssh -n "$DEVICE" "ARCA_ROOT= ARCA_BACKUP_ITEMS=$rd/items sh $rd/arca-backup items")" || rc=1
        ids="$(printf '%s\n' "$line" | awk '$1 == "item" { printf "%s ", $2 }')"
        [[ -n "$ids" ]] || rc=1
    fi
    if [[ $rc -eq 0 ]]; then
        out="$(ssh -n "$DEVICE" "ARCA_ROOT= ARCA_BACKUP_ITEMS=$rd/items sh $rd/arca-backup export $rd/out $ids")" || rc=1
        [[ $rc -eq 0 ]] || printf '%s\n' "$out" | sed -n 's/^error /  backup engine: /p' >&2
    fi
    if [[ $rc -eq 0 ]]; then
        f="$(printf '%s\n' "$out" | sed -n 's/^exported //p' | head -n 1)"
        case "$f" in "$rd"/out/ludbox-backup-*.tar.gz) ;; *) rc=1 ;; esac
    fi
    if [[ $rc -eq 0 ]]; then
        BACKUP_FILE="$bk/${f##*/}"
        if [[ -e "$BACKUP_FILE" ]]; then rc=1
        elif scp -q "$DEVICE:$f" "$BACKUP_FILE.part" && tar -tzf "$BACKUP_FILE.part" > /dev/null; then
            chmod 600 "$BACKUP_FILE.part" 2> /dev/null || true
            mv -- "$BACKUP_FILE.part" "$BACKUP_FILE" || rc=1
        else
            rc=1
        fi
        rm -f "$BACKUP_FILE.part"
    fi
    ssh -n "$DEVICE" "rm -rf $rd" || rc=1
    return "$rc"
}
if ! backup_unit; then
    rm -f "$PROV_PLAN"
    err "backup of $DEVICE failed — nothing was written to the device"
fi
ok "Backup taken: $BACKUP_FILE"

# The install verb's SOURCE bytes are on this host, so they go over scp first and
# the remote interpreter only sets the declared mode. That asymmetry is the whole
# reason the two executors exist; everything else about them is shared.
#
# ⚠️ The loop lives in lib/arca-provision.sh, not here. It was written out in full in
# this script and again in usb_host/build-and-deploy.sh, both reading the plan on
# stdin with an `ssh` in the body — so the first ssh ate the rest of the plan and
# both installed exactly one file. Do not inline it again.
arca_provision_push_installs "$PROV_PLAN" "$REPO_ROOT" "$DEVICE" \
    || { rm -f "$PROV_PLAN"; err "could not copy the provision sources to the device"; }

apply_plan "$PROV_PLAN"
rm -f "$PROV_PLAN"
ok "Boot scripts, boot links, sysctl and the config fix-ups done"

# ── 2. Apply the sysctl settings now ───────────────────────────────────────
#
# NOTE on firewall: this image has no iptables binary, no ip_tables.ko in
# /lib/modules/4.14.52, no busybox iptables applet, no TCP wrappers and no package
# manager to add any of them. The kernel has CONFIG_NETFILTER=y and ip_tables was
# never compiled. Network security is: no unnecessary services, sshd hardened,
# sysctl hardening, and a home network the device is not exposed through.
#
# The FILE was installed by the plan as /etc/sysctl.conf, which rcS.d/S30procps.sh
# applies at every boot; applying it to the running kernel now is again an action.
# No 2>/dev/null: a key this kernel lacks is an error on the boot console too.
info "Applying kernel security settings..."
ssh "$DEVICE" "sysctl -q -p /etc/sysctl.conf"
ok "Kernel security settings applied"

# ── Status summary ──────────────────────────────────────────────────────────
echo ""
echo "════════════════════════════════════════"
echo " Status Summary"
echo "════════════════════════════════════════"

ssh "$DEVICE" bash <<'REMOTE'
echo ""
echo "Disk:    $(df -h / | tail -1 | awk '{print $3 " used, " $4 " free (" $5 " used)"}')"
echo "Memory:  $(free -h | grep Mem | awk '{print $3 " used, " $7 " available"}')"
echo "HW watchdog:      $([ -c /dev/watchdog ] && echo 'active (fed by /usr/sbin/watchdog)' || echo 'n/a')"
echo "Default app:      $(cat /opt/arca/default-app 2>/dev/null || echo '(not set)')"
echo ""
echo "Active cron jobs:"
crontab -l 2>/dev/null | grep -v '^#' | grep -v '^$' | sed 's/^/  /'
REMOTE

# Confirm the init script this run just pushed is byte-identical on the device.
# scp reporting success is not the same as the right bytes landing, and a stale
# init script is silent.
echo ""
info "Deployed script versions:"
report_script_versions

# ── Reboot ──────────────────────────────────────────────────────────────────
echo ""
echo "════════════════════════════════════════"
echo " Rebooting"
echo "════════════════════════════════════════"

info "Rebooting device..."
ssh "$DEVICE" reboot || true
ok "Device is rebooting"

echo ""
echo "  System setup complete! Wait ~30 s then: ssh root@$DEVICE_IP"
echo ""
echo "  Next step — build and deploy all components:"
echo "    ./deploy-all.sh $DEVICE_IP"
echo ""
echo "  Or deploy individually:"
echo "    cd native_apps      && ./build-and-deploy.sh $DEVICE_IP set-default"
echo "    cd vnc_client        && ./build-and-deploy.sh $DEVICE_IP"
echo "    cd scummvm-arca && ./build-and-deploy.sh $DEVICE_IP"
echo ""
