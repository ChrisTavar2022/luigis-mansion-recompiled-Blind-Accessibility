// Debug output from game/library code, routed to the host console.
#include <dolphin/os.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>

extern "C" {

void OSVReport(const char* fmt, va_list args) {
    std::vfprintf(stdout, fmt, args);
    std::fflush(stdout);
}

void OSReport(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    OSVReport(fmt, args);
    va_end(args);
}

void OSPanic(const char* file, int line, const char* fmt, ...) {
    std::fprintf(stderr, "PANIC at %s:%d: ", file, line);
    va_list args;
    va_start(args, fmt);
    std::vfprintf(stderr, fmt, args);
    va_end(args);
    std::fputc('\n', stderr);
    std::fflush(stderr);
    std::abort();
}

}  // extern "C"
