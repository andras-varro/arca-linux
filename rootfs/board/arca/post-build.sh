#!/bin/sh
# Buildroot post-build hook: runs after the overlay is copied in and before the tarball is
# made. $1 is the target directory. It does what an overlay cannot:
#   - Git on Windows stores no symlinks and no modes, and the overlay is copied from a
#     DrvFs checkout where every file looks 0777; so every link is made here and every mode
#     is set here.
#   - Buildroot's own /etc/init.d/S??* scripts are removed so nothing starts twice.
#   - /etc/passwd and /etc/shadow are edited after Buildroot has generated them.
set -eu

TARGET=${1:?usage: post-build.sh <target dir>}
OVERLAY=$(cd "$(dirname "$0")" && pwd)/overlay

# Modes. Init scripts are executable, every other overlay file is plain data.
(cd "$OVERLAY" && find . -type d) | while read -r d; do
    chmod 0755 "$TARGET/$d"
done
(cd "$OVERLAY" && find . -type f) | while read -r f; do
    case "$f" in
        ./etc/init.d/*) chmod 0755 "$TARGET/$f" ;;
        *) chmod 0644 "$TARGET/$f" ;;
    esac
done

# Buildroot's own boot scripts. Ours are installed under plain names by the overlay.
rm -f "$TARGET"/etc/init.d/S??* "$TARGET"/etc/init.d/rcK

# SysV runlevel links. Relative, so they resolve inside a mounted card as well.
link() { # <runlevel dir> <link name> <script in init.d>
    mkdir -p "$TARGET/etc/$1"
    ln -sfn "../init.d/$3" "$TARGET/etc/$1/$2"
}
link rcS.d S01mountall    mountall
# After mountall by name order: seedrng writes its new seed to the root filesystem.
link rcS.d S01seedrng     seedrng
link rcS.d S02alignment   alignment
link rcS.d S03udev        udev
# Sorts before S04hostname: a generic image names itself before the kernel name is set.
link rcS.d S04firstboot-hostname firstboot-hostname
link rcS.d S04hostname    hostname
link rcS.d S05sysctl      sysctl
link rcS.d S06watchdog    watchdog
link rcS.d S07syslog      syslog
link rcS.d S08networking  networking
link rc5.d S02dbus-1      dbus-1
link rc5.d S09sshd        sshd
# mDNS from the image, so a unit that named itself is reachable as <name>.local with no
# further install. After dbus; provision-rules.conf makes the same link (ln -sf, idempotent).
link rc5.d S30avahi-daemon avahi-daemon
link rc5.d S20hwclock.sh  hwclock.sh
for lvl in rc0.d rc6.d; do
    link $lvl K10sshd       sshd
    link $lvl K20dbus-1     dbus-1
    link $lvl K30hwclock.sh hwclock.sh
    link $lvl K40syslog     syslog
    link $lvl K50seedrng    seedrng
    link $lvl K99watchdog   watchdog
done
mkdir -p "$TARGET/etc/rc1.d" "$TARGET/etc/rc2.d" "$TARGET/etc/rc3.d" "$TARGET/etc/rc4.d"

# /var layout: /var/volatile is a tmpfs (fstab); the rest of the volatile directories link
# into it, /var/log is the log partition, and /run and /tmp follow /var. The skeleton makes
# /run a real directory and /var/run a link to it, so both are replaced.
rm -rf "$TARGET/run" "$TARGET/tmp" "$TARGET/var/run" "$TARGET/var/lock" \
    "$TARGET/var/tmp" "$TARGET/var/cache" "$TARGET/var/log" "$TARGET/var/volatile"
# The directories exist in the image as well as on the tmpfs: Buildroot creates the dbus
# user's home /run/dbus after this script, and mkdir through a dangling link fails.
mkdir -p "$TARGET/var/volatile/run" "$TARGET/var/volatile/lock" "$TARGET/var/volatile/tmp" \
    "$TARGET/var/volatile/cache"
ln -s volatile/run "$TARGET/var/run"
ln -s volatile/lock "$TARGET/var/lock"
ln -s volatile/tmp "$TARGET/var/tmp"
ln -s volatile/cache "$TARGET/var/cache"
ln -s /home/root/log "$TARGET/var/log"
ln -s var/run "$TARGET/run"
ln -s var/tmp "$TARGET/tmp"
# udhcpc's default script writes /etc/resolv.conf; it lands on the tmpfs.
rm -f "$TARGET/etc/resolv.conf"
ln -s /var/run/resolv.conf "$TARGET/etc/resolv.conf"

# Mount points for the SD card partitions and the app tree.
mkdir -p "$TARGET/home/root/data" "$TARGET/home/root/log" \
    "$TARGET/opt/games" "$TARGET/opt/arca" "$TARGET/media"
mkdir -p "$TARGET/home/root/.ssh"
chmod 0700 "$TARGET/home/root/.ssh"
chmod 0755 "$TARGET/home/root"

# sshd host keys live on the data partition; /etc/init.d/sshd creates them there.
# Only ed25519: an RSA key added a second generation wait to first boot for no client we use.
rm -f "$TARGET/etc/ssh/ssh_host_rsa_key" "$TARGET/etc/ssh/ssh_host_rsa_key.pub"
rm -f "$TARGET/etc/ssh/ssh_host_ed25519_key" "$TARGET/etc/ssh/ssh_host_ed25519_key.pub"
ln -s /home/root/data/ssh/ssh_host_ed25519_key "$TARGET/etc/ssh/ssh_host_ed25519_key"
ln -s /home/root/data/ssh/ssh_host_ed25519_key.pub "$TARGET/etc/ssh/ssh_host_ed25519_key.pub"

# The on-device host-name tool is the commissioning script itself (one implementation of the
# name rules and the /etc/hosts rewrite), installed under a command name.
install -D -m 0755 "$(cd "$OVERLAY/../../../.." && pwd)/commissioning/set-hostname.sh" "$TARGET/usr/sbin/set-hostname"

# Identity marker: lib/arca-identify.sh's arca_is_rootfs recognises this tree by it, since
# none of the vendor's marker files are here.
printf 'Arca root filesystem, built from rootfs/ (Buildroot)\n' > "$TARGET/etc/arca-rootfs"
chmod 0644 "$TARGET/etc/arca-rootfs"

# The product name. Buildroot's target-finalize writes usr/lib/os-release (NAME=Buildroot) and an
# /etc/os-release link BEFORE it copies the overlay and runs this hook (Makefile, 2025.02), so
# this hook has the last word: our one source file replaces the generated one, and the link is
# re-made in case the overlay displaced it. native_apps/common/product.c reads NAME= from it.
install -D -m 0644 "$(cd "$OVERLAY/.." && pwd)/os-release" "$TARGET/usr/lib/os-release"
ln -sfn ../usr/lib/os-release "$TARGET/etc/os-release"
grep -q '^NAME=' "$TARGET/etc/os-release" || { echo "post-build: os-release not installed" >&2; exit 1; }

# Root: home on the card's tree, bash as the login shell (the skeleton gives BusyBox
# ash via /bin/sh), password locked (key-only login).
sed -i 's|^root:\([^:]*\):0:0:root:[^:]*:[^:]*$|root:\1:0:0:root:/home/root:/bin/bash|' "$TARGET/etc/passwd"
grep -q '^root:[^:]*:0:0:root:/home/root:/bin/bash$' "$TARGET/etc/passwd" || { echo "post-build: root passwd line not rewritten" >&2; exit 1; }
sed -i 's|^root:[^:]*:|root:*:|' "$TARGET/etc/shadow"

exit 0
