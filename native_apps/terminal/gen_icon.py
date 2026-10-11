#!/usr/bin/env python3
"""Draw terminal.ppm, the Terminal tile's 96x96 icon: a dark rounded square with a
green ">_" prompt.  Run from native_apps/terminal/ with Pillow (WSL has it):

    python3 gen_icon.py

Supersampled 4x and reduced, so the rounded corners and strokes are smooth.
The tile behind it is the launcher's own; the corners outside the rounded square
are the tile's background colour so the icon sits flush on it.
"""
from PIL import Image, ImageDraw

SIZE = 96
SS = 4                      # supersampling factor
BG = (24, 24, 24)           # outside the rounded square (matches the other tiles' ground)
BODY = (38, 50, 56)         # the terminal window
BAR = (22, 30, 34)          # its title bar
PROMPT = (110, 230, 130)    # green prompt


def main():
    n = SIZE * SS
    img = Image.new("RGB", (n, n), BG)
    d = ImageDraw.Draw(img)
    m = 6 * SS
    d.rounded_rectangle((m, m, n - m, n - m), radius=14 * SS, fill=BODY)
    d.rounded_rectangle((m, m, n - m, m + 16 * SS), radius=14 * SS, fill=BAR)
    d.rectangle((m, m + 10 * SS, n - m, m + 16 * SS), fill=BAR)
    for i, c in enumerate(((255, 95, 86), (255, 189, 46), (39, 201, 63))):
        cx = m + (10 + 9 * i) * SS
        cy = m + 8 * SS
        d.ellipse((cx - 3 * SS, cy - 3 * SS, cx + 3 * SS, cy + 3 * SS), fill=c)
    w = 5 * SS
    # ">" chevron
    d.line((28 * SS, 42 * SS, 44 * SS, 54 * SS, 28 * SS, 66 * SS), fill=PROMPT, width=w, joint="curve")
    # "_" cursor
    d.rectangle((50 * SS, 67 * SS, 72 * SS, 67 * SS + w), fill=PROMPT)
    img = img.resize((SIZE, SIZE), Image.LANCZOS)
    img.save("terminal.ppm", format="PPM")


if __name__ == "__main__":
    main()
