#!/usr/bin/env python3
#
#  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
#
#  SPDX-License-Identifier: GPL-3.0-or-later
#
# Draws the app icon: one pencil stroke, an S, on a paper squircle.
# Writes sc-apps-krita.svgz, the PNG sizes and krita.ico (file names the build expects) into every
# branding folder, so no build shows Krita's logo, and the .kra file icons (a page with the S) into
# ../mimetypes.
# Needs Pillow.

import gzip
import math
import os

from PIL import Image, ImageDraw

SIZE = 1024
HALF = 452            # the squircle's half width
PAPER_TOP = (247, 241, 230)
PAPER_BOTTOM = (232, 220, 199)
INK = (38, 35, 32)
EDGE = (205, 190, 165)  # keeps the paper apart from a light taskbar
HERE = os.path.dirname(os.path.abspath(__file__))
BRANDINGS = ('default', 'Beta', 'Next', 'Plus')


def smooth(x):
    x = min(max(x, 0.0), 1.0)
    return x * x * (3 - 2 * x)


def squircle(n=360):
    """|x|^4 + |y|^4 = 1, like the app's corners."""
    pts = []
    for i in range(n):
        a = 2 * math.pi * i / n
        c, s = math.cos(a), math.sin(a)
        x = math.copysign(abs(c) ** 0.5, c)
        y = math.copysign(abs(s) ** 0.5, s)
        pts.append((SIZE / 2 + HALF * x, SIZE / 2 + HALF * y))
    return pts


def stroke(n=240):
    """The S as a pen stroke: thin where it starts and lifts, full in the middle."""
    # two bowls of radius r stacked on the middle point, drawn top-right to bottom-left
    r, wide, slant = 136, 1.3, 0.14
    first, last = math.radians(-28), math.radians(150)
    turn = (first - math.radians(-270)) + (last - math.radians(-90))
    center, width = [], []
    for i in range(n + 1):
        u = i / n
        a = u * turn
        if a <= first + math.radians(270):
            th, cy = first - a, SIZE / 2 - r
        else:
            th, cy = math.radians(-90) + a - (first + math.radians(270)), SIZE / 2 + r
        x = SIZE / 2 + wide * r * math.cos(th)
        y = cy + r * math.sin(th)
        center.append((x + (SIZE / 2 - y) * slant, y))
        # pressure: a quick press, a long hold and a slower lift
        width.append(16 + 94 * smooth(u / 0.22) * smooth((1 - u) / 0.36))
    left, right = [], []
    for i, (x, y) in enumerate(center):
        a = center[max(i - 1, 0)]
        b = center[min(i + 1, n)]
        dx, dy = b[0] - a[0], b[1] - a[1]
        d = math.hypot(dx, dy) or 1
        nx, ny = -dy / d, dx / d
        w = width[i] / 2
        left.append((x + nx * w, y + ny * w))
        right.append((x - nx * w, y - ny * w))
    # round caps, each from one side around the tip to the other
    def cap(i, outward):
        x, y = center[i]
        a = center[max(i - 1, 0)]
        b = center[min(i + 1, n)]
        ang = math.atan2(b[1] - a[1], b[0] - a[0]) + (0 if outward else math.pi)
        r = width[i] / 2
        return [(x + r * math.cos(ang + math.pi * (0.5 - k / 12)),
                 y + r * math.sin(ang + math.pi * (0.5 - k / 12))) for k in range(1, 12)]
    return left + cap(n, True) + right[::-1] + cap(0, False)


def svg():
    def path(pts):
        return 'M' + ' L'.join('%.1f %.1f' % p for p in pts) + ' Z'
    return f'''<svg xmlns="http://www.w3.org/2000/svg" width="{SIZE}" height="{SIZE}" viewBox="0 0 {SIZE} {SIZE}">
 <defs>
  <linearGradient id="paper" x1="0" y1="0" x2="0" y2="1">
   <stop offset="0" stop-color="rgb{PAPER_TOP}"/>
   <stop offset="1" stop-color="rgb{PAPER_BOTTOM}"/>
  </linearGradient>
 </defs>
 <path d="{path(squircle())}" fill="url(#paper)" stroke="rgb{EDGE}" stroke-width="6"/>
 <path d="{path(stroke())}" fill="rgb{INK}"/>
</svg>
'''.encode()


def render(scale=4):
    big = SIZE * scale
    paper = Image.new('RGB', (1, 256))
    for y in range(256):
        f = y / 255
        paper.putpixel((0, y), tuple(round(a + (b - a) * f) for a, b in zip(PAPER_TOP, PAPER_BOTTOM)))
    paper = paper.resize((big, big), Image.BILINEAR)
    mask = Image.new('L', (big, big), 0)
    ImageDraw.Draw(mask).polygon([(x * scale, y * scale) for x, y in squircle(720)], fill=255)
    img = Image.new('RGBA', (big, big), (0, 0, 0, 0))
    img.paste(paper, (0, 0), mask)
    ImageDraw.Draw(img).polygon([(x * scale, y * scale) for x, y in squircle(720)], outline=EDGE + (255,), width=3 * scale)
    ImageDraw.Draw(img).polygon([(x * scale, y * scale) for x, y in stroke(480)], fill=INK + (255,))
    return img.resize((SIZE, SIZE), Image.LANCZOS)


def document(icon, scale=4):
    """A page with a folded corner and the app icon on it."""
    big = SIZE * scale
    img = Image.new('RGBA', (big, big), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    x0, y0, x1, y1, fold = 168 * scale, 64 * scale, 856 * scale, 960 * scale, 200 * scale
    page = [(x0, y0), (x1 - fold, y0), (x1, y0 + fold), (x1, y1), (x0, y1)]
    d.polygon(page, fill=(252, 250, 246, 255), outline=EDGE + (255,), width=5 * scale)
    d.polygon([(x1 - fold, y0), (x1 - fold, y0 + fold), (x1, y0 + fold)], fill=PAPER_BOTTOM + (255,), outline=EDGE + (255,), width=5 * scale)
    img = img.resize((SIZE, SIZE), Image.LANCZOS)
    mark = icon.resize((500, 500), Image.LANCZOS)
    img.alpha_composite(mark, (262, 330))
    return img


def main():
    icon = render()
    ico = [(s, s) for s in (16, 24, 32, 48, 64, 128, 256)]
    icon.save(os.path.join(HERE, 'krita.ico'), sizes=ico)
    for branding in BRANDINGS:
        folder = os.path.join(HERE, branding)
        with gzip.GzipFile(os.path.join(folder, 'sc-apps-krita.svgz'), 'wb', mtime=0) as f:
            f.write(svg())
        for s in (16, 22, 24, 32, 48, 64, 128, 256, 512, 1024):
            icon.resize((s, s), Image.LANCZOS).save(os.path.join(folder, '%d-apps-krita.png' % s), optimize=True)
        icon.save(os.path.join(folder, 'krita.ico'), sizes=ico)
    page = document(icon)
    for s in (16, 22, 24, 32, 48, 64, 128, 256, 512, 1024):
        page.resize((s, s), Image.LANCZOS).save(os.path.join(HERE, '..', 'mimetypes', '%d-mimetypes-application-x-krita.png' % s), optimize=True)


if __name__ == '__main__':
    main()
