// HLE: controllers, via Aurora's PAD (SDL gamepads + keyboard bindings).
#include "recomp/hle.h"

#include <dolphin/pad.h>

namespace recomp {

HLE(PADInit) { c.r[3] = PADInit(); }
HLE(PADReset) { c.r[3] = PADReset(c.r[3]); }
HLE(PADRecalibrate) { c.r[3] = PADRecalibrate(c.r[3]); }
HLE(PADSetSpec) { PADSetSpec(c.r[3]); }
HLE(PADSetAnalogMode) { PADSetAnalogMode(c.r[3]); }
HLE(PADControlMotor) { PADControlMotor(c.r[3], c.r[4]); }

// PADStatus (12 bytes): button, stickX/Y, substickX/Y, triggerL/R, analogA/B, err.
HLE(PADRead) {
    PADStatus st[PAD_MAX_CONTROLLERS] = {};
    const u32 motor = PADRead(st);
    for (int i = 0; i < PAD_MAX_CONTROLLERS; i++) {
        const u32 p = c.r[3] + 12 * i;
        wr16(p, st[i].button);
        wr8(p + 2, u8(st[i].stickX));
        wr8(p + 3, u8(st[i].stickY));
        wr8(p + 4, u8(st[i].substickX));
        wr8(p + 5, u8(st[i].substickY));
        wr8(p + 6, st[i].triggerLeft);
        wr8(p + 7, st[i].triggerRight);
        wr8(p + 8, st[i].analogA);
        wr8(p + 9, st[i].analogB);
        wr8(p + 10, u8(st[i].err));
    }
    c.r[3] = motor;
}

}  // namespace recomp
