#!/usr/bin/env python3
"""
Draws the accessory icons used on the settings screen (after the Voltra's own
accessory menu) and emits them as a 4 bpp LVGL font, so a label can show them
and recolour them like text.

  U+E000  eccentric       three nested chevrons pointing down
  U+E001  chains          chain link seen side on: two open rings joined by a bar
  U+E002  inverse chains  the chain link knocked out of a filled disc
  U+E003  mountain        a peak with a smaller one in front of its left flank
  U+E004  settings        five dots in a plus (the Voltra's settings button)

Shapes are drawn in their own design units at high resolution, then box-filtered
down to the target size. Needs Pillow.

Usage (the firmware uses 26 px on the settings screen and 18 px on the main screen):
  python gen_icons.py --size 26 --name font_icons_26 --out ../src/ui/fonts/font_icons_26.c
  python gen_icons.py --size 18 --name font_icons_18 --out ../src/ui/fonts/font_icons_18.c
"""
import argparse
import math

from PIL import Image, ImageDraw

from gen_font import pack_4bpp, write_font

CODEPOINT_BASE = 0xE000


def stroke(draw, points, width, scale, fill=255):
    """Fill the outline of a polyline stroke with mitred joins and butt ends."""
    h = width / 2
    dirs = []
    for (x0, y0), (x1, y1) in zip(points, points[1:]):
        length = math.hypot(x1 - x0, y1 - y0)
        dirs.append(((x1 - x0) / length, (y1 - y0) / length))
    normals = [(-dy, dx) for dx, dy in dirs]

    left, right = [], []
    for i, (px, py) in enumerate(points):
        if i == 0:
            nx, ny = normals[0]
            k = h
        elif i == len(points) - 1:
            nx, ny = normals[-1]
            k = h
        else:
            ax, ay = normals[i - 1]
            bx, by = normals[i]
            mx, my = ax + bx, ay + by
            m = math.hypot(mx, my)
            nx, ny = mx / m, my / m
            k = h / (nx * bx + ny * by)
        left.append(((px + nx * k) * scale, (py + ny * k) * scale))
        right.append(((px - nx * k) * scale, (py - ny * k) * scale))
    draw.polygon(left + right[::-1], fill=fill)


def ring_arc(draw, cx, cy, rx, ry, width, start, end, scale, fill=255):
    """Elliptical arc stroked about its centre line; angles clockwise from 3 o'clock."""
    h = width / 2
    box = [(cx - rx - h) * scale, (cy - ry - h) * scale, (cx + rx + h) * scale, (cy + ry + h) * scale]
    draw.arc(box, start, end, fill=fill, width=round(width * scale))


def chain(draw, cx, cy, w, h, t, bar_w, scale, fill=255):
    """
    A chain link seen side on, centred on (cx, cy) with an outer size of w x h:
    two stacked rings open where they face each other, and the link between them
    seen edge on as a vertical bar through the openings.
    """
    rx = (w - t) / 2
    ry = (h / 2 - t) / 2
    top_cy = cy - h / 2 + t / 2 + ry
    bot_cy = cy + h / 2 - t / 2 - ry
    gap = 42  # half-angle of each ring's opening, degrees
    ring_arc(draw, cx, top_cy, rx, ry, t, 90 + gap, 90 - gap, scale, fill)
    ring_arc(draw, cx, bot_cy, rx, ry, t, 270 + gap, 270 - gap, scale, fill)
    # the bar spans the hole-to-hole distance, so it reads as passing through both rings
    y0 = top_cy + ry - t / 2
    y1 = bot_cy - ry + t / 2
    draw.rectangle([(cx - bar_w / 2) * scale, y0 * scale, (cx + bar_w / 2) * scale, y1 * scale], fill=fill)


def draw_eccentric(draw, s):
    t = 34
    for apex_y, half_w in ((95, 47), (185, 84), (275, 121)):
        stroke(draw, [(-half_w, apex_y - half_w), (0, apex_y), (half_w, apex_y - half_w)], t, s)


def draw_chains(draw, s):
    chain(draw, 0, 0, 200, 290, 45, 44, s)


def draw_inverse(draw, s):
    r = 180
    draw.ellipse([-r * s, -r * s, r * s, r * s], fill=255)
    chain(draw, 0, 0, 150, 230, 28, 20, s, fill=0)


def draw_mountain(draw, s):
    t = 36
    base = 300
    # smaller peak in front, its left flank in line with the main peak's
    stroke(draw, [(88, 312), (150, 250), (212, 312)], t, s)
    # main peak, 45 degree flanks; its left flank stops just short of the small peak
    # (one stroke width along the line) so the two read as separate
    back = t / math.sqrt(2)
    stroke(draw, [(150 + back, 250 - back), (250, 150), (412, 312)], t, s)
    # feet end on a common flat baseline
    draw.rectangle([-1000 * s, base * s, 1000 * s, 1000 * s], fill=0)


def draw_settings(draw, s):
    r, gap = 9, 30   # dot radius and centre spacing, from the Voltra's own button
    for x, y in ((0, 0), (-gap, 0), (gap, 0), (0, -gap), (0, gap)):
        draw.ellipse([(x - r) * s, (y - r) * s, (x + r) * s, (y + r) * s], fill=255)


# (label, draw function, max width px, max height px) at the 26 px design size; other
# sizes scale these
ICONS = [
    ("eccentric", draw_eccentric, 22, 21),
    ("chains", draw_chains, 16, 22),
    ("inverse chains", draw_inverse, 24, 24),
    ("mountain", draw_mountain, 27, 16),
    ("settings", draw_settings, 24, 24),
]


def render(fn, max_w, max_h):
    """Draw at high resolution, crop to the ink, and box-filter down to fit max_w x max_h."""
    k = 2  # high resolution pixels per design unit
    size = 1200 * k
    img = Image.new("L", (size, size), 0)
    draw = ImageDraw.Draw(img)

    class Shifted:
        # design coordinates are centred on the origin; shift them into the canvas
        def __getattr__(self, name):
            op = getattr(draw, name)

            def call(xy, *args, **kw):
                off = size / 2
                if isinstance(xy[0], tuple):
                    xy = [(x + off, y + off) for x, y in xy]
                else:
                    xy = [xy[0] + off, xy[1] + off, xy[2] + off, xy[3] + off]
                return op(xy, *args, **kw)
            return call

    fn(Shifted(), k)
    ink = img.crop(img.getbbox())
    scale = min(max_w / ink.width, max_h / ink.height)
    w, h = max(1, round(ink.width * scale)), max(1, round(ink.height * scale))
    return ink.resize((w, h), Image.BOX)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--size", type=int, default=26, help="line height in px")
    ap.add_argument("--name", default="font_icons_26")
    ap.add_argument("--out", required=True)
    ap.add_argument("--preview", help="also save the glyphs, enlarged, to this PNG")
    a = ap.parse_args()

    glyphs, images = [], []
    k = a.size / 26
    for i, (label, fn, max_w, max_h) in enumerate(ICONS):
        img = render(fn, min(round(max_w * k), a.size), min(round(max_h * k), a.size))
        images.append(img)
        bw, bh = img.size
        top = (a.size - bh) // 2
        ofs_y = a.size - (top + bh)   # from the baseline (bottom of the line) up to the box
        glyphs.append((CODEPOINT_BASE + i, label, pack_4bpp(img), bw, bh, 1, ofs_y, bw + 2))

    write_font(a.out, a.name, a.size, "drawn shapes (" + ", ".join(i[0] for i in ICONS) + ")",
               glyphs, a.size, 0)

    if a.preview:
        zoom = 8
        cell = (a.size + 4) * zoom
        sheet = Image.new("L", (cell * len(images), cell), 24)
        for i, img in enumerate(images):
            big = img.resize((img.width * zoom, img.height * zoom), Image.NEAREST)
            sheet.paste(big, (i * cell + (cell - big.width) // 2, (cell - big.height) // 2))
        sheet.save(a.preview)


if __name__ == "__main__":
    main()
