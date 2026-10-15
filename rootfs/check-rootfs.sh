#!/bin/bash
# check-rootfs.sh - Static check of the boot-critical layout of a built root filesystem tarball.
# Reads the archive with tar only: no root, no mount, nothing is extracted to disk.
# It cannot say whether the image boots; it says the files init will look for are there.
#
# Usage: rootfs/check-rootfs.sh <rootfs.tar>
# Exit status: 0 when every check passes, 1 when any fails, 2 on a usage error.

set -u

usage() {
    cat <<'USAGE'
Usage: rootfs/check-rootfs.sh <rootfs.tar>

Prints one PASS or FAIL line per check and a summary. Exits non-zero if any check fails.
USAGE
}

case "${1:-}" in
    -h|--help) usage; exit 0 ;;
    "") usage >&2; exit 2 ;;
esac
TAR=$1
[ -f "$TAR" ] || { echo "no such file: $TAR" >&2; exit 2; }

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
LIST="$WORK/list"
tar -tvf "$TAR" > "$LIST" || { echo "cannot read $TAR" >&2; exit 2; }
[ -s "$LIST" ] || { echo "empty archive: $TAR" >&2; exit 2; }

pass=0
fail=0
check() { # <description> <exit status of the test>
    if [ "$2" -eq 0 ]; then
        pass=$((pass + 1)); echo "PASS  $1"
    else
        fail=$((fail + 1)); echo "FAIL  $1"
    fi
}

# Normalise a member name: strip the leading "./" and any trailing "/".
# tar -tvf lines: <mode> <owner/group> <size> <date> <time> <name>[ -> <target>]
entry() { # <path without leading ./>  -> prints the listing line, status 1 if absent
    awk -v p="./$1" '{
        name = $0
        sub(/^[^ ]+ +[^ ]+ +[0-9]+ +[^ ]+ +[^ ]+ +/, "", name)
        sub(/ -> .*/, "", name)
        sub(/\/$/, "", name)
        if (name == p) { print; found = 1; exit }
    } END { exit found ? 0 : 1 }' "$LIST"
}
has() { entry "$1" > /dev/null; }
target_of() { entry "$1" | sed -n 's/.* -> //p'; }
member() { tar -xOf "$TAR" "./$1" 2>/dev/null; }
# member_has <path> <fixed string>
member_has() { member "$1" | grep -qF -- "$2"; }

# A link in an rc directory must be a symlink whose target, resolved inside /etc/<dir>,
# is an executable regular file in /etc/init.d.
check_links() { # <dir>
    d=$1
    n=0
    bad=0
    while read -r name; do
        n=$((n + 1))
        t=$(target_of "etc/$d/$name")
        case "$t" in
            ../init.d/*) ;;
            *) echo "      etc/$d/$name -> '$t' is not a ../init.d link"; bad=1; continue ;;
        esac
        line=$(entry "etc/init.d/${t#../init.d/}") || { echo "      etc/$d/$name: no ${t#../init.d/}"; bad=1; continue; }
        case "$line" in
            -??x*) ;;
            *) echo "      etc/$d/$name: ${t#../init.d/} is not an executable file"; bad=1 ;;
        esac
    done <<EOF
$(awk -v p="./etc/$d/" '{ name = $0; sub(/^[^ ]+ +[^ ]+ +[0-9]+ +[^ ]+ +[^ ]+ +/, "", name); sub(/ -> .*/, "", name);
        if (index(name, p) == 1 && name != p) print substr(name, length(p) + 1) }' "$LIST")
EOF
    [ "$n" -gt 0 ] && [ "$bad" -eq 0 ]
}

# inittab
for want in '::sysinit:/etc/init.d/rcS' '::wait:/etc/init.d/rc 5' '::shutdown:/etc/init.d/rc 0' \
            '::restart:/sbin/init' '::ctrlaltdel:/sbin/reboot' \
            'ttyO1::respawn:/sbin/getty -L 115200 ttyO1 vt102'; do
    member etc/inittab | grep -qxF -- "$want"
    check "inittab has: $want" $?
done

# the runlevel directories
for d in rcS.d rc5.d rc0.d rc6.d; do
    check_links "$d"
    check "every etc/$d link resolves to an executable init.d script (and there is at least one)" $?
done
for want in rcS.d/S01mountall rcS.d/S04firstboot-hostname rcS.d/S04hostname rcS.d/S06watchdog rcS.d/S08networking rc5.d/S02dbus-1 rc5.d/S09sshd rc5.d/S30avahi-daemon rc5.d/S20hwclock.sh; do
    has "etc/$want"
    check "etc/$want exists" $?
done
has usr/sbin/set-hostname && entry usr/sbin/set-hostname | grep -q '^-..x'
check "usr/sbin/set-hostname present and executable (the Control Panel and first boot exec it)" $?
member_has usr/sbin/set-hostname 'is not a valid host name'
check "usr/sbin/set-hostname is the overlay script (carries its name validation)" $?
member_has etc/init.d/sshd 'sshd_off'
check "init.d/sshd honours the /etc/ssh/sshd_off marker" $?
! tar -tf "$TAR" | grep -E '(^|/)\.ssh/.|(^|/)authorized_keys' | grep -q .
check "no SSH key material in the tar (no file under any .ssh, no authorized_keys)" $?
has etc/init.d/avahi-daemon
check "etc/init.d/avahi-daemon exists" $?

# Buildroot's own boot scripts must be gone.
n=$(awk '{ name = $0; sub(/^[^ ]+ +[^ ]+ +[0-9]+ +[^ ]+ +[^ ]+ +/, "", name); sub(/ -> .*/, "", name);
           if (name ~ /^\.\/etc\/init\.d\/(S[0-9][0-9]|rcK)/) c++ } END { print c + 0 }' "$LIST")
[ "$n" -eq 0 ]; check "no Buildroot S??*/rcK left in etc/init.d (found $n)" $?

# The product name: post-build.sh replaces Buildroot's generated os-release with ours.
[ "$(target_of etc/os-release)" = "../usr/lib/os-release" ]; check "etc/os-release links to ../usr/lib/os-release" $?
member usr/lib/os-release | grep -q '^NAME=' && ! member usr/lib/os-release | grep -q '^NAME="\?Buildroot'
check "usr/lib/os-release has a NAME= that is not Buildroot's" $?

has lib/ld-linux.so.3
check "lib/ld-linux.so.3 present" $?
# The deploy scripts run their remote halves under bash; BusyBox ships no bash applet.
has bin/bash
check "bin/bash present" $?

# fstab: p2 is the log partition; /home/root/data is a plain directory on p3 (root), not a mount
member etc/fstab | tr -s ' \t' '  ' | grep -q "^/dev/mmcblk0p2 /home/root/log "
check "fstab mounts /dev/mmcblk0p2 /home/root/log" $?
! member etc/fstab | grep -v '^#' | grep -q '/home/root/data'
check "fstab has no line for /home/root/data (a directory on root)" $?
! member etc/fstab | grep -qE 'mmcblk0p(3|5|6|7)'
check "fstab mounts no p3/p5/p6/p7" $?
! member etc/fstab | grep -q 'mmcblk0p5'
check "fstab does not mount p5 (kept formatted but unmounted)" $?
! member etc/init.d/sshd | grep -q 'mountpoint'
check "init.d/sshd does not use mountpoint (keys always on root)" $?

# /var/log is the log partition
[ "$(target_of var/log)" = "/home/root/log" ]
check "var/log -> /home/root/log" $?
[ "$(target_of var/run)" = "volatile/run" ] && [ "$(target_of run)" = "var/run" ]
check "var/run -> volatile/run and run -> var/run" $?
[ "$(target_of etc/resolv.conf)" = "/var/run/resolv.conf" ]
check "etc/resolv.conf -> /var/run/resolv.conf" $?

member_has etc/udev/rules.d/local.rules 'input/touchscreen0'
check "local.rules creates input/touchscreen0" $?
has lib/udev/rules.d/60-input-id.rules
check "lib/udev/rules.d/60-input-id.rules present (sets ID_INPUT_TOUCHSCREEN)" $?

member etc/ssh/sshd_config | grep -qx 'PasswordAuthentication no'
check "sshd_config: PasswordAuthentication no" $?
member etc/ssh/sshd_config | grep -qx 'PermitRootLogin prohibit-password'
check "sshd_config: PermitRootLogin prohibit-password" $?
[ "$(target_of etc/ssh/ssh_host_ed25519_key)" = "/home/root/data/ssh/ssh_host_ed25519_key" ]
check "ssh host ed25519 key link points into /home/root/data/ssh" $?
! tar -tf "$TAR" | grep -q 'etc/ssh/ssh_host_rsa_key'
check "no ssh_host_rsa_key in the image" $?

member etc/passwd | grep -q '^root:[^:]*:0:0:[^:]*:/home/root:'
check "root's home is /home/root" $?
member etc/shadow | grep -q '^root:\*:'
check "root's password is locked" $?
member etc/group | grep -q '^tty:x:5:'
check "group tty is gid 5 (devpts gid=5)" $?

for mp in home/root/data home/root/log opt/games opt/arca media; do
    has "$mp"
    check "mount point /$mp exists" $?
done
entry home/root/.ssh | grep -q '^drwx------'
check "/home/root/.ssh exists with mode 0700" $?

n=$(awk '{ name = $0; sub(/^[^ ]+ +[^ ]+ +[0-9]+ +[^ ]+ +[^ ]+ +/, "", name);
           if (index(name, "./usr/share/zoneinfo/") == 1) c++ } END { print c + 0 }' "$LIST")
[ "$n" -gt 0 ]; check "usr/share/zoneinfo present ($n entries)" $?

echo "check-rootfs: $pass passed, $fail failed ($TAR)"
[ "$fail" -eq 0 ]
