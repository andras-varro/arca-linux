#!/usr/bin/env python3
"""Draw ludkino.ppm, the Ludkino tile's 96x96 icon: a dark rounded square holding a
screen (rounded rectangle outline) with a play triangle.  Run from native_apps/jfvideo/
with Pillow (WSL has it):

    python3 gen_icon.py

Supersampled 4x and reduced, so edges are smooth.  The corners outside the rounded
square are the tile's own background colour so the icon sits flush on it.
"""
from PIL import Image, ImageDraw

SIZE = 96
SS = 4
BG = (24, 24, 24)
BODY = (50, 28, 34)
FRAME = (235, 130, 90)
PLAY = (245, 200, 90)


def main():
    n = SIZE * SS
    img = Image.new("RGB", (n, n), BG)
    d = ImageDraw.Draw(img)
    m = 6 * SS
    d.rounded_rectangle((m, m, n - m, n - m), radius=14 * SS, fill=BODY)
    d.rounded_rectangle((20 * SS, 28 * SS, 76 * SS, 68 * SS), radius=6 * SS, outline=FRAME, width=5 * SS)
    c = n // 2
    cy = 48 * SS
    d.polygon(((c - 7 * SS, cy - 12 * SS), (c - 7 * SS, cy + 12 * SS), (c + 13 * SS, cy)), fill=PLAY)
    img = img.resize((SIZE, SIZE), Image.LANCZOS)
    img.save("ludkino.ppm", format="PPM")


if __name__ == "__main__":
    main()
