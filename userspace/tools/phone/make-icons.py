#!/usr/bin/python3
"""make-icons.py: the drawer icons of the phone apps, in the drawer's style
(a coloured tile with a dark rim and a white stroke glyph, 116 px), drawn
with titan_water4's SVG path reader. Run on the phone:
python3 make-icons.py /usr/local/share/titan/icons"""
import math
import os
import sys

import cairo
import titan_water4 as water

SIZE = 116
PHONE = ("M6.5 3.5h3l1.5 4.5-2 1.5a11 11 0 0 0 5.5 5.5l1.5-2 4.5 1.5v3a2 2 0 0 1-2 2"
         "A16 16 0 0 1 4.5 5.5a2 2 0 0 1 2-2z")
SIM = "M7 3.5h7l4 4v13H7zM10 12h5v5h-5z"
ICONS = {"phone": ((0.20, 0.62, 0.36), PHONE), "phone-settings": ((0.33, 0.40, 0.50), SIM)}


def tile(cr, colour):
    x, y, w, h, r = 6, 14, SIZE - 12, SIZE - 28, 14
    cr.new_sub_path()
    cr.arc(x + w - r, y + r, r, -math.pi / 2, 0)
    cr.arc(x + w - r, y + h - r, r, 0, math.pi / 2)
    cr.arc(x + r, y + h - r, r, math.pi / 2, math.pi)
    cr.arc(x + r, y + r, r, math.pi, 3 * math.pi / 2)
    cr.close_path()
    cr.set_source_rgb(*colour)
    cr.fill_preserve()
    cr.set_source_rgb(0.13, 0.15, 0.18)
    cr.set_line_width(5)
    cr.stroke()


def glyph(cr, d):
    cr.save()
    scale = 62 / 24
    cr.translate((SIZE - 24 * scale) / 2, (SIZE - 24 * scale) / 2)
    cr.scale(scale, scale)
    water._svg_path(cr, d)
    cr.set_source_rgb(1, 1, 1)
    cr.set_line_width(2.3)
    cr.set_line_cap(cairo.LINE_CAP_ROUND)
    cr.set_line_join(cairo.LINE_JOIN_ROUND)
    cr.stroke()
    cr.restore()


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "."
    os.makedirs(out, exist_ok=True)
    for name, (colour, d) in ICONS.items():
        surf = cairo.ImageSurface(cairo.FORMAT_ARGB32, SIZE, SIZE)
        cr = cairo.Context(surf)
        tile(cr, colour)
        glyph(cr, d)
        surf.write_to_png(os.path.join(out, name + ".png"))
        print(os.path.join(out, name + ".png"))


if __name__ == "__main__":
    main()
