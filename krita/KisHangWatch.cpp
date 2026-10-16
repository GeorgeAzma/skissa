/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisHangWatch.h"

#include <windows.h>

#include <dbghelp.h>

#include <atomic>
#include <thread>

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTextStream>
#include <QTimer>

#include <KisProfile.h>

namespace
{

std::atomic<qint64> beat {0};

qint64 now()
{
    return QDateTime::currentMSecsSinceEpoch();
}

/// The symbol helpers: Dr. Mingw's mgwhelp, which reads the DWARF of these MinGW builds, or Windows' own
struct Symbols {
    using Initialize = BOOL(WINAPI *)(HANDLE, PCSTR, BOOL);
    using SetOptions = DWORD(WINAPI *)(DWORD);
    using FromAddr = BOOL(WINAPI *)(HANDLE, DWORD64, PDWORD64, PSYMBOL_INFO);
    using LineFromAddr = BOOL(WINAPI *)(HANDLE, DWORD64, PDWORD, PIMAGEHLP_LINE64);
    using Walk = BOOL(WINAPI *)(DWORD, HANDLE, HANDLE, LPSTACKFRAME64, PVOID, PREAD_PROCESS_MEMORY_ROUTINE64,
                                PFUNCTION_TABLE_ACCESS_ROUTINE64, PGET_MODULE_BASE_ROUTINE64, PTRANSLATE_ADDRESS_ROUTINE64);
    using TableAccess = PVOID(WINAPI *)(HANDLE, DWORD64);
    using ModuleBase = DWORD64(WINAPI *)(HANDLE, DWORD64);

    Initialize initialize {nullptr};
    SetOptions setOptions {nullptr};
    FromAddr fromAddr {nullptr};
    LineFromAddr lineFromAddr {nullptr};
    Walk walk {nullptr};
    TableAccess tableAccess {nullptr};
    ModuleBase moduleBase {nullptr};

    bool load()
    {
        HMODULE module = LoadLibraryW(L"mgwhelp.dll");
        if (!module) module = LoadLibraryW(L"dbghelp.dll");
        if (!module) return false;
        initialize = reinterpret_cast<Initialize>(GetProcAddress(module, "SymInitialize"));
        setOptions = reinterpret_cast<SetOptions>(GetProcAddress(module, "SymSetOptions"));
        fromAddr = reinterpret_cast<FromAddr>(GetProcAddress(module, "SymFromAddr"));
        lineFromAddr = reinterpret_cast<LineFromAddr>(GetProcAddress(module, "SymGetLineFromAddr64"));
        walk = reinterpret_cast<Walk>(GetProcAddress(module, "StackWalk64"));
        tableAccess = reinterpret_cast<TableAccess>(GetProcAddress(module, "SymFunctionTableAccess64"));
        moduleBase = reinterpret_cast<ModuleBase>(GetProcAddress(module, "SymGetModuleBase64"));
        return initialize && fromAddr && walk && tableAccess && moduleBase;
    }
};

/// The main thread's call stack, one frame a line
QString stackOf(HANDLE thread)
{
    static Symbols symbols;
    static const bool loaded = symbols.load();
    if (!loaded) return QStringLiteral("(no symbol helper)\n");
    const HANDLE process = GetCurrentProcess();
    static const bool initialized = [&]() {
        if (symbols.setOptions) symbols.setOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
        return symbols.initialize(process, nullptr, TRUE) != FALSE;
    }();
    if (!initialized) return QStringLiteral("(symbols not loaded)\n");

    QString out;
    if (SuspendThread(thread) == DWORD(-1)) return QStringLiteral("(couldn't stop the thread)\n");
    CONTEXT context = {};
    context.ContextFlags = CONTEXT_FULL;
    if (GetThreadContext(thread, &context)) {
        STACKFRAME64 frame = {};
#if defined(_M_X64) || defined(__x86_64__)
        const DWORD machine = IMAGE_FILE_MACHINE_AMD64;
        frame.AddrPC.Offset = context.Rip;
        frame.AddrFrame.Offset = context.Rbp;
        frame.AddrStack.Offset = context.Rsp;
#else
        const DWORD machine = IMAGE_FILE_MACHINE_ARM64;
        frame.AddrPC.Offset = context.Pc;
        frame.AddrFrame.Offset = context.Fp;
        frame.AddrStack.Offset = context.Sp;
#endif
        frame.AddrPC.Mode = frame.AddrFrame.Mode = frame.AddrStack.Mode = AddrModeFlat;
        alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 512] = {};
        auto *symbol = reinterpret_cast<SYMBOL_INFO *>(buffer);
        for (int i = 0; i < 64; i++) {
            if (!symbols.walk(machine, process, thread, &frame, &context, nullptr, symbols.tableAccess, symbols.moduleBase, nullptr)) break;
            const DWORD64 address = frame.AddrPC.Offset;
            if (!address) break;
            symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
            symbol->MaxNameLen = 511;
            DWORD64 offset = 0;
            wchar_t moduleName[MAX_PATH] = L"";
            HMODULE module = nullptr;
            if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   reinterpret_cast<LPCWSTR>(address), &module)) {
                GetModuleFileNameW(module, moduleName, MAX_PATH);
            }
            QString line = QStringLiteral("%1 %2!").arg(address, 16, 16, QLatin1Char('0')).arg(QFileInfo(QString::fromWCharArray(moduleName)).fileName());
            if (symbols.fromAddr(process, address, &offset, symbol)) {
                line += QString::fromLatin1(symbol->Name) + QStringLiteral("+0x") + QString::number(offset, 16);
            }
            IMAGEHLP_LINE64 source = {};
            source.SizeOfStruct = sizeof(source);
            DWORD column = 0;
            if (symbols.lineFromAddr && symbols.lineFromAddr(process, address, &column, &source) && source.FileName) {
                line += QStringLiteral("  [%1 @ %2]").arg(QString::fromLatin1(source.FileName)).arg(source.LineNumber);
            }
            out += line + QLatin1Char('\n');
        }
    }
    ResumeThread(thread);
    return out;
}

void write(const QString &stack, qint64 stalled)
{
    QFile file(KisProfile::configFile(QStringLiteral("krita-hang.log")));
    if (!file.open(QIODevice::Append | QIODevice::Text)) return;
    QTextStream stream(&file);
    stream << QDateTime::currentDateTime().toString(Qt::ISODate) << ": not responding for " << stalled / 1000 << " s, the main thread is at\n"
           << stack << "\n";
}

} // namespace

void KisHangWatch::start()
{
    HANDLE main = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &main,
                         THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0)) {
        return;
    }
    beat = now();
    auto *timer = new QTimer(qApp);
    timer->setInterval(500);
    QObject::connect(timer, &QTimer::timeout, timer, []() { beat = now(); });
    timer->start();

    std::thread([main]() {
        qint64 reported = 0;
        while (true) {
            Sleep(2000);
            const qint64 last = beat;
            const qint64 stalled = now() - last;
            // once per hang, while it lasts
            if (stalled > 10000 && reported != last) {
                reported = last;
                write(stackOf(main), stalled);
            }
        }
    }).detach();
}
