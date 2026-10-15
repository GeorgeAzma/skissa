#!/usr/bin/env python3
#
#  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
#
#  SPDX-License-Identifier: GPL-3.0-or-later
#
"""
Gives Skissa's own text the translations Krita already has.

Krita's translators never saw the text Skissa added or changed, so it would show in English. Many of
those strings are ones Krita already translates in another context, or Krita's own sentences with the
app's name changed. This finds them and writes an entry for each to po-skissa/<lang>.po:

    python packaging/skissa-translations.py --gettext <folder with xgettext.exe and msgfmt.exe>

Krita's own po/<lang>/krita.po stay as upstream has them, so merging translations never conflicts; the
build joins the two (--join, from skissa_install_translations in CMakeLists.txt), leaving out any entry
upstream has since added.

Only translations that already exist are reused, adapted at most by:
  - "Krita" -> "Skissa", where the translation keeps the name as it is;
  - a trailing colon or ellipsis, or a keyboard accelerator, dropped.
Run it again after merging translations from upstream or changing text.
"""

import argparse
import collections
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SOURCE = os.path.normpath(os.path.join(HERE, '..'))
HEADER = '# Skissa: its own text, with translations Krita already has (packaging/skissa-translations.py)\n\n'
UPSTREAM = 'origin/master'

KDE_KEYWORDS = ['-ki18n:1', '-ki18nc:1c,2', '-ki18np:1,2', '-ki18ncp:1c,2,3', '-kki18n:1', '-kki18nc:1c,2',
                '-kki18np:1,2', '-kki18ncp:1c,2,3', '-kxi18n:1', '-kxi18nc:1c,2', '-kxi18np:1,2', '-kxi18ncp:1c,2,3',
                '-kkxi18n:1', '-kkxi18nc:1c,2', '-kkxi18np:1,2', '-kkxi18ncp:1c,2,3', '-kI18N_NOOP:1',
                '-kI18NC_NOOP:1c,2', '-kI18N_NOOP2:1c,2', '-kI18N_NOOP2_NOSTRIP:1c,2', '-ktr2i18n:1', '-ktr2xi18n:1',
                '-kkli18n:1', '-kkli18nc:1c,2', '-kkli18np:1,2', '-kkli18ncp:1c,2,3']
UNDO_KEYWORDS = ['-kkundo2_i18n:1', '-kkundo2_i18np:1,2', '-kkundo2_i18nc:1c,2', '-kkundo2_i18ncp:1c,2,3']


# ---------------------------------------------------------------- po files

Entry = collections.namedtuple('Entry', 'ctx id plural strs fuzzy obsolete')


def unquote(text):
    return bytes(text, 'utf-8').decode('unicode_escape').encode('latin-1').decode('utf-8')


def quote(text):
    out = text.replace('\\', '\\\\').replace('"', '\\"').replace('\t', '\\t').replace('\n', '\\n')
    return '"%s"' % out


def parse(path):
    """The entries of a po file (its header left out)"""
    entries = []
    with open(path, encoding='utf-8') as f:
        blocks = f.read().split('\n\n')
    for block in blocks:
        fields, current, fuzzy, obsolete = {}, None, False, False
        for line in block.split('\n'):
            if line.startswith('#~'):
                obsolete = True
                line = line[2:].strip()
                if line.startswith('|'):
                    continue
            elif line.startswith('#'):
                if line.startswith('#,') and 'fuzzy' in line:
                    fuzzy = True
                continue
            if not line.strip():
                continue
            m = re.match(r'^(msgctxt|msgid_plural|msgid|msgstr(?:\[\d+\])?)\s+(".*")$', line)
            if m:
                current = m.group(1)
                fields[current] = unquote(m.group(2)[1:-1])
            elif line.startswith('"') and current:
                fields[current] += unquote(line[1:-1])
        if 'msgid' not in fields or fields['msgid'] == '':
            continue
        strs = [fields[k] for k in sorted((k for k in fields if k.startswith('msgstr')), key=lambda k: (len(k), k))]
        entries.append(Entry(fields.get('msgctxt'), fields['msgid'], fields.get('msgid_plural'), strs, fuzzy, obsolete))
    return entries


def write_entry(out, ctx, msgid, plural, strs):
    if ctx is not None:
        out.write('msgctxt %s\n' % quote(ctx))
    out.write('msgid %s\n' % quote(msgid))
    if plural is not None:
        out.write('msgid_plural %s\n' % quote(plural))
        for i, s in enumerate(strs):
            out.write('msgstr[%d] %s\n' % (i, quote(s)))
    else:
        out.write('msgstr %s\n' % quote(strs[0]))
    out.write('\n')


# ---------------------------------------------------------------- the fork's text

def fork_strings(gettext):
    """Every (context, text, plural) in the files the fork changed, as gettext extracts them"""
    base = subprocess.run(['git', 'merge-base', 'HEAD', UPSTREAM], cwd=SOURCE, capture_output=True, text=True, check=True).stdout.strip()
    changed = subprocess.run(['git', 'diff', '--name-only', '--diff-filter=AM', base, 'HEAD'], cwd=SOURCE,
                             capture_output=True, text=True, check=True).stdout.split()
    code = [f for f in changed if f.endswith(('.cpp', '.cc', '.h')) and '/tests/' not in f and os.path.exists(os.path.join(SOURCE, f))]
    actions = [f for f in changed if f.endswith('.action')]
    with tempfile.TemporaryDirectory() as tmp:
        rc = os.path.join(tmp, 'rc.cpp')
        with open(rc, 'w', encoding='utf-8') as out:
            if actions:
                out.write(subprocess.run(['perl', 'action_i18n.pl', '--context=action'] + actions, cwd=SOURCE,
                                         capture_output=True, text=True, encoding='utf-8', check=True).stdout)
        files = os.path.join(tmp, 'files.txt')
        with open(files, 'w', encoding='utf-8') as out:
            out.write('\n'.join([rc] + [os.path.join(SOURCE, f) for f in code]))
        xgettext = os.path.join(gettext, 'xgettext.exe' if os.name == 'nt' else 'xgettext')
        found = []
        for keywords, undo in ((KDE_KEYWORDS, False), (UNDO_KEYWORDS, True)):
            pot = os.path.join(tmp, 'undo.pot' if undo else 'normal.pot')
            subprocess.run([xgettext, '--from-code=UTF-8', '-C', '-k', '--kde', '--force-po', '-f', files, '-o', pot] + keywords,
                           check=True, capture_output=True)
            for e in parse(pot):
                ctx = e.ctx
                if undo:
                    ctx = '(qtundo-format)' if ctx is None else '(qtundo-format) ' + ctx
                found.append((ctx, e.id, e.plural))
    return list(dict.fromkeys(found))


# ---------------------------------------------------------------- reusing translations

def without_accelerator(text):
    text = re.sub(r'\s*\(&\w\)', '', text)  # CJK style: "ファイル(&F)"
    return re.sub(r'&(?=\S)', '', text.replace('&&', '\0')).replace('\0', '&')


ENDINGS = [('...', ('...', '…')), ('…', ('…', '...')), (':', (':', '：', ' :'))]


def trimmer(forms):
    def trim(s):
        s = s.rstrip()
        for form in forms:
            if s.endswith(form):
                return s[:-len(form)].rstrip()
        return None
    return trim


def candidates(msgid, plural):
    """Ways Krita may have written the same text: (text, plural, accelerator ignored, how to adapt its translation)"""
    keep = lambda s: s
    out = [(msgid, plural, False, keep)]
    if 'Skissa' in msgid:
        out.append((msgid.replace('Skissa', 'Krita'), plural and plural.replace('Skissa', 'Krita'), False,
                    lambda s: s.replace('Krita', 'Skissa') if 'Krita' in s else None))
    if plural is None:
        for ending, forms in ENDINGS:
            out.append((msgid + ending, None, False, trimmer(forms)))
    if '&' not in msgid:
        for text, plural_, _, adapt in list(out):
            out.append((text, plural_, True, lambda s, adapt=adapt: adapt(without_accelerator(s))))
    return out


# text Skissa uses in another sense than Krita does (a noun where Krita has the verb, a theme's name)
OTHER_SENSE = {
    ('layer names', 'Copy'), ('default name of a clone layer', 'Clone'), ('default name of a fill layer', 'Fill'),
    ('theme preset', 'Light'), ('pen pressure', 'Light'), ('@info:tooltip', 'Blend mode'), ('held key: drag', 'Drag'),
    ('pen pressure', 'Hard'), ('frames at a quarter', 'Quarter'), ('pressure curve', 'Straight'), ('@action:button the brushes', 'Map'),
    ('settings section', 'Tools'), ('@title find results', 'Tools'), ('@title tools rail', 'Tools'), ('keys group', 'Tools'),
}


def short(text):
    return len(text.split()) <= 2


def translate(entries, wanted):
    """Translations for the (context, text, plural) in wanted that the po file's entries don't have"""
    # an old, unused entry with the same text still counts as a definition for msgfmt
    have = {(e.ctx, e.id) for e in entries}
    pools = {False: collections.defaultdict(collections.Counter), True: collections.defaultdict(collections.Counter)}
    for e in entries:
        if e.fuzzy or not e.strs or not all(e.strs):
            continue
        # current translations count for more than obsolete ones
        weight = 1 if e.obsolete else 10
        pools[False][(e.id, e.plural)][tuple(e.strs)] += weight
        if '&' in e.id:
            pools[True][(without_accelerator(e.id), e.plural)][tuple(e.strs)] += weight

    out = []
    for ctx, msgid, plural in wanted:
        if (ctx, msgid) in have or any(ctx and ctx.startswith(c) and msgid == t for c, t in OTHER_SENSE):
            continue
        for text, plural_, bare, adapt in candidates(msgid, plural):
            pool = pools[bare].get((text, plural_))
            if not pool:
                continue
            if '&' not in msgid:
                adapt = lambda s, adapt=adapt: adapt(without_accelerator(s))
            # a word or two can mean different things: only one Krita translates the same way in several places
            if short(text) and (len({tuple(without_accelerator(x).strip().lower() for x in strs) for strs in pool}) > 1
                                or sum(pool.values()) < 20):
                break
            strs = [adapt(s) for s in pool.most_common(1)[0][0]]
            if all(strs):
                out.append((ctx, msgid, plural, strs))
                have.add((ctx, msgid))
                break
    return out


def join(upstream, fork, out):
    """Krita's po file with the fork's entries added, those upstream has left out"""
    present = {(e.ctx, e.id) for e in parse(upstream)}
    with open(upstream, encoding='utf-8', newline='') as f:
        text = f.read()
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, 'w', encoding='utf-8', newline='') as f:
        f.write(text.rstrip('\r\n') + '\n\n')
        for e in parse(fork):
            if (e.ctx, e.id) not in present:
                write_entry(f, e.ctx, e.id, e.plural, e.strs)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--gettext', help='the folder with xgettext and msgfmt')
    parser.add_argument('--join', nargs=3, metavar=('UPSTREAM', 'FORK', 'OUT'), help='write UPSTREAM with the entries of FORK it lacks to OUT')
    args = parser.parse_args()
    if args.join:
        join(*args.join)
        return
    if not args.gettext:
        parser.error('--gettext is needed')

    wanted = fork_strings(args.gettext)
    print(len(wanted), 'strings in the files Skissa changed')
    podir = os.path.join(SOURCE, 'po')
    forkdir = os.path.join(SOURCE, 'po-skissa')
    os.makedirs(forkdir, exist_ok=True)
    msgfmt = os.path.join(args.gettext, 'msgfmt.exe' if os.name == 'nt' else 'msgfmt')
    report = []
    for lang in sorted(os.listdir(podir)):
        path = os.path.join(podir, lang, 'krita.po')
        if not os.path.isfile(path):
            continue
        entries = parse(path)
        present = {(e.ctx, e.id) for e in entries if not e.obsolete}
        missing = {(w[0], w[1]) for w in wanted} - present
        added = translate(entries, wanted)
        fork = os.path.join(forkdir, lang + '.po')
        if not added:
            if os.path.exists(fork):
                os.remove(fork)
            continue
        with open(fork, 'w', encoding='utf-8', newline='') as out:
            out.write(HEADER)
            for ctx, msgid, plural, strs in added:
                write_entry(out, ctx, msgid, plural, strs)
        with tempfile.TemporaryDirectory() as tmp:
            joined = os.path.join(tmp, 'krita.po')
            join(path, fork, joined)
            check = subprocess.run([msgfmt, '-c', '-o', os.path.join(tmp, 'krita.mo'), joined], capture_output=True, text=True, encoding='utf-8', errors='replace')
            if check.returncode:
                sys.exit('%s: %s' % (fork, check.stderr[-2000:]))
        report.append((lang, len(added), len(missing)))
    for lang, added, missing in report:
        print('%-12s %4d of %4d' % (lang, added, missing))


if __name__ == '__main__':
    sys.exit(main())
