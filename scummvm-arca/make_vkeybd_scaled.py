#!/usr/bin/env python3
"""
Scale vkeybd_small.zip from 320x240 to 640x480 (2x) and produce
vkeybd_arca.zip suitable for the Arca 800x480 screen.

Usage: python3 make_vkeybd_scaled.py <input_zip> <output_zip> [scale]
  default scale = 2
"""
import sys, re, zipfile, io
from PIL import Image

def scale_bmp(data, scale):
    img = Image.open(io.BytesIO(data))
    new_size = (img.width * scale, img.height * scale)
    img_scaled = img.resize(new_size, Image.NEAREST)
    buf = io.BytesIO()
    img_scaled.save(buf, format="BMP")
    return buf.getvalue()

def scale_xml(xml_text, scale, old_res, new_res, old_w, old_h, new_w, new_h):
    # Update resolution attributes
    xml_text = xml_text.replace(f'resolutions="{old_res}"', f'resolutions="{new_res}"')
    xml_text = xml_text.replace(f'resolution="{old_res}"', f'resolution="{new_res}"')
    # Update bitmap filenames (320x240 → 640x480 in name)
    xml_text = xml_text.replace(f'{old_w}x{old_h}.bmp', f'{new_w}x{new_h}.bmp')

    def scale_coords(m):
        nums = [int(x) * scale for x in m.group(1).split(',')]
        return 'coords="' + ','.join(str(n) for n in nums) + '"'

    xml_text = re.sub(r'coords="([\d,]+)"', scale_coords, xml_text)
    return xml_text

def main():
    input_zip  = sys.argv[1] if len(sys.argv) > 1 else "vkeybd_small.zip"
    output_zip = sys.argv[2] if len(sys.argv) > 2 else "vkeybd_arca.zip"
    scale      = int(sys.argv[3]) if len(sys.argv) > 3 else 2

    old_w, old_h = 320, 240
    new_w, new_h = old_w * scale, old_h * scale
    old_res = f"{old_w}x{old_h}"
    new_res = f"{new_w}x{new_h}"

    with zipfile.ZipFile(input_zip, 'r') as zin, \
         zipfile.ZipFile(output_zip, 'w', zipfile.ZIP_DEFLATED) as zout:
        for name in zin.namelist():
            data = zin.read(name)
            if name.endswith('.bmp'):
                new_name = name.replace(f'{old_w}x{old_h}', f'{new_w}x{new_h}')
                print(f"  Scaling {name} -> {new_name}")
                zout.writestr(new_name, scale_bmp(data, scale))
            elif name.endswith('.xml'):
                # Rename xml from vkeybd_small.xml to vkeybd_arca.xml
                new_name = name.replace('vkeybd_small', 'vkeybd_arca')
                xml_text = data.decode('utf-8')
                xml_text = scale_xml(xml_text, scale, old_res, new_res, old_w, old_h, new_w, new_h)
                print(f"  Patching {name} -> {new_name}")
                zout.writestr(new_name, xml_text.encode('utf-8'))
            else:
                zout.writestr(name, data)

    print(f"Done: {output_zip} ({new_w}x{new_h})")

if __name__ == '__main__':
    main()
