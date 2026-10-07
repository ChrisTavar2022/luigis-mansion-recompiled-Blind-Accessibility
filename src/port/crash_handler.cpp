// Prints a symbolized stack trace on abort() / std::terminate / unhandled SEH
// exceptions, so crashes in game or library code can be located from a log.
#include "port/crash_handler.h"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <exception>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dbghelp.h>
#endif

namespace port {
namespace {

void printStack(const char* reason) {
    std::fprintf(stderr, "\n=== crash: %s ===\n", reason);
#ifdef _WIN32
    HANDLE process = GetCurrentProcess();
    static bool sSymInit = SymInitialize(process, nullptr, TRUE);
    (void)sSymInit;
    void* frames[64];
    const USHORT count = CaptureStackBackTrace(0, 64, frames, nullptr);
    alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 256];
    auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
    for (USHORT i = 0; i < count; i++) {
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = 255;
        DWORD64 displacement = 0;
        IMAGEHLP_LINE64 line{};
        line.SizeOfStruct = sizeof(line);
        DWORD lineDisp = 0;
        const DWORD64 addr = reinterpret_cast<DWORD64>(frames[i]);
        const bool haveSym = SymFromAddr(process, addr, &displacement, symbol);
        const bool haveLine = SymGetLineFromAddr64(process, addr, &lineDisp, &line);
        std::fprintf(stderr, "  #%02u %s+0x%llx", i, haveSym ? symbol->Name : "?",
                     static_cast<unsigned long long>(displacement));
        if (haveLine) {
            std::fprintf(stderr, " (%s:%lu)", line.FileName, line.LineNumber);
        }
        std::fputc('\n', stderr);
    }
#endif
    std::fflush(stderr);
}

void onAbort(int) {
    printStack("abort()");
    std::_Exit(3);
}

#ifdef _WIN32
LONG WINAPI onException(EXCEPTION_POINTERS* info) {
    char reason[64];
    std::snprintf(reason, sizeof(reason), "exception 0x%08lX", info->ExceptionRecord->ExceptionCode);
    printStack(reason);
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

}  // namespace

void installCrashHandler() {
    std::signal(SIGABRT, onAbort);
    std::set_terminate([] {
        printStack("std::terminate");
        std::_Exit(3);
    });
#ifdef _WIN32
    SetUnhandledExceptionFilter(onException);
    // Route CRT abort() through SIGABRT instead of an immediate fast-fail.
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
}

}  // namespace port
