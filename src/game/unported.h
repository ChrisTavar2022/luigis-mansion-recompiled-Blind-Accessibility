#pragma once
// Ledger of retail functions the ported code calls but that are not ported yet.
//
// Every entry is named after its original address so it can be found in
// decomp/config/GLME01/symbols.txt and the disassembly. Each stub logs once the
// first time it runs and does the minimum needed for the caller to proceed (a
// return value that means "nothing to do" / "finished"). Porting a function =
// implementing it properly elsewhere and deleting its entry here.

#include <dolphin/types.h>

class JKRMemArchive;

namespace lm::unported {

// --- Audio (JAudio / JAI wrapper, 0x8018xxxx) ---
void fn_80188594_initAudioHeap(void* buf, u32 size, u32 aramSize);
void fn_80188714_setBootSound(void* pcm);
void fn_80188638_startAudioLoad();
bool fn_80188740_audioLoading();
void fn_8018865C_audioFrame();
void fn_80188710_audioFrameAlt();
void fn_801886D8_setAudioListener(void* camera, void* viewMtx);
void fn_8004D4C8_loadSoundTables();

// --- Memory card manager (lbl_803DAFA0) ---
void fn_801858E4_initCardManager(s32 priority);
void fn_80185910_probeCards();
u8 fn_80185938_cardManagerState();

// --- Disc error / reset handling ---
u8 fn_8018A8B4_resetState();
u8 fn_8018A860_resetReady();
void fn_8018A78C_beginReset(s32);
void fn_8018A8BC_setResetState(s32);
void fn_800473F8_setDiscErrorMessage(s16 status);
s32 fn_80005CF8_pollResetButton();

// --- Screen wipe (class at lbl_803A3668) ---
void fn_800395BC_wipeStart(void* wipe, s32 type, s16 frames);
void fn_80039628_wipeReverse(void* wipe);
bool fn_80039770_wipeUpdate(void* wipe);
void fn_80039C74_wipeDraw(void* wipe, void* texObj);

// --- Misc ---
void fn_80066FAC_setStaticArchive(JKRMemArchive* archive);
void fn_8005FD80_fillRandomTable();     // 0x2000-byte table from the game RNG (fn_80009638)
void fn_8003EB44_initSystemFont();      // JUTResFont from the DOL-embedded font + J2DPrint

}  // namespace lm::unported
