# Ludbox Linux

Ludbox Linux turns a Steelcase RoomWizard wall display into a small games and apps box:
seven touch games, a touch instrument, a terminal, a classic-adventure engine (ScummVM),
a remote-desktop viewer and a settings app. It runs from its own microSD card, so the
display's original software stays untouched on the card it came with.

Everything runs on the display's own 600 MHz processor with no graphics chip, so expect
simple games, not modern ones. No Wi-Fi is fitted; the unit uses its Ethernet cable,
which is also its power (PoE).

> **No warranty.** Opening the unit is feasible but takes care, and an inexperienced
> attempt can break the enclosure or the touch cable. Steelcase and RoomWizard are
> trademarks of their respective owner; this project is unaffiliated with, and not
> endorsed by, Steelcase.

## 1. Before you start

- **Keep the card the unit came with and flash a spare.** Never write over the original.
  Put it back and the unit returns to its old software.
- You need a microSD card of **2 GB or more** and a card writer for your PC. The image is
  1.57 GiB; the system grows to fill a bigger card on first boot.
- ⚠️ **The card socket is inside the unit, but easy to reach.** The microSD card sits on
  the main board and you get at it by removing the **back** cover with plastic prying
  tools. No screwdriver is needed. First unplug the RJ45 (PoE) cable, since that is the
  only power lead. Then pry around the back cover and pop the box open; the card is
  right there. Go gently: plastic clips break.

## 2. Flash, insert, use

1. Download the image from the project's **Releases** page on GitHub, with its checksum
   file, and check the checksum. [unverified: the release page, the image's file name and
   its compression do not exist yet]
2. Write the image to the spare card with a card writer (Raspberry Pi Imager, balenaEtcher
   or Rufus, using its "write a disk image" option). This erases the card.
3. Put the card back in the unit. Refit the back cover; it can sit loosely without fully
   clicking in, which is handy if you re-flash the card later.
4. Connect the **PoE (power over Ethernet) adapter**: the unit gets its power over the
   Ethernet cable. For network, plug a network cable into the adapter's data side (the
   network needs a DHCP server). It also works with no network at all: every app starts, the network apps say so, and plugging the cable in later brings the network up.
5. **The first boot is slower.** On a large card (128 GB) it takes about **3 minutes**
   before the launcher appears; later boots are fast. What you will see: the two LEDs
   flash for the first few seconds; after about 8 seconds the screen comes on and boot
   text scrolls; it pauses a little (around 11 seconds) at "starting udevd"; then the
   unit names itself (`arca-` plus four random hex digits; "arca" is the project's
   codename and shows up in names like this) and spends about 2 minutes 20 seconds
   growing its storage to fill the card, with little on screen. After that it starts the
   network. With no DHCP server it waits about 15 seconds, then carries on without it.
   The launcher is up at about 3 minutes. A smaller card grows faster. [unverified: times
   on cards other than 128 GB]
6. The launcher appears: a grid of tiles. Tap one to start it. Leaving an app returns to
   the launcher.
7. If the status line says **NOT CALIBRATED**, open Control Panel, **Input**,
   **CALIBRATE**, and tap the targets as shown. Touch calibration is fitted per unit and
   is not part of the image.

Besides touch, a USB keyboard, mouse and Xbox 360 controller work in the launcher, the
Control Panel and the games (arrows, Enter/Space, Esc/Backspace to go back). The unit's USB
port is micro-USB, so you need a micro-USB-to-USB (OTG) adapter and/or a USB hub. An Xbox
controller needs a **powered** hub.

## 3. The apps

| Tile | What it is |
|---|---|
| Snake, Tetris, Pong, Brick Breaker, SameGame, Frogger, Office Runner | Games. They take touch, keyboard and pad. Scores are kept. |
| Tap-a-Theremin | A touch instrument: slide a finger to change the pitch. |
| Terminal | A text login on the unit's screen. Root needs a password first (section 5). |
| Control Panel | Settings and tests for the unit itself (section 4). |
| VNC Client | Shows another computer's screen (section 4). |
| ScummVM | Runs classic point-and-click adventures. **No game data is included.** |

**ScummVM games: you bring your own.** There is no on-screen way to add them yet, and
USB thumb drives are not supported yet (on the roadmap). The only route today is a copy
over SSH (section 5), for example
`scp -r <game-folder> root@<ip>:/home/root/data/games/<game-folder>/`, then **Add Game**
in ScummVM's own launcher. Free, legal games (Beneath a Steel Sky, Flight of the Amazon
Queen and others) are listed at <https://www.scummvm.org/games/>; prefer the floppy
versions, which are a few MiB. The ScummVM in the default build has the SCUMM, HE, AGI,
SCI, AGOS, Sky and Queen engines; other engines are not in it.
[unverified: that the published image was built with the default engine set, and that the
Add Game browser on the panel can reach `/home/root/data/games`; the path is the one the
ScummVM port's own README gives]

## 4. Configure

Open **Control Panel** from the launcher. Its pages, from the source:

| Page | What it does |
|---|---|
| Audio | Sound on or off, music and effects, output device (speaker, USB audio, a paired Bluetooth headset), a test chime. |
| Display | Backlight, portrait or landscape, which screen area is visible and which is touchable, display tests. |
| LED | The two indicator LEDs: on or off, brightness, tests. |
| USB | The devices on the USB port; RESCAN revives a port that came up dead. |
| Bluetooth | Scan, pair, connect, remove; use a headset for audio. Needs a USB Bluetooth adapter, since the unit has no radio. Tested with the TRENDnet TWB-107UB; other adapters are [unverified]. |
| Input | Touch calibration and diagnostics; keyboard, mouse and pad testers; which pad is player 1 to 4. |
| Network | Gateway, DNS, and each interface's state, IP address and MAC. |
| SSH | Remote login: mode, root login, root password, host name. |
| Monitor | Uptime, load, temperature, memory and storage, with graphs. |
| System | Date, time and time zone. |
| Log | The unit's logs, read-only. |
| Information | Kernel, host name, boot target and other identity figures; **RESET DEFAULTS** (backs the settings file up first). |

**VNC Client** shows another computer's desktop, for example a Raspberry Pi running a VNC
server such as x11vnc. No server is configured out of the box, so the first launch cannot
connect: on the reconnect screen tap **SETTINGS**, enter host, port and password, then
**SAVE & RECONNECT**. Inside a session, hold the top-left corner for 3 seconds to reopen
the settings. VNC passwords are cut to 8 characters, so use a throwaway LAN password.
[unverified: connecting to a server that has no password]

## 5. Reaching the unit over SSH (optional)

You need SSH only to add ScummVM games or copy files off; the screen is enough for
everything else. The image ships with **root locked and SSH closed to everyone**. The
root password is first set **on the unit itself: Control Panel, SSH page, PASSWORD** (typed
twice on the on-screen keyboard, 6 to 32 characters). The Terminal app prints the same
hint while root is locked ("bad salt" means exactly this).

1. SSH page: **PASSWORD**, set one.
2. **ROOT LOGIN**: tap **ALLOW** and confirm.
3. **MODE**: choose **KEY + PASSWORD** and confirm.
4. From your PC run `ssh-copy-id root@<ip>` (Linux, macOS, WSL) and give the password. The
   IP is on the Control Panel **Network** page, the host name on **Information**;
   `ssh root@arca-xxxx.local` may work too.
   [unverified: that `.local` names resolve from Windows and macOS PCs, and a Windows
   command for copying the key without `ssh-copy-id`]
5. Check that `ssh root@<ip>` logs in with the key, then on the SSH page choose
   **KEY ONLY**. **OFF** closes the port instead, also across reboots.

After you flash a card again, the unit has a new host key: run `ssh-keygen -R <ip>` or SSH
refuses with a warning.

## 6. Updating

There is no update mechanism yet. **To update, flash the new image to the card again.**
⚠️ A re-flash wipes everything on the card: **high scores, touch calibration, settings,
the SSH password and keys, and ScummVM games and saves.** Copy what you want to keep off
the unit first (over SSH, from `/home/root/data`).

## 7. Troubleshooting

| Symptom | Try |
|---|---|
| Blank screen just after power-up | Normal for the first 8 seconds. The first boot on a large card takes about 3 minutes. If it lasts much longer, see the LED table below, then re-flash the card and check the download's checksum. |
| Does not boot from the new card | Re-seat the card and re-flash it. Put the original card back to confirm the unit itself is fine. |
| Touch is off | Control Panel, **Input**, **CALIBRATE**. |
| Cannot find the unit on the network | Check the Control Panel **Network** page, or your router's device list for `arca-xxxx`. The unit needs a DHCP server. |
| Terminal says "bad salt" | Root has no password yet: section 5, step 1. |
| SSH warns the host key changed | You re-flashed: `ssh-keygen -R <ip>`. |
| VNC cannot connect | Check host, port and password in its settings; the server must be reachable from the unit's network. |
| Silent | Control Panel, **Audio**: check the output device and press TEST. |
| Go back to the original software | Power off and put the original card back. |

### What the two LEDs mean

The LEDs report boot progress before Linux starts. A good cold boot: green at 1 second,
red at 2, green at 3, off at 4, three short red flashes, three short green flashes, green
then off at 6, dark until the screen comes on at about 8 to 9 seconds. When Linux takes
over, the LEDs turn amber (both lit at half brightness). The **last LED state shows where
a failed boot stopped**:

| LED | Meaning |
|---|---|
| dark from the start | The first boot stage never ran: the card's boot files are missing or unreadable. |
| red, then red + green | Early boot is working. Staying red means the memory test failed. |
| red + green, steady | Stuck before handing over: the boot loader file is missing or unreadable. |
| red blinking, twice a second | A failure was detected in early boot. |
| green, steady | Waiting for the card, or the card does not answer. |
| dark (after the flashes) | Loading the system failed or hung. |
| yellow blinking, twice a second | Nothing could boot: neither the system file nor its fallback loaded. |

(Source: `SYSTEM_ANALYSIS.md` section 4.7, "Reading a failed boot by the two side LEDs", and
`uboot/BUILD.md`, "Boot-progress LEDs".) The red blink and the yellow blink have not been
seen on hardware, so treat them as [unverified].

Once the system is running: a **red LED with a red square on screen** means the processor
is over temperature (the monitor service lights it until it cools). The Control Panel
**LED** page can switch the LEDs off or change their brightness, and some games blink the
LEDs for feedback. [unverified: any LED pattern for running normally, for shutdown, or for
errors after boot; none was found in the source]

## Licence and source

The project's own code is **MIT**. The image also carries GPL, LGPL and other open-source
programs (the Linux kernel, BusyBox, ScummVM, BlueZ and more), each under its own licence.
Their licence texts are on the card in `/usr/share/doc/arca-licenses/` (`LICENSE.md`,
`manifest.csv`, `licenses/`, `SOURCE-OFFER.txt`). **The complete corresponding source of
every image is attached to the same GitHub release as a source archive with its checksum;
that is the written offer under GPL-2.0 section 3(a).** The full list is in `LICENSE.md`.
[unverified: the exact wording and asset name; take both from the release that is actually
published]

Developers: building, deploying and the repository layout are in [DEVELOPING.md](DEVELOPING.md).
