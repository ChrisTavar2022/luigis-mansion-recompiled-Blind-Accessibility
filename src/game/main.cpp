// Game-thread entry point. Temporary handoff test until the boot sequence
// (main -> fn_80005760 init -> fn_8000B7F4 scene loop) is ported.
#include <dolphin/gx.h>
#include <dolphin/os.h>
#include <dolphin/vi.h>

#include <chrono>

namespace lm {

void game_main() {
    static u8 sFakeXfb[1];
    auto start = std::chrono::steady_clock::now();
    for (u32 frame = 0;; ++frame) {
        if (frame % 90 == 0) {
            const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            OSReport("[handoff test] tick %u at %.2fs (%.1f ticks/s)\n", frame, secs, secs > 0 ? frame / secs : 0.0);
        }
        const u8 pulse = static_cast<u8>((frame * 4) & 0xFF);
        GXSetCopyClear(GXColor{0, pulse, 40, 255}, GX_MAX_Z24);
        VISetNextFrameBuffer(sFakeXfb);
        VIWaitForRetrace();
        VIWaitForRetrace();  // LM ticks at 30 Hz: two retraces per game frame
    }
}

}  // namespace lm
