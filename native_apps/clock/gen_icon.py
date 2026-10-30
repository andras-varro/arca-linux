#!/usr/bin/env python3
"""Draw clock.ppm, the Clock tile's 96x96 icon: the launcher's dark rounded square holding a round
clock face.  Run from native_apps/clock/ with Pillow (WSL has it):

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
    c = n // 2
    r = 31 * SS
    d.ellipse((c - r, c - r, c + r, c + r), fill=(235, 235, 235))
    r2 = 27 * SS
    d.ellipse((c - r2, c - r2, c + r2, c + r2), fill=(24, 30, 36))
    d.line((c, c, c, c - 20 * SS), fill=(255, 200, 60), width=4 * SS)
    d.line((c, c, c + 15 * SS, c + 6 * SS), fill=(235, 235, 235), width=4 * SS)
    d.ellipse((c - 3 * SS, c - 3 * SS, c + 3 * SS, c + 3 * SS), fill=(235, 235, 235))
    img.resize((SIZE, SIZE), Image.LANCZOS).save("clock.ppm")


if __name__ == "__main__":
    main()
