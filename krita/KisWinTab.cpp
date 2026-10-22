/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisWinTab.h"

#include <windows.h>

#include <QByteArray>
#include <QLibrary>

bool KisWinTab::refused()
{
    QLibrary winTab(QStringLiteral("wintab32"));
    if (!winTab.load()) return false;
    using Info = UINT(WINAPI *)(UINT, UINT, LPVOID);
    using Open = HANDLE(WINAPI *)(HWND, LPVOID, BOOL);
    using Close = BOOL(WINAPI *)(HANDLE);
    const auto info = reinterpret_cast<Info>(winTab.resolve("WTInfoW"));
    const auto open = reinterpret_cast<Open>(winTab.resolve("WTOpenW"));
    const auto close = reinterpret_cast<Close>(winTab.resolve("WTClose"));
    if (!info || !open || !close) return false;

    const UINT WTI_DEFSYSCTX = 4;
    const UINT size = info(WTI_DEFSYSCTX, 0, nullptr);
    if (!size) return false;
    QByteArray context(int(size), '\0');
    info(WTI_DEFSYSCTX, 0, context.data());

    // the context Qt asks for, on a window of its kind
    HWND window = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
    HANDLE opened = open(window, context.data(), TRUE);
    if (opened) close(opened);
    DestroyWindow(window);
    return !opened;
}
