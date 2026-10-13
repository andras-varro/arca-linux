#!/bin/bash
#
# make-fake-card.sh — build a synthetic MOUNTED card of OUR root image under $1.
#
# NOT a repo tool: a scratch fixture builder for exercising commissioning/commission-offline.sh
# without a card and without a device. It mirrors what rootfs/make-card-image.sh hands that
# script: p6 is our Buildroot root (marker etc/arca-rootfs, the rcS.d/rc5.d links and
# the key-only sshd_config that rootfs/board/arca/{post-build.sh,overlay} produce),
# p2/p3/p5 are empty ext4 trees. There is no vendor software anywhere in it. It is
# committed because the alternative is retyping it, and because the one thing it must get
# right — REAL SYMLINKS in rc*.d — is exactly what partitions/ and partitions.new/ lost on
# the way through Windows (CLAUDE.md → "Working from this host").
#
#   wsl.exe -u root -e bash -lc "cd /mnt/c/work/roomwizard && tests/make-fake-card.sh /tmp/fake"
#
# Build it under /tmp, never under /mnt/c: DrvFs cannot hold a symlink and
# discards chmod, so a fixture there can neither carry a boot link nor show a
# missing +x.
set -eu

BASE="${1:?usage: make-fake-card.sh <dir>}"
case "$BASE" in
    /tmp/*) ;;
    *) echo "refusing to build a fixture outside /tmp: $BASE" >&2; exit 1 ;;
esac
rm -rf "$BASE"

mkdir -p "$BASE"/root/etc/{init.d,rcS.d,rc0.d,rc1.d,rc2.d,rc3.d,rc4.d,rc5.d,rc6.d,ssh,network,sysctl.d} \
         "$BASE"/root/{opt/games,opt/arca,usr/sbin,usr/lib,usr/share} \
         "$BASE"/root/home/root/{data,log,backup,.ssh} \
         "$BASE"/data/lost+found "$BASE"/log/lost+found

# arca_is_rootfs's four required files, our marker, and the per-unit name.
echo 'root:*:19000:0:99999:7:::'                    > "$BASE/root/etc/shadow"
printf '127.0.0.1 localhost\n127.0.1.1 rwfake\n'    > "$BASE/root/etc/hosts"
printf 'rwfake\n'                                   > "$BASE/root/etc/hostname"
printf 'auto lo\niface lo inet loopback\n\nauto eth0\niface eth0 inet dhcp\n' \
    > "$BASE/root/etc/network/interfaces"
printf 'Arca root filesystem, built from rootfs/ (Buildroot)\n' > "$BASE/root/etc/arca-rootfs"
# Key-only, as rootfs/board/arca/overlay/etc/ssh/sshd_config.
printf 'PermitRootLogin prohibit-password\nPubkeyAuthentication yes\nPasswordAuthentication no\nPermitEmptyPasswords no\nAuthorizedKeysFile .ssh/authorized_keys\n' \
    > "$BASE/root/etc/ssh/sshd_config"

# Our init scripts (rootfs/board/arca/overlay/etc/init.d), and REAL symlinks to them,
# named as rootfs/board/arca/post-build.sh makes them.
for n in mountall seedrng alignment udev hostname sysctl watchdog syslog networking \
         dbus-1 sshd hwclock.sh; do
    printf '#!/bin/sh\n# fixture stub\nexit 0\n' > "$BASE/root/etc/init.d/$n"
    chmod 755 "$BASE/root/etc/init.d/$n"
done

l() { ln -sf "../init.d/$2" "$BASE/root/etc/$1/$3"; }
l rcS.d mountall S01mountall;   l rcS.d seedrng S01seedrng
l rcS.d alignment S02alignment; l rcS.d udev S03udev
l rcS.d hostname S04hostname;   l rcS.d sysctl S05sysctl
l rcS.d watchdog S06watchdog;   l rcS.d syslog S07syslog
l rcS.d networking S08networking
l rc5.d dbus-1 S02dbus-1;       l rc5.d sshd S09sshd
l rc5.d hwclock.sh S20hwclock.sh
for lvl in rc0.d rc6.d; do
    l $lvl sshd K10sshd;          l $lvl dbus-1 K20dbus-1
    l $lvl hwclock.sh K30hwclock.sh; l $lvl syslog K40syslog
    l $lvl seedrng K50seedrng;    l $lvl watchdog K99watchdog
done

echo "fixture built: $BASE"
