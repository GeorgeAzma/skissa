#!/usr/bin/env python3
#
#  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
#
#  SPDX-License-Identifier: GPL-3.0-or-later
#
"""
Makes Skissa's own brush set (krita/data/skissa_brushes) from the brushes it is chosen from: Krita's default
bundle, Krita's MyPaint pencil and Rakurri's brush set (CC-BY-SA). Each is the original preset under a short name,
some with settings of their own; they stay ordinary Krita presets.

    python packaging/skissa-brushes.py --krita-bundle <Krita_4_Default_Resources.bundle>
        --mypaint <folder with c)_Pencil_1_Sketch_(mypaint).myb> --rakurri <Rakurri_Brush_Set_V2.1.bundle>
"""
import argparse
import os
import re
import shutil
import struct
import zipfile
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, '..', 'krita', 'data', 'skissa_brushes')

# file in Krita's bundle -> Skissa's name
FROM_KRITA = {
    'b)_Basic-1.kpp': 'Round',
    'b)_Basic-2_Opacity.kpp': 'Opacity',
    'b)_Basic-3_Flow.kpp': 'Flow',
    'b)_Basic-5_Size_default.kpp': 'Size',
    'b)_Airbrush_Soft.kpp': 'Soft',
    'k)_Blender_Basic.kpp': 'Blend',
    'k)_Blender_Blur.kpp': 'Blur',
    't)_Shapes_Fill.kpp': 'Fill',
    'v)_Clone_Tool.kpp': 'Clone',
}
# made from another brush of Krita's: Skissa's name -> its file
DERIVED = {
    'Smear': 'k)_Blender_Basic.kpp',
}
FROM_RAKURRI = {
    'rbr) Charred.kpp': 'Rough',
}
MYPAINT = {
    'c)_Pencil_1_Sketch_(mypaint)': 'Pencil',
}


def chunks(data):
    i = 8
    while i < len(data):
        n = struct.unpack('>I', data[i:i + 4])[0]
        yield data[i + 4:i + 8], data[i + 8:i + 8 + n]
        i += 12 + n


def chunk(kind, body):
    return struct.pack('>I', len(body)) + kind + body + struct.pack('>I', zlib.crc32(kind + body) & 0xffffffff)


def text_of(kind, body):
    key, rest = body.split(b'\0', 1)
    if kind == b'tEXt':
        return key, rest.decode('latin-1')
    if kind == b'zTXt':
        return key, zlib.decompress(rest[1:]).decode('utf-8')
    compressed = rest[0]
    rest = rest[2:]
    _, rest = rest.split(b'\0', 1)
    _, rest = rest.split(b'\0', 1)
    return key, (zlib.decompress(rest) if compressed else rest).decode('utf-8')


def set_param(xml, name, value):
    """Sets a preset parameter, in either attribute order, adding it when the preset has none"""
    pattern = re.compile(r'(<param (?:name="%s" type="string"|type="string" name="%s")><!\[CDATA\[)(.*?)(\]\]></param>)'
                         % (re.escape(name), re.escape(name)), re.S)
    if pattern.search(xml):
        return pattern.sub(lambda m: m.group(1) + value + m.group(3), xml, count=1)
    return xml.replace('</Preset>', ' <param name="%s" type="string"><![CDATA[%s]]></param>\n</Preset>' % (name, value), 1)


def flow(xml):
    """Flow: soft edged, the pen's pressure taking the flow from none to full and the size from 15%, about 7 px"""
    xml = set_param(xml, 'PressureSize', 'true')
    xml = set_param(xml, 'SizeUseCurve', 'true')
    xml = set_param(xml, 'SizeUseSameCurve', 'true')
    xml = set_param(xml, 'SizeSensor', '<!DOCTYPE params> <params id="pressure"> <curve>0,0.15;1,1;</curve> </params> ')
    xml = set_param(xml, 'FlowUseCurve', 'true')
    xml = set_param(xml, 'FlowUseSameCurve', 'true')
    xml = set_param(xml, 'FlowValue', '1')
    xml = set_param(xml, 'FlowSensor', '<!DOCTYPE params> <params id="pressure"> <curve>0,0;1,1;</curve> </params> ')
    brush = ('<Brush autoSpacingCoeff="0.5" BrushVersion="2" useAutoSpacing="1" density="1" spacing="0.1" angle="0" '
             'randomness="0" type="auto_brush"> <MaskGenerator ratio="1" antialiasEdges="1" diameter="7" vfade="1" '
             'id="gauss" hfade="1" spikes="2" type="circle"/> </Brush> ')
    return set_param(xml, 'brush_definition', brush)


def smear(xml):
    """Smear: Krita's Blender Basic carrying the paint along (smearing) instead of dulling it, without a texture or
    the paint color. Krita's own Blender Smear was made for the smudge engine of before Krita 5 and smears
    nothing in the one Skissa has."""
    xml = set_param(xml, 'SmudgeRateMode', '0')
    xml = set_param(xml, 'SmudgeRateUseNewEngine', 'true')
    xml = set_param(xml, 'SmudgeRateSmearAlpha', 'true')
    xml = set_param(xml, 'PressureColorRate', 'false')
    xml = set_param(xml, 'ColorRateValue', '0')
    # dabs close together, or a smear drags the paint in steps
    xml = re.sub(r'useAutoSpacing="0"', 'useAutoSpacing="1"', xml, count=1)
    xml = re.sub(r'autoSpacingCoeff="[^"]*"', 'autoSpacingCoeff="0.2"', xml, count=1)
    return set_param(xml, 'Texture/Pattern/Enabled', 'false')


def renamed(data, name, change=None):
    out = data[:8]
    for kind, body in chunks(data):
        if kind in (b'tEXt', b'zTXt', b'iTXt'):
            key, text = text_of(kind, body)
            if key == b'preset':
                text = re.sub(r'(<Preset[^>]*\bname=")[^"]*(")', lambda m: m.group(1) + name + m.group(2), text, count=1)
                if change:
                    text = change(text)
                out += chunk(b'zTXt', key + b'\0\0' + zlib.compress(text.encode('utf-8')))
                continue
        out += chunk(kind, body)
    return out


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--krita-bundle', required=True)
    parser.add_argument('--mypaint', required=True)
    parser.add_argument('--rakurri', required=True)
    args = parser.parse_args()
    os.makedirs(OUT, exist_ok=True)
    changes = {'Flow': flow, 'Smear': smear}
    for bundle, chosen in ((args.krita_bundle, FROM_KRITA), (args.rakurri, FROM_RAKURRI)):
        with zipfile.ZipFile(bundle) as z:
            for source, name in chosen.items():
                data = renamed(z.read('paintoppresets/' + source), name, changes.get(name))
                with open(os.path.join(OUT, name + '.kpp'), 'wb') as f:
                    f.write(data)
    with zipfile.ZipFile(args.krita_bundle) as z:
        for name, source in DERIVED.items():
            with open(os.path.join(OUT, name + '.kpp'), 'wb') as f:
                f.write(renamed(z.read('paintoppresets/' + source), name, changes.get(name)))
    for source, name in MYPAINT.items():
        shutil.copyfile(os.path.join(args.mypaint, source + '.myb'), os.path.join(OUT, name + '.myb'))
        shutil.copyfile(os.path.join(args.mypaint, source + '_prev.png'), os.path.join(OUT, name + '_prev.png'))
    print(len(FROM_KRITA) + len(DERIVED) + len(FROM_RAKURRI) + len(MYPAINT), 'brushes in', os.path.normpath(OUT))


if __name__ == '__main__':
    main()
