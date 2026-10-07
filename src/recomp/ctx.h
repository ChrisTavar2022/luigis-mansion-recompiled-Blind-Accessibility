#pragma once
// Guest CPU state and memory access for code produced by tools/recomp/recomp.py.
//
// Guest memory: MEM1 (24 MiB, physical 0x00000000) is reachable through the
// cached (0x8...) and uncached (0xC...) mirrors. The 16 KiB locked cache at
// 0xE0000000 lives just past MEM1. Hardware registers (0xCC......) go through
// mmio_*; the write-gather pipe (0xCC008000) feeds Aurora's GX FIFO.
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace recomp {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s8 = std::int8_t;
using s16 = std::int16_t;
using s32 = std::int32_t;
using s64 = std::int64_t;

constexpr u32 kMem1Size = 0x01800000;
constexpr u32 kLockedCacheOffset = 0x01800000;  // host offset of the 0xE0000000 locked cache
constexpr u32 kHostMemSize = 0x02000000;

#if defined(_MSC_VER) && !defined(__clang__)
inline u32 bswap32(u32 v) { return _byteswap_ulong(v); }
inline u16 bswap16(u16 v) { return _byteswap_ushort(v); }
#else
inline u32 bswap32(u32 v) { return __builtin_bswap32(v); }
inline u16 bswap16(u16 v) { return __builtin_bswap16(v); }
#endif

extern u8* g_mem;
extern std::atomic<bool> g_irqPending;
extern bool g_traceOn;  // LM_TRACE: log/record guest function entries
extern bool g_watchOn;  // LM_WATCH: report guest stores to an address range
void watch_store(u32 a, u32 v, int size);

inline u8* hostp(u32 a) {
    return g_mem + ((a >> 28) == 0xE ? (kLockedCacheOffset | (a & 0x3FFF)) : (a & 0x01FFFFFF));
}
inline bool is_mmio(u32 a) { return (a >> 24) == 0xCC; }

u32 mmio_rd(u32 a, int size);
void mmio_wr(u32 a, u32 v, int size);

inline u32 rd32(u32 a) {
    if (is_mmio(a)) [[unlikely]] return mmio_rd(a, 4);
    u32 v;
    std::memcpy(&v, hostp(a), 4);
    return bswap32(v);
}
inline u32 rd16(u32 a) {
    if (is_mmio(a)) [[unlikely]] return mmio_rd(a, 2);
    u16 v;
    std::memcpy(&v, hostp(a), 2);
    return bswap16(v);
}
inline u32 rd8(u32 a) {
    if (is_mmio(a)) [[unlikely]] return mmio_rd(a, 1);
    return *hostp(a);
}
inline u32 rd32r(u32 a) {
    u32 v;
    std::memcpy(&v, hostp(a), 4);
    return v;
}
inline u32 rd16r(u32 a) {
    u16 v;
    std::memcpy(&v, hostp(a), 2);
    return v;
}
inline void wr32(u32 a, u32 v) {
    if (g_watchOn) [[unlikely]] watch_store(a, v, 4);
    if (is_mmio(a)) [[unlikely]] return mmio_wr(a, v, 4);
    v = bswap32(v);
    std::memcpy(hostp(a), &v, 4);
}
inline void wr16(u32 a, u32 v) {
    if (g_watchOn) [[unlikely]] watch_store(a, v, 2);
    if (is_mmio(a)) [[unlikely]] return mmio_wr(a, v & 0xFFFF, 2);
    const u16 s = bswap16(u16(v));
    std::memcpy(hostp(a), &s, 2);
}
inline void wr8(u32 a, u32 v) {
    if (g_watchOn) [[unlikely]] watch_store(a, v, 1);
    if (is_mmio(a)) [[unlikely]] return mmio_wr(a, v & 0xFF, 1);
    *hostp(a) = u8(v);
}
inline void wr32r(u32 a, u32 v) { std::memcpy(hostp(a), &v, 4); }
inline void wr16r(u32 a, u32 v) {
    const u16 s = u16(v);
    std::memcpy(hostp(a), &s, 2);
}
inline float rdf32(u32 a) { return std::bit_cast<float>(rd32(a)); }
inline double rdf64(u32 a) { return std::bit_cast<double>(u64(rd32(a)) << 32 | rd32(a + 4)); }
inline void wrf32(u32 a, float v) { wr32(a, std::bit_cast<u32>(v)); }
inline void wrf64(u32 a, double v) {
    const u64 b = std::bit_cast<u64>(v);
    wr32(a, u32(b >> 32));
    wr32(a + 4, u32(b));
}

struct GuestThread;
struct Ctx;
using GuestFn = void (*)(Ctx&);
struct FuncEntry {
    u32 addr;
    GuestFn fn;
};
struct SymEntry {
    const char* name;
    u32 addr;
};

void service_irq(Ctx& c);
void trace_enter(u32 addr, Ctx& c);
[[noreturn]] void fatal(const char* fmt, ...);

struct Ctx {
    u32 r[32]{};
    double f[32]{};
    double ps1[32]{};
    u8 cr[8]{};
    u32 lr = 0, ctr = 0;
    u32 ca = 0, so = 0, ov = 0;
    u32 fpscr = 0;
    u32 gqr[8]{};
    u32 msr = 0x8000;  // external interrupts enabled
    GuestThread* thread = nullptr;
    u32 lastFn = 0;  // last guest function entered (only maintained while tracing)
    u32 ring[32]{};  // recent function entries (tracing only)
    u32 ringPos = 0;

    // --- condition register ---
    bool crbit(int n) const { return (cr[n >> 2] >> (3 - (n & 3))) & 1; }
    void setCrbit(int n, bool v) {
        const u8 m = u8(1 << (3 - (n & 3)));
        cr[n >> 2] = v ? (cr[n >> 2] | m) : (cr[n >> 2] & ~m);
    }
    void cmps(int f, s32 a, s32 b) { cr[f] = u8((a < b ? 8 : a > b ? 4 : 2) | so); }
    void cmpu(int f, u32 a, u32 b) { cr[f] = u8((a < b ? 8 : a > b ? 4 : 2) | so); }
    void setCr0(s32 v) { cmps(0, v, 0); }
    void setCr1() { cr[1] = u8(fpscr >> 28); }
    void fcmp(int f, double a, double b, bool) {
        cr[f] = std::isnan(a) || std::isnan(b) ? 1 : a < b ? 8 : a > b ? 4 : 2;
        fpscr = (fpscr & ~0xF000u) | (u32(cr[f]) << 12);
    }
    u32 getCr() const {
        u32 v = 0;
        for (int i = 0; i < 8; i++) v |= u32(cr[i] & 0xF) << (28 - 4 * i);
        return v;
    }
    void mtcrf(u32 crm, u32 v) {
        for (int i = 0; i < 8; i++)
            if (crm & (0x80 >> i)) cr[i] = (v >> (28 - 4 * i)) & 0xF;
    }
    u32 getXer() const { return (so << 31) | (ov << 30) | (ca << 29); }
    void setXer(u32 v) {
        so = (v >> 31) & 1;
        ov = (v >> 30) & 1;
        ca = (v >> 29) & 1;
    }

    // --- integer helpers ---
    static u32 rotl(u32 v, u32 n) { return std::rotl(v, int(n & 31)); }
    static u32 cntlzw(u32 v) { return u32(std::countl_zero(v)); }
    static u32 divw(u32 a, u32 b) {
        const s32 x = s32(a), y = s32(b);
        if (y == 0 || (x == INT32_MIN && y == -1)) return x < 0 && y == 0 ? 0xFFFFFFFFu : 0;
        return u32(x / y);
    }
    static u32 divwu(u32 a, u32 b) { return b == 0 ? 0 : a / b; }
    void sraw(int dst, u32 s, u32 sh) {
        const s32 v = s32(s);
        if (sh >= 32) {
            r[dst] = v < 0 ? 0xFFFFFFFFu : 0;
            ca = v < 0;
            return;
        }
        r[dst] = u32(v >> sh);
        ca = v < 0 && sh != 0 && (u32(v) & ((1u << sh) - 1)) != 0;
    }

    // --- floating point ---
    void setSingle(int d, float v) {
        f[d] = v;
        ps1[d] = v;
    }
    static double fctiw(double v, int truncate) {
        s32 i;
        if (std::isnan(v)) i = INT32_MIN;
        else if (v >= 2147483647.0) i = INT32_MAX;
        else if (v <= -2147483648.0) i = INT32_MIN;
        else i = s32(truncate ? std::trunc(v) : std::nearbyint(v));
        return std::bit_cast<double>(0xFFF8000000000000ull | u32(i));
    }
    void psqLoad(int d, u32 ea, int w, int i);
    void psqStore(int d, u32 ea, int w, int i);

    // --- system ---
    u32 mfspr(u32 spr);
    void mtspr(u32 spr, u32 v);
    u32 mftb(u32 spr);
    static void dcbz(u32 ea) { std::memset(hostp(ea & ~31u), 0, 32); }

    void enter(u32 addr) {
        if (g_traceOn) [[unlikely]] trace_enter(addr, *this);
    }
    void poll() {
        if (g_irqPending.load(std::memory_order_relaxed) && (msr & 0x8000)) [[unlikely]]
            service_irq(*this);
    }
    void call(u32 addr);
    [[noreturn]] void unimplemented(u32 pc, u32 insn);
};

}  // namespace recomp
