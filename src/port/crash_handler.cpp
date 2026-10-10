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
// First chance, before anything unwinds: stack overflows and faults in the game module DLL
// may never reach the unhandled-exception filter with enough stack to symbolize, so report
// the code, the faulting address and the module it is in with minimal stack use.
LONG WINAPI onVectored(EXCEPTION_POINTERS* info) {
    const DWORD code = info->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_STACK_OVERFLOW && code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION)
        return EXCEPTION_CONTINUE_SEARCH;
    static volatile LONG sOnce = 0;
    if (InterlockedExchange(&sOnce, 1)) return EXCEPTION_CONTINUE_SEARCH;
    char buf[256];
    HMODULE mod = nullptr;
    char name[128] = "?";
    const void* pc = info->ExceptionRecord->ExceptionAddress;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCSTR>(pc), &mod))
        GetModuleFileNameA(mod, name, sizeof(name));
    const int n = wsprintfA(buf, "\n=== exception 0x%08lX at %p (%s+0x%lX), rsp %p ===\n", code, pc, name,
                            static_cast<unsigned long>(static_cast<const char*>(pc) - reinterpret_cast<const char*>(mod)),
                            reinterpret_cast<void*>(info->ContextRecord->Rsp));
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), buf, DWORD(n), &written, nullptr);
    return EXCEPTION_CONTINUE_SEARCH;
}

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
    AddVectoredExceptionHandler(1, onVectored);
    // Route CRT abort() through SIGABRT instead of an immediate fast-fail.
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
}

}  // namespace port
