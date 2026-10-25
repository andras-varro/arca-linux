#!/usr/bin/env python3
"""Draw retro_picker.ppm, the Retro tile's 96x96 icon: the launcher's dark rounded square
holding a game cartridge (grip ridges, a label with a pixel-art invader).  Run from
native_apps/retro_picker/ with Pillow (WSL has it):

    python3 gen_icon.py
"""
from PIL import Image, ImageDraw

SIZE, SS = 96, 4
BG = (24, 24, 24)
BODY = (38, 50, 56)
CART = (120, 124, 140)
CART_DK = (80, 84, 100)
LABEL = (235, 170, 40)
INK = (30, 30, 40)
INVADER = ["00100000100", "00010001000", "00111111100", "01101110110",
           "11111111111", "10111111101", "10100000101", "00011011000"]


def main():
    n = SIZE * SS
    img = Image.new("RGB", (n, n), BG)
    d = ImageDraw.Draw(img)
    m = 6 * SS
    d.rounded_rectangle((m, m, n - m, n - m), radius=14 * SS, fill=BODY)
    x0, x1, y0, y1 = 20 * SS, 76 * SS, 14 * SS, 82 * SS
    d.rounded_rectangle((x0, y0, x1, y1), radius=4 * SS, fill=CART)
    d.rectangle((x0 + 6 * SS, y0 - 1, x1 - 6 * SS, y0 + 5 * SS), fill=CART_DK)
    for i in range(5):                                   # grip ridges
        y = y0 + 8 * SS + i * 3 * SS
        d.rectangle((x0 + 6 * SS, y, x1 - 6 * SS, y + SS), fill=CART_DK)
    lx0, ly0, lx1, ly1 = x0 + 5 * SS, y0 + 26 * SS, x1 - 5 * SS, y1 - 6 * SS
    d.rounded_rectangle((lx0, ly0, lx1, ly1), radius=2 * SS, fill=LABEL)
    px = 4 * SS
    ox = (lx0 + lx1) // 2 - len(INVADER[0]) * px // 2 + SS // 2
    oy = (ly0 + ly1) // 2 - len(INVADER) * px // 2 + SS // 2
    for r, row in enumerate(INVADER):
        for c, ch in enumerate(row):
            if ch == "1":
                d.rectangle((ox + c * px, oy + r * px, ox + (c + 1) * px - 1, oy + (r + 1) * px - 1), fill=INK)
    img = img.resize((SIZE, SIZE), Image.LANCZOS)
    img.save("retro_picker.ppm")


if __name__ == "__main__":
    main()
