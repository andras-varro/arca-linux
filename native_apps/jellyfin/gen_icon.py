#!/usr/bin/env python3
"""Draw jellyfin.ppm, the Jellyfin tile's 96x96 icon: a dark rounded square with a
violet-to-blue play triangle in a ring.  Run from native_apps/jellyfin/ with Pillow
(WSL has it):

    python3 gen_icon.py

Supersampled 4x and reduced, so edges are smooth.  The corners outside the rounded
square are the tile's own background colour so the icon sits flush on it.
"""
from PIL import Image, ImageDraw

SIZE = 96
SS = 4
BG = (24, 24, 24)
BODY = (30, 34, 60)
RING = (120, 110, 235)
PLAY = (90, 190, 235)


def main():
    n = SIZE * SS
    img = Image.new("RGB", (n, n), BG)
    d = ImageDraw.Draw(img)
    m = 6 * SS
    d.rounded_rectangle((m, m, n - m, n - m), radius=14 * SS, fill=BODY)
    c = n // 2
    r = 28 * SS
    d.ellipse((c - r, c - r, c + r, c + r), outline=RING, width=5 * SS)
    d.polygon(((c - 9 * SS, c - 15 * SS), (c - 9 * SS, c + 15 * SS), (c + 16 * SS, c)), fill=PLAY)
    img = img.resize((SIZE, SIZE), Image.LANCZOS)
    img.save("jellyfin.ppm", format="PPM")


if __name__ == "__main__":
    main()
