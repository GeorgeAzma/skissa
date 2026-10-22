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
std::atomic<bool> quitting {false};
/// The main thread's stack, for walking it safely
ULONG_PTR stackLow = 0;
ULONG_PTR stackHigh = 0;

/// Milliseconds on a clock the computer's sleep doesn't move, so waking up isn't taken for a hang
qint64 now()
{
    ULONGLONG time = 0;
    QueryUnbiasedInterruptTime(&time);
    return qint64(time / 10000);
}

#if defined(_M_X64) || defined(__x86_64__)
/**
 * The unwind entry of \p pc, read from its module's exception directory directly: RtlLookupFunctionEntry
 * takes the loader's lock, which the stopped thread may hold
 */
PRUNTIME_FUNCTION lookup(DWORD64 pc, DWORD64 *imageBase)
{
    MEMORY_BASIC_INFORMATION memory;
    if (!VirtualQuery(reinterpret_cast<void *>(pc), &memory, sizeof(memory)) || memory.Type != MEM_IMAGE) return nullptr;
    auto *base = static_cast<BYTE *>(memory.AllocationBase);
    auto *dos = reinterpret_cast<IMAGE_DOS_HEADER *>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
    auto *nt = reinterpret_cast<IMAGE_NT_HEADERS64 *>(base + dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY &directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    if (!directory.VirtualAddress || !directory.Size) return nullptr;
    auto *table = reinterpret_cast<PRUNTIME_FUNCTION>(base + directory.VirtualAddress);
    const DWORD rva = DWORD(pc - DWORD64(base));
    int low = 0;
    int high = int(directory.Size / sizeof(RUNTIME_FUNCTION)) - 1;
    while (low <= high) {
        const int middle = (low + high) / 2;
        if (rva < table[middle].BeginAddress) high = middle - 1;
        else if (rva >= table[middle].EndAddress) low = middle + 1;
        else {
            *imageBase = DWORD64(base);
            return &table[middle];
        }
    }
    return nullptr;
}
#endif

/// The stopped thread's return addresses, taking no locks and allocating nothing: it may hold the heap's
int walk(HANDLE thread, DWORD64 *frames, int most)
{
    int count = 0;
#if defined(_M_X64) || defined(__x86_64__)
    CONTEXT context = {};
    context.ContextFlags = CONTEXT_FULL;
    if (!GetThreadContext(thread, &context)) return 0;
    while (count < most && context.Rip) {
        if (context.Rsp < stackLow || context.Rsp + 8 > stackHigh || (context.Rsp & 7)) break;
        frames[count++] = context.Rip;
        DWORD64 imageBase = 0;
        PRUNTIME_FUNCTION function = lookup(context.Rip, &imageBase);
        if (function) {
            PVOID handlerData = nullptr;
            DWORD64 establisher = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, context.Rip, function, &context, &handlerData, &establisher, nullptr);
        } else {
            // a leaf function: the return address is on top of the stack
            context.Rip = *reinterpret_cast<DWORD64 *>(context.Rsp);
            context.Rsp += 8;
        }
    }
#else
    Q_UNUSED(thread);
    Q_UNUSED(frames);
    Q_UNUSED(most);
#endif
    return count;
}

/// The symbol helpers: Dr. Mingw's mgwhelp, which reads the DWARF of these MinGW builds, or Windows' own
struct Symbols {
    using Initialize = BOOL(WINAPI *)(HANDLE, PCSTR, BOOL);
    using SetOptions = DWORD(WINAPI *)(DWORD);
    using FromAddr = BOOL(WINAPI *)(HANDLE, DWORD64, PDWORD64, PSYMBOL_INFO);
    using LineFromAddr = BOOL(WINAPI *)(HANDLE, DWORD64, PDWORD, PIMAGEHLP_LINE64);

    Initialize initialize {nullptr};
    SetOptions setOptions {nullptr};
    FromAddr fromAddr {nullptr};
    LineFromAddr lineFromAddr {nullptr};

    bool load()
    {
        HMODULE module = LoadLibraryW(L"mgwhelp.dll");
        if (!module) module = LoadLibraryW(L"dbghelp.dll");
        if (!module) return false;
        initialize = reinterpret_cast<Initialize>(GetProcAddress(module, "SymInitialize"));
        setOptions = reinterpret_cast<SetOptions>(GetProcAddress(module, "SymSetOptions"));
        fromAddr = reinterpret_cast<FromAddr>(GetProcAddress(module, "SymFromAddr"));
        lineFromAddr = reinterpret_cast<LineFromAddr>(GetProcAddress(module, "SymGetLineFromAddr64"));
        return initialize && fromAddr;
    }
};

/// The main thread's call stack, one frame a line
QString stackOf(HANDLE thread)
{
    // copied while the thread is stopped, named once it runs again
    DWORD64 frames[64];
    if (SuspendThread(thread) == DWORD(-1)) return QStringLiteral("(couldn't stop the thread)\n");
    const int count = walk(thread, frames, 64);
    ResumeThread(thread);

    static Symbols symbols;
    static const bool loaded = symbols.load();
    const HANDLE process = GetCurrentProcess();
    static const bool initialized = [&]() {
        if (!loaded) return false;
        if (symbols.setOptions) symbols.setOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
        return symbols.initialize(process, nullptr, TRUE) != FALSE;
    }();

    QString out;
    alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 512] = {};
    auto *symbol = reinterpret_cast<SYMBOL_INFO *>(buffer);
    for (int i = 0; i < count; i++) {
        const DWORD64 address = frames[i];
        wchar_t moduleName[MAX_PATH] = L"";
        HMODULE module = nullptr;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(address), &module)) {
            GetModuleFileNameW(module, moduleName, MAX_PATH);
        }
        QString line = QStringLiteral("%1 %2!").arg(address, 16, 16, QLatin1Char('0')).arg(QFileInfo(QString::fromWCharArray(moduleName)).fileName());
        if (initialized) {
            symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
            symbol->MaxNameLen = 511;
            DWORD64 offset = 0;
            // a return address points after its call: look up the call itself
            const DWORD64 call = i ? address - 1 : address;
            if (symbols.fromAddr(process, call, &offset, symbol)) {
                line += QString::fromLatin1(symbol->Name) + QStringLiteral("+0x") + QString::number(offset, 16);
            }
            IMAGEHLP_LINE64 source = {};
            source.SizeOfStruct = sizeof(source);
            DWORD column = 0;
            if (symbols.lineFromAddr && symbols.lineFromAddr(process, call, &column, &source) && source.FileName) {
                line += QStringLiteral("  [%1 @ %2]").arg(QString::fromLatin1(source.FileName)).arg(source.LineNumber);
            }
        }
        out += line + QLatin1Char('\n');
    }
    if (!count) out = QStringLiteral("(no stack)\n");
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
    GetCurrentThreadStackLimits(&stackLow, &stackHigh);
    beat = now();
    // the beat stops with the event loop on the way out, which is no hang
    QObject::connect(qApp, &QCoreApplication::aboutToQuit, []() { quitting = true; });
    auto *timer = new QTimer(qApp);
    timer->setInterval(500);
    QObject::connect(timer, &QTimer::timeout, timer, []() { beat = now(); });
    timer->start();

    std::thread([main]() {
        qint64 reported = 0;
        while (!quitting) {
            Sleep(2000);
            if (quitting) break;
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
