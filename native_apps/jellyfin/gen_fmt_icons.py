#!/usr/bin/env python3
"""Draw icons/ludfin-fmt-<container>.ppm, the format icon of a Ludfin track row: a
light page with a coloured band across it carrying the format's name (FLAC, MP3, ...).
Run from native_apps/jellyfin/ with Pillow (WSL has it):

    python3 gen_fmt_icons.py

SIZE is the icon square of a Ludfin list row: the list is 40 px rows (jf_ui.c lv_init)
and list_view.c draws the icon in row_h - 8 = 32 px.  The widget scales any tile to that
square, so a different row height only softens the icon.  The corners outside the page are
pure white, the blit's colour key, so the row shows through on every row (cursor too); edge
pixels are blended against LV_COLOR_ROW, and no pixel of the page or band is pure white.

The container names are what the Jellyfin server sends as a track's Container (the
first name of its comma list).  Ludfin loads icons/ludfin-fmt-<container>.ppm from
/opt/arca/icons/ by that name and keeps the page glyph for any name with no file, so
this table is the only list of formats.  Lossless is teal, lossy is amber, the rest
(uncompressed PCM) is blue.
"""
import os
from PIL import Image, ImageDraw, ImageFont

SIZE = 32
SS = 4
KEY_BG = (255, 255, 255)     # the blit's colour key (list_view.c passes 0xFFFFFFFF): the corners stay see-through on any row
ROW_BG = (30, 30, 45)        # list_view.c LV_COLOR_ROW: what the edge pixels blend against
PAGE = (232, 232, 238)
FOLD = (176, 176, 190)
FONT = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"

LOSSLESS = (14, 120, 112)
LOSSY = (196, 110, 14)
PCM = (46, 98, 190)

FORMATS = {
    "flac": ("FLAC", LOSSLESS),
    "alac": ("ALAC", LOSSLESS),
    "ape":  ("APE",  LOSSLESS),
    "wv":   ("WV",   LOSSLESS),
    "wav":  ("WAV",  PCM),
    "aiff": ("AIFF", PCM),
    "dsf":  ("DSD",  PCM),
    "mp3":  ("MP3",  LOSSY),
    "asf":  ("WMA",  LOSSY),
    "mov":  ("M4A",  LOSSY),
    "m4a":  ("M4A",  LOSSY),
    "aac":  ("AAC",  LOSSY),
    "ogg":  ("OGG",  LOSSY),
    "opus": ("OPUS", LOSSY),
}


def draw(label, colour):
    n = SIZE * SS
    img = Image.new("RGB", (n, n), ROW_BG)
    d = ImageDraw.Draw(img)
    # the page: two thirds of the square wide, nearly full height, top-right corner folded
    pw = SIZE * 2 // 3
    px0 = (SIZE - pw) // 2 * SS
    px1 = px0 + pw * SS
    py0, py1 = 1 * SS, (SIZE - 1) * SS
    cut = 6 * SS
    d.polygon(((px0, py0), (px1 - cut, py0), (px1, py0 + cut), (px1, py1), (px0, py1)), fill=PAGE)
    d.polygon(((px1 - cut, py0), (px1 - cut, py0 + cut), (px1, py0 + cut)), fill=FOLD)
    # the band: wider than the page so four letters fit
    bx0, bx1 = 1 * SS, (SIZE - 1) * SS
    by0, by1 = 13 * SS, 25 * SS
    d.rectangle((bx0, by0, bx1, by1), fill=colour)
    # the largest bold size that fits the band with a 1 px margin
    aw, ah = (bx1 - bx0) - 3 * SS, (by1 - by0) - 3 * SS
    size = 40 * SS
    while size > 4:
        f = ImageFont.truetype(FONT, size)
        l, t, r, b = d.textbbox((0, 0), label, font=f)
        if r - l <= aw and b - t <= ah:
            break
        size -= 1
    cx, cy = (bx0 + bx1) // 2, (by0 + by1) // 2
    d.text((cx - (l + r) // 2, cy - (t + b) // 2), label, font=f, fill=(250, 250, 250))
    # BOX, not LANCZOS: LANCZOS overshoots the letters' edges back to pure white, which the blit then skips.
    # Edges blend against the row colour; only a pixel wholly outside page and band becomes the key, and
    # every other pixel is held under it.
    small = img.resize((SIZE, SIZE), Image.BOX)
    inside = Image.new("L", (n, n), 0)
    m = ImageDraw.Draw(inside)
    m.polygon(((px0, py0), (px1 - cut, py0), (px1, py0 + cut), (px1, py1), (px0, py1)), fill=255)
    m.rectangle((bx0, by0, bx1, by1), fill=255)
    inside = inside.resize((SIZE, SIZE), Image.BOX)
    out = Image.new("RGB", (SIZE, SIZE), KEY_BG)
    for y in range(SIZE):
        for x in range(SIZE):
            if inside.getpixel((x, y)):
                out.putpixel((x, y), tuple(min(c, 254) for c in small.getpixel((x, y))))
    return out


def main():
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "icons")
    os.makedirs(out, exist_ok=True)
    for name, (label, colour) in FORMATS.items():
        draw(label, colour).save(os.path.join(out, "ludfin-fmt-%s.ppm" % name), format="PPM")


if __name__ == "__main__":
    main()
