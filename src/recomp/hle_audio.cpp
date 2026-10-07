// HLE: audio interface and DSP. Output is silent for now: DMA callbacks never
// fire and the DSP mailboxes stay empty.
#include "recomp/hle.h"

namespace recomp {
namespace {
u32 sDmaCallback = 0;
u32 sStreamCallback = 0;
}  // namespace

HLE(AIInit) {}
HLE(AIInitDMA) {}
HLE(AIStartDMA) {}
HLE(AIStopDMA) {}
HLE(AISetDSPSampleRate) {}
HLE(AIGetDSPSampleRate) { c.r[3] = 1; }  // 48 kHz
HLE(AIRegisterDMACallback) {
    const u32 old = sDmaCallback;
    sDmaCallback = c.r[3];
    c.r[3] = old;
}
HLE(AIRegisterStreamCallback) {
    const u32 old = sStreamCallback;
    sStreamCallback = c.r[3];
    c.r[3] = old;
}
HLE(AISetStreamSampleRate) {}
HLE(AISetStreamVolLeft) {}
HLE(AISetStreamVolRight) {}
HLE(AIResetStreamSampleCount) {}
HLE(AISetStreamPlayState) {}
HLE(AISetStreamTrigger) {}
HLE(AIGetStreamSampleCount) { c.r[3] = 0; }
HLE(AIGetStreamTrigger) { c.r[3] = 0; }

HLE(DSPInit) {}
HLE(DSPCheckMailToDSP) { c.r[3] = 0; }
HLE(DSPCheckMailFromDSP) { c.r[3] = 0; }
HLE(DSPReadMailFromDSP) { c.r[3] = 0; }
HLE(DSPSendMailToDSP) {}
HLE(DSPAssertInt) {}

}  // namespace recomp
