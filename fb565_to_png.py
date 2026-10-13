#!/usr/bin/env python3
"""
Convert an Arca /dev/fb0 framebuffer dump to PNG.

CONFIRMED HARDWARE (RW09 / .73, fbset): /dev/fb0 is 800x480, **32bpp**,
XRGB8888 little-endian (rgba 8/16,8/8,8/0,0/0 -> byte order B,G,R,X),
stride 3200, one page = 800*480*4 = 1,536,000 bytes. `cat /dev/fb0` yields
exactly one 32bpp frame.

Historical note: a one-frame 32bpp dump (1,536,000 bytes) is the SAME size as
two 16bpp RGB565 pages, which is why the old 16bpp decoder appeared to "work"
on file size while producing garbage pixels. Default is now 32bpp; pass
--bpp 16 only for genuinely 16bpp firmware.

Usage:
  python3 fb565_to_png.py input.raw output.png [--bpp 32|16] [--page N]

Typical capture:
  ssh root@<ip> cat /dev/fb0 > fb.raw
  python3 fb565_to_png.py fb.raw fb.png
"""

import sys
from PIL import Image

WIDTH, HEIGHT = 800, 480


def convert_32(page_data):
    # XRGB8888 little-endian: bytes are B, G, R, X per pixel.
    out = bytearray(WIDTH * HEIGHT * 3)
    n = WIDTH * HEIGHT
    j = 0
    for i in range(0, n * 4, 4):
        out[j]     = page_data[i + 2]  # R
        out[j + 1] = page_data[i + 1]  # G
        out[j + 2] = page_data[i]      # B
        j += 3
    return out


def convert_16(page_data):
    # RGB565 little-endian -> RGB888
    out = bytearray(WIDTH * HEIGHT * 3)
    j = 0
    for i in range(0, WIDTH * HEIGHT * 2, 2):
        v = page_data[i] | (page_data[i + 1] << 8)
        r = (v >> 11) & 0x1F
        g = (v >> 5) & 0x3F
        b = v & 0x1F
        out[j]     = (r << 3) | (r >> 2)
        out[j + 1] = (g << 2) | (g >> 4)
        out[j + 2] = (b << 3) | (b >> 2)
        j += 3
    return out


def convert(input_file, output_file, bpp=32, page=0):
    with open(input_file, "rb") as f:
        data = f.read()

    bytes_per_pixel = bpp // 8
    page_bytes = WIDTH * HEIGHT * bytes_per_pixel

    start = page * page_bytes
    page_data = data[start:start + page_bytes]
    if len(page_data) < page_bytes:
        page_data += b"\x00" * (page_bytes - len(page_data))

    out = convert_32(page_data) if bpp == 32 else convert_16(page_data)

    img = Image.frombytes("RGB", (WIDTH, HEIGHT), bytes(out))
    img.save(output_file, "PNG")
    print("Saved %s (%d-bit, page %d)" % (output_file, bpp, page))


if __name__ == "__main__":
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(1)

    bpp = 32
    if "--bpp" in sys.argv:
        bpp = int(sys.argv[sys.argv.index("--bpp") + 1])
        if bpp not in (16, 32):
            print("--bpp must be 16 or 32")
            sys.exit(1)

    page = 0
    if "--page" in sys.argv:
        page = int(sys.argv[sys.argv.index("--page") + 1])

    convert(sys.argv[1], sys.argv[2], bpp=bpp, page=page)
