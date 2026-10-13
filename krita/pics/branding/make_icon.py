#!/usr/bin/env python3
#
#  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
#
#  SPDX-License-Identifier: GPL-3.0-or-later
#
# Draws the app icon: one pencil stroke, an S, with nothing behind it.
#   light_krita-branding.svgz  white S with a soft shadow, for dark themes (in the app, and the splash)
#   dark_krita-branding.svgz   ink S, for light themes (Krita names icons by their own color)
#   sc-apps-krita.svgz, *-apps-krita.png, krita.ico
#                              the white S again, for places that can't follow the theme (the .exe in
#                              the file browser, Start, shortcuts); small sizes get a stronger shadow
#                              so it still reads on light backgrounds
# into every branding folder, so no build shows Krita's logo, and the .kra file icons (a page with
# the S) into ../mimetypes. Needs Pillow.

import base64
import gzip
import io
import math
import os

from PIL import Image, ImageChops, ImageDraw, ImageFilter

SIZE = 1024
GROW = 1.22             # the S fills the icon; there's no tile around it
INK = (38, 35, 32)
LIGHT = (250, 248, 244)
PAGE = (252, 250, 246)
PAGE_EDGE = (205, 190, 165)
FOLD = (232, 220, 199)
HERE = os.path.dirname(os.path.abspath(__file__))
BRANDINGS = ('default', 'Beta', 'Next', 'Plus')
PNG_SIZES = (16, 22, 24, 32, 48, 64, 128, 256, 512, 1024)
ICO_SIZES = (16, 24, 32, 48, 64, 128, 256)


def smooth(x):
    x = min(max(x, 0.0), 1.0)
    return x * x * (3 - 2 * x)


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
        center.append((SIZE / 2 + (x + (SIZE / 2 - y) * slant - SIZE / 2) * GROW, SIZE / 2 + (y - SIZE / 2) * GROW))
        # pressure: a quick press, a long hold and a slower lift
        width.append((16 + 94 * smooth(u / 0.22) * smooth((1 - u) / 0.36)) * GROW)
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


def svg(fill, shade=False):
    d = 'M' + ' L'.join('%.1f %.1f' % p for p in stroke()) + ' Z'
    under = ''
    if shade:
        # Qt's SVG has no blur, so the shadow is a picture; it's soft anyway, so a small one will do
        buf = io.BytesIO()
        shadow(SIZE).resize((256, 256), Image.LANCZOS).save(buf, 'PNG', optimize=True)
        under = (f' <image width="{SIZE}" height="{SIZE}" '
                 f'xlink:href="data:image/png;base64,{base64.b64encode(buf.getvalue()).decode()}"/>\n')
    return (f'<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" '
            f'width="{SIZE}" height="{SIZE}" viewBox="0 0 {SIZE} {SIZE}">\n'
            f'{under} <path d="{d}" fill="rgb{fill}"/>\n</svg>\n').encode()


def render(fill, scale=4):
    big = SIZE * scale
    img = Image.new('RGBA', (big, big), (0, 0, 0, 0))
    ImageDraw.Draw(img).polygon([(x * scale, y * scale) for x, y in stroke(480)], fill=fill + (255,))
    return img.resize((SIZE, SIZE), Image.LANCZOS)


def shadow(size):
    """The soft shadow under the S, for an icon shown at size pixels: never thinner than a pixel there."""
    px = SIZE / size
    dy, blur = max(14, 1.0 * px), max(22, 1.1 * px)
    alpha = 0.55 if size >= 64 else 0.9
    a = render((0, 0, 0)).getchannel('A').point(lambda v: int(v * alpha))
    a = ImageChops.offset(a, 0, round(dy)).filter(ImageFilter.GaussianBlur(blur))
    img = Image.new('RGBA', (SIZE, SIZE), (0, 0, 0, 0))
    img.putalpha(a)
    return img


def shaded(size):
    return Image.alpha_composite(shadow(size), render(LIGHT)).resize((size, size), Image.LANCZOS)


def document(scale=4):
    """A page with a folded corner and the ink S on it."""
    big = SIZE * scale
    img = Image.new('RGBA', (big, big), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    x0, y0, x1, y1, fold = 168 * scale, 64 * scale, 856 * scale, 960 * scale, 200 * scale
    d.polygon([(x0, y0), (x1 - fold, y0), (x1, y0 + fold), (x1, y1), (x0, y1)], fill=PAGE + (255,), outline=PAGE_EDGE + (255,), width=5 * scale)
    d.polygon([(x1 - fold, y0), (x1 - fold, y0 + fold), (x1, y0 + fold)], fill=FOLD + (255,), outline=PAGE_EDGE + (255,), width=5 * scale)
    img = img.resize((SIZE, SIZE), Image.LANCZOS)
    mark = render(INK).resize((560, 560), Image.LANCZOS)
    img.alpha_composite(mark, (232, 300))
    return img


def write_svgz(path, data):
    with gzip.GzipFile(path, 'wb', mtime=0) as f:
        f.write(data)


def main():
    pngs = {s: shaded(s) for s in PNG_SIZES}
    ico = [pngs[s] for s in ICO_SIZES]
    ico[-1].save(os.path.join(HERE, 'krita.ico'), sizes=[(s, s) for s in ICO_SIZES], append_images=ico[:-1])
    for branding in BRANDINGS:
        folder = os.path.join(HERE, branding)
        write_svgz(os.path.join(folder, 'sc-apps-krita.svgz'), svg(LIGHT, True))
        write_svgz(os.path.join(folder, 'light_krita-branding.svgz'), svg(LIGHT, True))
        write_svgz(os.path.join(folder, 'dark_krita-branding.svgz'), svg(INK))
        for s, im in pngs.items():
            im.save(os.path.join(folder, '%d-apps-krita.png' % s), optimize=True)
        ico[-1].save(os.path.join(folder, 'krita.ico'), sizes=[(s, s) for s in ICO_SIZES], append_images=ico[:-1])
    page = document()
    for s in PNG_SIZES:
        page.resize((s, s), Image.LANCZOS).save(os.path.join(HERE, '..', 'mimetypes', '%d-mimetypes-application-x-krita.png' % s), optimize=True)


if __name__ == '__main__':
    main()
