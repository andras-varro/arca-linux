#!/bin/bash
#
# arca_provision_test.sh — regression for lib/arca-provision.sh and
#                        device-files/provision-rules.conf
#
# Host-only, no device, no SD card, no root. Run it:
#
#   wsl.exe -e bash -lc "cd /mnt/c/work/roomwizard && ./tests/arca_provision_test.sh"
#
# Path mapping and the base guard (arca_offline_path, arca_offline_base_ok) are
# tests/arca_identify_test.sh's.
#
# ── What each group of cases is for ────────────────────────────────────────
#
#   A  the parser and the validator. Every check is a thing that would otherwise
#      fail silently and in the permissive direction. The two that matter most:
#      an ABSOLUTE link source (correct on a device, dangling on a card, skipped
#      in silence at boot), and a mode read off disk instead of declared.
#   B  plan compilation and ORDER. Order is the interface: unlink must precede
#      link or the glob eats the link just made; install must precede link or the
#      link dangles on a card.
#   D  the offline executor against a synthetic card, plus the canary: nothing
#      outside the base is touched.
#   E  ⚠️ THE case this file exists for — both executors' --dry-run over the same
#      inputs print the same resolved set. It is the only check that catches the
#      drift between them, and the online executor is exercised through the same
#      interpreter the SSH path pipes to the device, not a re-implementation.
#
# ── Modes cannot be verified here ──────────────────────────────────────────
#
# /mnt/c reports every file 0777 and discards chmod, and WSL's own /tmp does honour
# modes — so group D runs under $(mktemp -d) and CAN assert a mode. What it cannot
# do is prove the DEVICE gets it; that assertion is rootfs/commission-offline.sh's verify
# phase against real ext4.

# Source-path directive: resolves the source= hints below against this script's directory.
# shellcheck source-path=SCRIPTDIR
set -u

# ⚠️ Diagnostics from the stub-log guard in push() must survive a CALL SITE that
# redirects push, and F9 does exactly that (`if push … > "$FW/bad.out" 2>&1`). Sent to
# plain stderr they land in that file, so the guard exits 2 while run-all.sh prints the
# PREVIOUS case as the last line — the harness error becomes invisible at the one site
# that can trigger it. Measured. fd 9 is a copy of the real stderr, taken before any
# redirection, and the guard writes there.
exec 9>&2

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

# shellcheck source=../lib/arca-identify.sh
. "$REPO_DIR/lib/arca-identify.sh"
# shellcheck source=../lib/arca-provision.sh
# $ARCA_PROVISION_LIB points the suite at a staged copy of the library — the hook
# tests/measure_provision_sabotage.sh drives, so a sabotage stages ONE file instead
# of copying a tree out of /mnt/c (which blows a 300 s budget over DrvFs).
. "${ARCA_PROVISION_LIB:-$REPO_DIR/lib/arca-provision.sh}"

RULES="$REPO_DIR/device-files/provision-rules.conf"

RED='\033[0;31m'; GREEN='\033[0;32m'; NC='\033[0m'
PASS=0; FAIL=0
ok()  { PASS=$((PASS + 1)); echo -e "  ${GREEN}pass${NC}  $1"; }
bad() { FAIL=$((FAIL + 1)); echo -e "  ${RED}FAIL${NC}  $1"; }
assert_eq() { if [ "$1" = "$2" ]; then ok "$3"; else bad "$3 (want '$1', got '$2')"; fi; }
exists() { if [ -e "$1" ] || [ -L "$1" ]; then ok "$2"; else bad "$2 — missing: $1"; fi; }
gone()   { if [ -e "$1" ] || [ -L "$1" ]; then bad "$2 — still present: $1"; else ok "$2"; fi; }

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT INT TERM

# ═══════════════════════════════════════════════════════════════════════════
echo ""
echo "A. the data-file parser and validator"
# ═══════════════════════════════════════════════════════════════════════════

if arca_provision_validate "$RULES" >/dev/null; then
    ok "A1 device-files/provision-rules.conf validates"
else
    bad "A1 device-files/provision-rules.conf validates"
    arca_provision_validate "$RULES" | sed 's/^/        /'
fi

RCOUNT=$(arca_provision_parse "$RULES" | grep -c . || true)
if [ "$RCOUNT" -gt 25 ]; then
    ok "A2 the shipped file has $RCOUNT records"
else
    bad "A2 the shipped file has $RCOUNT records (expected > 25 — did the parse drop everything?)"
fi

# reject <body> <description> — a conf that must NOT validate
reject() {
    local body="$1" desc="$2" f="$TMP/reject.conf"
    printf '%s\n' "$body" > "$f"
    if arca_provision_validate "$f" >/dev/null 2>&1; then
        bad "$desc — validate ACCEPTED it"
    else
        ok "$desc"
    fi
}
R() { printf '%s\t%s\t%s\t%s\t%s\t%s' "$1" "$2" "$3" "$4" "$5" "$6"; }

reject "$(printf 'install\tbase\t0755\t/etc/init.d/x\tdevice-files/audio-enable')" \
    "A3 a 5-field line (no reason) is rejected"
reject "$(R install base 0755 /etc/init.d/x device-files/audio-enable '')" \
    "A4 an empty reason field is rejected"
reject "$(R frobnicate base - /etc/x - why)" \
    "A5 an unknown record type is rejected"
reject "$(R install bogus 0755 /etc/init.d/x device-files/audio-enable why)" \
    "A6 an unknown group is rejected"
reject "install base 0755 /etc/init.d/x device-files/audio-enable spaces not tabs" \
    "A7 space-separated fields are rejected"
reject "$(R install base 0755 etc/init.d/x device-files/audio-enable why)" \
    "A8 a relative target is rejected"
reject "$(R install base 0755 /etc/../shadow device-files/audio-enable why)" \
    "A9 a target containing .. is rejected"
reject "$(R install base 0755 / device-files/audio-enable why)" \
    "A10 a target of / is rejected"
reject "$(R install base 0755 /etc/init.d/x/ device-files/audio-enable why)" \
    "A11 a trailing slash is rejected"
reject "" "A12 a file with no records at all is rejected"

# ⚠️ The mode is DECLARED. A missing or malformed one must not be tolerated,
# because it cannot be recovered from disk on this host.
reject "$(R install base - /etc/init.d/x device-files/audio-enable why)" \
    "A13 an install with no declared mode is rejected"
reject "$(R install base rwxr-xr-x /etc/init.d/x device-files/audio-enable why)" \
    "A14 a symbolic mode is rejected — octal only"
reject "$(R install base 0999 /etc/init.d/x device-files/audio-enable why)" \
    "A15 a non-octal digit in a mode is rejected"
reject "$(R link base 0755 /etc/rc5.d/S28x ../init.d/x why)" \
    "A16 a link with a mode is rejected — the field means nothing there"

# ⚠️ THE case: an absolute link source is correct on a device and dangling on a card.
reject "$(R link base - /etc/rc5.d/S28time-sync /etc/init.d/time-sync why)" \
    "A17 an ABSOLUTE link source is rejected (it dangles on a mounted card)"
reject "$(R link-opt base - /etc/rc5.d/S30avahi-daemon /etc/init.d/avahi-daemon why)" \
    "A18 and for link-opt too"

reject "$(R install base 0755 /etc/init.d/x does-not-exist-anywhere why)" \
    "A19 an install whose source is not in the repo is rejected"
reject "$(R install base 0755 /etc/init.d/x /etc/passwd why)" \
    "A20 an absolute install source is rejected — sources are repo-relative"
reject "$(R unlink base - '/etc/rc*.d/S99arca-app' - why)" \
    "A22 a glob outside the last component is rejected (it would silently match nothing)"

# rc0.d / rc6.d are shutdown. Unreachable by construction.
reject "$(R unlink base - /etc/rc6.d/K09sshd - why)" \
    "A23 no rule may name rc6.d — shutdown, not startup"
reject "$(R link base - /etc/rc0.d/S20sendsigs ../init.d/x why)" \
    "A24 nor rc0.d"

# The backup, directive, dropline and touch verbs are deleted: no rule uses them and
# neither executor implements them, so a record naming one must not validate.
reject "$(R backup base - /etc/x.orig /etc/x why)" \
    "A24b the deleted verb backup is rejected"
reject "$(R directive base - /etc/ssh/sshd_config PermitRootLogin=no why)" \
    "A24c the deleted verb directive is rejected"
reject "$(R dropline base - /etc/inittab '^4:' why)" \
    "A24d the deleted verb dropline is rejected"
reject "$(R touch base 0644 /var/x - why)" \
    "A24e the deleted verb touch is rejected"

# Comments, blanks, and a tab inside the reason.
CMT="$TMP/comments.conf"
printf '# c\n\n   \n%s\n' "$(R install base 0644 /var/x - a reason)" > "$CMT"
assert_eq "1" "$(arca_provision_parse "$CMT" | grep -c . || true)" \
    "A25 comments and blank lines are not records"
TABBY="$TMP/tabby.conf"
printf '%s\n' "$(printf 'install\tbase\t0644\t/var/x\t-\ta reason\twith a tab')" > "$TABBY"
assert_eq "install	base	0644	/var/x	-" "$(arca_provision_parse "$TABBY")" \
    "A26 a tab inside the reason does not shift the first five fields"

# ═══════════════════════════════════════════════════════════════════════════
echo ""
echo "B. plan compilation and order"
# ═══════════════════════════════════════════════════════════════════════════

PLAN_ALL=$(arca_provision_plan "$RULES" "$(arca_provision_default_groups)")
if [ -n "$PLAN_ALL" ]; then
    ok "B0 the default plan compiles"
else
    bad "B0 the default plan compiles — it is EMPTY, so every assertion below is meaningless"
fi

has()    { printf '%s\n' "$2" | grep -qxF "$1"; }
expect() { if has "$1" "$2"; then ok "$3"; else bad "$3 — plan has no '$1'"; fi; }
absent() { if has "$1" "$2"; then bad "$3 — plan HAS '$1'"; else ok "$3"; fi; }

expect "$(printf 'install\t0755\t/etc/init.d/audio-enable\tdevice-files/audio-enable')" \
    "$PLAN_ALL" "B1 the audio-enable install, with its declared mode"
expect "$(printf 'install\t0644\t/etc/sysctl.conf\tdevice-files/sysctl.conf')" \
    "$PLAN_ALL" "B2 sysctl.conf is 0644, not 0755"
expect "$(printf 'install\t0755\t/etc/init.d/arca-app\tdevice-files/arca-app')" \
    "$PLAN_ALL" "B3 the init script is installed under a different name from its source"
expect "$(printf 'link\t-\t/etc/rc5.d/S99arca-app\t../init.d/arca-app')" \
    "$PLAN_ALL" "B4 the rc5.d app link"
if printf '%s\n' "$PLAN_ALL" | grep -q 'sshd_config'; then
    bad "B7 no default-group record touches sshd_config — the overlay owns it"
else
    ok "B7 no default-group record touches sshd_config — the overlay owns it"
fi
expect "$(printf 'link-opt\t-\t/etc/rc5.d/S30avahi-daemon\t../init.d/avahi-daemon')" \
    "$PLAN_ALL" "B9 the avahi link is optional, not mandatory"

# PermitRootLogin must NOT be here: root is the only account and there is no serial
# console to recover through if keys break.
if printf '%s\n' "$PLAN_ALL" | grep -q 'PermitRootLogin'; then
    bad "B10 PermitRootLogin is deliberately not a directive"
else
    ok "B10 PermitRootLogin is deliberately not a directive"
fi

# ── Order is the interface ────────────────────────────────────────────────
pos() { printf '%s\n' "$PLAN_ALL" | grep -n "^$1" | head -1 | cut -d: -f1; }
LAST_UNLINK=$(printf '%s\n' "$PLAN_ALL" | grep -n '^unlink' | tail -1 | cut -d: -f1)
FIRST_LINK=$(pos link)
LAST_INSTALL=$(printf '%s\n' "$PLAN_ALL" | grep -n '^install' | tail -1 | cut -d: -f1)

if [ "$LAST_UNLINK" -lt "$FIRST_LINK" ]; then
    ok "B11 every unlink precedes every link — else the glob eats the link just made"
else
    bad "B11 every unlink precedes every link (last unlink $LAST_UNLINK, first link $FIRST_LINK)"
fi
if [ "$LAST_INSTALL" -lt "$FIRST_LINK" ]; then
    ok "B12 every install precedes every link — else the link dangles on a card"
else
    bad "B12 every install precedes every link (last install $LAST_INSTALL, first link $FIRST_LINK)"
fi

# ── Groups ────────────────────────────────────────────────────────────────
PLAN_NOMDNS=$(arca_provision_plan "$RULES" "base usb bluetooth")
absent "$(printf 'link-opt\t-\t/etc/rc5.d/S30avahi-daemon\t../init.d/avahi-daemon')" \
    "$PLAN_NOMDNS" "B14 --no-mdns drops the avahi link"
expect "$(printf 'link\t-\t/etc/rc5.d/S28time-sync\t../init.d/time-sync')" \
    "$PLAN_NOMDNS" "B15 and nothing else"
# The sshd groups are gone: sshd_config is the root overlay's alone.
if arca_provision_plan "$RULES" "base sshd" >/dev/null 2>&1 || arca_provision_plan "$RULES" "base sshd-key" >/dev/null 2>&1; then
    bad "B16 the sshd groups no longer exist — a plan naming one is refused"
else
    ok "B16 the sshd groups no longer exist — a plan naming one is refused"
fi
if arca_provision_plan "$RULES" "mdns usb" >/dev/null 2>&1; then
    bad "B17 a group list without 'base' is refused"
else
    ok "B17 a group list without 'base' is refused"
fi
if arca_provision_plan "$RULES" "base nosuchgroup" >/dev/null 2>&1; then
    bad "B18 an unknown group name is refused"
else
    ok "B18 an unknown group name is refused"
fi

# ── Vendor-era leftovers: units imaged with an older bundle carry these ──────
# Our own root has no vendor watchdog and its syslog init script never sources
# /etc/default/syslogd, so the files are dead weight and are only removed, never made.
for _p in /opt/arca/disable-steelcase.sh /var/watchdog_test /etc/default/syslogd; do
    expect "$(printf 'unlink\t-\t%s\t-' "$_p")" \
        "$PLAN_ALL" "B6 the stale vendor-era file $_p is unlinked"
done

# ── avahi narrowed ───────────────────────────────────────────────────────────expect "$(printf 'install	0644	/etc/avahi/avahi-daemon.conf	device-files/avahi-daemon.conf')" \
expect "$(printf 'install\t0644\t/etc/avahi/avahi-daemon.conf\tdevice-files/avahi-daemon.conf')" \
    "$PLAN_ALL" "B20 avahi-daemon.conf is installed with the mdns group"
absent "$(printf 'install\t0644\t/etc/avahi/avahi-daemon.conf\tdevice-files/avahi-daemon.conf')" \
    "$PLAN_NOMDNS" "B21 --no-mdns leaves the vendor avahi config alone"


# avahi-daemon.conf is INI; a commented key is NOT set — the vendor file ships
# "#allow-interfaces=eth0", so a grep for the key alone would pass on it.
ini() {   # FILE SECTION KEY -> value, empty if unset
    awk -F= -v s="[$2]" -v k="$3" '
        /^[[:space:]]*\[/ { sec = $0; gsub(/[[:space:]]/, "", sec); next }
        sec == s && $1 == k { v = $2; sub(/^[[:space:]]+/, "", v); sub(/[[:space:]]+$/, "", v); val = v }
        END { print val }' "$1"
}
printf '[server]\n#allow-interfaces=eth0\nuse-ipv4=yes\n' > "$TMP/ini-control"
assert_eq "" "$(ini "$TMP/ini-control" server allow-interfaces)" \
    "B24 control: the INI reader does not read a commented key as set"
assert_eq "yes" "$(ini "$TMP/ini-control" server use-ipv4)" \
    "B24b control: and does read a set one"
AV="$REPO_DIR/device-files/avahi-daemon.conf"
if [ -f "$AV" ]; then
    assert_eq "eth0" "$(ini "$AV" server allow-interfaces)" "B25 avahi answers on eth0 only"
    assert_eq "no"   "$(ini "$AV" server use-ipv6)"         "B26 and over IPv4 only"
    assert_eq "no"   "$(ini "$AV" wide-area enable-wide-area)" \
        "B27 wide-area off — the second, random-port UDP socket avahi opened"
    assert_eq "no"   "$(ini "$AV" reflector enable-reflector)" "B28 no reflector, explicitly"
    # mDNS must keep working: provision.sh and the operator reach units as <name>.local.
    assert_eq "yes"  "$(ini "$AV" server use-ipv4)"         "B29 mDNS over IPv4 stays on"
    if [ "$(ini "$AV" publish disable-publishing)" != yes ] && [ "$(ini "$AV" publish publish-addresses)" != no ]; then
        ok  "B30 the host's A record is still published — <name>.local resolves"
    else
        bad "B30 the host's A record is still published — <name>.local resolves"
    fi
else
    bad "B25 device-files/avahi-daemon.conf exists — B25-B30 are vacuous without it"
fi

# ═══════════════════════════════════════════════════════════════════════════
echo ""
echo "D. the offline executor, against a synthetic card"
# ═══════════════════════════════════════════════════════════════════════════

CARD="$TMP/card"
build_card() {
    rm -rf "$CARD"
    mkdir -p "$CARD/root/etc/init.d" "$CARD/root/etc/rc2.d" "$CARD/root/etc/rc3.d" \
             "$CARD/root/etc/rc4.d" "$CARD/root/etc/rc5.d" "$CARD/root/etc/ssh" \
             "$CARD/root/etc/sysctl.d" "$CARD/root/usr/sbin" "$CARD/root/var" \
             "$CARD/log" "$CARD/backup"
    # The vendor's sshd_config, with the factory default this hardening exists for.
    printf 'PermitRootLogin yes\nPermitEmptyPasswords yes\n' > "$CARD/root/etc/ssh/sshd_config"
    # /etc/profile as measured on both card captures.
    printf 'export PATH=/usr/bin\n. /home/root/data/websign/wsplatform.conf\numask 022\n' \
        > "$CARD/root/etc/profile"
    printf '::sysinit:/etc/init.d/rcS\n4:12345:respawn:/sbin/getty 38400 tty4\n' \
        > "$CARD/root/etc/inittab"
    # avahi, so link-opt has something to resolve to.
    : > "$CARD/root/etc/init.d/avahi-daemon"; chmod +x "$CARD/root/etc/init.d/avahi-daemon"
    : > "$CARD/root/usr/sbin/avahi-daemon"
    # Stale links from an older setup run — the drift this file fixes.
    ln -sf ../init.d/arca-app   "$CARD/root/etc/rc5.d/S50arca-app"
}

# The canary: laid out like a root and OUTSIDE the base, so a prefix bug lands
# somewhere visible instead of only showing up as a passing test.
CANARY="$TMP/canary"
mkdir -p "$CANARY/etc/init.d" "$CANARY/etc/rc5.d" "$CANARY/var"
echo "root:x:0:0" > "$CANARY/etc/shadow"
printf 'PermitEmptyPasswords yes\n' > "$CANARY/etc/ssh_config_lookalike"
CANARY_MD5=$(cd "$CANARY" && find . -type f | LC_ALL=C sort | xargs md5sum | md5sum)

build_card
arca_provision_plan "$RULES" "$(arca_provision_default_groups)" > "$TMP/plan"

# ── the dry run must change nothing ──────────────────────────────────────
CARD_MD5=$(cd "$CARD" && find . | LC_ALL=C sort | md5sum)
DRY_OUT=$(ARCA_PROVISION_DRY=1 arca_provision_apply_offline "$CARD" "$TMP/plan" "$REPO_DIR")
assert_eq "$CARD_MD5" "$(cd "$CARD" && find . | LC_ALL=C sort | md5sum)" \
    "D1 --dry-run changes nothing at all"
DRYLINES=$(printf '%s\n' "$DRY_OUT" | grep -c 'would ' || true)
if [ "$DRYLINES" -gt 20 ]; then
    ok "D2 --dry-run printed $DRYLINES actions"
else
    bad "D2 --dry-run printed only $DRYLINES actions (expected > 20)"
fi

# ── for real ─────────────────────────────────────────────────────────────
build_card
arca_provision_apply_offline "$CARD" "$TMP/plan" "$REPO_DIR" > "$TMP/apply.out" 2>&1 \
    || bad "D3 arca_provision_apply_offline returned non-zero"

exists "$CARD/root/etc/init.d/audio-enable"          "D4 audio-enable installed"
exists "$CARD/root/etc/init.d/time-sync"             "D5 time-sync installed"
exists "$CARD/root/etc/sysctl.conf"    "D6 sysctl.conf installed"
exists "$CARD/root/etc/init.d/arca-app"        "D7 the init script installed under its DEPLOYED name"

# The bytes must be the repo's, not a heredoc's idea of them.
assert_eq "$(md5sum < "$REPO_DIR/device-files/audio-enable")" \
          "$(md5sum < "$CARD/root/etc/init.d/audio-enable")" \
    "D9 audio-enable is byte-for-byte the repo's copy"
assert_eq "$(md5sum < "$REPO_DIR/device-files/arca-app")" \
          "$(md5sum < "$CARD/root/etc/init.d/arca-app")" \
    "D10 and so is the init script, despite the rename"

# Modes. WSL's /tmp honours them; /mnt/c would not.
assert_eq "755" "$(stat -c %a "$CARD/root/etc/init.d/audio-enable")" "D11 audio-enable is 0755"
assert_eq "644" "$(stat -c %a "$CARD/root/etc/sysctl.conf")" "D12 sysctl.conf is 0644, as declared"

# Links, and that they RESOLVE — a dangling rc5.d link is skipped in silence.
for l in rc5.d/S28time-sync rc5.d/S29audio-enable rc5.d/S99arca-app; do
    if [ -L "$CARD/root/etc/$l" ] && [ -e "$CARD/root/etc/$l" ]; then
        ok "D14.$(basename "$l") $l links and resolves"
    else
        bad "D14 $l — $( [ -L "$CARD/root/etc/$l" ] && echo 'DANGLING' || echo 'missing')"
    fi
done
if [ -L "$CARD/root/etc/rc5.d/S30avahi-daemon" ] && [ -e "$CARD/root/etc/rc5.d/S30avahi-daemon" ]; then
    ok "D15 the avahi link is made when the image has avahi"
else
    bad "D15 the avahi link is made when the image has avahi"
fi

# The drift: stale links gone.
gone "$CARD/root/etc/rc5.d/S50arca-app"   "D17 and the wrong-priority copy of our own link"

# /etc/default and /etc/avahi are not in build_card: the installer must create them.
for pair in "etc/avahi/avahi-daemon.conf:avahi-daemon.conf"; do
    t="${pair%%:*}"; s="${pair#*:}"
    if [ -f "$CARD/root/$t" ] && cmp -s "$REPO_DIR/device-files/$s" "$CARD/root/$t"; then
        ok "D19b /$t is byte-for-byte device-files/$s"
    else
        bad "D19b /$t is byte-for-byte device-files/$s"
    fi
    assert_eq "644" "$(stat -c %a "$CARD/root/$t" 2>/dev/null)" "D19c /$t is 0644"
done

# ── link-opt on an image WITHOUT avahi: skip, do not dangle ──────────────
build_card
rm -f "$CARD/root/etc/init.d/avahi-daemon"
arca_provision_apply_offline "$CARD" "$TMP/plan" "$REPO_DIR" >/dev/null 2>&1
if [ -L "$CARD/root/etc/rc5.d/S30avahi-daemon" ]; then
    bad "D32 link-opt must NOT create a dangling link on an image without avahi"
else
    ok "D32 link-opt skips when its target is absent, rather than dangling"
fi
exists "$CARD/root/etc/rc5.d/S99arca-app" "D33 and the rest of the plan still ran"

# ── the guard, and the canary ────────────────────────────────────────────
if arca_provision_apply_offline "" "$TMP/plan" "$REPO_DIR" >/dev/null 2>&1; then
    bad "D34 an empty base is refused"
else
    ok "D34 an empty base is refused"
fi
if arca_provision_apply_offline "/" "$TMP/plan" "$REPO_DIR" >/dev/null 2>&1; then
    bad "D35 a base of / is refused — that is this host's root"
else
    ok "D35 a base of / is refused — that is this host's root"
fi
assert_eq "$CANARY_MD5" "$(cd "$CANARY" && find . -type f | LC_ALL=C sort | xargs md5sum | md5sum)" \
    "D36 the canary tree outside the base is byte-for-byte unchanged"

# ═══════════════════════════════════════════════════════════════════════════
echo ""
echo "E. ⚠️ both executors' dry runs print the same resolved set"
echo "   (the only check that catches drift between the two executors)"
# ═══════════════════════════════════════════════════════════════════════════

build_card
OFF=$(ARCA_PROVISION_DRY=1 arca_provision_apply_offline "$CARD" "$TMP/plan" "$REPO_DIR" \
        | arca_provision_canonical)

# The ONLINE executor, run here as the device would run it: arca_provision_online_script
# emits the interpreter that provision.sh pipes to `ssh <target> sh -s`, and it is
# run against a chroot-shaped copy so nothing touches this host. That is what makes
# this a comparison of the two EXECUTORS rather than of one executor and a wish.
ONLINE_ROOT="$TMP/online"
rm -rf "$ONLINE_ROOT"; cp -a "$CARD/root" "$ONLINE_ROOT"
ON=$(ARCA_PROVISION_DRY=1 ARCA_PROVISION_ROOT="$ONLINE_ROOT" \
        sh -c "$(arca_provision_online_script)" -- "$TMP/plan" | arca_provision_canonical)

if [ -n "$OFF" ] && [ -n "$ON" ]; then
    ok "E1 both dry runs produced output"
else
    bad "E1 both dry runs produced output (offline $(printf '%s' "$OFF" | wc -l) lines, online $(printf '%s' "$ON" | wc -l))"
fi
if [ "$OFF" = "$ON" ]; then
    ok "E2 the two resolved sets are IDENTICAL, modulo the prefix"
else
    bad "E2 the two resolved sets are identical, modulo the prefix"
    diff <(printf '%s\n' "$OFF") <(printf '%s\n' "$ON") | head -20 | sed 's/^/        /'
fi
NREC=$(grep -cve '^[[:space:]]*#' -e '^[[:space:]]*$' "$TMP/plan" || true)
assert_eq "$NREC" "$(printf '%s\n' "$OFF" | grep -c . || true)" \
    "E3 and the offline set covers every record in the plan, not a subset"

# The negative control on E2 itself: if one executor drops a verb, E2 must fire.
SHORT=$(grep -v '^unlink' "$TMP/plan")
printf '%s\n' "$SHORT" > "$TMP/plan.short"
OFF2=$(ARCA_PROVISION_DRY=1 arca_provision_apply_offline "$CARD" "$TMP/plan.short" "$REPO_DIR" \
        | arca_provision_canonical)
if [ "$OFF2" = "$ON" ]; then
    bad "E4 E2 would notice a dropped verb"
else
    ok "E4 E2 would notice a dropped verb"
fi

# ═══════════════════════════════════════════════════════════════════════════
echo ""
echo "F. the online path's copy step — the one-of-eight install defect"
# ═══════════════════════════════════════════════════════════════════════════
#
# ⚠️ Group E cannot reach this, and the gap is structural rather than an oversight.
# E compares two --dry-run PLANS; a dry run copies nothing, so the scp step is
# precisely the part of the online path that has no offline counterpart to be
# compared against. That is how the defect shipped past 94 passing cases:
# provision.sh installed 1 of its 8 files, because `ssh` inside
# `while read … done < "$PLAN"` reads its own stdin and forwarded the whole rest of
# the plan to the remote `mkdir`.
#
# So this group runs the REAL arca_provision_push_installs against a local directory
# through the $ARCA_SSH/$ARCA_SCP stubs lib/arca-bundle.sh also honours, and
# asserts 8 of 8. **Never "more than one"** — the defect produced exactly one, so
# any threshold below the full count passes on the broken tree.
#
# The got-vs-want guard inside the function is not reachable from here (it needs a
# loop that actually loses records); its negative control is
# tests/measure_provision_sabotage.sh, which puts the plan back on stdin.

FW="$TMP/push"; mkdir -p "$FW/bin" "$FW/dev"

# ⚠️ The stub ssh MUST slurp its stdin. That is the behaviour that caused the defect, so a
# stub that skips it cannot fail the way production failed and every case below
# becomes a vacuous pass. F1 is the check that this one can.
cat > "$FW/bin/ssh" <<'STUB'
#!/bin/sh
shift                                  # the target
cat > /dev/null                        # what a real ssh does with a non-tty stdin
echo "$*" >> "$STUBLOG/ssh.calls"
d=$(printf '%s' "$*" | sed -n "s/^mkdir -p '\(.*\)'$/\1/p")
[ -n "$d" ] && mkdir -p "$STUBDEV$d"
exit 0
STUB
cat > "$FW/bin/scp" <<'STUB'
#!/bin/sh
[ "$1" = "-q" ] && shift
src=$1; dst=$2; p=${dst#*:}
# Faithful to scp: it does NOT create the parent, it fails. So a dropped mkdir -p
# is measurable here rather than papered over by the stub.
[ -d "$STUBDEV${p%/*}" ] || { echo "scp: $p: No such file or directory" >&2; exit 1; }
cp "$src" "$STUBDEV$p" || exit 1
echo "$src -> $p" >> "$STUBLOG/scp.calls"
STUB
chmod +x "$FW/bin/ssh" "$FW/bin/scp"
export STUBLOG="$FW" STUBDEV="$FW/dev"
reset_stubs() { : > "$FW/ssh.calls"; : > "$FW/scp.calls"; rm -rf "$FW/dev"; mkdir -p "$FW/dev"; }
push() {
    ARCA_SSH="$FW/bin/ssh" ARCA_SCP="$FW/bin/scp" arca_provision_push_installs "$@"
    local rc=$?
    # ⚠️ Every assertion in this group counts lines in the two stub call logs, and the
    # NEGATIVE one — F10, "it refused before running anything on the device" — is
    # VACUOUS if a log is absent. It does not even fail cleanly: on a missing file
    # `grep -c .` writes nothing to stdout, so
    #     $(( $(grep -c . <absent>) + $(grep -c . <absent>) ))
    # collapses to `$(( + ))`, a bash SYNTAX error whose value is the empty string —
    # and the assertion then reports `want '0', got ''`, a missing fixture wearing a
    # subject defect's clothes. That is not hypothetical: a whole plan entry was
    # opened against this suite on the strength of such a failure, and the suite was
    # fine. So refuse to judge rather than judge on absent evidence. Exit 2 is the
    # harness-error channel tests/run-all.sh reports apart from BOTH pass and fail.
    local f
    for f in "$FW/ssh.calls" "$FW/scp.calls"; do
        [ -f "$f" ] && continue
        echo "HARNESS: a stub call log is absent after a push, so reset_stubs did not run." >&9
        echo "HARNESS: every call-count assertion here would be vacuous. Not judging." >&9
        echo "HARNESS: missing stub call log: $f" >&9
        exit 2
    done
    return $rc
}

PPLAN="$FW/plan"
arca_provision_plan "$RULES" "base usb" > "$PPLAN" || bad "F0 the plan compiles"
NINST=$(awk -F'\t' '$1 == "install"' "$PPLAN" | wc -l | tr -d ' ')
if [ "$NINST" -ge 8 ]; then
    ok "F0 the shipped plan has $NINST install record(s)"
else
    bad "F0 the shipped plan has $NINST install record(s) — expected >= 8, this group is vacuous below that"
fi

# ── F1: the harness can reproduce the defect ─────────────────────────────────
reset_stubs
(
    PATH="$FW/bin:$PATH"; D=root@fake
    while IFS=$'\t' read -r k m t s; do
        [ "$k" = install ] || continue
        ssh "$D" "mkdir -p '${t%/*}'"
        scp -q "$REPO_DIR/$s" "$D:$t"
    done < "$PPLAN"
) >/dev/null 2>&1
assert_eq 1 "$(grep -c . "$FW/scp.calls" || :)" \
    "F1 the pre-fix shape (plan on stdin) copies exactly 1 — the stubs do reproduce B28"

# ── F2-F8: the real function ─────────────────────────────────────────────────
reset_stubs
if push "$PPLAN" "$REPO_DIR" root@fake > "$FW/out" 2>&1; then
    ok "F2 arca_provision_push_installs succeeds over the shipped plan"
else
    bad "F2 arca_provision_push_installs succeeds over the shipped plan"
    sed 's/^/        /' "$FW/out"
fi
assert_eq "$NINST" "$(grep -c . "$FW/scp.calls" || :)" \
    "F3 all $NINST install records were copied, not 1"
assert_eq "$NINST" "$(grep -c '^  copied' "$FW/out" || :)" \
    "F4 one reported copied line per install record"

MISS=0; DIFFER=0
while IFS=$'\t' read -r k m t s; do
    [ "$k" = install ] || continue
    if [ ! -f "$FW/dev$t" ]; then MISS=$((MISS + 1)); continue; fi
    cmp -s "$REPO_DIR/$s" "$FW/dev$t" || DIFFER=$((DIFFER + 1))
done < "$PPLAN"
assert_eq 0 "$MISS"   "F5 every install target exists on the fake device"
assert_eq 0 "$DIFFER" "F6 every installed file is byte-identical to its device-files source"

assert_eq "$NINST" "$(grep -c '^mkdir -p' "$FW/ssh.calls" || :)" \
    "F7 a mkdir -p preceded every copy"
# The directory that may not exist on a unit — /etc/init.d does, so it proves nothing.
NODIR=0
grep -q "mkdir -p '/usr/local/bin'" "$FW/ssh.calls" || NODIR=1
assert_eq 0 "$NODIR" "F8 the directory a unit may lack is created first"

# ── F9-F11: a missing source is a refusal, before the device is touched ──────
reset_stubs
printf 'install\t0755\t/etc/init.d/x\tdevice-files/does-not-exist\n' > "$FW/badplan"
if push "$FW/badplan" "$REPO_DIR" root@fake > "$FW/bad.out" 2>&1; then
    bad "F9 a missing install source is refused"
else
    ok "F9 a missing install source is refused"
fi
# ⚠️ "It returned non-zero" is NOT the measurement: scp fails on a missing source
# anyway, so deleting the check entirely still refuses. What the check buys is that
# nothing ran on the device first and that the message names the host-side path —
# so those are what F10 and F11 assert.
assert_eq 0 "$(( $(grep -c . "$FW/scp.calls" || :) + $(grep -c . "$FW/ssh.calls" || :) ))" \
    "F10 it refused before running anything on the device"
if grep -q 'device-files/does-not-exist' "$FW/bad.out"; then
    ok "F11 the refusal names the missing host-side source"
else
    bad "F11 the refusal names the missing host-side source"
fi

# ── F12: the other caller's shape — usb_host/build-and-deploy.sh ─────────────
UPLAN="$FW/usbplan"
arca_provision_plan_component "$RULES" usb > "$UPLAN" || bad "F12 the usb component plan compiles"
NU=$(awk -F'\t' '$1 == "install"' "$UPLAN" | wc -l | tr -d ' ')
reset_stubs
push "$UPLAN" "$REPO_DIR" root@fake >/dev/null 2>&1
if [ "$NU" -ge 2 ]; then
    assert_eq "$NU" "$(grep -c . "$FW/scp.calls" || :)" \
        "F12 the usb-group plan copies all $NU of its install records"
else
    bad "F12 the usb-group plan has only $NU install record(s) — nothing to measure"
fi

# ── F12b: the bluetooth component's group ─────────────────────────────────────
# bluetooth/build-and-deploy.sh compiles its own group the same way. Its boot link
# must come out of the plan (S91, after S90usb-host: the dongle is on that port) and
# every one of its verbatim files must be copied.
BPLAN="$FW/btplan"
if arca_provision_plan_component "$RULES" bluetooth > "$BPLAN" 2>/dev/null; then
    ok "F12b the bluetooth component plan compiles"
else
    bad "F12b the bluetooth component plan compiles"
fi
assert_eq "6" "$(awk -F'\t' '$1 == "install"' "$BPLAN" | wc -l | tr -d ' ')" \
    "F12b the bluetooth group installs the init script, two dbus policies, main.conf, input.conf and 20-bluealsa.conf"
# CVE-2023-45866: BlueZ 5.66 accepts HID input from an unbonded device unless input.conf
# says otherwise (the default flipped only in 5.71). The record and the uncommented key
# are both needed — a commented-out key is 5.66's own shipped file, which protects nothing.
BT_INPUT_SRC="$(awk -F'\t' '$1 == "install" && $3 == "/etc/bluetooth/input.conf" {print $4}' "$BPLAN")"
assert_eq "device-files/bluetooth-input.conf" "$BT_INPUT_SRC" \
    "F12b the bluetooth group installs /etc/bluetooth/input.conf from device-files/bluetooth-input.conf"
if [[ -n "$BT_INPUT_SRC" ]] && awk '/^\[/ {s=$0} s == "[General]" && /^ClassicBondedOnly=true[[:space:]]*$/ {f=1} END {exit !f}' "$REPO_DIR/$BT_INPUT_SRC" 2>/dev/null; then
    ok "F12b input.conf sets [General] ClassicBondedOnly=true (HID only from bonded devices)"
else
    bad "F12b input.conf sets [General] ClassicBondedOnly=true (HID only from bonded devices)"
fi
assert_eq "../init.d/bluetooth" "$(awk -F'\t' '$1 == "link" && $3 == "/etc/rc5.d/S91bluetooth" {print $4}' "$BPLAN")" \
    "F12b the bluetooth group links S91bluetooth after S90usb-host"

# ── F12c: which bonded devices the init script reconnects ─────────────────────
# BlueZ 5.66 initiates no BR/EDR connection when an adapter powers up, so the init
# script's watcher connects the bonded devices itself, at boot and whenever hci0 is
# re-created. Its list comes from bluetoothd's storage: one adapter's devices that are
# Trusted, not Blocked, and hold a link key. Anything else must stay off it — an
# untrusted or unbonded device would be paged every time, and a device bonded to another
# dongle is not reachable through this one.
BTS="$FW/btstore"; BTA="A0:AD:9F:70:DD:CA"
bt_info() { # <adapter> <device> <general lines> [linkkey]
    mkdir -p "$BTS/$1/$2"
    { printf '[General]\nName=x\n%b\n' "$3"
      [ -z "${4:-}" ] || printf '\n[LinkKey]\nKey=00\nType=4\n'
    } > "$BTS/$1/$2/info"
}
bt_info "$BTA" 11:11:11:11:11:01 'Trusted=true\nBlocked=false' key
bt_info "$BTA" 11:11:11:11:11:02 'Trusted=false\nBlocked=false' key
bt_info "$BTA" 11:11:11:11:11:03 'Trusted=true\nBlocked=false'
bt_info "$BTA" 11:11:11:11:11:04 'Trusted=true\nBlocked=true' key
bt_info "$BTA" 11:11:11:11:11:05 'Trusted=true' key
bt_info 00:00:00:00:00:99 11:11:11:11:11:06 'Trusted=true\nBlocked=false' key
mkdir -p "$BTS/$BTA/cache/11:11:11:11:11:07"; : > "$BTS/$BTA/settings"
BT_CAND=$(BT_STORAGE="$BTS" sh "$REPO_DIR/device-files/bluetooth" candidates "$BTA" 2>/dev/null </dev/null | sort | tr '\n' ' ')
assert_eq "11:11:11:11:11:01 11:11:11:11:11:05 " "$BT_CAND" \
    "F12c the reconnect list is the adapter's trusted, unblocked, bonded devices only"
BT_NONE=$(BT_STORAGE="$BTS" sh "$REPO_DIR/device-files/bluetooth" candidates 22:22:22:22:22:22 2>/dev/null </dev/null)
assert_eq "" "$BT_NONE" "F12c control: an adapter with no storage reconnects nothing"

# ── F13-F14: the summary line accounts for every action ──────────────────────
#
# A hand-rolled summary once listed 27 of 35 actions; the breakdown must add up to the
# total, or a verb nobody is executing hides in the arithmetic.
SUM=$(arca_provision_plan_summary "$PPLAN")
STOT=$(printf '%s' "$SUM" | sed 's/ action.*//')
SADD=$(printf '%s' "$SUM" | sed 's/^.*— //' | tr ',' '\n' | awk '{s += $1} END {print s + 0}')
assert_eq "$(grep -c . "$PPLAN")" "$STOT" "F13 the summary's total equals the plan's line count"
assert_eq "$STOT" "$SADD" "F14 the summary's per-type counts add up to its total"

# F14 sees only the types the shipped plan happens to contain — link-opt lives in the
# mdns group, so a summary that forgot it passed F14. One record of every known type,
# plus one this library has never heard of, makes the subject independent of the rules.
SYNPLAN="$FW/summary-every-type.plan"
: > "$SYNPLAN"
for t in $ARCA_PROVISION_TYPES frob; do printf '%s\t-\t/x/%s\t-\n' "$t" "$t" >> "$SYNPLAN"; done
SYNSUM=$(arca_provision_plan_summary "$SYNPLAN")
SYNMISS=""
for t in $ARCA_PROVISION_TYPES frob; do
    printf '%s' "$SYNSUM" | grep -qE "(— |, )1 $t(,|\$)" || SYNMISS="$SYNMISS $t"
done
assert_eq "" "$SYNMISS" "F15 the summary names every record type, including one it does not know"

# ═══════════════════════════════════════════════════════════════════════════
echo ""
echo "G. sshd policy is not this library's: the --ssh-auth machinery is gone"
# ═══════════════════════════════════════════════════════════════════════════
for fn in arca_provision_plan_sshd arca_provision_ssh_auth_group arca_provision_ssh_auth_default arca_sshd_guard_script; do
    if declare -F "$fn" >/dev/null; then bad "G1 $fn no longer exists"; else ok "G1 $fn no longer exists"; fi
done
if [ -e "$REPO_DIR/lib/arca-sshd.sh" ]; then bad "G2 lib/arca-sshd.sh is deleted"; else ok "G2 lib/arca-sshd.sh is deleted"; fi
if grep -qE '^[a-z-]+[[:space:]]+sshd' "$RULES"; then bad "G3 no record in provision-rules.conf names an sshd group"; else ok "G3 no record in provision-rules.conf names an sshd group"; fi


# ═══════════════════════════════════════════════════════════════════════════
echo ""
echo "════════════════════════════════════════"
TOTAL=$((PASS + FAIL))
echo "  $PASS passed, $FAIL failed, $TOTAL total"
if [ "$TOTAL" -lt 70 ]; then
    echo -e "  ${RED}✗ only $TOTAL cases ran — the harness itself is broken${NC}"
    exit 1
fi
if [ "$FAIL" -gt 0 ]; then
    echo -e "  ${RED}✗ $FAIL failure(s)${NC}"
    exit 1
fi
echo -e "  ${GREEN}✓ all $TOTAL cases passed${NC}"
echo ""
exit 0
