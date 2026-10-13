# Arca Backend for ScummVM

This is a custom ScummVM backend for the Steelcase RoomWizard device, implementing native framebuffer graphics and touch input support.

## Features

- **Native Framebuffer Rendering**: Direct rendering to `/dev/fb0` (800x480 panel @ 16bpp RGB565)
- **Touch Input Support**: Single-touch projected-capacitive touchscreen via `/dev/input/event0`
- **Automatic Scaling**: Games are aspect-fitted and centred in the visible screen area (the
  framebuffer layer excludes the pixels hidden by the device's bezel)
- **Software Cursor**: Rendered cursor with palette support
- **Audio via ALSA**: OPL/AdLib music and SFX through `../../native_apps/common/audio_out.c` (`plughw:N,0`, panel speaker or USB DAC)
- **Virtual Keyboard**: On-screen keyboard via triple-tap gesture
- **POSIX Filesystem**: Standard save/load functionality

## Hardware Requirements

- **Display**: 800x480 framebuffer (RGB/ARGB)
- **Input**: Projected-capacitive touchscreen (single-touch)
- **CPU**: ARMv7 with NEON SIMD
- **RAM**: 128+ MB available
- **OS**: Linux with framebuffer and input event support

## Building

Build with `../build-and-deploy.sh` — the recipe (soft-float `arm-linux-gnueabi`, dynamic, ALSA) is in
[`../README.md`](../README.md) → *Build*.

## Supported Pixel Formats

- **CLUT8**: 8-bit indexed color (with palette)
- **RGB565**: 16-bit color
- **ARGB8888**: 32-bit color with alpha

## Input Mapping

- **Touch**: Left mouse button
- **Long Press (500ms)**: Right mouse button
- **Drag**: Mouse movement
- **Triple-tap bottom-right**: Global Main Menu (Ctrl+F5)
- **Triple-tap bottom-left**: Virtual Keyboard
- **Triple-tap top-right**: Enter key

## Limitations

- **Single-Touch Only**: No multi-touch gestures; right-click = long-press 500 ms
- **No MIDI**: NullMidiDriver (OPL emulation works for AdLib music)
- **Software Rendering**: No hardware acceleration
- **Volume**: Fixed 50% attenuation to prevent speaker distortion

## Dependencies

The backend links against the Arca native libraries:

- `native_apps/common/framebuffer.c` - Framebuffer management
- `native_apps/common/touch_input.c` - Touch input handling

## Architecture

```
ScummVM Core
    ↓
OSystem_Arca (backends/platform/arca/arca.cpp)
    ├── ArcaGraphicsManager (arca-graphics.cpp)
    │   └── framebuffer.c (native library)
    ├── ArcaEventSource (arca-events.cpp)
    │   └── touch_input.c (native library)
    ├── OssMixerManager (oss-mixer.cpp)
    │   └── audio_out.c (native library) → ALSA plughw:N,0
    ├── DefaultTimerManager
    ├── DefaultEventManager
    └── DefaultSaveFileManager
```
```

## Testing

Tested with:
- Monkey Island 1 & 2
- Day of the Tentacle
- King's Quest 1–6 (AGI/SCI)
- Leisure Suit Larry 1–5
- Space Quest 1–5
- Simon the Sorcerer
- Beneath a Steel Sky
- Flight of the Amazon Queen
- Putt-Putt, Freddi Fish, Pajama Sam (HE)

## Author

Created for the Arca project, on the Steelcase RoomWizard embedded device.

## License

GPL v3+ (same as ScummVM)
