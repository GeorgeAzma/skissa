#!/usr/bin/env python3
#
#  SPDX-License-Identifier: GPL-2.0-or-later
#
"""
The Studio icons: one drawing language for the icons that are in sight all the
time (the tools, the top bar, the panels, a layer's switches).

  - a 24 px grid, the drawing within 2.5..21.5
  - 1.75 px lines with round ends and joins, nothing filled but small marks;
    a switch that is on is solid where it is off drawn in line (locked, a
    visible layer's eye, clipped, alpha locked), so its state reads at a glance
  - a button's icon with an <name>_on drawing is that drawing while the button
    is on (a panel shown): KisIconUtils gives it to the icon as its On state
  - rounded corners are superellipse corners (as everywhere in Studio), not arcs

Each icon is written as dark_studio_<name>.svg (for light themes) and
light_studio_<name>.svg (for dark themes), and listed in svg-icons.qrc.
KisIconUtils::loadIcon() uses them in place of Krita's own icons of the
names in its studioIcons table.

    python3 make_studio_icons.py            # writes the icons next to this file
    python3 make_studio_icons.py sheet.svg  # also a sheet of all of them, to look at
"""
import math
import os
import sys

STROKE = 1.75
COLORS = {'dark': '#303030', 'light': '#dedede'}


def f(v):
    s = ('%.2f' % v).rstrip('0').rstrip('.')
    return '0' if s == '-0' else s


def sq(x, y, w, h, r):
    """A rectangle with superellipse corners (n = 4, as round as a circle of r at the corner's middle)."""
    reach = min(1.84 * r, w / 2, h / 2)
    k = 0.909  # a cubic through the middle of the quarter superellipse
    d = []
    d.append('M%s %s' % (f(x + reach), f(y)))
    d.append('H%s' % f(x + w - reach))
    d.append('C%s %s %s %s %s %s' % (f(x + w - reach + k * reach), f(y), f(x + w), f(y + reach - k * reach), f(x + w), f(y + reach)))
    d.append('V%s' % f(y + h - reach))
    d.append('C%s %s %s %s %s %s' % (f(x + w), f(y + h - reach + k * reach), f(x + w - reach + k * reach), f(y + h), f(x + w - reach), f(y + h)))
    d.append('H%s' % f(x + reach))
    d.append('C%s %s %s %s %s %s' % (f(x + reach - k * reach), f(y + h), f(x), f(y + h - reach + k * reach), f(x), f(y + h - reach)))
    d.append('V%s' % f(y + reach))
    d.append('C%s %s %s %s %s %s' % (f(x), f(y + reach - k * reach), f(x + reach - k * reach), f(y), f(x + reach), f(y)))
    d.append('Z')
    return '<path d="%s"/>' % ''.join(d)


def p(d):
    return '<path d="%s"/>' % d


def c(cx, cy, r):
    return '<circle cx="%s" cy="%s" r="%s"/>' % (f(cx), f(cy), f(r))


def dot(cx, cy, r=1.15):
    """A small filled mark"""
    return '<circle cx="%s" cy="%s" r="%s" fill="INK" stroke="none"/>' % (f(cx), f(cy), f(r))


def box(x, y, w, h):
    """A small filled square (a handle)"""
    return '<rect x="%s" y="%s" width="%s" height="%s" rx="0.6" fill="INK" stroke="none"/>' % (f(x), f(y), f(w), f(h))


def hole_circle(cx, cy, r):
    """A circle as a subpath, to cut out of a solid shape (fill-rule evenodd)"""
    return 'M%s %sa%s %s 0 1 0 %s 0a%s %s 0 1 0 %s 0Z' % (f(cx - r), f(cy), f(r), f(r), f(2 * r), f(r), f(r), f(-2 * r))


def solid(d):
    """A solid shape, its subpaths cut out of it (evenodd), its edge drawn in line as the others' are"""
    return '<path d="%s" fill="INK" fill-rule="evenodd"/>' % d


def outline_of(shape):
    """The path data of a shape made by sq() (or p())"""
    return shape[9:-3]


def dashed(shape, dash='2.4 2.6'):
    return shape.replace('/>', ' stroke-dasharray="%s"/>' % dash, 1)


def star(cx, cy, r):
    """A four-pointed sparkle, its sides drawn in"""
    k = r * 0.28
    return p('M%s %s Q%s %s %s %s Q%s %s %s %s Q%s %s %s %s Q%s %s %s %s Z' % (
        f(cx), f(cy - r), f(cx + k), f(cy - k), f(cx + r), f(cy), f(cx + k), f(cy + k), f(cx), f(cy + r),
        f(cx - k), f(cy + k), f(cx - r), f(cy), f(cx - k), f(cy - k), f(cx), f(cy - r)))


# ---------------------------------------------------------------- the drawings

BRUSH = (p('M12.6 10.4 18.7 4.3a2 2 0 0 1 2.9 2.9l-6.1 6.1') + p('M12.6 10.4l3 3')
         + p('M11.3 11.7c-2.4 0-4.2 1.6-4.5 4-.2 1.7-1.1 2.9-2.9 3.5 2.3 1.3 5.1 1.4 7.1.3 2-1.1 2.9-3.3 2.4-5.4'))



ERASER = (p('M8.6 20.5 3.9 15.8a2 2 0 0 1 0-2.8l9.3-9.3a2 2 0 0 1 2.8 0l4.4 4.4a2 2 0 0 1 0 2.8L11.4 20.5Z')
          + p('M8.6 8.5l7.4 7.4') + p('M8.6 20.5H20'))
SELECT_RECT = (p('M3.5 8V6.9a3.4 3.4 0 0 1 3.4-3.4H8') + p('M16 3.5h1.1a3.4 3.4 0 0 1 3.4 3.4V8')
               + p('M20.5 16v1.1a3.4 3.4 0 0 1-3.4 3.4H16') + p('M8 20.5H6.9a3.4 3.4 0 0 1-3.4-3.4V16')
               + p('M10.8 3.5h2.4') + p('M20.5 10.8v2.4') + p('M10.8 20.5h2.4') + p('M3.5 10.8v2.4'))
LOCK_BODY = sq(5, 10.5, 14, 10, 2.6) + dot(12, 15.5, 1.3)
# locked: the body solid, its keyhole cut out of it
LOCK_BODY_SOLID = (sq(5, 10.5, 14, 10, 2.6)
                   + '<path d="%s M12 13.9a1.6 1.6 0 1 0 0 3.2a1.6 1.6 0 1 0 0-3.2Z" fill="INK" fill-rule="evenodd" stroke="none"/>'
                   % sq(5, 10.5, 14, 10, 2.6)[9:-3])
# shown: solid, its pupil cut out of it (hidden, it is drawn in line and crossed out); a ring for the iris blurs at row size
EYE = solid('M2.8 12C4.6 8.2 8 5.8 12 5.8s7.4 2.4 9.2 6.2c-1.8 3.8-5.2 6.2-9.2 6.2S4.6 15.8 2.8 12Z' + hole_circle(12, 12, 3.3))
CHECKER = (sq(3, 3, 11, 11, 2.6) + '<path d="M3.6 3.6h4.9v4.9H3.6zM8.5 8.5h4.9v4.9H8.5z" fill="INK" stroke="none" opacity="0.55"/>')


def checks(x, y, w, h, cols, rows, inset):
    """A checkerboard's dark squares inside a box (path data), the top left one dark"""
    cw, ch = (w - 2 * inset) / cols, (h - 2 * inset) / rows
    d = []
    for j in range(rows):
        for i in range(cols):
            if (i + j) % 2 == 0:
                d.append('M%s %sh%sv%sh%sz' % (f(x + inset + i * cw), f(y + inset + j * ch), f(cw), f(ch), f(-cw)))
    return ''.join(d)


# alpha lock: a checkerboard (the transparency it keeps) in a squircle; locked, the checkerboard is a padlock's body, solid
ALPHA_FREE = (sq(3.5, 3.5, 17, 17, 4)
              + '<path d="%s" fill="INK" stroke="none" opacity="0.55"/>' % checks(3.5, 3.5, 17, 17, 3, 3, 1.7))
ALPHA_LOCKED = (sq(4, 10.5, 16, 10.5, 3)
                + '<path d="%s" fill="INK" stroke="none"/>' % checks(4, 10.5, 16, 10.5, 3, 2, 1.6)
                + p('M8 10.5V7.5a4 4 0 0 1 8 0v3'))


def mirror(vertical, on=False):
    left = '<path d="M9 6.5 3.5 17H9Z" fill="INK" stroke-linejoin="round"/>' if on else p('M9 6.5 3.5 17H9Z')
    g = p('M12 2.5v2.4M12 8v2.4M12 13.6V16M12 19.1v2.4') + left + '<path d="M15 6.5 20.5 17H15Z" fill="INK" stroke-linejoin="round"/>'
    # across the other axis: the same drawing, its x and y swapped (turned, it would point like a play button)
    return '<g transform="matrix(0 1 1 0 0 0)">%s</g>' % g if vertical else g


ICONS = {
    # ---- tools: what is on the rail and on the top bar's tool chip
    'paint': BRUSH,
    'blend': ('<g transform="rotate(-38 12 12)">%s</g>' % (
                  p('M9.5 13V4.2a1.9 1.9 0 0 1 3.8 0V11')
                  + p('M13.3 10.6a1.75 1.75 0 0 1 3.5 0v1a1.75 1.75 0 0 1 3.5 0V16a5.5 5.5 0 0 1-5.5 5.5h-2a5.5 5.5 0 0 1-4.4-2.2l-2.6-3.5a1.6 1.6 0 0 1 2.4-2.1L9.5 15')))
             + p('M2.5 6.5c1.5-.8 3-.8 4.5 0'),
    'erase': ERASER,
    'select': SELECT_RECT,
    'move': (p('M12 3v18M3 12h18') + p('M9 5.8 12 3l3 2.8M9 18.2l3 2.8 3-2.8M5.8 9 3 12l2.8 3M18.2 9 21 12l-2.8 3')),
    'fill': (p('M18.5 10.8 10.7 3 3.3 10.4a2 2 0 0 0 0 2.8l5 5a2 2 0 0 0 2.8 0Z') + p('M3 11.6h15.2')
             + p('M21.5 19.2a1.9 1.9 0 1 1-3.8 0c0-1.5 1.6-2.4 1.9-3.9.3 1.5 1.9 2.4 1.9 3.9Z') + p('M5.8 3.2 9 6.4')),
    'shape': sq(3.5, 10, 10.5, 10.5, 2.6) + c(15.5, 8.5, 5),
    'text': p('M5 6.5V4h14v2.5') + p('M12 4v16') + p('M9 20h6'),
    'guides': p('M4 20V4l16 16Z') + p('M8 16v-4.4l4.4 4.4Z') + p('M4 8h2M4 12h1.5M4 16h2'),
    'effects': star(10, 11, 7.5) + star(18.3, 5.5, 3.2) + star(18, 17.5, 2.6),

    # ---- tools behind the rail's slots (their own Krita icons)
    'line': p('M5 19 19 5') + dot(5, 19, 1.9) + dot(19, 5, 1.9),
    'rectangle': sq(3.5, 5.5, 17, 13, 1.6),
    'ellipse': '<ellipse cx="12" cy="12" rx="8.5" ry="6.5"/>',
    'polygon': p('M12 3.5 20.5 9.7 17.3 19.7H6.7L3.5 9.7Z'),
    'polyline': p('M3.5 17.5 9 7.5l5.5 8L20.5 5') + dot(3.5, 17.5, 1.6) + dot(9, 7.5, 1.6) + dot(14.5, 15.5, 1.6) + dot(20.5, 5, 1.6),
    'path': p('M4 19C6 9 14 17 20 5') + p('M4 19 8.5 12.5M20 5l-4.7 4') + box(2.6, 17.6, 2.8, 2.8) + box(18.6, 3.6, 2.8, 2.8) + dot(8.5, 12.5, 1.4) + dot(15.3, 9, 1.4),
    'freehand_path': p('M3.5 16c2-6 5-8 7-5s1.5 7 4 6.5 3-6 6-9') + box(2.2, 14.6, 2.8, 2.8) + box(19.1, 7.1, 2.8, 2.8),
    'dyna': BRUSH + p('M3 4.5h5M3 8h3.5M3 11.5h2'),
    # a shape and its reflection: symmetry
    'multihand': p('M12 3.5v17') + p('M9 5.5a6.5 6.5 0 0 0 0 13Z') + '<path d="M15 5.5a6.5 6.5 0 0 1 0 13Z" fill="INK"/>',
    'gradient': (sq(3.5, 3.5, 17, 17, 3.4) + '<path d="M10.5 3.5H6.9a3.4 3.4 0 0 0-3.4 3.4v10.2a3.4 3.4 0 0 0 3.4 3.4h3.6Z" fill="INK" stroke="none"/>'
                 + dot(13.2, 6.5, 1.15) + dot(13.2, 10.2, 1.15) + dot(13.2, 13.8, 1.15) + dot(13.2, 17.5, 1.15)
                 + dot(16.6, 8.3, 0.75) + dot(16.6, 12, 0.75) + dot(16.6, 15.7, 0.75)),
    'enclose': dashed(sq(3.5, 3.5, 17, 17, 3.4)) + '<path d="%s" fill="INK" stroke="none"/>' % sq(8, 8, 8, 8, 1.8)[9:-3],
    'sampler': (p('M14.6 5.4 17.2 2.8a2.1 2.1 0 0 1 3 3l-2.6 2.6') + p('M12.3 6.7l5 5') + p('M15 9.2 6.8 17.4 3.5 18.5l1.1-3.3 8.2-8.2')),
    'patch': ('<g transform="rotate(-45 12 12)">%s</g>' % (sq(2.5, 8.5, 19, 7, 3.2) + p('M9.5 8.5v7M14.5 8.5v7'))) + dot(12, 12, 0.9),
    'transform': (sq(5.5, 5.5, 13, 13, 1.2) + box(3.6, 3.6, 3.8, 3.8) + box(16.6, 3.6, 3.8, 3.8) + box(3.6, 16.6, 3.8, 3.8) + box(16.6, 16.6, 3.8, 3.8)),
    'crop': p('M6.5 2.5v13a2 2 0 0 0 2 2h13') + p('M17.5 21.5v-13a2 2 0 0 0-2-2h-13'),
    'zoom': c(10.5, 10.5, 7) + p('m20.5 20.5-5-5') + p('M10.5 7.5v6M7.5 10.5h6'),
    'measure': ('<g transform="rotate(-45 12 12)">%s</g>' % (sq(1.5, 8.5, 21, 7, 1.6) + p('M6 8.5v3M10 8.5v2M14 8.5v3M18 8.5v2'))),
    'reference': sq(3.5, 5, 17, 14.5, 2.6) + p('M3.8 16.5 9 11.5l3.5 3.5 2.5-2.5 5.2 5') + dot(15.5, 9.5, 1.5),
    'pointer': p('M5 3.5 11 20.5l2.4-7 7.1-2.4Z'),
    'calligraphy': (p('M12.5 3.5 20.5 11.5 15 17l-5.5 1.5-6 3 3-6L8 10Z') + p('M3.5 20.5l5-5') + c(10, 14, 1.5)),
    'select_ellipse': dashed('<ellipse cx="12" cy="12" rx="8.5" ry="7"/>', '2.6 2.4'),
    'select_polygon': dashed(p('M12 3.5 20.5 9.7 17.3 19.7H6.7L3.5 9.7Z'), '2.6 2.4'),
    'lasso': (dashed(p('M7.2 17C4.6 15.9 3 14 3 11.8 3 7.8 7 4.5 12 4.5s9 3.3 9 7.3-4 7.2-9 7.2'), '2.6 2.4')
              + c(6.8, 17.8, 1.8) + p('M6.5 19.6c-.4 1-1.2 1.6-2.3 1.9')),
    'wand': p('M3.5 20.5 14 10') + p('M12.4 8.4l3.2 3.2') + star(17.5, 6.5, 3.8) + star(8.5, 5, 2) + star(19.5, 15, 2),

    # ---- the top bar's switches
    'pressure': (p('M15.5 2.8 21.2 8.5 14.6 15.1 10.4 15.8 9.7 13.6 8.9 11.4Z') + p('M13.7 4.6l5.7 5.7')
                 + '<path d="M2.6 21.4C5 21 7.3 19.8 9.2 17.7l1.3 1.3C8.2 21 5.4 21.9 2.6 21.6Z" fill="INK" stroke="INK" stroke-width="0.8"/>'),
    'pressure_off': (p('M15.5 2.8 21.2 8.5 14.6 15.1 10.4 15.8 9.7 13.6 8.9 11.4Z') + p('M13.7 4.6l5.7 5.7')
                     + p('M2.6 21h7.4') + p('M3 3l18 18')),
    'brush_settings': p('M3.5 7h10M17.5 7h3M3.5 12h3M10.5 12h10M3.5 17h12M19.5 17h1') + c(15.5, 7, 2) + c(8.5, 12, 2) + c(17.5, 17, 2),
    'alpha_lock': ALPHA_LOCKED,
    'alpha_unlock': ALPHA_FREE,
    'reload': p('M3.5 12a8.5 8.5 0 1 0 2.9-6.4L3.5 8.3') + p('M3.5 3.5v4.8h4.8'),
    'mirror_x': mirror(False),
    'mirror_y': mirror(True),
    'mirror_x_on': mirror(False, True),
    'mirror_y_on': mirror(True, True),
    'wrap': p('M16.5 3.5 20 7l-3.5 3.5') + p('M4 11V9.5a2.5 2.5 0 0 1 2.5-2.5H20') + p('M7.5 20.5 4 17l3.5-3.5') + p('M20 13v1.5a2.5 2.5 0 0 1-2.5 2.5H4'),

    # ---- a layer's switches
    'visible': EYE,
    'hidden': p('M9.9 6c.7-.1 1.4-.2 2.1-.2 4 0 7.4 2.4 9.2 6.2-.6 1.3-1.4 2.4-2.3 3.3M14.1 14.1a3 3 0 0 1-4.2-4.2M17 17c-1.4.8-3.1 1.2-5 1.2-4 0-7.4-2.4-9.2-6.2.9-1.9 2.2-3.5 3.9-4.6') + p('M3.5 3.5l17 17'),
    'locked': LOCK_BODY_SOLID + p('M8 10.5V7.5a4 4 0 0 1 8 0v3'),
    'unlocked': LOCK_BODY + p('M8 10.5V7.5a4 4 0 0 1 7.8-1.2'),
    'clip': p('M6.5 3.5v8.5a4 4 0 0 0 4 4h9') + p('M16.5 13l3 3-3 3'),
    # on: its arrowhead solid
    'clip_on': p('M6.5 3.5v8.5a4 4 0 0 0 4 4h8') + '<path d="M16 12.2 20.5 16 16 19.8Z" fill="INK"/>',

    # ---- panels
    'layers': p('M12 3.5 20.5 8 12 12.5 3.5 8Z') + p('M3.5 12 12 16.5 20.5 12') + p('M3.5 16 12 20.5 20.5 16'),
    'color': (p('M12 3a9 9 0 1 0 0 18c1.1 0 1.8-.8 1.8-1.7 0-.5-.2-.9-.5-1.2-.3-.3-.5-.8-.5-1.2 0-1 .8-1.8 1.8-1.8H17a4 4 0 0 0 4-4C21 6.6 17 3 12 3Z')
              + dot(7.5, 11, 1.3) + dot(9.5, 7, 1.3) + dot(14.5, 7, 1.3) + dot(17, 10.8, 1.3)),
    'brushes': sq(3.5, 3.5, 7.2, 7.2, 2) + sq(13.3, 3.5, 7.2, 7.2, 2) + sq(3.5, 13.3, 7.2, 7.2, 2) + sq(13.3, 13.3, 7.2, 7.2, 2),
    'palette': (sq(10.5, 3, 9, 18, 2.6) + p('M10.5 9h9M10.5 14.5h9') + dot(15, 17.8, 1.2)
                + p('M10.5 18.2 4.1 8.9a1.9 1.9 0 0 1 .5-2.6l5.9-4')),
    'history': p('M3.5 12a8.5 8.5 0 1 0 2.9-6.4L3.5 8.3') + p('M3.5 3.5v4.8h4.8') + p('M12 7.5V12l3 2'),
    'timelapse': c(12, 12, 8.5) + p('M10.2 8.6v6.8l5.6-3.4Z'),
    'shapes': c(8.2, 8.2, 4.7) + sq(12.3, 12.3, 8.2, 8.2, 2),
    # the view turned around its axis
    'mirror_view': solid('M10 6.5 3.5 12l6.5 5.5Z') + solid('M14 6.5l6.5 5.5-6.5 5.5Z'),
    'tool_options': p('M7 3.5v3M7 10.5v10M12 3.5v10M12 17.5v3M17 3.5v6M17 13.5v7') + c(7, 8.5, 2) + c(12, 15.5, 2) + c(17, 11.5, 2),

    # ---- the panels, shown (see KisIconUtils: a button's On state)
    'layers_on': (solid('M12 3.5 20.5 8 12 12.5 3.5 8Z') + p('M3.5 12 12 16.5 20.5 12') + p('M3.5 16 12 20.5 20.5 16')),
    'color_on': solid('M12 3a9 9 0 1 0 0 18c1.1 0 1.8-.8 1.8-1.7 0-.5-.2-.9-.5-1.2-.3-.3-.5-.8-.5-1.2 0-1 .8-1.8 1.8-1.8H17a4 4 0 0 0 4-4C21 6.6 17 3 12 3Z'
                      + hole_circle(7.5, 11, 1.7) + hole_circle(9.5, 7, 1.7) + hole_circle(14.5, 7, 1.7) + hole_circle(17, 10.8, 1.7)),
    'brushes_on': ''.join(solid(outline_of(sq(x, y, 7.2, 7.2, 2))) for x, y in ((3.5, 3.5), (13.3, 3.5), (3.5, 13.3), (13.3, 13.3))),
    'palette_on': (solid(outline_of(sq(10.5, 3, 9, 18, 2.6)) + 'M10.5 8.4h9v1.2h-9ZM10.5 13.9h9v1.2h-9Z' + hole_circle(15, 17.8, 1.3))
                   + p('M10.5 18.2 4.1 8.9a1.9 1.9 0 0 1 .5-2.6l5.9-4')),
    'history_on': (p('M3.5 12a8.5 8.5 0 1 0 2.9-6.4L3.5 8.3') + p('M3.5 3.5v4.8h4.8')
                   + '<path d="%sM11.3 6.6h1.4v4.7l2.7 1.8-.8 1.2-3.3-2.2Z" fill="INK" fill-rule="evenodd" stroke="none"/>' % hole_circle(12, 12, 5.9).replace('M6.1 12', 'M6.1 12', 1)),
    'timelapse_on': solid('M3.5 12a8.5 8.5 0 1 0 17 0a8.5 8.5 0 1 0-17 0ZM9.4 7.4v9.2l7.6-4.6Z'),
    'shapes_on': solid('M3.5 8.2a4.7 4.7 0 1 0 9.4 0a4.7 4.7 0 1 0-9.4 0Z') + solid(outline_of(sq(12.3, 12.3, 8.2, 8.2, 2))),
    'tool_options_on': p('M7 3.5v3M7 10.5v10M12 3.5v10M12 17.5v3M17 3.5v6M17 13.5v7') + dot(7, 8.5, 2.6) + dot(12, 15.5, 2.6) + dot(17, 11.5, 2.6),
    'find': c(10.5, 10.5, 7) + p('m20.5 20.5-5-5'),
    'more': dot(5.5, 12, 1.6) + dot(12, 12, 1.6) + dot(18.5, 12, 1.6),

    # ---- common commands
    'add': p('M12 4.5v15M4.5 12h15'),
    'delete': (p('M3.5 6.5h17') + p('M9 6.5V5a1.5 1.5 0 0 1 1.5-1.5h3A1.5 1.5 0 0 1 15 5v1.5')
               + p('M5.8 6.5l.9 12.2a2 2 0 0 0 2 1.8h6.6a2 2 0 0 0 2-1.8l.9-12.2') + p('M10 10.5v6M14 10.5v6')),
    'refresh': p('M20 12a8 8 0 0 1-14 5.3M4 12A8 8 0 0 1 18 6.7') + p('M18.2 3v3.9h-3.9M5.8 21v-3.9h3.9'),
    'undo': p('M9 14 4 9l5-5') + p('M4 9h10.5a5.5 5.5 0 0 1 0 11H11'),
    'filter': p('M3.5 4.5h17l-6.5 7.7v6.3l-4 2v-8.3Z'),
}


def svg(body, ink):
    body = body.replace('INK', ink)
    return ('<svg width="24" height="24" viewBox="0 0 24 24" xmlns="http://www.w3.org/2000/svg">'
            '<g fill="none" stroke="%s" stroke-width="%s" stroke-linecap="round" stroke-linejoin="round">%s</g></svg>\n'
            % (ink, STROKE, body))


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    names = []
    for name, body in ICONS.items():
        for variant, ink in COLORS.items():
            filename = '%s_studio_%s.svg' % (variant, name)
            with open(os.path.join(here, filename), 'w', encoding='utf-8', newline='\n') as out:
                out.write(svg(body, ink))
            names.append(filename)

    qrc = os.path.join(here, 'svg-icons.qrc')
    text = open(qrc, encoding='utf-8', newline='').read()
    nl = '\r\n' if '\r\n' in text else '\n'
    missing = [n for n in names if '<file>%s</file>' % n not in text]
    if missing:
        anchor = '        <file>light_hamburger_menu.svg</file>' + nl
        assert anchor in text
        text = text.replace(anchor, anchor + ''.join('        <file>%s</file>%s' % (n, nl) for n in missing))
        open(qrc, 'w', encoding='utf-8', newline='').write(text)
    print(len(ICONS), 'icons,', len(missing), 'files added to the qrc')

    if len(sys.argv) > 1:
        # a sheet: every icon at 24 px and at 4x, on dark and on light, named
        cols = 8
        cell = 120
        rows = math.ceil(len(ICONS) / cols)
        parts = []
        for i, (name, body) in enumerate(ICONS.items()):
            x = (i % cols) * cell
            y = (i // cols) * cell
            for variant, ink, bg, ox in (('light', COLORS['light'], '#2c2c2c', 0), ('dark', COLORS['dark'], '#f2f2f2', 60)):
                parts.append('<rect x="%d" y="%d" width="60" height="%d" fill="%s"/>' % (x + ox, y, cell, bg))
                parts.append('<g transform="translate(%d %d) scale(2)"><g fill="none" stroke="%s" stroke-width="%s" stroke-linecap="round" stroke-linejoin="round">%s</g></g>'
                             % (x + ox + 6, y + 6, ink, STROKE, body.replace('INK', ink)))
                parts.append('<g transform="translate(%d %d)"><g fill="none" stroke="%s" stroke-width="%s" stroke-linecap="round" stroke-linejoin="round">%s</g></g>'
                             % (x + ox + 18, y + 64, ink, STROKE, body.replace('INK', ink)))
            parts.append('<text x="%d" y="%d" font-family="Segoe UI" font-size="10" fill="#888">%s</text>' % (x + 4, y + cell - 6, name))
        with open(sys.argv[1], 'w', encoding='utf-8') as out:
            out.write('<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d">%s</svg>' % (cols * cell, rows * cell, ''.join(parts)))


if __name__ == '__main__':
    main()
