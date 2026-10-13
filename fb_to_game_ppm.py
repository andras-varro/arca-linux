#!/usr/bin/env python3
"""
Recover a ScummVM game surface from an Arca /dev/fb0 grab.

The Arca ScummVM backend blits the game surface to the panel with a
nearest-neighbour integer upscale. That map is SURJECTIVE: every source pixel
lands on at least one destination pixel whenever the scale is >= 1, so it can be
inverted exactly, with no filtering and no guessing, by reading one
representative destination pixel per source pixel.

FORWARD MAP (measured in scummvm-arca/backend-files/arca-graphics.cpp,
the upscale loop at 440-573). Integer, TRUNCATING:

    srcX = (dx * srcW) // scaledW      clamped to srcW - 1
    srcY = (dy * srcH) // scaledH      clamped to srcH - 1
    destination pixel is LOGICAL (offsetX + dx, offsetY + dy)

INVERSE: bucket every dx by its srcX in one forward pass, then take the MIDDLE
dx of each bucket. The middle is chosen deliberately - a bucket edge is where the
truncation boundary sits, so an off-by-one in the rect or the stride shows up as
wrong pixels instead of being absorbed. Rows are inverted the same way.

If any source column or row ends up with no representative, the map was a
DOWNSCALE on that axis and is not invertible; this tool refuses (exit 2) rather
than inventing pixels.

WHERE --rect COMES FROM. It is (offsetX, offsetY, scaledWidth, scaledHeight) as
getScalingInfo() computes them - arca-graphics.cpp:138-172. Run that same
integer chain with the unit's own bezel and touch insets; do not guess it and do
not let a bounding box stand in for it.

COORDINATES. The grab is in PANEL pixels; the backend works in LOGICAL pixels,
which are panel pixels minus the bezel viewport origin. So:

    panel_x = view_x + offsetX + dx
    panel_y = view_y + offsetY + dy

--rect is in LOGICAL coordinates and --view supplies the viewport origin, which
is (bezel_left, bezel_top). Pass --view 0,0 to treat --rect as panel coordinates.

STRIDE. A grab has line_length bytes per row, not logical-width bytes, so rows
are addressed with --stride-px (default 800, the panel width) and NOT with the
rect width. A wrong stride skews the read; --self-test has a control for it.

BPP. ScummVM and the VNC remote session run 16bpp RGB565 little-endian, which is
the default here. Native apps run 32bpp XRGB8888. Run "fbset | grep geometry" on
the device and believe it: a 32bpp frame is exactly the size of two 16bpp pages,
so file size never catches the mistake.

--auto-rect IS A SANITY CHECK, NOT A MEASUREMENT, and it has already been caught
lying. It reports the non-black bounding box, which is not the scaled rect: black
art at an edge shrinks it and any non-picture writer widens it. Measured on a real
KQ2 grab from this device, the box came back 732x450 at panel (0,30) - a box whose
bottom row (479) the backend cannot write at all under that unit's bezel, and
whose left edge disagrees with the engine arithmetic by tens of columns. Its
aspect (1.6267) is nevertheless only 1.7% off the 320x200 source aspect (1.6), so
it sails through the 2% aspect gate below while being wrong. Use --auto-rect only
to confirm that a computed rect is not absurd.

CURSOR. drawCursor() (arca-graphics.cpp:575-642) writes destructively at
OUTPUT resolution, after the scale. Pixels under a visible mouse cursor are not
game-surface pixels and this tool cannot tell the difference. Grab a frame with
the cursor hidden, or ignore that region.

Usage:
  python3 fb_to_game_ppm.py <fb.raw> <out.ppm> --rect X,Y,W,H --src WxH
                            [--bpp 16|32] [--stride-px N] [--view X,Y]
  python3 fb_to_game_ppm.py <fb.raw> <out.ppm> --auto-rect --src WxH [...]
  python3 fb_to_game_ppm.py --self-test

Exit codes: 0 ok, 1 usage or I/O error, 2 refused (not invertible, or a rect this
tool will not trust).

Typical capture (no scripted input is needed - passing the game id skips the
ScummVM launcher menu, and one PID must be verified before and after the grab):
  ssh root@<ip> /etc/init.d/arca-app stop
  ssh root@<ip> "nohup /opt/games/scummvm kq2 >/tmp/svm.log 2>&1 </dev/null &"
  ssh root@<ip> dd if=/dev/fb0 bs=768000 count=1 > fb.raw
  python3 fb_to_game_ppm.py fb.raw kq2.ppm --rect 39,0,722,451 --src 320x200 --view 0,15
"""

import sys

DEFAULT_STRIDE_PX = 800
ASPECT_TOLERANCE = 0.02


class Refused(Exception):
    """Raised when no trustworthy output can be produced (exit 2)."""


# ---------------------------------------------------------------- the inverse

def bucket_reps(src_n, scaled_n):
    """One forward pass. Returns (reps, n_missing).

    reps[s] is the middle destination index that maps to source index s.
    n_missing counts source indices with no destination pixel at all.
    """
    first = [-1] * src_n
    count = [0] * src_n
    for d in range(scaled_n):
        s = (d * src_n) // scaled_n
        if s >= src_n:
            s = src_n - 1
        if first[s] < 0:
            first[s] = d
        count[s] += 1
    reps = [(first[s] + count[s] // 2) if count[s] else -1 for s in range(src_n)]
    n_missing = count.count(0)
    return reps, n_missing


# ------------------------------------------------------------- pixel plumbing

def pack565(r, g, b):
    """RGB888 -> RGB565 word."""
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)


def unpack565(v):
    """RGB565 word -> RGB888 by bit replication."""
    r = (v >> 11) & 0x1F
    g = (v >> 5) & 0x3F
    b = v & 0x1F
    return ((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2))


def quantise565(r, g, b):
    """Round-trip a colour through RGB565 so a compare can be bit-exact."""
    return unpack565(pack565(r, g, b))


def px_reader(data, stride_px, bpp):
    """Return a function (x, y) -> (r, g, b) over a raw grab."""
    if bpp == 16:
        def rd(x, y):
            i = (y * stride_px + x) * 2
            return unpack565(data[i] | (data[i + 1] << 8))
    else:
        def rd(x, y):
            # XRGB8888 little-endian: bytes are B, G, R, X.
            i = (y * stride_px + x) * 4
            return (data[i + 2], data[i + 1], data[i])
    return rd


def px_writer(data, stride_px, bpp):
    """Return a function (x, y, r, g, b) -> None. For the self-test only."""
    if bpp == 16:
        def wr(x, y, r, g, b):
            i = (y * stride_px + x) * 2
            v = pack565(r, g, b)
            data[i] = v & 0xFF
            data[i + 1] = (v >> 8) & 0xFF
    else:
        def wr(x, y, r, g, b):
            i = (y * stride_px + x) * 4
            data[i] = b
            data[i + 1] = g
            data[i + 2] = r
            data[i + 3] = 0
    return wr


def panel_rows(data, stride_px, bpp):
    return len(data) // (stride_px * (bpp // 8))


# ----------------------------------------------------------------- extraction

def extract(data, stride_px, bpp, rect, src, view):
    """Recover the source surface. Returns RGB888 bytes, srcH rows of srcW."""
    rx, ry, dw, dh = rect
    src_w, src_h = src
    vx, vy = view

    reps_x, miss_x = bucket_reps(src_w, dw)
    reps_y, miss_y = bucket_reps(src_h, dh)
    if miss_x or miss_y:
        raise Refused(
            "not invertible: %d of %d source columns and %d of %d source rows "
            "have no destination pixel. %dx%d -> %dx%d is a DOWNSCALE on at "
            "least one axis, and nearest-neighbour discards those pixels."
            % (miss_x, src_w, miss_y, src_h, src_w, src_h, dw, dh))

    rows = panel_rows(data, stride_px, bpp)
    min_x = vx + rx + reps_x[0]
    min_y = vy + ry + reps_y[0]
    max_x = vx + rx + reps_x[-1]
    max_y = vy + ry + reps_y[-1]
    if min_x < 0 or min_y < 0 or max_x >= stride_px or max_y >= rows:
        raise Refused(
            "read out of bounds: the rect needs panel x %d..%d, y %d..%d but "
            "the grab is %d px wide (--stride-px) and %d rows deep."
            % (min_x, max_x, min_y, max_y, stride_px, rows))

    rd = px_reader(data, stride_px, bpp)
    out = bytearray(src_w * src_h * 3)
    j = 0
    for sy in range(src_h):
        py = vy + ry + reps_y[sy]
        for sx in range(src_w):
            r, g, b = rd(vx + rx + reps_x[sx], py)
            out[j] = r
            out[j + 1] = g
            out[j + 2] = b
            j += 3
    return out


def write_ppm(path, width, height, rgb):
    with open(path, "wb") as f:
        f.write(b"P6\n%d %d\n255\n" % (width, height))
        f.write(bytes(rgb))


# ------------------------------------------------------------------ auto-rect

def _row_planes(data, stride_px, bpp, y):
    """Return the colour planes of one panel row, as bytes objects."""
    bpb = bpp // 8
    start = y * stride_px * bpb
    row = data[start:start + stride_px * bpb]
    if bpp == 16:
        return (row[0::2], row[1::2])
    # Deliberately excludes the X byte: an XRGB pad of 0xFF would make every
    # pixel look non-black.
    return (row[0::4], row[1::4], row[2::4])


def auto_rect(data, stride_px, bpp, src):
    """Non-black bounding box of the grab, in PANEL coordinates.

    Returns (x, y, w, h). Refuses if the box is empty, or if its aspect is more
    than ASPECT_TOLERANCE away from the source aspect.
    """
    rows = panel_rows(data, stride_px, bpp)
    min_x, max_x = stride_px, -1
    min_y, max_y = rows, -1
    for y in range(rows):
        planes = _row_planes(data, stride_px, bpp, y)
        if not any(any(p) for p in planes):
            continue
        if y < min_y:
            min_y = y
        max_y = y
        for x in range(stride_px):
            if any(p[x] for p in planes):
                if x < min_x:
                    min_x = x
                break
        for x in range(stride_px - 1, -1, -1):
            if any(p[x] for p in planes):
                if x > max_x:
                    max_x = x
                break
    if max_x < 0:
        raise Refused("--auto-rect found no non-black pixel at all.")

    w = max_x - min_x + 1
    h = max_y - min_y + 1
    src_aspect = float(src[0]) / float(src[1])
    box_aspect = float(w) / float(h)
    err = abs(box_aspect - src_aspect) / src_aspect
    print("--auto-rect: non-black box is %dx%d at panel (%d,%d); aspect %.4f "
          "vs source %.4f, %.2f%% off"
          % (w, h, min_x, min_y, box_aspect, src_aspect, err * 100.0))
    print("--auto-rect WARNING: this is a SANITY CHECK, not a measurement. The "
          "non-black box is not the scaled rect - black art at an edge shrinks "
          "it and any non-picture writer widens it. Compute the rect from the "
          "engine arithmetic and use this only to confirm it is not absurd.")
    if err > ASPECT_TOLERANCE:
        raise Refused(
            "--auto-rect box aspect %.4f is %.2f%% away from the source aspect "
            "%.4f, over the %.0f%% tolerance. That box is not a scaled %dx%d "
            "surface." % (box_aspect, err * 100.0, src_aspect,
                          ASPECT_TOLERANCE * 100.0, src[0], src[1]))
    return (min_x, min_y, w, h)


# ------------------------------------------------------------------ self-test

def _synth(panel_h, stride_px, bpp, rect, src, view, art):
    """Apply the forward map: paint art scaled into a fresh black grab."""
    data = bytearray(stride_px * panel_h * (bpp // 8))
    rx, ry, dw, dh = rect
    src_w, src_h = src
    vx, vy = view
    wr = px_writer(data, stride_px, bpp)
    for dy in range(dh):
        sy = (dy * src_h) // dh
        if sy >= src_h:
            sy = src_h - 1
        py = vy + ry + dy
        for dx in range(dw):
            sx = (dx * src_w) // dw
            if sx >= src_w:
                sx = src_w - 1
            r, g, b = art(sx, sy)
            wr(vx + rx + dx, py, r, g, b)
    return data


def _art(sx, sy):
    """Per-pixel-varying art with no black pixel, quantised to RGB565.

    No black pixel means the non-black box equals the scaled rect exactly, so
    case (f) tests auto-rect rather than the art. Per-pixel variation is what
    gives the shift and stride controls something to fail on.
    """
    r = 8 + ((sx * 7 + sy * 13) % 240)
    g = 8 + ((sx * 3 + sy * 5) % 240)
    b = 8 + ((sx + sy * 2) % 240)
    return quantise565(r, g, b)


def _expected(src):
    src_w, src_h = src
    out = bytearray(src_w * src_h * 3)
    j = 0
    for sy in range(src_h):
        for sx in range(src_w):
            r, g, b = _art(sx, sy)
            out[j] = r
            out[j + 1] = g
            out[j + 2] = b
            j += 3
    return out


SRC = (320, 200)
PANEL_H = 480


def case_a_roundtrip():
    """(a) bit-exact round trip at three rects."""
    ok = True
    want = _expected(SRC)
    for rect in ((16, 0, 767, 479), (40, 0, 720, 450), (80, 40, 640, 400)):
        data = _synth(PANEL_H, DEFAULT_STRIDE_PX, 16, rect, SRC, (0, 0), _art)
        got = extract(data, DEFAULT_STRIDE_PX, 16, rect, SRC, (0, 0))
        same = bytes(got) == bytes(want)
        print("  rect %-18s %s" % (str(rect), "exact" if same else "MISMATCH"))
        ok = ok and same
    return ok


def case_b_shift_control():
    """(b) negative control: a rect off by one pixel must not recover."""
    want = _expected(SRC)
    rect = (16, 0, 767, 479)
    data = _synth(PANEL_H, DEFAULT_STRIDE_PX, 16, rect, SRC, (0, 0), _art)
    bad = 0
    for shifted in ((17, 0, 767, 479), (16, 1, 767, 479), (17, 1, 767, 479)):
        got = extract(data, DEFAULT_STRIDE_PX, 16, shifted, SRC, (0, 0))
        differs = bytes(got) != bytes(want)
        print("  shifted %-18s %s" % (str(shifted),
              "differs (good)" if differs else "RECOVERED - control is vacuous"))
        if not differs:
            bad += 1
    return bad == 0


def case_c_downscale_refused():
    """(c) negative control: a downscale must be refused."""
    data = bytearray(DEFAULT_STRIDE_PX * PANEL_H * 2)
    try:
        extract(data, DEFAULT_STRIDE_PX, 16, (0, 0, 160, 100), SRC, (0, 0))
    except Refused as exc:
        print("  refused as expected: %s" % str(exc).split(".")[0])
        return True
    print("  NOT REFUSED - a 320x200 -> 160x100 map was treated as invertible")
    return False


def case_d_stride_control():
    """(d) negative control: a wrong --stride-px must not recover."""
    want = _expected(SRC)
    rect = (40, 0, 720, 450)
    data = _synth(PANEL_H, DEFAULT_STRIDE_PX, 16, rect, SRC, (0, 0), _art)
    ok = True
    for stride in (799, 801):
        try:
            got = extract(data, stride, 16, rect, SRC, (0, 0))
            differs = bytes(got) != bytes(want)
        except Refused:
            differs = True   # refusing is also not recovering
        print("  --stride-px %d %s" % (stride,
              "differs (good)" if differs else "RECOVERED - control is vacuous"))
        ok = ok and differs
    return ok


def case_e_view_roundtrip():
    """(e) bit-exact round trip with a non-zero --view."""
    want = _expected(SRC)
    rect = (40, 0, 720, 450)
    view = (15, 13)
    data = _synth(PANEL_H, DEFAULT_STRIDE_PX, 16, rect, SRC, view, _art)
    got = extract(data, DEFAULT_STRIDE_PX, 16, rect, SRC, view)
    same = bytes(got) == bytes(want)
    print("  view %s rect %s %s" % (str(view), str(rect),
          "exact" if same else "MISMATCH"))
    if not same:
        return False
    # The same grab read with view 0,0 must NOT recover, or --view is a no-op
    # and this case proves nothing.
    other = extract(data, DEFAULT_STRIDE_PX, 16, rect, SRC, (0, 0))
    differs = bytes(other) != bytes(want)
    print("  same grab with view (0,0) %s" % ("differs (good)" if differs
          else "RECOVERED - --view is a no-op, case is vacuous"))
    return differs


def case_f_autorect():
    """(f) auto-rect finds a black-barred rect, and refuses a wrong aspect."""
    rect = (40, 15, 720, 450)
    data = _synth(PANEL_H, DEFAULT_STRIDE_PX, 16, rect, SRC, (0, 0), _art)
    found = auto_rect(data, DEFAULT_STRIDE_PX, 16, SRC)
    ok = found == rect
    print("  found %s, wanted %s: %s" % (str(found), str(rect),
          "match" if ok else "MISMATCH"))

    # A wrong-aspect box: a 720x300 block is 2.40 against 1.60.
    bad = bytearray(DEFAULT_STRIDE_PX * PANEL_H * 2)
    wr = px_writer(bad, DEFAULT_STRIDE_PX, 16)
    for y in range(20, 320):
        for x in range(40, 760):
            wr(x, y, 200, 100, 50)
    try:
        auto_rect(bad, DEFAULT_STRIDE_PX, 16, SRC)
        print("  NOT REFUSED - a 720x300 box was accepted as a 320x200 surface")
        return False
    except Refused:
        print("  wrong aspect refused as expected")
    return ok


def self_test():
    cases = (
        ("a  round trip, three rects", case_a_roundtrip),
        ("b  control: rect off by one", case_b_shift_control),
        ("c  control: downscale refused", case_c_downscale_refused),
        ("d  control: wrong stride", case_d_stride_control),
        ("e  round trip with --view", case_e_view_roundtrip),
        ("f  --auto-rect find and refuse", case_f_autorect),
    )
    failures = 0
    for name, fn in cases:
        print("[%s]" % name)
        try:
            ok = fn()
        except Exception as exc:
            print("  EXCEPTION: %r" % (exc,))
            ok = False
        print("  -> %s" % ("PASS" if ok else "FAIL"))
        if not ok:
            failures += 1
    print("")
    print("self-test: %d of %d cases failed" % (failures, len(cases)))
    return 0 if failures == 0 else 1


# ---------------------------------------------------------------------- main

def _opt(argv, name, default=None):
    if name not in argv:
        return default
    i = argv.index(name)
    if i + 1 >= len(argv):
        print("%s needs a value" % name)
        sys.exit(1)
    return argv[i + 1]


def _ints(text, count, name):
    try:
        vals = [int(v) for v in text.replace("x", ",").split(",")]
    except ValueError:
        print("%s: cannot parse %r" % (name, text))
        sys.exit(1)
    if len(vals) != count:
        print("%s: expected %d numbers, got %d" % (name, count, len(vals)))
        sys.exit(1)
    return vals


def main(argv):
    if "--self-test" in argv:
        return self_test()
    if "--help" in argv or "-h" in argv or len(argv) < 3:
        print(__doc__)
        return 0 if ("--help" in argv or "-h" in argv) else 1

    in_path, out_path = argv[1], argv[2]

    src_text = _opt(argv, "--src")
    if src_text is None:
        print("--src WxH is required (the game surface size, e.g. 320x200)")
        return 1
    src = tuple(_ints(src_text, 2, "--src"))

    bpp = int(_opt(argv, "--bpp", "16"))
    if bpp not in (16, 32):
        print("--bpp must be 16 or 32")
        return 1
    stride_px = int(_opt(argv, "--stride-px", str(DEFAULT_STRIDE_PX)))
    view = tuple(_ints(_opt(argv, "--view", "0,0"), 2, "--view"))

    try:
        with open(in_path, "rb") as f:
            data = f.read()
    except IOError as exc:
        print("cannot read %s: %s" % (in_path, exc))
        return 1

    try:
        if "--auto-rect" in argv:
            if _opt(argv, "--rect") is not None:
                print("--auto-rect and --rect are alternatives, not both")
                return 1
            bx, by, w, h = auto_rect(data, stride_px, bpp, src)
            # auto_rect works in panel coordinates; --rect is logical.
            rect = (bx - view[0], by - view[1], w, h)
            print("--auto-rect: using logical rect %d,%d,%d,%d" % rect)
        else:
            rect_text = _opt(argv, "--rect")
            if rect_text is None:
                print("--rect X,Y,W,H is required (or --auto-rect)")
                return 1
            rect = tuple(_ints(rect_text, 4, "--rect"))
        rgb = extract(data, stride_px, bpp, rect, src, view)
    except Refused as exc:
        print("REFUSED: %s" % exc)
        return 2

    write_ppm(out_path, src[0], src[1], rgb)
    print("Saved %s (P6 %dx%d) from rect %d,%d,%d,%d view %d,%d, %d-bit, "
          "stride %d px" % (out_path, src[0], src[1], rect[0], rect[1],
                            rect[2], rect[3], view[0], view[1], bpp, stride_px))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
