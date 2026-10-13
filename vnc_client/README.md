# VNC Client for Arca

A lightweight VNC client for the Steelcase RoomWizard embedded device.
Displays a remote desktop (e.g. Raspberry Pi weather/clock display) on the
the Steelcase RoomWizard's 800×480 screen with touch pass-through.

## Features

- **Direct framebuffer rendering** — no X11, no compositor overhead
- **Bilinear interpolation** — smooth downscaling (1920×1080 → 795×447)
- **Tight/ZRLE encoding** — 10-100× bandwidth reduction vs raw
- **16bpp RGB565** — halved memory bandwidth (same as ScummVM backend)
- **Partial region updates** — only dirty rectangles are re-rendered
- **Touch input** — touchscreen mapped to VNC pointer events
- **USB keyboard forwarding** — full key press/release forwarded as X11 keysyms
- **USB mouse forwarding** — relative movements with 3-tier acceleration, all buttons + scroll
- **Runtime config file** — all settings in `/opt/vnc_client/vnc_client.conf`
- **Settings GUI** — long-press top-left corner (3 s) to edit config on-screen
- **Watchdog integration** — prevents system reboot during operation

### Performance (v2.3)

| Metric | v1.0 (MVP) | v2.3 (Current) |
|--------|-----------|----------------|
| CPU | 99% | ~5% |
| Frame rate | <1 fps | 5-7 fps |
| Bandwidth | ~120 Mbps | <5 Mbps |
| Encoding | Raw | Tight |

---

## Quick Start

### Prerequisites

Linux build machine with ARM cross-compiler (tested on WSL 2):

```bash
sudo apt-get install build-essential cmake wget tar
sudo apt-get install gcc-arm-linux-gnueabihf g++-arm-linux-gnueabihf
```

### Build

```bash
cd vnc_client

# First time: build dependencies (~5 min)
chmod +x build-deps.sh
./build-deps.sh

# Build client
make
```

Produces `vnc_client_stripped` (~870 KB statically linked binary).

### Deploy

```bash
make deploy DEVICE_IP=192.168.50.53
```

This copies the binary and config file to `/opt/vnc_client/` on the device.

### Run

```bash
ssh root@192.168.50.53 '/opt/vnc_client/vnc_client'
```

---

## Configuration

Settings are loaded in this order (later overrides earlier):

1. **Compile-time defaults** — `config.h`
2. **Config file** — `/opt/vnc_client/vnc_client.conf`
3. **Command-line arguments**

### Config File

```ini
# /opt/vnc_client/vnc_client.conf
host = 192.168.50.56
port = 5900
password = CHANGEME
encodings = tight zrle copyrect hextile zlib raw
compress_level = 6
quality_level = 5
content_area = safe
```

**Note:** `password` is a placeholder here on purpose — this file is tracked, and until
2026-08-03 this example carried the reference setup's **real** password. Put the real one only in
the gitignored `vnc_client.conf`. It is sent under the legacy
VncAuth scheme, which **truncates it to 8 characters** at both ends, so use a throwaway LAN
credential and don't rely on characters past position 8.

> **Tip:** All settings (HOST, PORT, PASSWORD, ENCODINGS, COMPRESS, QUALITY,
> CONTENT) can be changed at runtime via the on-screen settings GUI (long-press
> top-left corner for 3 seconds).

### `content_area` — reachability vs. size

`safe` (the default) letterboxes the remote desktop into the part of the screen a finger can
actually reach. The touch digitizer saturates a little before the panel edge, so a band at each
end is visible but not pressable — and on someone else's desktop, the bottom row may be a
taskbar you need to click. The cost is size: a 1920×1080 desktop scales to about 747×420 instead
of 795×447.

Set `content_area = visible` to use the whole screen instead — bigger and easier to read, at the
price of a band top and bottom that you cannot tap. Either way this app's own touch UI (settings
screen, reconnect buttons, exit-gesture corner) stays reachable.

The setting does nothing until the panel's touch reach has been measured — Control Panel →
Input → `CALIBRATE`, the `REACH` step. Before that, `safe` and `visible` are the same
rectangle.

On the settings GUI it is the **CONTENT** row: `TOGGLE` flips between `SAFE (ALL TAPPABLE)` and
`FULL (EDGES UNTAPPABLE)`, and the change takes effect when you press **SAVE & RECONNECT** — the
picture is letterboxed once per session, so BACK alone leaves the current session as it was.

### Command-Line

```
vnc_client [OPTIONS] [host [port]]

Options:
  --config <path>   Config file (default: /opt/vnc_client/vnc_client.conf)
  --host <ip>       VNC server host
  --port <port>     VNC server port
  --password <pw>   VNC password
  --help, -h        Show help
```

Legacy positional syntax still works: `vnc_client 192.168.50.56 5900`

### Compile-Time Settings (config.h)

| Setting | Default | Description |
|---------|---------|-------------|
| `TARGET_FPS` | 30 | Frame rate cap |
| `USE_16BPP` | 1 | Enable 16bpp RGB565 framebuffer |
| `EXIT_ZONE_SIZE` | 60 | Exit gesture corner size (pixels) |
| `EXIT_HOLD_MS` | 3000 | Exit gesture hold duration (ms) |
| `DEBUG_ENABLED` | 1 | Print debug info to stderr |

---

## Architecture

```
RPi3 VNC Server (192.168.50.56:5900)
         │ RFB Protocol (Tight encoding)
         ▼
    LibVNCClient (decode)
         │
         ▼
   vnc_renderer.c (bilinear scale + BGR→RGB565)
         │
         ▼
    /dev/fb0 (800×480 16bpp)

/dev/input/event0 (touch)         ─┐
/dev/input/eventN (USB keyboard)  ─┼─→ vnc_input.c ─→ VNC key/pointer events → RPi3
/dev/input/eventN (USB mouse)     ─┘    (coordinate mapping, acceleration,
                                          exit gesture detection)
```

### Source Files

| File | Purpose |
|------|---------|
| `vnc_client.c` | Main application, config parser, event loop |
| `vnc_renderer.c/h` | Bilinear scaling, BGR→RGB565, partial updates |
| `vnc_input.c/h` | Touch/keyboard/mouse→VNC mapping, acceleration, exit gesture |
| `config.h` | Compile-time defaults and constants |
| `vnc_client.conf` | Runtime configuration template |
| `Makefile` | Cross-compilation and deployment |
| `build-deps.sh` | Downloads and builds zlib, libjpeg-turbo, LibVNCClient |
| `rpi_setup.py` | RPi VNC server setup script |
| `setup-vnc-viewer.sh` | VNC viewer (Remmina) setup for RPi display clients |
| `vnc_settings.c/h` | Touch-based settings GUI with full alphanumeric keypad; all fields editable |

### Shared Libraries (from native_apps)

- `framebuffer.c` — mmap'd double-buffered `/dev/fb0` access
- `touch_input.c` — `/dev/input/event0` polling
- `hardware.c` — LEDs, backlight control

---

## RPi VNC Server Setup

The RPi runs **x11vnc** on port 5900 (standard VncAuth), compatible with both
LibVNCClient (Arca) and RealVNC Viewer (other clients). The proprietary
RealVNC Server is disabled.

```bash
# Requires: pip install paramiko
python3 rpi_setup.py <rpi_host> <rpi_user> <rpi_password> [action]
```

| Action | Description |
|--------|-------------|
| `fix-and-install` | Fix Buster EOL repos + install x11vnc (default) |
| `manual-install` | Direct .deb download if apt fails |
| `verify` | Check x11vnc service status |
| `inspect` | Inspect RPi display environment |

This creates a systemd service (`x11vnc-arca`) on port 5900 with
password authentication. The setup script disables RealVNC Server if present.

---

## RPi VNC Viewer Setup

To use another Raspberry Pi as a dedicated VNC viewer/display that connects to the
Arca RPi VNC server, use `setup-vnc-viewer.sh`. It installs **Remmina** as
the VNC viewer with fullscreen auto-scaling and auto-connect on boot.

### Prerequisites

- Raspberry Pi running Raspberry Pi OS (Bullseye/Bookworm) with a desktop environment
- Network access to the VNC server RPi
- SSH key-based authentication for remote setup (see below)

### SSH Key Setup (for remote execution)

To run the setup script remotely from your workstation:

    # Generate an SSH key if you don't have one:
    ssh-keygen -t ed25519

    # Copy your public key to the target Pi:
    ssh-copy-id pi@<viewer-pi-ip>

    # Verify passwordless login works:
    ssh pi@<viewer-pi-ip> echo "OK"

### Usage

    # Remote setup from workstation (recommended):
    ./setup-vnc-viewer.sh --remote pi@192.168.50.121 install

    # Local setup (run directly on the viewer Pi):
    ./setup-vnc-viewer.sh install

    # Custom VNC server address:
    ./setup-vnc-viewer.sh --remote pi@192.168.50.121 --server 10.0.0.5 --port 5900 install

    # Skip nightly reboot cron job:
    ./setup-vnc-viewer.sh --remote pi@192.168.50.121 --no-reboot-cron install

    # Check current setup:
    ./setup-vnc-viewer.sh --remote pi@192.168.50.121 verify

    # Remove viewer setup:
    ./setup-vnc-viewer.sh --remote pi@192.168.50.121 uninstall

The script prompts for the VNC password interactively. It is stored in
Remmina's encrypted format — never saved in plaintext.

### What It Does

| Step | Description |
|------|-------------|
| Remove RealVNC Viewer | Uninstalls proprietary viewer if present |
| Install Remmina | Installs Remmina + VNC plugin via apt |
| Create profile | `~/.local/share/remmina/arca.remmina` with scaled fullscreen |
| Autostart | `~/.config/autostart/vnc-viewer.desktop` — connects on login |
| Desktop shortcut | `~/Desktop/vnc-viewer.desktop` — manual launch |
| Nightly reboot | `/etc/cron.d/nightly-reboot` — reboots at 3am daily |

### Manual Remmina Launch

Double-click the "VNC to Arca" icon on the desktop, or from a terminal:

    remmina -c ~/.local/share/remmina/arca.remmina

---

## Settings Screen

**Long-press the top-left corner** of the screen for 3 seconds to open the
settings screen (or hold **Select** on a pad, or **Esc** on a keyboard, for 1.5 s). There a mouse moves an arrow and a left click taps. From there you can:

- **Edit HOST and PORT** — touch the field to open a numeric keypad
- **Edit PASSWORD** — touch to open a full alphanumeric keypad with shift toggle (max 8 chars, VncAuth limit)
- **Edit ENCODINGS** — touch to open an alphanumeric keypad
- **Adjust COMPRESS and QUALITY** — use the +/- buttons (range 1–9)

With a game pad or keyboard, the D-pad, stick or arrow keys move the focus, **A** (or Enter / Space) activates and **B** (or Esc) is Back / Cancel.

### Action Buttons

| Button | Action |
|--------|--------|
| **BACK** | Return to VNC session without changes |
| **EXIT TO LAUNCHER** | Quit VNC client, return to app launcher |
| **SAVE & RECONNECT** | Save settings to config file, reconnect to server |

Changes saved via "Save & Reconnect" are written to the config file
(`/opt/vnc_client/vnc_client.conf`) and take effect immediately.

No touch events are sent to the VNC server while holding the exit zone.
A progress bar appears in the top-left corner during the 3-second hold.

### Reconnect Screen

When the VNC connection drops, the client shows a reconnect screen with three buttons:

| Button | Action |
|--------|--------|
| **CANCEL** | Return to app launcher |
| **SETTINGS** | Open the settings screen to change server/password |
| **CONNECT NOW** | Skip the countdown and reconnect immediately |

The SETTINGS button is useful when the connection fails due to wrong server address,
port, or password — you can fix the configuration without restarting the client.

---

## USB Input Forwarding

In addition to touch, the VNC client forwards USB keyboard and mouse events to the remote VNC server. Input handling is implemented in [`vnc_input.c`](vnc_input.c) / [`vnc_input.h`](vnc_input.h).

### USB Keyboard

Full forwarding of key press/release events to the VNC server as **X11 keysyms**:

- All standard keys mapped — letters, numbers, F-keys (F1–F12), arrow keys, modifiers (Shift, Ctrl, Alt), punctuation, Enter, Backspace, Tab, Escape
- Auto-repeat events from the kernel are forwarded as-is
- Key press and key release are sent as separate VNC key events, preserving modifier state
- Works identically to a keyboard connected directly to the remote machine

### USB Mouse

Relative mouse movements are accumulated with **3-tier acceleration** and forwarded as VNC absolute pointer events in remote desktop coordinate space:

- **Slow** (< 3 px) — 1:1 mapping for precision
- **Medium** (3–10 px) — 2× multiplier
- **Fast** (> 10 px) — 4× multiplier

Button support:
- Left click, right click, middle click
- Scroll wheel up/down (VNC button 4/5)

> **Note:** The mouse operates in **remote desktop coordinates** (e.g. 1920×1080), not local 800×480 screen space. Cursor position is tracked internally and scaled to the remote resolution before sending pointer events.

### USB Game Pad

A game pad drives the remote pointer. Buttons follow the pad's configured button map (`/etc/input_config.conf`); the names are an Xbox layout.

| Control | Action |
|---------|--------|
| Left stick | Move the pointer; full deflection crosses the screen in about 1.5 s |
| D-pad | Move the pointer slower (40 % of full deflection) |
| **A** | Left button, held while pressed, so dragging works |
| **B** | Right button |
| **LB** / **RB** | Scroll wheel up / down, one notch per press |
| Hold **Select** 1.5 s | Open the Settings screen |

### Auto-Detection

USB devices are auto-detected by watching the `/dev/input` fingerprint **every 1 s** and rescanning on a change. USB hotplug is fully supported — plug in a keyboard or mouse while the VNC client is running and it starts working immediately.

### Touch Preserved

Existing touch forwarding and the exit gesture (long-press top-left corner) are unchanged. All input methods work simultaneously — touch, keyboard, and mouse can be used interchangeably.

### Configuration

Mouse sensitivity is configurable via `/etc/input_config.conf` (shared with native apps):

```ini
# /etc/input_config.conf
mouse_sensitivity=1.0
mouse_acceleration=2.0
```

See [native_apps/README.md](../native_apps/README.md#input-configuration) for the full configuration reference.

---

## Optimizations

Key techniques adopted from the ScummVM Arca backend:

| ID | Technique | Impact |
|----|-----------|--------|
| O1 | Tight/ZRLE encoding negotiation | 10-100× bandwidth reduction |
| O2 | Precomputed bilinear X-LUT | No per-pixel division |
| O3 | Border-only clearing | Avoids full-screen memset |
| O8 | 16bpp RGB565 framebuffer | Halves fb_swap cost |
| O11 | Row deduplication (temp row) | Skips ~57% of identical rows |
| — | Bilinear interpolation | Smooth text at 2.4:1 downscale |
| — | Partial region updates | Only dirty rectangles rendered |
| — | Frame-rate-capped present | No unnecessary fb_swap calls |

---

## Hardware

| Component | Specification |
|-----------|--------------|
| CPU | ARM Cortex-A8 @ 600 MHz |
| RAM | 234 MB (182 MB available) |
| Display | 800×480 framebuffer (`/dev/fb0`) |
| Input | Projected-capacitive touch, single-touch via driver (`/dev/input/event0`) + USB keyboard/mouse (`/dev/input/event*`) |
| Network | 10/100 Mbps Ethernet |

## Dependencies

| Library | Version | License |
|---------|---------|---------|
| LibVNCClient | 0.9.14 | GPL-2.0-or-later |
| zlib | 1.2.13 | zlib |
| libjpeg-turbo | 2.1.5.1 | BSD-style |

Built as static libraries by `build-deps.sh` using the ARM cross-compiler.

---

## Troubleshooting

### Connection Failed
- Verify x11vnc is running: `ssh pi@<rpi> 'systemctl status x11vnc-arca'`
- Check port: `nmap -p 5900 <rpi_ip>`
- Check password in `/opt/vnc_client/vnc_client.conf`

### Colors Wrong (orange ↔ blue)
- x11vnc sends BGR byte order; the renderer handles this automatically
- If colors are swapped, check `vnc_renderer.c` BGR→RGB565 conversion

### Touch Not Working
- Verify device: `ls -l /dev/input/event0`
- Check debug output for "Touch" messages

### Poor Frame Rate
- Lower `quality_level` in config (trades JPEG quality for speed)
- Raise `compress_level` (more server CPU, less bandwidth)
- Normal for this hardware is 5-7 fps with 1080p source

### System Reboots
- The watchdog thread feeds `/dev/watchdog` every 30 s
- If the client crashes without cleanup, the system will reboot in ~60 s

---

## Auto-Start on Boot

**Do not write a second init script.** Boot is owned by `/etc/init.d/arca-app`, which respawns
whatever `/opt/arca/default-app` points at — so a private `S99vnc-client` gives you two things
starting apps and two different ideas of how to stop them, which is the defect the project spent a
session chasing. The supported route is one command:

```bash
./build-and-deploy.sh <ip> set-default        # boot straight into the VNC client
```

To go back to the launcher grid:

```bash
ssh root@<ip> 'echo /opt/arca/app_launcher > /opt/arca/default-app'
ssh root@<ip> '/etc/init.d/arca-app restart'
```

Stopping and starting by hand, and seeing what is actually running:

```bash
ssh root@<ip> '/etc/init.d/arca-app stop'      # matches on the executable, so it
                                                     # catches a client the launcher started
ssh root@<ip> '/etc/init.d/arca-app start'
ssh root@<ip> '/etc/init.d/arca-app status'    # use this, not `ps w` — see
                                                     # ../SYSTEM_ANALYSIS.md section 5.3
```

---

## Version History

| Version | Date | Changes |
|---------|------|---------|
| v1.0 | 2026-02-27 | MVP: raw encoding, 99% CPU, <1 fps |
| v2.0 | 2026-03-01 | Tight encoding, 16bpp, partial updates, row dedup → 5% CPU, 5 fps |
| v2.1 | 2026-03-01 | Bilinear scaling, runtime config file, exit gesture, merged docs |
| v2.3 | 2026-03-20 | Settings GUI: on-screen config editor (host, port, compress, quality) |
| v2.4 | 2026-03-22 | Full keyboard: all settings editable, SETTINGS button on reconnect screen |

---

## License

Custom code (`vnc_client.c`, `vnc_renderer.c`, `vnc_input.c`) is provided as-is
for use with the Steelcase RoomWizard device. Dependencies carry their own licenses (see above).
