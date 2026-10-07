// Game-thread entry point: retail main() -> fn_80005760 (system init) ->
// fn_8000B7F4 (scene machine). Ported from auto_03_80005760.
#include <dolphin/dvd.h>
#include <dolphin/os.h>
#include <dolphin/pad.h>
#include <dolphin/vi.h>

#include <JSystem/JKernel/JKRAram.h>
#include <JSystem/JKernel/JKRExpHeap.h>

#include "game/scene/scene.h"
#include "game/system/display.h"
#include "game/system/dvd_file.h"
#include "game/system/gx_util.h"
#include "game/system/heap.h"
#include "game/system/pad.h"
#include "game/unported.h"

namespace lm {

// fn_80005760
static void initSystem() {
    OSInit();
    PADRecalibrate(0xF0000000);
    // (Retail programs the paired-single quantization registers GQR2-5 here.)
    initHeaps();
    initGamePad();
    DVDInit();
    unported::fn_801858E4_initCardManager(3);
    gGameHeap->changeGroupID(3);
    JKRAram::create(0xF00000, 0xFFFFFFFF, 8, 8, 8);
    gGameHeap->changeGroupID(0x1A);
    initDvdFiles();
    initializeGX();
    VIInit();
    initDisplay();
    // (Retail sets up JUTDirectPrint/JUTException crash screens here: fn_80005B98.)
    gGameHeap->becomeCurrentHeap();
    gGameHeap->changeGroupID(9);
    // TODO(port): JMANewSinTable(12) (fn_801CDD70) for LM-era JMath callers.
    useSceneHeap();

    // PORT: tables the retail build keeps in .data, loaded from the user's DOL.
    loadTrigTables();
}

void game_main() {
    initSystem();
    runScenes();
}

}  // namespace lm
