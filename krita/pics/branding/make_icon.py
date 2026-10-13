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
# into every branding folder, so no build shows Krita's logo; the .kra and .krz file icons (a page
# with the S, the .krz one zipped) into ../mimetypes and the small in-app glyphs into /pics; and
# the packages' own icons: macOS (.icon bundles, .icns), Android (launcher) and the Windows Store
# (tiles, and a taskbar S that follows the light or dark taskbar). Needs Pillow.

import base64
import gzip
import io
import math
import json
import os
import shutil

from PIL import Image, ImageChops, ImageDraw, ImageFilter

SIZE = 1024
GROW = 1.22             # the S fills the icon; there's no tile around it
INK = (38, 35, 32)
LIGHT = (250, 248, 244)
PAGE = (252, 250, 246)
PAGE_EDGE = (205, 190, 165)
FOLD = (232, 220, 199)
BACKDROP = (32, 30, 28)  # behind the S where a platform wants a full tile (Android, macOS)
GLYPH = {'dark': '#373737', 'light': '#d2d2d2'}
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..', '..'))
MIMETYPES = os.path.join(HERE, '..', 'mimetypes')
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


# the page: its outline with a folded corner, the fold, and where the S sits on it
PAGE_X0, PAGE_Y0, PAGE_X1, PAGE_Y1, PAGE_FOLD = 168, 64, 856, 960, 200


def page_outline():
    x0, y0, x1, y1, f = PAGE_X0, PAGE_Y0, PAGE_X1, PAGE_Y1, PAGE_FOLD
    return [(x0, y0), (x1 - f, y0), (x1, y0 + f), (x1, y1), (x0, y1)]


def page_fold():
    x1, y0, f = PAGE_X1, PAGE_Y0, PAGE_FOLD
    return [(x1 - f, y0), (x1 - f, y0 + f), (x1, y0 + f)]


def zipper():
    """The .krz page's zip: teeth down its right side, alternating."""
    teeth, x, w, h = [], PAGE_X1 - 130, 44, 40
    for k, y in enumerate(range(PAGE_Y0 + PAGE_FOLD + 40, PAGE_Y1 - 60, h)):
        dx = 0 if k % 2 else w
        teeth.append([(x + dx, y), (x + dx + w, y), (x + dx + w, y + h), (x + dx, y + h)])
    return teeth


def mark_box(zipped):
    """Where the S sits on the page: (left, top, side)."""
    return (170, 310, 500) if zipped else (232, 300, 560)


def document(zipped=False, scale=4):
    """A page with a folded corner and the ink S on it."""
    big = SIZE * scale
    img = Image.new('RGBA', (big, big), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    up = lambda pts: [(x * scale, y * scale) for x, y in pts]
    d.polygon(up(page_outline()), fill=PAGE + (255,), outline=PAGE_EDGE + (255,), width=5 * scale)
    d.polygon(up(page_fold()), fill=FOLD + (255,), outline=PAGE_EDGE + (255,), width=5 * scale)
    if zipped:
        for tooth in zipper():
            d.polygon(up(tooth), fill=PAGE_EDGE + (255,))
    img = img.resize((SIZE, SIZE), Image.LANCZOS)
    x, y, side = mark_box(zipped)
    img.alpha_composite(render(INK).resize((side, side), Image.LANCZOS), (x, y))
    return img


def document_svg(zipped=False, glyph=None, size=SIZE):
    """The page as an SVG: in its own colors, or as a one-color in-app glyph (outlined page, solid S)."""
    pts = lambda p: ' '.join('%g,%g' % q for q in p)
    x, y, side = mark_box(zipped)
    s = 'M' + ' L'.join('%.1f %.1f' % p for p in stroke()) + ' Z'
    body = []
    if glyph:
        line = max(5, 1.2 * SIZE / size)
        body.append(f' <polygon points="{pts(page_outline())}" fill="none" stroke="{glyph}" stroke-width="{line:g}" stroke-linejoin="round"/>')
        body.append(f' <polyline points="{pts(page_fold())}" fill="none" stroke="{glyph}" stroke-width="{line:g}" stroke-linejoin="round"/>')
        ink = teeth = glyph
    else:
        body.append(f' <polygon points="{pts(page_outline())}" fill="rgb{PAGE}" stroke="rgb{PAGE_EDGE}" stroke-width="5"/>')
        body.append(f' <polygon points="{pts(page_fold())}" fill="rgb{FOLD}" stroke="rgb{PAGE_EDGE}" stroke-width="5"/>')
        ink, teeth = 'rgb%s' % (INK,), 'rgb%s' % (PAGE_EDGE,)
    if zipped:
        for tooth in zipper():
            body.append(f' <polygon points="{pts(tooth)}" fill="{teeth}"/>')
    body.append(f' <path transform="translate({x} {y}) scale({side / SIZE:g})" d="{s}" fill="{ink}"/>')
    return (f'<svg xmlns="http://www.w3.org/2000/svg" width="{size}" height="{size}" viewBox="0 0 {SIZE} {SIZE}">\n'
            + '\n'.join(body) + '\n</svg>\n').encode()


def tile(size, round_=False, mark=0.6):
    """The white S on a dark tile (or circle), for platforms that mask icons themselves."""
    scale = 4
    big = size * scale
    img = Image.new('RGBA', (big, big), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    if round_:
        d.ellipse((0, 0, big - 1, big - 1), fill=BACKDROP + (255,))
    else:
        d.rectangle((0, 0, big, big), fill=BACKDROP + (255,))
    img = img.resize((size, size), Image.LANCZOS)
    side = round(size * mark)
    img.alpha_composite(shaded(side), ((size - side) // 2, (size - side) // 2))
    return img


def centered(width, height, mark_of_side, share):
    """A transparent picture of the given size with the S in the middle, share of the shorter side."""
    img = Image.new('RGBA', (width, height), (0, 0, 0, 0))
    side = round(min(width, height) * share)
    img.alpha_composite(mark_of_side(side), ((width - side) // 2, (height - side) // 2))
    return img


def ink_s(side):
    return render(INK).resize((side, side), Image.LANCZOS)


def write_icon_bundle(folder):
    """A macOS 26 .icon bundle: the white S on the dark tile; the system adds the glass and shadow."""
    if os.path.isdir(folder):
        shutil.rmtree(folder)
    os.makedirs(os.path.join(folder, 'Assets'))
    with open(os.path.join(folder, 'Assets', 'skissa.svg'), 'wb') as f:
        f.write(svg(LIGHT))
    r, g, b = (c / 255 for c in BACKDROP)
    doc = {
        'fill': {'solid': 'srgb:%.5f,%.5f,%.5f,1.00000' % (r, g, b)},
        'groups': [{
            'layers': [{'glass': True, 'image-name': 'skissa.svg', 'name': 'skissa'}],
            'shadow': {'kind': 'neutral', 'opacity': 0.5},
            'translucency': {'enabled': True, 'value': 0.4},
        }],
        'supported-platforms': {'circles': ['watchOS'], 'squares': 'shared'},
    }
    with open(os.path.join(folder, 'icon.json'), 'w', newline='\n') as f:
        json.dump(doc, f, indent=2)
        f.write('\n')


def hex_color(c):
    return '#%02X%02X%02X' % c


def android_foreground():
    """The S as an Android vector drawable, inside the adaptive icon's safe circle."""
    d = 'M' + ' L'.join('%.1f,%.1f' % p for p in stroke()) + 'Z'
    return ('<vector xmlns:android="http://schemas.android.com/apk/res/android"\n'
            '    android:width="108dp"\n    android:height="108dp"\n'
            f'    android:viewportWidth="{SIZE}"\n    android:viewportHeight="{SIZE}">\n'
            f'  <group android:pivotX="{SIZE // 2}" android:pivotY="{SIZE // 2}" android:scaleX="0.58" android:scaleY="0.58">\n'
            f'    <path android:fillColor="{hex_color(LIGHT)}" android:pathData="{d}"/>\n'
            '  </group>\n</vector>\n')


def android_background():
    return ('<vector xmlns:android="http://schemas.android.com/apk/res/android"\n'
            '    android:width="108dp"\n    android:height="108dp"\n'
            '    android:viewportWidth="108"\n    android:viewportHeight="108">\n'
            f'  <path android:fillColor="{hex_color(BACKDROP)}" android:pathData="M0,0h108v108h-108z"/>\n'
            '</vector>\n')


def write_text(path, text):
    with open(path, 'w', newline='\n') as f:
        f.write(text)


def packages():
    # macOS
    for branding in BRANDINGS:
        write_icon_bundle(os.path.join(HERE, branding, 'krita.icon'))
    tile(1024, mark=0.62).save(os.path.join(ROOT, 'packaging', 'macos', 'KritaIcon.icns'))
    document().save(os.path.join(MIMETYPES, 'krita-kra.icns'))
    document(True).save(os.path.join(MIMETYPES, 'krz', 'krita-krz.icns'))
    # Android
    apk = os.path.join(ROOT, 'packaging', 'android', 'apk')
    res = os.path.join(apk, 'res')
    for name in ('ic_launcher', 'ic_launcher_next'):
        write_text(os.path.join(res, 'drawable', name + '_foreground.xml'), android_foreground())
        write_text(os.path.join(res, 'drawable', name + '_background.xml'), android_background())
        tile(512).save(os.path.join(apk, name + '-playstore.png'), optimize=True)
        for density, px in (('mdpi', 48), ('hdpi', 72), ('xhdpi', 96), ('xxhdpi', 144), ('xxxhdpi', 192)):
            folder = os.path.join(res, 'mipmap-' + density)
            tile(px).save(os.path.join(folder, name + '.webp'), lossless=True)
            tile(px, round_=True, mark=0.56).save(os.path.join(folder, name + '_round.webp'), lossless=True)
    for name in ('ic_launcher_background', 'ic_launcher_next_background'):
        path = os.path.join(res, 'values', name + '.xml')
        if os.path.exists(path):
            write_text(path, '<?xml version="1.0" encoding="utf-8"?>\n<resources>\n'
                             f'    <color name="{name}">{hex_color(BACKDROP)}</color>\n</resources>\n')
    # the Windows Store: the plated and dark-taskbar S is white, the light-taskbar one is ink
    assets = os.path.join(ROOT, 'packaging', 'windows', 'msix', 'pkg', 'Assets')
    for name in sorted(os.listdir(assets)):
        path = os.path.join(assets, name)
        w, h = Image.open(path).size
        if name.startswith('fileicon'):
            im = document().resize((w, h), Image.LANCZOS)
        elif 'lightunplated' in name:
            im = centered(w, h, ink_s, 0.9)
        elif name.startswith('Square44x44'):
            im = centered(w, h, shaded, 0.9)
        else:
            im = centered(w, h, shaded, 0.6)
        im.save(path, optimize=True)


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
    for zipped, folder, name in ((False, MIMETYPES, 'krita'), (True, os.path.join(MIMETYPES, 'krz'), 'krz')):
        page = document(zipped)
        for s in PNG_SIZES:
            page.resize((s, s), Image.LANCZOS).save(os.path.join(folder, '%d-mimetypes-application-x-%s.png' % (s, name)), optimize=True)
        for s, suffix in ((16, '-16'), (22, '-22'), (24, '-24'), (SIZE, '')):
            with open(os.path.join(folder, 'application-x-%s%s.svg' % (name, suffix)), 'wb') as f:
                f.write(document_svg(zipped, size=s))
        for s in (16, 22, 32, 64):
            for theme, color in GLYPH.items():
                with open(os.path.join(ROOT, 'pics', '%d_%s_application-x-%s.svg' % (s, theme, name)), 'wb') as f:
                    f.write(document_svg(zipped, color, s))
    packages()


if __name__ == '__main__':
    main()
