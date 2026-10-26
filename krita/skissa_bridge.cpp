/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

// krita.exe of Skissa: starts skissa.exe beside it with the same arguments, for what still points here (a pin, a
// shortcut, "Open with", the update of a Skissa from before the program was renamed)

#include <windows.h>

#include <cwchar>

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int)
{
    static wchar_t program[32768];
    const DWORD length = GetModuleFileNameW(nullptr, program, 32768);
    if (length == 0 || length >= 32768 - 16) return 1;
    wchar_t *name = std::wcsrchr(program, L'\\');
    if (!name) return 1;
    std::wcscpy(name + 1, L"skissa.exe");

    // the arguments as given, past this program's own name
    const wchar_t *arguments = GetCommandLineW();
    if (*arguments == L'"') {
        arguments++;
        while (*arguments && *arguments != L'"') arguments++;
        if (*arguments) arguments++;
    } else {
        while (*arguments && *arguments != L' ' && *arguments != L'\t') arguments++;
    }

    static wchar_t command[32768 + 64];
    if (std::swprintf(command, sizeof(command) / sizeof(command[0]), L"\"%ls\"%ls", program, arguments) < 0) return 1;

    STARTUPINFOW startup;
    ZeroMemory(&startup, sizeof(startup));
    startup.cb = sizeof(startup);
    GetStartupInfoW(&startup);
    PROCESS_INFORMATION started;
    if (!CreateProcessW(program, command, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &started)) return 1;
    // its window comes to the front as this one's would have
    AllowSetForegroundWindow(started.dwProcessId);
    CloseHandle(started.hThread);
    CloseHandle(started.hProcess);
    return 0;
}
