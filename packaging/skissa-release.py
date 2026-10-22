#!/usr/bin/env python3
#
#  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
#
#  SPDX-License-Identifier: GPL-3.0-or-later
#
"""
Publishes a release of Skissa: version, build, zip, tag, push, GitHub release. Releases update themselves
(KisSkissaUpdater), so a published release reaches everyone the next time they open Skissa.

    python packaging/skissa-release.py --build CMD --install <prefix> --toolchain <llvm-mingw> --out <folder>
                                       [--version 0.3.1] [--notes-file notes.md] [--no-publish]

The version is the next patch after packaging/skissa-version.txt's unless given; it is committed alone. --build is
a shell command that builds and installs this checkout into --install. Without --notes-file, the notes list the
fork's own commits since the last release. --no-publish stops after the zip (nothing committed, tagged or pushed).
"""

import argparse
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SOURCE = os.path.normpath(os.path.join(HERE, '..'))
VERSION_FILE = os.path.join(HERE, 'skissa-version.txt')
REMOTE = 'skissa'
BRANCH = 'main'
REPO = 'GeorgeAzma/skissa'


def git(*args):
    result = subprocess.run(['git'] + list(args), cwd=SOURCE, capture_output=True, text=True, encoding='utf-8', errors='replace')
    if result.returncode:
        sys.exit('git %s failed:\n%s%s' % (' '.join(args), result.stdout, result.stderr))
    return result.stdout.strip()


def run(command, what):
    print('%s: %s' % (what, command if isinstance(command, str) else ' '.join(command)), flush=True)
    if subprocess.run(command, cwd=SOURCE, shell=isinstance(command, str)).returncode:
        sys.exit('%s failed' % what)


def current_version():
    with open(VERSION_FILE) as text:
        return text.read().strip()


def default_notes(since):
    """The fork's own commits since the last release (Krita's arrive through merges)"""
    subjects = git('log', '--first-parent', '--no-merges', '--format=%s', '%s..HEAD' % since).splitlines() if since else []
    subjects = [s for s in subjects if not s.startswith('Skissa ')]
    return ''.join('- %s\n' % s for s in reversed(subjects)) or 'Fixes.\n'


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--build', required=True, help='a shell command that builds and installs this checkout')
    parser.add_argument('--install', required=True, help='the install prefix the build installs into')
    parser.add_argument('--toolchain', required=True, help='the llvm-mingw toolchain')
    parser.add_argument('--out', required=True, help='where the zip is made')
    parser.add_argument('--version')
    parser.add_argument('--notes-file')
    parser.add_argument('--no-publish', action='store_true')
    args = parser.parse_args()

    if git('rev-parse', '--abbrev-ref', 'HEAD') != BRANCH:
        sys.exit('releases are made from %s' % BRANCH)
    if git('status', '--porcelain', '--untracked-files=no'):
        sys.exit('commit or set aside the changes first: the release is built from what is committed')

    last = current_version()
    if args.version:
        version = args.version
    elif git('tag', '--list', 'v' + last):
        major, minor, patch = (int(n) for n in last.split('.'))
        version = '%d.%d.%d' % (major, minor, patch + 1)
    else:
        version = last  # not released yet
    tag = 'v' + version
    if git('tag', '--list', tag):
        sys.exit('%s is already released' % tag)
    previous = git('describe', '--tags', '--abbrev=0', '--match', 'v0.*', 'HEAD') if git('tag', '--list', 'v0.*') else None
    print('Skissa %s (last release %s)' % (version, previous or 'none'), flush=True)

    if version != last:
        with open(VERSION_FILE, 'w', newline='\n') as text:
            text.write(version + '\n')
        if not args.no_publish:
            git('add', VERSION_FILE)
            git('commit', '-m', 'Skissa %s' % version, '-m', 'Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>')

    try:
        run(args.build, 'build')
        run([sys.executable, os.path.join(HERE, 'windows', 'make-release.py'), '--install', args.install, '--toolchain', args.toolchain,
             '--version', version, '--tag', tag, '--out', args.out], 'zip')
    finally:
        if args.no_publish and version != last:
            with open(VERSION_FILE, 'w', newline='\n') as text:
                text.write(last + '\n')
    archive = os.path.join(args.out, 'Skissa-%s-windows-x64.zip' % version)
    if args.no_publish:
        print('made', archive)
        return 0

    notes = os.path.join(args.out, 'notes-%s.md' % version)
    if args.notes_file:
        notes = args.notes_file
    else:
        with open(notes, 'w', encoding='utf-8', newline='\n') as text:
            text.write(default_notes(previous))
    git('tag', '-a', tag, '-m', 'Skissa %s' % version)
    run(['git', 'push', REMOTE, BRANCH], 'push')
    run(['git', 'push', REMOTE, tag], 'push tag')
    run(['gh', 'release', 'create', tag, archive, '--repo', REPO, '--title', 'Skissa %s' % version, '--notes-file', notes], 'release')
    print('released', tag)
    return 0


if __name__ == '__main__':
    sys.exit(main())
