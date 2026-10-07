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
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace recomp {

extern const FuncEntry kFuncTable[];
extern const unsigned kFuncTableSize;
extern const SymEntry kSymTable[];
extern const unsigned kSymTableSize;

u8* g_mem = nullptr;

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

void mmio_wr(u32 a, u32 v, int size) {
    if ((a & 0xFFFFFFE0) == 0xCC008000) {  // write-gather pipe -> GX FIFO
        hw::fifoWrite(v, size);
        return;
    }
    logMmio("write", a, v);
}

// ---------------------------------------------------------------------------
// Dispatch
// ---------------------------------------------------------------------------

GuestFn lookupFunc(u32 addr) {
    const FuncEntry* end = kFuncTable + kFuncTableSize;
    const FuncEntry* it =
        std::lower_bound(kFuncTable, end, addr, [](const FuncEntry& e, u32 a) { return e.addr < a; });
    return it != end && it->addr == addr ? it->fn : nullptr;
}

void Ctx::call(u32 addr) {
    if (GuestFn fn = lookupFunc(addr)) [[likely]] {
        fn(*this);
        return;
    }
    if (addr == 0) fatal("call to null (lr=%08X)", lr);
    fatal("call to %08X: no function starts there (lr=%08X)", addr, lr);
}

void Ctx::unimplemented(u32 pc, u32 insn) { fatal("unimplemented instruction %08X at %08X", insn, pc); }

u32 guestSymbol(const char* name) {
    static const auto* map = [] {
        auto* m = new std::unordered_map<std::string, u32>;
        for (unsigned i = 0; i < kSymTableSize; i++) m->emplace(kSymTable[i].name, kSymTable[i].addr);
        return m;
    }();
    auto it = map->find(name);
    if (it == map->end()) fatal("guest symbol %s not found", name);
    return it->second;
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
// Paired singles
// ---------------------------------------------------------------------------

namespace {
double dequant(u32 ea, int type, int scale, u32* size) {
    const double s = std::ldexp(1.0, -scale);
    switch (type) {
    case 4: *size = 1; return double(u8(rd8(ea))) * s;
    case 5: *size = 2; return double(u16(rd16(ea))) * s;
    case 6: *size = 1; return double(s8(rd8(ea))) * s;
    case 7: *size = 2; return double(s16(rd16(ea))) * s;
    default: *size = 4; return double(rdf32(ea));
    }
}

template <typename T>
u32 saturate(double v) {
    if (std::isnan(v)) return 0;
    v = std::clamp(v, double(std::numeric_limits<T>::min()), double(std::numeric_limits<T>::max()));
    return u32(T(v));
}

u32 quant(u32 ea, double v, int type, int scale) {
    const double q = v * std::ldexp(1.0, scale);
    switch (type) {
    case 4: wr8(ea, saturate<u8>(q)); return 1;
    case 5: wr16(ea, saturate<u16>(q)); return 2;
    case 6: wr8(ea, saturate<s8>(q)); return 1;
    case 7: wr16(ea, saturate<s16>(q)); return 2;
    default: wrf32(ea, float(v)); return 4;
    }
}

int sext6(u32 v) { return (v & 0x20) ? int(v) - 64 : int(v); }
}  // namespace

void Ctx::psqLoad(int d, u32 ea, int w, int i) {
    const u32 g = gqr[i];
    const int type = (g >> 16) & 7;
    const int scale = sext6((g >> 24) & 0x3F);
    u32 size;
    f[d] = dequant(ea, type, scale, &size);
    ps1[d] = w ? 1.0 : dequant(ea + size, type, scale, &size);
}

void Ctx::psqStore(int d, u32 ea, int w, int i) {
    const u32 g = gqr[i];
    const int type = g & 7;
    const int scale = sext6((g >> 8) & 0x3F);
    const u32 size = quant(ea, f[d], type, scale);
    if (!w) quant(ea + size, ps1[d], type, scale);
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
