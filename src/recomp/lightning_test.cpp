// LM_LIGHTNING=<first>[,<period>[,<type>]] (testing, US game): forces lightning strikes so
// the lightning shadows (the shadow map rendered from the window, docs/RENDERING.md
// "Shadows") can be captured deterministically.
//
// The room update fn_800168EC starts a strike when the room's "Thunder" field (room info,
// read by fn_8002D7C8) is non-zero, no strike is running and fn_8000FC60() exceeds the
// threshold at lbl_804D9B30 (1.87). The strike (fn_80029250) points the flashlight shadow
// camera at the room's thunder light for 60 frames and sets the shadow strength to 150.
// This aid counts calls of fn_800168EC (one per gameplay frame) and, on call <first> and then
// every <period> calls, lowers the threshold for that call; with <type> (1..n) a room without
// thunder gets that thunder type. Each forced strike is logged with its call number.
#include "recomp/hle.h"
#include "recomp/runtime.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace recomp {
namespace {

constexpr u32 kThreshold = 0x804D9B30;  // f32, sdata2

struct Config {
    long first = -1, period = 0, type = 0;
};

const Config& config() {
    static const Config c = [] {
        Config r;
        if (const char* e = std::getenv("LM_LIGHTNING")) {
            std::sscanf(e, "%ld,%ld,%ld", &r.first, &r.period, &r.type);
        }
        return r;
    }();
    return c;
}

long sCalls = 0;
bool sForcing = false;

}  // namespace

HLE(fn_800168EC) {
    const Config& cfg = config();
    const long n = sCalls++;
    const bool strike = cfg.first >= 0 &&
                        (n == cfg.first || (cfg.period > 0 && n > cfg.first && (n - cfg.first) % cfg.period == 0));
    if (!strike) {
        callOriginal(c, "fn_800168EC");
        return;
    }
    const u32 saved = rd32(kThreshold);
    wr32(kThreshold, 0xFF800000u);  // -inf: any fn_8000FC60() result starts the strike
    sForcing = true;
    log("[lightning] forcing a strike at room-update call %ld", n);
    callOriginal(c, "fn_800168EC");
    sForcing = false;
    wr32(kThreshold, saved);
}

// Room info "Thunder" field: (room info, room number) -> thunder type, 0 = none.
HLE(fn_8002D7C8) {
    callOriginal(c, "fn_8002D7C8");
    if (sForcing && c.r[3] == 0 && config().type > 0) c.r[3] = u32(config().type);
}

}  // namespace recomp
