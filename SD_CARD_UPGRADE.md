# Larger SD Card: Write the Image, Grow p6

A card image from `rootfs/make-card-image.sh` is about 4 GB, and its p6 (our root filesystem) is
~981 MB. That is enough for the apps and not for much ScummVM game data. This document is the optional
step of writing the image to a larger card and growing p6 to fill it, done by
[`commissioning/clone-to-32gb.sh`](commissioning/clone-to-32gb.sh). Building the image is
[COMMISSIONING.md](COMMISSIONING.md).

## 1. What the script does

`sudo ./commissioning/clone-to-32gb.sh --help` is current. Two modes:

```bash
sudo ./commissioning/clone-to-32gb.sh --clone-from <image_or_device> <target_device>
sudo ./commissioning/clone-to-32gb.sh --expand-only <target_device>   # image already written
```

`--dry-run` shows the steps without changing anything. In order, it:

1. writes the source image (or copies the source device) to the target with `dd` (clone mode only);
2. checks the layout below, and records p6's UUID;
3. rewrites the partition table with `sfdisk`: p4 (extended) and p6 grow to the end of the disk, p7 is dropped;
4. `e2fsck -f`, then `resize2fs` on p6;
5. checks the UUID is the one it recorded.

Safety checks, none bypassable by a flag: it must be root, refuses the disk carrying `/`, refuses a target
with anything mounted below it, refuses a partition (the target is a whole disk), and wants 16-128 GB
(`MAX_TARGET_SIZE_GB=n` raises the ceiling). Removable media is required unless `--allow-fixed-disk` is
given, which a card reader that reports as fixed, and any disk attached under WSL, need. It asks for
confirmation before the destructive steps.

⚠️ **The target must be a block device the machine running the script can see.** A USB card reader that
reaches only Windows is not one; that case is unverified here and is not covered by this document.

## 2. The layout it expects

| Part | Type | Size | Role |
|---|---|---|---|
| p1 | FAT32, bootable | ~70.6 MB | boot chain: `mlo`, `u-boot.bin`, `ctrlblock.bin`, the kernel image — never written by this script's resize |
| p2 | ext3 | ~251 MB | data |
| p3 | ext3 | ~243 MB | logs |
| p4 | extended | ~3.14 GB | container for p5-p7; grown |
| p5 | ext3 | ~1.40 GB | unused, kept in the table |
| p6 | ext3 | ~981 MB | our root filesystem; grown |
| p7 | swap | ~259 MB | dropped |

The script checks the start and size of p1, p2, p3, p5 and p6 (`ARCA_LAYOUT` in `lib/arca-identify.sh`) and
deliberately not p4 or p7, which absorb the difference in card size. It expects seven partitions. A
mismatch is a warning with a confirmation, not a refusal; a card that has already been grown also
mismatches, and re-running on it achieves nothing.

### Why p5 stays

U-Boot passes `root=/dev/mmcblk0p6`, compiled in with no `saveenv`, and `/etc/fstab` names partitions by
device path. Everything is by position, so p6 must remain p6. Deleting p5 would renumber the root to
`mmcblk0p5` and the unit would not boot. p5 stays at its size and is never mounted.

### Why p7 goes

The unit has 234 MB of RAM and swap on an SD card is slow; our image keeps swap off. Dropping p7 lets p6
take the whole rest of the extended partition. A swap file on p6 is the way to get swap back.

### The UUID

Not a gate, and not a value to match against any other card: a filesystem UUID is assigned at `mkfs`, so
it names one card. The script writes with `dd` and only resizes, so p6's UUID survives, and it compares
against the value it recorded on **this** card before repartitioning. A change means more than a resize
happened to p6, most likely an accidental `mkfs`, which also destroyed the root. ⚠️ Never run
`tune2fs -U` to make a card match another: two cards claiming one UUID is a harder bug than the one it
hides.

## 3. By hand

The script is the supported way. The mechanics, for when it cannot run, with `DEST` the whole target disk
(for example `/dev/sdb`) and everything on it already written from the image:

```bash
sudo umount ${DEST}* 2>/dev/null
sudo sfdisk -d ${DEST} > partition-table-before.txt      # keep this

# p4 and p6 carry no size, so sfdisk fills them. p5 keeps its start and size; p7 is omitted.
sudo sfdisk ${DEST} << EOF
label: dos
label-id: 0x00000000
unit: sectors
sector-size: 512

${DEST}1 : start=          63, size=      144522, type=c, bootable
${DEST}2 : start=      144585, size=      514080, type=83
${DEST}3 : start=      658665, size=      498015, type=83
${DEST}4 : start=     1156680, type=5
${DEST}5 : start=     1156743, size=     2939832, type=83
${DEST}6 : start=     4096638, type=83
EOF
sudo partprobe ${DEST}

sudo blkid -s UUID -o value ${DEST}6                      # record before, compare after
sudo e2fsck -f ${DEST}6                                   # required before resize2fs
sudo resize2fs ${DEST}6                                   # no size argument: fill
sudo blkid -s UUID -o value ${DEST}6
```

On an NVMe or `mmcblk` disk the partition suffix is `p6`, not `6`. The start sectors are the 63-sector
(CHS-era) alignment the original layout uses, kept as is because the boot chain was built for it.

Check the result with `sfdisk -l ${DEST}` (p1-p3 unchanged, p4 to the end of the disk, p5 unchanged, p6
taking the rest, no p7), `e2fsck -f` on p2, p3 and p6, and a mount of p6 with `df -h`.

If `sfdisk` rejects the table, wipe nothing blindly: re-read the saved `partition-table-before.txt` and
confirm `DEST` is the right disk first. If `e2fsck` finds errors, `sudo e2fsck -fy ${DEST}6` and
only then `resize2fs`.

## 4. After the resize

The card boots like any card from the image. Take the unit's per-unit state with
`commissioning/backup.sh` before moving it to a different card, and put it back with
`commissioning/restore.sh`. If a grown card does not boot, rewrite it from the image; the image file is
the recovery, and p1 was never touched.

## 5. ScummVM game storage

Games live on p6, so growing it is what makes large ones fit. The path is the one given when the game is
added in ScummVM; nothing here fixes a directory.

Space on a grown 32 GB card is ~27 GB for p6, less what the image already uses.

### Example game sizes (classic SCUMM)

| Game | ScummVM ID | Size |
|------|-----------|------|
| Maniac Mansion | maniac | ~1 MB |
| Zak McKracken | zak | ~1 MB |
| Indiana Jones: Last Crusade | indy3 | ~3 MB |
| Loom | loom | ~5 MB |
| Monkey Island 1 (VGA) | monkey | ~12 MB |
| Monkey Island 2 | monkey2 | ~40 MB |
| Indiana Jones: Fate of Atlantis | atlantis | ~15 MB |
| Day of the Tentacle | tentacle | ~10 MB |
| Sam & Max Hit the Road | samnmax | ~30 MB |
| Full Throttle | ft | ~50 MB |
| The Dig | dig | ~500 MB |

### Example game sizes (remastered / later)

| Game | Size |
|------|------|
| Day of the Tentacle Remastered | ~2.5 GB |
| Full Throttle Remastered | ~5 GB |
| Grim Fandango Remastered | ~5.6 GB |
| Broken Sword 1 | ~700 MB |
| Beneath a Steel Sky | ~70 MB |

### Copying games over

```bash
scp -r /path/to/game/data root@<device-ip>:<games-dir>/tentacle/
ssh root@<device-ip> "du -sh <games-dir>/*"
```

For very large transfers a USB drive may beat SCP over Ethernet (after enabling USB host mode):

```bash
mount /dev/sda1 /mnt
cp -r /mnt/scummvm-games/* <games-dir>/
umount /mnt
```
