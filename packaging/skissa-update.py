#!/usr/bin/env python3
#
#  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
#
#  SPDX-License-Identifier: GPL-3.0-or-later
#
"""
Brings Krita's newest changes into Skissa: fetch, merge, translations, build, check.

    python packaging/skissa-update.py --gettext <folder with xgettext and msgfmt> [--build CMD] [--check CMD]

The merge uses git's rerere (enabled here), so a conflict settled once is settled the same way the next
time. When a conflict is left, the merge is left open with the files listed, to settle and commit by hand
(or `git merge --abort`); run the script again afterwards for the rest. --build and --check are shell
commands run after the merge (say, a build and a start of the app); a failure stops the script.

Exit codes: 0 up to date or updated, 1 a step failed, 2 conflicts are left.
"""

import argparse
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SOURCE = os.path.normpath(os.path.join(HERE, '..'))
UPSTREAM_REMOTE = 'origin'
UPSTREAM_BRANCH = 'master'
# Skissa's own copies of some of Krita's files, built in their place (a folder and Krita's beside it, or a file and
# Krita's): Krita's changes to those files don't reach them by merging, so they are listed to bring over by hand
KEPT = [
    ('plugins/dockers/skissa_layers', 'plugins/dockers/layerdocker'),
    ('plugins/dockers/skissa_palette', 'plugins/dockers/palettedocker'),
    ('plugins/dockers/skissa_recorder', 'plugins/dockers/recorder'),
    ('plugins/tools/basictools/KisStudioMultiBrush.cpp', 'plugins/tools/basictools/kis_tool_multihand.cpp'),
    ('plugins/tools/basictools/KisStudioMultiBrush.h', 'plugins/tools/basictools/kis_tool_multihand.h'),
]


def git(*args, check=True):
    result = subprocess.run(['git'] + list(args), cwd=SOURCE, capture_output=True, text=True, encoding='utf-8', errors='replace')
    if check and result.returncode:
        sys.exit('git %s failed:\n%s%s' % (' '.join(args), result.stdout, result.stderr))
    return result


def run(command, what):
    print('%s: %s' % (what, command))
    if subprocess.run(command, cwd=SOURCE, shell=True).returncode:
        sys.exit('%s failed' % what)


def conflicts():
    return git('diff', '--name-only', '--diff-filter=U').stdout.split()


def report_kept(since, upstream):
    """Krita's commits since `since` that changed the files Skissa keeps copies of"""
    paths = []
    for own, krita in KEPT:
        folder = os.path.join(SOURCE, own)
        if os.path.isfile(folder):
            paths.append(krita)
        for name in sorted(os.listdir(folder)) if os.path.isdir(folder) else []:
            if name != 'CMakeLists.txt' and os.path.exists(os.path.join(SOURCE, krita, name)):
                paths.append('%s/%s' % (krita, name))
    if not paths:
        return
    log = git('log', '--oneline', '--no-merges', '%s..%s' % (since, upstream), '--', *paths).stdout.strip()
    if log:
        print('Krita changed files Skissa keeps its own copies of; bring these over by hand:\n' + log)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--gettext', help='the folder with xgettext and msgfmt, for the translations')
    parser.add_argument('--build', help='a command that builds (and installs)')
    parser.add_argument('--check', help='a command that checks the built app')
    parser.add_argument('--no-fetch', action='store_true', help='merge what was fetched already')
    args = parser.parse_args()

    git('config', 'rerere.enabled', 'true')
    git('config', 'rerere.autoupdate', 'true')
    upstream = '%s/%s' % (UPSTREAM_REMOTE, UPSTREAM_BRANCH)

    merge_head = os.path.join(git('rev-parse', '--absolute-git-dir').stdout.strip(), 'MERGE_HEAD')
    if os.path.exists(merge_head):
        left = conflicts()
        if left:
            print('The merge still has conflicts:\n  ' + '\n  '.join(left))
            return 2
        sys.exit('A merge is waiting to be committed: commit it, then run this again')
    if git('status', '--porcelain', '--untracked-files=no').stdout.strip():
        sys.exit('Commit or stash the changes to tracked files first')

    if not args.no_fetch:
        print('Fetching %s' % upstream)
        git('fetch', UPSTREAM_REMOTE, UPSTREAM_BRANCH)
    new = int(git('rev-list', '--count', 'HEAD..' + upstream).stdout.strip())
    if new:
        print('%d new commits from Krita' % new)
        report_kept(git('merge-base', 'HEAD', upstream).stdout.strip(), upstream)
        merge = git('merge', '--no-edit', '-m', 'Merge Krita %s' % git('rev-parse', '--short', upstream).stdout.strip(), upstream, check=False)
        if merge.returncode:
            left = conflicts()
            if not os.path.exists(merge_head):
                # it never started (files in the way, say)
                sys.exit('The merge failed:\n%s%s' % (merge.stdout, merge.stderr))
            if not left:
                # rerere settled every conflict: the merge only needs committing
                git('commit', '--no-edit')
            else:
                print(merge.stdout)
                print('Conflicts to settle (then commit, and run this again):\n  ' + '\n  '.join(left))
                return 2
    else:
        print('Already up to date with Krita')

    if args.gettext:
        script = os.path.join(HERE, 'skissa-translations.py')
        env = dict(os.environ)
        if not shutil.which('perl'):
            # Git for Windows ships one, in usr/bin of the folder git.exe is somewhere under
            folder = os.path.dirname(shutil.which('git') or '')
            while folder and not os.path.exists(os.path.join(folder, 'usr', 'bin', 'perl.exe')) and os.path.dirname(folder) != folder:
                folder = os.path.dirname(folder)
            env['PATH'] = os.path.join(folder, 'usr', 'bin') + os.pathsep + env.get('PATH', '')
        if subprocess.run([sys.executable, script, '--gettext', args.gettext], cwd=SOURCE, env=env,
                          stdout=subprocess.DEVNULL).returncode:
            sys.exit('translations failed')
        if git('status', '--porcelain', 'po-skissa').stdout.strip():
            git('add', 'po-skissa')
            git('commit', '-m', 'Translations: Krita\'s newest for the fork\'s text')
            print('Translations updated')

    if args.build:
        run(args.build, 'Build')
    if args.check:
        run(args.check, 'Check')
    return 0


if __name__ == '__main__':
    sys.exit(main())
