// Entry point of the recompiled build: loads the DOL from the user's disc into
// guest memory, boots it on a guest thread, and runs the Aurora host loop.
#include <aurora/aurora.h>
#include <aurora/dvd.h>
#include <aurora/event.h>
#include <aurora/main.h>
#include <dolphin/dvd.h>

#include "port/crash_handler.h"
#include "port/dol.h"
#include "port/options.h"
#include "recomp/frame.h"
#include "recomp/hle.h"
#include "recomp/runtime.h"
#include "recomp/sched.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

namespace recomp {
namespace vi {
void startTimer();
void stopTimer();
}  // namespace vi

namespace {

constexpr u32 kInitRegisters = 0x800031F0;  // __init_registers: lis/ori pairs for r1, r2, r13

void loadDol(const u8* dol) {
    auto be = [&](u32 off) { return u32(dol[off]) << 24 | u32(dol[off + 1]) << 16 | u32(dol[off + 2]) << 8 | dol[off + 3]; };
    // .bss overlaps the small-data sections, so clear it before copying them in.
    std::memset(hostp(be(0xD8)), 0, be(0xDC));
    for (int i = 0; i < 18; i++) {
        const u32 off = be(i * 4), addr = be(0x48 + i * 4), size = be(0x90 + i * 4);
        if (size) std::memcpy(hostp(addr), dol + off, size);
    }
}

// Reads the `lis rD, hi; ori rD, rD, lo` pairs of __init_registers.
void initRegisters(Ctx& c) {
    for (u32 pc = kInitRegisters; pc < kInitRegisters + 0x18; pc += 8) {
        const u32 hi = rd32(pc), lo = rd32(pc + 4);
        const u32 reg = (hi >> 21) & 31;
        c.r[reg] = (hi << 16) | (lo & 0xFFFF);
    }
}

void boot() {
    Ctx init;
    initRegisters(init);
    const u32 stackTop = init.r[1];
    init.r[1] = stackTop - 8;
    wr32(init.r[1], 0);

    const u32 defaultThread = hleAlloc(0x310);
    GuestThread* t = sched::adoptBootThread(defaultThread, init);
    wr32(defaultThread + 0x304, stackTop);
    wr32(defaultThread + 0x308, stackTop - 0x10000);
    const u32 irqStack = hleAlloc(0x10000);
    sched::setIrqContext(init, irqStack + 0x10000 - 8);

    Ctx& c = sched::ctxOf(t);
    guestCall(c, guestSymbol("OSInit"));
    vi::startTimer();
    guestCall(c, guestSymbol("__init_user"));
    log("[boot] entering main");
    guestCall(c, guestSymbol("main"), 0, 0);
    log("[boot] main returned %d", s32(c.r[3]));
}

void logCallback(AuroraLogLevel level, const char* module, const char* message, unsigned int) {
    static const char* names[] = {"DEBUG", "INFO", "WARNING", "ERROR", "FATAL"};
    std::fprintf(level >= LOG_ERROR ? stderr : stdout, "[%s: %s] %s\n", names[level], module, message);
    std::fflush(stdout);
    if (level == LOG_FATAL) {
        std::fflush(stderr);
        std::abort();
    }
}

std::string findDisc() {
    if (!port::options().disc.empty()) return port::options().disc;
    for (auto dir = std::filesystem::current_path();; dir = dir.parent_path()) {
        const auto candidate = dir / "orig" / "GLME01.iso";
        if (std::filesystem::exists(candidate)) return candidate.string();
        if (dir == dir.parent_path()) return {};
    }
}

}  // namespace
}  // namespace recomp

int main(int argc, char* argv[]) {
    using namespace recomp;
    port::installCrashHandler();
    port::parseOptions(argc, argv);

    AuroraConfig config{};
    config.appName = "Luigi's Mansion PC";
    config.logCallback = &logCallback;
    aurora_initialize(argc, argv, &config);

    const std::string disc = findDisc();
    if (disc.empty() || !aurora_dvd_open(disc.c_str())) {
        std::fprintf(stderr, "Could not open a Luigi's Mansion disc image. Pass --disc <path to .iso/.rvz>.\n");
        aurora_shutdown();
        return 1;
    }
    if (port::dol::load() != port::dol::Version::GLME01) {
        std::fprintf(stderr, "This build was recompiled from Luigi's Mansion USA (GLME01); the disc does not match.\n");
        aurora_shutdown();
        return 1;
    }
    s32 dolSize = 0;
    const u8* dol = DVDGetDOLLocation(&dolSize);

    g_mem = static_cast<u8*>(std::calloc(1, kHostMemSize));
    loadDol(dol);
    initTimeBase();
    std::printf("Opened %s\n", disc.c_str());

    frame::start(&boot);

    bool exiting = false;
    bool paused = false;
    while (!exiting) {
        for (const AuroraEvent* ev = aurora_update(); ev && ev->type != AURORA_NONE; ++ev) {
            if (ev->type == AURORA_EXIT) exiting = true;
            else if (ev->type == AURORA_PAUSED) paused = true;
            else if (ev->type == AURORA_UNPAUSED) paused = false;
        }
        if (exiting || paused || !aurora_begin_frame()) continue;
        const bool running = frame::runUntilPresent();
        aurora_end_frame();
        if (!running) break;
    }

    vi::stopTimer();
    frame::stop();
    aurora_shutdown();
    std::_Exit(0);
}
