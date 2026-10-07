// HLE: memory card. Not emulated yet: every slot reports no card, which the
// game handles with its "no memory card" flow.
#include "recomp/hle.h"
#include "recomp/sched.h"

namespace recomp {
namespace {
constexpr u32 kNoCard = u32(-3);  // CARD_RESULT_NOCARD

void result(Ctx& c, u32 cbArgIndex) {
    const u32 cb = c.r[cbArgIndex];
    const u32 chan = c.r[3];
    if (cb) sched::postIrq([cb, chan](Ctx& ic) { guestCall(ic, cb, chan, kNoCard); });
    c.r[3] = kNoCard;
}
}  // namespace

HLE(CARDInit) {}
HLE(CARDProbe) { c.r[3] = 0; }
HLE(CARDProbeEx) { c.r[3] = kNoCard; }
HLE(CARDMount) { c.r[3] = kNoCard; }
HLE(CARDMountAsync) { result(c, 6); }       // (chan, workArea, detachCallback, attachCallback)
HLE(__CARDMountCallback) {}
HLE(CARDUnmount) { c.r[3] = kNoCard; }
HLE(CARDCheck) { c.r[3] = kNoCard; }
HLE(CARDCheckAsync) { result(c, 4); }
HLE(CARDFormat) { c.r[3] = kNoCard; }
HLE(CARDFreeBlocks) { c.r[3] = kNoCard; }
HLE(__CARDReadStatus) { c.r[3] = kNoCard; }
HLE(CARDCreate) { c.r[3] = kNoCard; }
HLE(CARDOpen) { c.r[3] = kNoCard; }
HLE(CARDClose) { c.r[3] = kNoCard; }
HLE(CARDRead) { c.r[3] = kNoCard; }
HLE(CARDWrite) { c.r[3] = kNoCard; }
HLE(CARDGetStatus) { c.r[3] = kNoCard; }
HLE(CARDSetStatus) { c.r[3] = kNoCard; }

}  // namespace recomp
