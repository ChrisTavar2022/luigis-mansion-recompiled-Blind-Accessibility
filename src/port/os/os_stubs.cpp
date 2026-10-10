// Hardware/OS functions the game or JSystem reference that have no meaning on
// PC. Each returns the value the retail console would in normal operation.
#include <dolphin/gx.h>
#include <dolphin/os.h>

#include <JSystem/JKernel/JKRHeap.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>

extern "C" {

u32 __OSFpscrEnableBits;  // FP exception enables; host FP exceptions stay masked

OSErrorHandler OSSetErrorHandler(OSError, OSErrorHandler) { return nullptr; }

// Console reset / power-off (reset button combo, disc errors): exit the process.
void OSResetSystem(int reset, u32 resetCode, BOOL forceMenu) {
    std::fprintf(stderr, "OSResetSystem(%d, %u, %d): exiting\n", reset, resetCode, forceMenu);
    std::fflush(stderr);
    std::exit(0);
}

void OSSetProgressiveMode(u32) {}  // output mode is the host window's concern

// GXSetCopyClamp: Aurora (patch 0018: copy filter edge lines of texture copies).
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
