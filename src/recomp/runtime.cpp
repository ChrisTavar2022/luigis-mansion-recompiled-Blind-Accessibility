#include "recomp/ctx.h"
#include "recomp/hle.h"
#include "recomp/runtime.h"
#include "recomp/sched.h"

#include <dolphin/gx.h>

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace recomp {

const GameModule* g_module = nullptr;
u8* g_mem = nullptr;
bool g_traceOn = false;

// Instruction patch switches (hooks.txt "patch <id> ..."); see setPatch().
u8 g_patchStore[kMaxPatches];
u8* g_patches = g_patchStore;
void setPatch(u32 id, bool on) {
    if (id < kMaxPatches) g_patchStore[id] = on ? 1 : 0;
}

// ---------------------------------------------------------------------------
// Tracing (LM_TRACE=all | ring | <hex addr>,<hex addr>...)
// ---------------------------------------------------------------------------

namespace {
enum class TraceMode { Off, Ring, Set, All };
TraceMode sTraceMode = TraceMode::Off;
std::unordered_set<u32> sTraceSet;
constexpr int kRingSize = 64;
thread_local u32 tRing[kRingSize];
thread_local u32 tRingLr[kRingSize];
thread_local unsigned tRingPos = 0;
}  // namespace

void initTrace() {
    const char* env = std::getenv("LM_TRACE");
    if (!env || !*env) return;
    const std::string v = env;
    if (v == "all") sTraceMode = TraceMode::All;
    else if (v == "ring") sTraceMode = TraceMode::Ring;
    else {
        sTraceMode = TraceMode::Set;
        size_t pos = 0;
        while (pos < v.size()) {
            const size_t comma = v.find(',', pos);
            sTraceSet.insert(u32(std::strtoul(v.substr(pos, comma - pos).c_str(), nullptr, 16)));
            if (comma == std::string::npos) break;
            pos = comma + 1;
        }
    }
    g_traceOn = true;
}

void trace_enter(u32 addr, Ctx& c) {
    c.lastFn = addr;
    c.ring[c.ringPos++ & 31] = addr;
    tRing[tRingPos % kRingSize] = addr;
    tRingLr[tRingPos % kRingSize] = c.lr;
    tRingPos++;
    if (sTraceMode == TraceMode::All || (sTraceMode == TraceMode::Set && sTraceSet.count(addr)))
        log("[trace] %08X lr %08X r3 %08X r4 %08X r5 %08X", addr, c.lr, c.r[3], c.r[4], c.r[5]);
}

// LM_WATCH=<hex addr>[,<hex len>]: report every guest store into the range, with
// the function that made it (needs LM_TRACE, e.g. LM_TRACE=ring, for the name).
bool g_watchOn = false;
namespace {
u32 sWatchLo = 0, sWatchHi = 0;
}  // namespace

void initWatch() {
    const char* env = std::getenv("LM_WATCH");
    if (!env || !*env) return;
    char* end = nullptr;
    sWatchLo = u32(std::strtoul(env, &end, 16));
    sWatchHi = sWatchLo + (end && *end == ',' ? u32(std::strtoul(end + 1, nullptr, 16)) : 4);
    g_watchOn = true;
}

void watch_store(u32 a, u32 v, int size) {
    const u32 phys = a & 0x01FFFFFF;
    if (phys + size <= (sWatchLo & 0x01FFFFFF) || phys >= (sWatchHi & 0x01FFFFFF)) return;
    const u32 fn = tRingPos ? tRing[(tRingPos - 1) % kRingSize] : 0;
    log("[watch] store%d %08X = %08X in %08X", size * 8, a, v, fn);
}

void watchRange(u32 a, u32 len, const char* what) {
    if (!g_watchOn) return;
    const u32 lo = a & 0x01FFFFFF, hi = lo + len;
    if (hi <= (sWatchLo & 0x01FFFFFF) || lo >= (sWatchHi & 0x01FFFFFF)) return;
    log("[watch] %s writes %08X+%X", what, a, len);
}

void dumpTraceRing() {
    if (!g_traceOn) return;
    std::fprintf(stderr, "last guest calls on this thread (oldest first):\n");
    const unsigned n = tRingPos < kRingSize ? tRingPos : kRingSize;
    for (unsigned i = tRingPos - n; i < tRingPos; i++)
        std::fprintf(stderr, "  %08X (from %08X)\n", tRing[i % kRingSize], tRingLr[i % kRingSize]);
}

void log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stdout, fmt, ap);
    va_end(ap);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

void fatal(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::fprintf(stderr, "[recomp FATAL] ");
    std::vfprintf(stderr, fmt, ap);
    va_end(ap);
    std::fputc('\n', stderr);
    dumpTraceRing();
    std::fflush(stderr);
    std::abort();
}

// ---------------------------------------------------------------------------
// Hardware registers
// ---------------------------------------------------------------------------

namespace {
std::mutex sMmioLogM;
std::unordered_set<u32> sMmioLogged;

void logMmio(const char* what, u32 a, u32 v) {
    std::lock_guard lk(sMmioLogM);
    if (sMmioLogged.insert(a).second) log("[mmio] unhandled %s %08X (%08X)", what, a, v);
}
}  // namespace

u32 mmio_rd(u32 a, int size) {
    switch (a & 0xFFFFF000) {
    case 0xCC002000:  // VI
        return hw::viRead(a, size);
    default:
        logMmio("read", a, 0);
        return 0;
    }
}

namespace hw {
u32 (*fifoFilter)(u32 v, int size) = nullptr;
}

void mmio_wr(u32 a, u32 v, int size) {
    if ((a & 0xFFFFFFE0) == 0xCC008000) {  // write-gather pipe -> GX FIFO
        if (hw::fifoFilter) [[unlikely]] v = hw::fifoFilter(v, size);
        hw::fifoWrite(v, size);
        return;
    }
    logMmio("write", a, v);
}

// ---------------------------------------------------------------------------
// Dispatch
// ---------------------------------------------------------------------------

GuestFn lookupFunc(u32 addr) {
    const FuncEntry* end = g_module->funcs + g_module->funcCount;
    const FuncEntry* it =
        std::lower_bound(g_module->funcs, end, addr, [](const FuncEntry& e, u32 a) { return e.addr < a; });
    return it != end && it->addr == addr ? it->fn : nullptr;
}

void Ctx::call(u32 addr) {
    if (GuestFn fn = lookupFunc(addr)) [[likely]] {
        fn(*this);
        return;
    }
    callMissing(*this, addr);
}

void callMissing(Ctx& c, u32 addr) {
    if (addr == 0) {
        // Usually a virtual call: show the object and its vtable.
        const u32 obj = c.r[3];
        const u32 vt = (obj >> 24) == 0x80 || (obj >> 24) == 0x81 ? rd32(obj) : 0;
        fatal("call to null (lr=%08X) r3=%08X vtable=%08X r12=%08X", c.lr, obj, vt, c.r[12]);
    }
    fatal("call to %08X: no function starts there (lr=%08X)", addr, c.lr);
}

void Ctx::unimplemented(u32 pc, u32 insn) { fatal("unimplemented instruction %08X at %08X", insn, pc); }

u32 guestSymbol(const char* name) {
    static const auto* map = [] {
        auto* m = new std::unordered_map<std::string, u32>;
        for (u32 i = 0; i < g_module->symCount; i++) m->emplace(g_module->syms[i].name, g_module->syms[i].addr);
        return m;
    }();
    auto it = map->find(name);
    if (it == map->end()) fatal("guest symbol %s not found", name);
    return it->second;
}

void callOriginal(Ctx& c, const char* name) {
    const u32 addr = guestSymbol(name);
    for (u32 i = 0; i < g_module->origCount; i++) {
        if (g_module->orig[i].addr == addr) {
            g_module->orig[i].fn(c);
            return;
        }
    }
    fatal("callOriginal: %s is not listed in tools/recomp/hooks.txt", name);
}

u32 guestCall(Ctx& c, u32 fn, u32 a0, u32 a1, u32 a2, u32 a3) {
    const u32 savedLr = c.lr;
    c.r[3] = a0;
    c.r[4] = a1;
    c.r[5] = a2;
    c.r[6] = a3;
    c.call(fn);
    c.lr = savedLr;
    return c.r[3];
}

// ---------------------------------------------------------------------------
// HLE registry
// ---------------------------------------------------------------------------

namespace {
std::unordered_map<std::string, HleFn>& registry() {
    static std::unordered_map<std::string, HleFn> r;
    return r;
}
}  // namespace

HleReg::HleReg(const char* name, HleFn fn) { registry()[name] = fn; }

HleFn hleLookup(const char* name) {
    auto it = registry().find(name);
    return it == registry().end() ? nullptr : it->second;
}

void hleMissing(const char* name, u32 addr, Ctx& c) {
    fatal("SDK function %s (%08X) has no HLE implementation (lr=%08X)", name, addr, c.lr);
}

namespace {
u32 sHleHeap = 0x81700000;
std::mutex sHleHeapM;
}  // namespace

u32 hleAlloc(u32 size, u32 align) {
    std::lock_guard lk(sHleHeapM);
    sHleHeap = (sHleHeap + align - 1) & ~(align - 1);
    const u32 p = sHleHeap;
    sHleHeap += size;
    if (sHleHeap > 0x81800000) fatal("HLE guest heap exhausted");
    std::memset(hostp(p), 0, size);
    return p;
}

// ---------------------------------------------------------------------------
// Special registers and time base
// ---------------------------------------------------------------------------

namespace {
u32 sSpr[1024];
const auto sBootClock = std::chrono::steady_clock::now();
u64 sTbBias = 0;
}  // namespace

void initTimeBase() {
    // The console's clock counts from 2000-01-01 local time.
    const std::time_t now = std::time(nullptr);
    std::tm g{};
#ifdef _WIN32
    gmtime_s(&g, &now);
#else
    gmtime_r(&now, &g);
#endif
    g.tm_isdst = -1;
    const s64 tz = s64(now) - s64(std::mktime(&g));
    const s64 secs = s64(now) + tz - 946684800;
    sTbBias = u64(double(secs) * kTimerClock);
}

u64 timeBase() {
    const auto ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - sBootClock).count();
    return sTbBias + u64(double(ns) * (kTimerClock / 1e9));
}

void setTimeBaseBias(u64 bias) { sTbBias = bias; }

u32 Ctx::mfspr(u32 spr) { return sSpr[spr & 1023]; }
void Ctx::mtspr(u32 spr, u32 v) { sSpr[spr & 1023] = v; }
u32 Ctx::mftb(u32 spr) {
    const u64 tb = timeBase();
    return spr == 269 ? u32(tb >> 32) : u32(tb);
}

}  // namespace recomp

// Host-side OSReport for shared port code (e.g. port/dol.cpp).
extern "C" void OSReport(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stdout, fmt, ap);
    va_end(ap);
    std::fflush(stdout);
}
