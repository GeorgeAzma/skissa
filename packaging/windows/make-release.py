#!/usr/bin/env python3
#
#  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
#
#  SPDX-License-Identifier: GPL-3.0-or-later
#
"""
Makes a portable Windows release of Skissa: a zip that runs on its own, with the licenses of
everything in it and where to find the sources.

    python make-release.py --install <install prefix> --toolchain <llvm-mingw> --version 0.1.0 --tag v0.1.0

The DLLs come from following the imports of krita.exe, every plugin and the tools the app runs
(ffmpeg for videos), so nothing the app doesn't load is shipped and nothing it loads is missed.
"""

import argparse
import datetime
import os
import re
import shutil
import subprocess
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
SOURCE = os.path.normpath(os.path.join(HERE, '..', '..'))
HOMEPAGE = 'https://github.com/GeorgeAzma/skissa'

# programs the app runs or that run it
PROGRAMS = ['krita.exe', 'krita_version.exe', 'ffmpeg.exe', 'ffprobe.exe']
# folders that only matter for building
SHARE_SKIP = {'aclocal', 'cmake', 'doc', 'man', 'pkgconfig', 'eigen3', 'ECM', 'gettext', 'metainfo', 'applications',
              'kservices5', 'unofficial-angle', 'dlfcn-win32'}
# Windows' own libraries, there on every machine
SYSTEM = re.compile(r'^(api-ms-|ext-ms-)|^(kernel32|user32|gdi32|gdiplus|advapi32|shell32|ole32|oleaut32|comdlg32|comctl32|'
                    r'ws2_32|winmm|imm32|version|uxtheme|dwmapi|setupapi|shlwapi|crypt32|bcrypt|secur32|ntdll|msvcrt|ucrtbase|'
                    r'opengl32|glu32|d3d9|d3d11|d3d12|dxgi|dxguid|dwrite|d2d1|mpr|netapi32|userenv|wtsapi32|iphlpapi|'
                    r'dnsapi|winhttp|wininet|rpcrt4|psapi|dbghelp|hid|mfplat|mf|mfreadwrite|mfuuid|strmiids|wintab32|'
                    r'propsys|normaliz|powrprof|authz|ncrypt|cfgmgr32|dcomp|wldap32|ole32|usp10|mscms|wevtapi|'
                    r'shcore|avrt|ksuser|winspool\.drv|comctl32|evr|d3dcompiler_47|dxva2|mmdevapi|winusb|windowscodecs|'
                    r'oleacc|uiautomationcore|msimg32|odbc32)\.dll$|\.drv$', re.IGNORECASE)


def imports(objdump, binary):
    out = subprocess.run([objdump, '-p', binary], capture_output=True, text=True, errors='replace').stdout
    return re.findall(r'DLL Name:\s*(\S+)', out)


def find_dll(name, folders):
    for folder in folders:
        path = os.path.join(folder, name)
        if os.path.isfile(path):
            return path
    return None


def copy_tree(source, target, skip=()):
    for root, dirs, files in os.walk(source):
        dirs[:] = [d for d in dirs if d not in skip]
        relative = os.path.relpath(root, source)
        for name in files:
            if name.endswith(('.a', '.la', '.prl', '.pdb', '.cmake', '.pc')):
                continue
            os.makedirs(os.path.join(target, relative), exist_ok=True)
            shutil.copy2(os.path.join(root, name), os.path.join(target, relative, name))


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--install', required=True, help='the install prefix the build installed into')
    parser.add_argument('--toolchain', required=True, help='the llvm-mingw toolchain, for its runtime and objdump')
    parser.add_argument('--version', required=True)
    parser.add_argument('--tag', required=True, help='the git tag of this release')
    parser.add_argument('--out', default=os.path.join(SOURCE, 'dist'))
    args = parser.parse_args()

    install = os.path.abspath(args.install)
    objdump = os.path.join(args.toolchain, 'bin', 'llvm-objdump.exe')
    runtime = os.path.join(args.toolchain, 'x86_64-w64-mingw32', 'bin')
    name = 'Skissa-%s-windows-x64' % args.version
    release = os.path.join(os.path.abspath(args.out), name)
    if os.path.exists(release):
        shutil.rmtree(release)
    os.makedirs(os.path.join(release, 'bin'))

    # data and plugins, as the app finds them beside its bin folder
    for folder in ('lib', 'qml', 'translations', 'etc', 'python'):
        if os.path.isdir(os.path.join(install, folder)):
            copy_tree(os.path.join(install, folder), os.path.join(release, folder),
                      skip={'cmake', 'pkgconfig', 'gettext', 'metatypes', 'pyqtbuild', 'sipbuild', '__pycache__'})
    copy_tree(os.path.join(install, 'share'), os.path.join(release, 'share'), skip=SHARE_SKIP)
    # Qt looks for its plugins (platforms, image formats…) beside the program
    copy_tree(os.path.join(install, 'plugins'), os.path.join(release, 'bin'), skip={'designer'})
    shutil.copy2(os.path.join(HERE, 'qt.conf'), os.path.join(release, 'bin', 'qt.conf'))

    # every DLL something shipped imports, wherever it is found
    # Python's own DLLs sit beside it and load from there
    python = os.path.join(release, 'python')
    folders = [os.path.join(install, 'bin'), os.path.join(install, 'lib'), runtime]
    queue = [os.path.join(install, 'bin', p) for p in PROGRAMS]
    for root, _, files in os.walk(release):
        queue += [os.path.join(root, f) for f in files if f.lower().endswith(('.dll', '.pyd'))]
    seen, missing = set(), set()
    while queue:
        binary = queue.pop()
        for dll in imports(objdump, binary):
            key = dll.lower()
            if key in seen or SYSTEM.search(dll):
                continue
            seen.add(key)
            if find_dll(dll, [python]):
                continue
            path = find_dll(dll, folders)
            if not path:
                missing.add(dll)
                continue
            target = os.path.join(release, 'bin', os.path.basename(path))
            if not os.path.exists(target):
                shutil.copy2(path, target)
            queue.append(path)
    for program in PROGRAMS:
        shutil.copy2(os.path.join(install, 'bin', program), os.path.join(release, 'bin', program))
    if missing:
        print('not found (expected from Windows):', ', '.join(sorted(missing)))

    # licenses: the app's, and everything it ships
    licenses = os.path.join(release, 'licenses')
    os.makedirs(licenses)
    for text in ('LICENSES/GPL-3.0-or-later.txt', 'NOTICE', 'README.md'):
        shutil.copy2(os.path.join(SOURCE, text), os.path.join(release, os.path.basename(text) if text != 'NOTICE' else 'NOTICE.txt'))
    shutil.copytree(os.path.join(SOURCE, 'LICENSES'), os.path.join(licenses, 'Skissa and Krita'))
    third = os.path.join(licenses, 'third-party')
    os.makedirs(third)
    for entry in os.listdir(install):
        if 'license' in entry.lower() and os.path.isfile(os.path.join(install, entry)):
            shutil.copy2(os.path.join(install, entry), third)
    for folder in (os.path.join(install, 'share', 'licenses'), os.path.join(install, 'share', 'doc')):
        if os.path.isdir(folder):
            for root, _, files in os.walk(folder):
                for text in files:
                    if re.search(r'licen[cs]e|copying|copyright|notice', text, re.IGNORECASE):
                        package = os.path.relpath(root, folder).split(os.sep)[0]
                        os.makedirs(os.path.join(third, package), exist_ok=True)
                        shutil.copy2(os.path.join(root, text), os.path.join(third, package, text))

    with open(os.path.join(release, 'SOURCE.txt'), 'w', newline='\r\n') as out:
        out.write('Skissa %s, built %s\n\n' % (args.version, datetime.date.today().isoformat()))
        out.write('Its source, exactly as built:\n  %s/tree/%s\n\n' % (HOMEPAGE, args.tag))
        out.write('Skissa is an independent fork of Krita (https://invent.kde.org/graphics/krita), under the GNU GPL\n'
                  'version 3 or later. The libraries it ships are built by KDE for Krita; their sources and the exact\n'
                  'versions are listed at https://invent.kde.org/packaging/krita-deps-management, and the license of\n'
                  'each is in licenses/third-party.\n')

    archive = os.path.join(os.path.abspath(args.out), name + '.zip')
    with zipfile.ZipFile(archive, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for root, _, files in os.walk(release):
            for f in files:
                path = os.path.join(root, f)
                z.write(path, os.path.join(name, os.path.relpath(path, release)))
    print(archive, '%.0f MB' % (os.path.getsize(archive) / 1e6))


if __name__ == '__main__':
    sys.exit(main())
