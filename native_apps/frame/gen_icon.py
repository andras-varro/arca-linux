#!/usr/bin/env python3
"""Draw frame.ppm, the Pictures tile's 96x96 icon: the launcher's dark rounded square holding a
picture frame with a tiny mountain and sun.  Run from native_apps/frame/ with Pillow (WSL has it):

    python3 gen_icon.py
"""
from PIL import Image, ImageDraw

SIZE, SS = 96, 4


def main():
    n = SIZE * SS
    img = Image.new("RGB", (n, n), (24, 24, 24))
    d = ImageDraw.Draw(img)
    m = 6 * SS
    d.rounded_rectangle((m, m, n - m, n - m), radius=14 * SS, fill=(38, 50, 56))
    x0, y0, x1, y1 = 16 * SS, 22 * SS, 80 * SS, 74 * SS
    d.rectangle((x0, y0, x1, y1), fill=(200, 160, 70))              # frame
    ix0, iy0, ix1, iy1 = x0 + 5 * SS, y0 + 5 * SS, x1 - 5 * SS, y1 - 5 * SS
    d.rectangle((ix0, iy0, ix1, iy1), fill=(120, 180, 235))         # sky
    d.ellipse((ix1 - 18 * SS, iy0 + 3 * SS, ix1 - 6 * SS, iy0 + 15 * SS), fill=(250, 220, 90))
    d.polygon([(ix0, iy1), (ix0 + 16 * SS, iy0 + 14 * SS), (ix0 + 30 * SS, iy1)], fill=(70, 110, 80))
    d.polygon([(ix0 + 18 * SS, iy1), (ix0 + 34 * SS, iy0 + 22 * SS), (ix1, iy1)], fill=(50, 90, 60))
    img.resize((SIZE, SIZE), Image.LANCZOS).save("frame.ppm")


if __name__ == "__main__":
    main()
