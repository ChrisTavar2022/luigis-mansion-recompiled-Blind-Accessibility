// Hardware/OS functions the game or JSystem reference that have no meaning on
// PC. Each returns the value the retail console would in normal operation.
#include <dolphin/gx.h>
#include <dolphin/os.h>

#include <JSystem/JKernel/JKRHeap.h>

#include <cstdarg>
#include <cstdio>

extern "C" {

u32 __OSFpscrEnableBits;  // FP exception enables; host FP exceptions stay masked

OSErrorHandler OSSetErrorHandler(OSError, OSErrorHandler) { return nullptr; }
BOOL OSGetResetSwitchState() { return FALSE; }

// Values for a 1:1 EFB-to-XFB copy (NTSC 640x480 render modes used by LM).
f32 GXGetYScaleFactor(u16 efbHeight, u16 xfbHeight) { return f32(xfbHeight) / f32(efbHeight); }
u16 GXGetNumXfbLines(u16 efbHeight, f32 yScale) { return u16(f32(efbHeight) * yScale); }

}  // extern "C"

void OSReport_Error(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    std::vfprintf(stderr, fmt, args);
    va_end(args);
    std::fflush(stderr);
}

// Debug heap dump (game builds this only for its debug menu).
bool JKRHeap::dump_sort() { return true; }
