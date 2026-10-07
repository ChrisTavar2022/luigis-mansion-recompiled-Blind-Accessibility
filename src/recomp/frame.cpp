#include "recomp/frame.h"

#include "recomp/gx_bridge.h"
#include "recomp/hidden_mansion.h"
#include "recomp/hle.h"
#include "recomp/sched.h"

#include <condition_variable>
#include <mutex>
#include <thread>

namespace recomp {
u32 viRetraceCount();
}

namespace recomp::frame {
namespace {

enum class Turn { Host, Guest };
std::mutex sM;
std::condition_variable sCv;
Turn sTurn = Turn::Host;
bool sFinished = false;
bool sStopping = false;
std::thread sBoot;

}  // namespace

void start(Entry boot) {
    sBoot = std::thread([boot] {
        {
            std::unique_lock lk(sM);
            sCv.wait(lk, [] { return sTurn == Turn::Guest || sStopping; });
        }
        if (!sStopping) {
            try {
                boot();
            } catch (const sched::ThreadExit&) {
            }
        }
        std::lock_guard lk(sM);
        sFinished = true;
        sTurn = Turn::Host;
        sCv.notify_all();
    });
}

bool runUntilPresent() {
    std::unique_lock lk(sM);
    if (sFinished) return false;
    sTurn = Turn::Guest;
    sCv.notify_all();
    sCv.wait(lk, [] { return sTurn == Turn::Host; });
    return !sFinished;
}

void stop() {
    {
        std::lock_guard lk(sM);
        sStopping = true;
        sCv.notify_all();
    }
    sched::shutdown();
    if (sBoot.joinable()) sBoot.detach();
}

bool gamePaused() { return false; }

void present(Ctx&, u32, bool) {
    gx::endFrame();
    hm::onFrame();
    static u32 sPresents = 0;
    if (++sPresents % 30 == 0 || sPresents < 60) log("[frame] %u presents, %u retraces", sPresents, viRetraceCount());
    std::unique_lock lk(sM);
    sTurn = Turn::Host;
    sCv.notify_all();
    sCv.wait(lk, [] { return sTurn == Turn::Guest || sStopping; });
    if (sStopping) {
        lk.unlock();
        throw sched::ThreadExit{};
    }
}

}  // namespace recomp::frame
