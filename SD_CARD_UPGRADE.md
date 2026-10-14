# Larger SD Card: Write the Image, Grow p3

A card image from `rootfs/make-card-image.sh` is about 1.6 GB, and its p3 (our root filesystem, which also
holds `/home/root/data`) is 512 MB. That is enough for the apps and not for much ScummVM game data. This
document is the optional step of writing the image to a larger card and growing p3 to fill it, done by
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
2. checks the layout (three primaries: p1 FAT boot, p2 log, p3 root; `ARCA_LAYOUT` in `lib/arca-identify.sh`;
   a table with any other partition count is refused) and records p3's UUID;
3. rewrites the partition table with `sfdisk`: p1 and p2 as they are, p3 without a size, so it runs to the end of the disk;
4. `e2fsck -f`, then `resize2fs` on p3;
5. checks the UUID is the one it recorded.

Safety checks, none bypassable by a flag: it must be root, refuses the disk carrying `/`, refuses a target
with anything mounted below it, refuses a partition (the target is a whole disk), and wants 16-128 GB
(`MAX_TARGET_SIZE_GB=n` raises the ceiling). Removable media is required unless `--allow-fixed-disk` is
given, which a card reader that reports as fixed, and any disk attached under WSL, need. It asks for
confirmation before the destructive steps.

⚠️ **The target must be a block device the machine running the script can see.** A USB card reader that
reaches only Windows is not one; that case is unverified here and is not covered by this document.

The UUID is not a gate and not a value to match against another card: a filesystem UUID is assigned at
`mkfs`, so it names one card. The script compares against the value it recorded on **this** card before
repartitioning; a change means more than a resize happened to p3. ⚠️ Never run `tune2fs -U` to make a card
match another.

If a grown card does not boot, rewrite it from the image; the image file is the recovery, and p1 was never
touched. Take the unit's per-unit state with `commissioning/backup.sh` first and put it back with
`commissioning/restore.sh`.

## 2. ScummVM game storage

Games live under `/home/root/data`, which is a directory on p3, so growing p3 is what makes large ones fit.
The path is the one given when the game is added in ScummVM; nothing here fixes a directory below it.

Space on a grown 32 GB card is ~28 GB for p3, less what the image already uses.

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
scp -r /path/to/game/data root@<device-ip>:/home/root/data/tentacle/
ssh root@<device-ip> "du -sh /home/root/data/*"
```

For very large transfers a USB drive may beat SCP over Ethernet (after enabling USB host mode):

```bash
mount /dev/sda1 /mnt
cp -r /mnt/scummvm-games/* /home/root/data/
umount /mnt
```
