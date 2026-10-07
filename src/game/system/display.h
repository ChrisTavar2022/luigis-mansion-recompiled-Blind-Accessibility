#pragma once
// Video output, frame presentation and screen wipes (retail: Unsorted/LMDisplayUtil).
#include <dolphin/types.h>

namespace lm {

void initDisplay();                   // LMInitDisplay
void beginFrame();                    // fn_80007800
void presentFrame();                  // LMChangeFrameBuffer
void checkRenderMode();               // fn_800079B0
void defaultOrthoView();              // LMDefaultOrthoView
void setViewportJitter(f32 left, f32 top, f32 width, f32 height, f32 nearZ, f32 farZ);  // LMSetViewportJitter
void setDisplayFlags(u16 flags);      // fn_80007E50 (bit 0: video output enabled / not black)
void setRetraceInterval(s32 retraces);// fn_80007E60 (1 = 60 Hz, 2 = 30 Hz)
void setDoubleBuffered(bool enable);  // fn_80007E70
void* getMainFrameBuffer();           // LMGetMainFrameBuffer
void* getPrevMainFrameBuffer();       // LMGetPrevMainFrameBuffer
void checkProgressiveModeAtBoot();    // fn_80007F28
bool isProgressivePromptActive();     // lbl_804D8058 == 1
void updateProgressiveSelect();       // fn_80007F90: left/right toggles the choice
void updateProgressiveMode();         // LMUpdateProgressiveMode: apply the choice
u32 getRetraceInterval();             // LMGetVIMessageQueueUnk4

// --- Screen wipe between scenes (lbl_803A3810 state machine) ---
enum WipeState : s32 {
    WIPE_IDLE = 0,
    WIPE_OUT = 1,          // covering the old scene with the captured frame
    WIPE_HOLD = 2,
    WIPE_IN = 3,           // revealing the new scene
    WIPE_REQUEST_OUT = 4,  // capture current frame, then start WIPE_OUT
    WIPE_OUT_DONE = 8,
};
struct WipeControl {
    s32 state;
    s32 type;
    s32 frames;
};
extern WipeControl gWipe;  // lbl_803A3810

void wipeDraw(WipeControl* wipe);       // fn_800081A8
void wipeUpdate(WipeControl* wipe);     // fn_8000812C
void wipeAfterPresent(WipeControl* wipe);  // fn_800080DC
void requestWipeOut();                  // fn_80008394
void setWipeType(s32 type);             // fn_800083A4
void setWipeFrames(s32 frames);         // fn_800083B4

}  // namespace lm
