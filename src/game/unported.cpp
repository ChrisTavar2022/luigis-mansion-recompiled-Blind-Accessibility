#include "game/unported.h"

#include <dolphin/os.h>

#include <atomic>

// Logs "<name> is not ported yet" the first time the enclosing stub runs.
#define UNPORTED()                                                                                 \
    do {                                                                                           \
        static std::atomic_bool sLogged{false};                                                    \
        if (!sLogged.exchange(true)) {                                                             \
            OSReport("[unported] %s\n", __func__);                                                 \
        }                                                                                          \
    } while (0)

namespace lm::unported {

void fn_80188594_initAudioHeap(void*, u32, u32) { UNPORTED(); }
void fn_80188714_setBootSound(void*) { UNPORTED(); }
void fn_80188638_startAudioLoad() { UNPORTED(); }
bool fn_80188740_audioLoading() { UNPORTED(); return false; }
void fn_8018865C_audioFrame() { UNPORTED(); }
void fn_80188710_audioFrameAlt() { UNPORTED(); }
void fn_801886D8_setAudioListener(void*, void*) { UNPORTED(); }
void fn_8004D4C8_loadSoundTables() { UNPORTED(); }

void fn_801858E4_initCardManager(s32) { UNPORTED(); }
void fn_80185910_probeCards() { UNPORTED(); }
u8 fn_80185938_cardManagerState() { UNPORTED(); return 0; }

u8 fn_8018A8B4_resetState() { UNPORTED(); return 0; }
u8 fn_8018A860_resetReady() { UNPORTED(); return 0; }
void fn_8018A78C_beginReset(s32) { UNPORTED(); }
void fn_8018A8BC_setResetState(s32) { UNPORTED(); }
void fn_800473F8_setDiscErrorMessage(s16) { UNPORTED(); }

void fn_800395BC_wipeStart(void*, s32, s16) { UNPORTED(); }
void fn_80039628_wipeReverse(void*) { UNPORTED(); }
bool fn_80039770_wipeUpdate(void*) { UNPORTED(); return true; }
void fn_80039C74_wipeDraw(void*, void*) { UNPORTED(); }

void fn_8018A2A0_playSystemSound(u32) { UNPORTED(); }
void fn_80188760_playStream(u32, u32) { UNPORTED(); }
void fn_80188D0C_stopStream(s32) { UNPORTED(); }
void fn_80047444_showProgressiveMessage(s32) { UNPORTED(); }
void setLuigiAppearPoint(u32) { UNPORTED(); }

void fn_80066FAC_setStaticArchive(JKRMemArchive*) { UNPORTED(); }
void fn_8005FD80_fillRandomTable() { UNPORTED(); }
void fn_8003EB44_initSystemFont() { UNPORTED(); }

}  // namespace lm::unported
