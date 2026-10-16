#!/usr/bin/env python3
"""Draw file_manager.ppm, the Files tile's 96x96 icon: the same dark rounded square as the
Terminal tile, holding two side-by-side panes (a header bar and three rows each), the
left one with an amber folder row on top and the right one with file rows.  Run from
native_apps/file_manager/ with Pillow (WSL has it):

    python3 gen_icon.py

Supersampled 4x and reduced, so the rounded corners and strokes are smooth.  The corners
outside the rounded square are the tile's background colour so the icon sits flush on it.
"""
from PIL import Image, ImageDraw

SIZE = 96
SS = 4                      # supersampling factor
BG = (24, 24, 24)           # outside the rounded square (matches the other tiles' ground)
BODY = (38, 50, 56)         # the window
PANE = (22, 30, 34)         # a pane's ground
HDR_ON = (40, 70, 120)      # the active pane's header
HDR_OFF = (60, 62, 78)
ROW = (52, 66, 74)
FOLDER = (235, 170, 40)
FILE = (205, 205, 215)
MARK = (255, 210, 60)


def main():
    n = SIZE * SS
    img = Image.new("RGB", (n, n), BG)
    d = ImageDraw.Draw(img)
    m = 6 * SS
    d.rounded_rectangle((m, m, n - m, n - m), radius=14 * SS, fill=BODY)

    pw = 33 * SS                     # a pane's width
    top = 15 * SS
    bot = n - 15 * SS
    for i, hdr in enumerate((HDR_ON, HDR_OFF)):
        x0 = 13 * SS + i * (pw + 4 * SS)
        d.rounded_rectangle((x0, top, x0 + pw, bot), radius=3 * SS, fill=PANE)
        d.rounded_rectangle((x0, top, x0 + pw, top + 9 * SS), radius=3 * SS, fill=hdr)
        d.rectangle((x0, top + 5 * SS, x0 + pw, top + 9 * SS), fill=hdr)
        for r in range(4):
            y = top + 13 * SS + r * 14 * SS
            d.rectangle((x0 + 3 * SS, y, x0 + pw - 3 * SS, y + 10 * SS), fill=ROW)
            ix = x0 + 5 * SS
            if i == 0 or r == 0:
                # a folder: a tab and a body
                d.rectangle((ix, y + 2 * SS, ix + 5 * SS, y + 4 * SS), fill=FOLDER)
                d.rectangle((ix, y + 3 * SS, ix + 10 * SS, y + 8 * SS), fill=FOLDER)
            else:
                d.rectangle((ix + SS, y + 2 * SS, ix + 8 * SS, y + 8 * SS), fill=FILE)
            d.rectangle((ix + 13 * SS, y + 4 * SS, ix + 13 * SS + (7 + 3 * (r % 3)) * SS, y + 6 * SS),
                        fill=MARK if (i == 1 and r == 2) else (150, 160, 170))
    img = img.resize((SIZE, SIZE), Image.LANCZOS)
    img.save("file_manager.ppm", format="PPM")


if __name__ == "__main__":
    main()
