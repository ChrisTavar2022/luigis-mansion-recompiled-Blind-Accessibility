// Temporary hooks that unblock the game until the subsystem they stand in for
// is emulated. Each entry names what removes the need for it.
#include "recomp/hle.h"

namespace recomp {

// (none at the moment; the streamed-BGM poll fn_80189054 now runs for real since
// the DSP stand-in plays the stream's ring-buffer voices, see docs/AUDIO.md)

}  // namespace recomp
