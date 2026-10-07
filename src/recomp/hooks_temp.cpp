// Temporary hooks that unblock the game until the subsystem they stand in for
// is emulated. Each entry names what removes the need for it.
#include "recomp/hle.h"

namespace recomp {

// "Streamed BGM ready?" (JAudio stream status). Remove once streamed audio plays.
HLE(fn_80189054) { c.r[3] = 1; }

}  // namespace recomp
