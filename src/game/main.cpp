// Game-thread entry point. Currently a bring-up smoke test for the disc and
// JSystem paths; replaced by the ported boot sequence
// (main -> fn_80005760 init -> fn_8000B7F4 scene loop) as it lands.
#include <dolphin/gx.h>
#include <dolphin/os.h>
#include <dolphin/vi.h>

#include <JSystem/JKernel/JKRAram.h>
#include <JSystem/JKernel/JKRArchive.h>
#include <JSystem/JKernel/JKRExpHeap.h>

#include "port/dol.h"

namespace lm {

static void smoke_test_disc() {
    OSInit();  // the retail init (fn_80005760) also calls this first; sets up the heap arena
    JKRExpHeap* root = JKRExpHeap::createRoot(1, false);
    JKRExpHeap* game = JKRExpHeap::create(0x1000000, root, false);
    game->becomeSystemHeap();
    game->becomeCurrentHeap();

    // Same arguments as the retail init (fn_80005760). Besides ARAM, this starts the
    // ARAM-stream and decompression threads that compressed archive mounts rely on.
    JKRAram::create(0xF00000, 0xFFFFFFFF, 8, 8, 8);

    // game_usa.szp is a Yay0-compressed RARC holding the US build's core game resources.
    JKRArchive* arc = JKRArchive::mount("/Game/game_usa.szp", JKRArchive::MOUNT_MEM, game,
                                        JKRArchive::MOUNT_DIRECTION_HEAD);
    if (arc) {
        OSReport("[smoke] mounted /Game/game_usa.szp: %u files, heap free %u\n", arc->countFile(),
                 game->getFreeSize());
    } else {
        OSReport("[smoke] FAILED to mount /Game/game_usa.szp\n");
    }

    OSReport("[smoke] DOL version %d, word at 0x80003100 = %08x\n", int(port::dol::version()),
             port::dol::be32(0x80003100));
}

void game_main() {
    smoke_test_disc();

    static u8 sFakeXfb[1];
    const OSTime start = OSGetTime();
    for (u32 frame = 0;; ++frame) {
        if (frame % 90 == 0 && frame != 0) {
            const f64 secs = f64(OSGetTime() - start) / OS_TIMER_CLOCK;
            OSReport("[timing] tick %u at %.2fs (%.2f ticks/s)\n", frame, secs, frame / secs);
        }
        const u8 pulse = static_cast<u8>((frame * 4) & 0xFF);
        GXSetCopyClear(GXColor{0, pulse, 40, 255}, GX_MAX_Z24);
        VISetNextFrameBuffer(sFakeXfb);
        VIWaitForRetrace();
        VIWaitForRetrace();  // LM ticks at 30 Hz: two retraces per game frame
    }
}

}  // namespace lm
