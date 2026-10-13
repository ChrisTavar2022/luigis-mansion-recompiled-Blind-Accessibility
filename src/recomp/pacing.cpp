#include "recomp/pacing.h"

#include "port/settings.h"
#include "recomp/frame.h"
#include "recomp/hle.h"
#include "recomp/interp.h"

#include <aurora/aurora.h>
#include <aurora/present_timing.h>
#include <SDL3/SDL_properties.h>
#include <SDL3/SDL_timer.h>
#include <SDL3/SDL_video.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace recomp::pacing {
namespace {

double secs(Clock::duration d) { return std::chrono::duration<double>(d).count(); }
Clock::duration dur(double s) { return std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(s)); }

// --- vblank monitor (Windows: D3DKMTWaitForVerticalBlankEvent on the window's display) -----------
// Gives the display's refresh times: the fixed-refresh schedule starts frames at vblanks, and the
// spacing of refreshes tells a variable refresh rate display (refreshes follow the presents) from a
// fixed one (refreshes are multiples of the period).
struct Vblank {
    std::mutex m;
    std::condition_variable cv;
    u64 count = 0;
    Clock::time_point last{};
    std::deque<double> intervals;  // ms, newest last
    std::atomic<bool> running{false};
    std::thread th;
    std::wstring device;          // requested by the host thread
    std::atomic<bool> deviceChanged{false};
};
Vblank sVb;

#ifdef _WIN32
struct KmtOpen {
    WCHAR DeviceName[32];
    UINT hAdapter;
    LUID AdapterLuid;
    UINT VidPnSourceId;
};
struct KmtWait {
    UINT hAdapter;
    UINT hDevice;
    UINT VidPnSourceId;
};
struct KmtClose {
    UINT hAdapter;
};
using OpenFn = LONG(APIENTRY*)(KmtOpen*);
using WaitFn = LONG(APIENTRY*)(const KmtWait*);
using CloseFn = LONG(APIENTRY*)(const KmtClose*);

void vblankThread() {
    HMODULE gdi = GetModuleHandleW(L"gdi32.dll");
    if (!gdi) gdi = LoadLibraryW(L"gdi32.dll");
    const auto open = gdi ? reinterpret_cast<OpenFn>(GetProcAddress(gdi, "D3DKMTOpenAdapterFromGdiDisplayName")) : nullptr;
    const auto wait = gdi ? reinterpret_cast<WaitFn>(GetProcAddress(gdi, "D3DKMTWaitForVerticalBlankEvent")) : nullptr;
    const auto close = gdi ? reinterpret_cast<CloseFn>(GetProcAddress(gdi, "D3DKMTCloseAdapter")) : nullptr;
    if (!open || !wait) return;
    KmtWait w{};
    bool opened = false;
    while (sVb.running) {
        if (!opened || sVb.deviceChanged.exchange(false)) {
            if (opened && close) {
                KmtClose c{w.hAdapter};
                close(&c);
            }
            opened = false;
            KmtOpen o{};
            {
                std::lock_guard lk(sVb.m);
                wcsncpy_s(o.DeviceName, sVb.device.c_str(), 31);
                sVb.intervals.clear();
                sVb.last = {};
            }
            if (o.DeviceName[0] && open(&o) == 0) {
                w = {o.hAdapter, 0, o.VidPnSourceId};
                opened = true;
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                continue;
            }
        }
        if (wait(&w) != 0) {
            opened = false;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }
        const auto now = Clock::now();
        {
            std::lock_guard lk(sVb.m);
            if (sVb.last != Clock::time_point{}) {
                sVb.intervals.push_back(secs(now - sVb.last) * 1000.0);
                if (sVb.intervals.size() > 360) sVb.intervals.pop_front();
            }
            sVb.last = now;
            sVb.count++;
        }
        sVb.cv.notify_all();
    }
    if (opened && close) {
        KmtClose c{w.hAdapter};
        close(&c);
    }
}

// The GDI name (\\.\DISPLAYn) of the display the window is on.
std::wstring windowDevice() {
    SDL_Window* win = aurora_get_window();
    if (!win) return {};
    HWND hwnd = static_cast<HWND>(SDL_GetPointerProperty(SDL_GetWindowProperties(win), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
    if (!hwnd) return {};
    HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (!mon || !GetMonitorInfoW(mon, &mi)) return {};
    return mi.szDevice;
}
#endif

// Called on the host thread now and then: (re)starts the monitor on the window's display.
void vblankFollowWindow() {
#ifdef _WIN32
    static const bool sOff = std::getenv("LM_NO_VBLANK") != nullptr;
    if (sOff) return;
    static Clock::time_point checked{};
    const auto now = Clock::now();
    if (checked != Clock::time_point{} && now - checked < std::chrono::milliseconds(500)) return;
    checked = now;
    const std::wstring dev = windowDevice();
    {
        std::lock_guard lk(sVb.m);
        if (dev == sVb.device) return;
        sVb.device = dev;
    }
    sVb.deviceChanged = true;
    if (!sVb.running.exchange(true)) {
        sVb.th = std::thread(vblankThread);
        sVb.th.detach();
    }
#endif
}

bool vblankAvailable() {
    std::lock_guard lk(sVb.m);
    return sVb.count > 0 && Clock::now() - sVb.last < std::chrono::milliseconds(200);
}

// --- frame cost --------------------------------------------------------------------------------
struct FrameRec {
    u64 frame = 0;
    Clock::time_point begun{}, ended{};  // after aurora_begin_frame / after aurora_end_frame (host)
    double gpuDone = 0;                   // s since epoch (steady clock), 0 = not yet
};
std::deque<FrameRec> sFrames;     // recent frames, oldest first
std::deque<double> sCost;         // per frame: the time it occupied the busiest stage (s)
double sLastDone = 0;

double nsToSec(int64_t ns) { return double(ns) * 1e-9; }
double tpSec(Clock::time_point t) { return secs(t.time_since_epoch()); }

void pollTimings() {
    AuroraPresentTiming t[32];
    for (size_t n; (n = aurora_present_timings(t, 32)) > 0;) {
        for (size_t i = 0; i < n; i++) {
            interp::logPresentTiming(t[i].frame, t[i].submitNs, t[i].presentNs, t[i].doneNs);
            if (!t[i].doneNs) continue;
            const double done = nsToSec(t[i].doneNs);
            for (FrameRec& r : sFrames) {
                if (r.frame != t[i].frame) continue;
                r.gpuDone = done;
                // The GPU side (render worker + GPU) can start the frame once the host handed it
                // over (end_frame) and the previous frame is finished.
                const double start = std::max(tpSec(r.ended), sLastDone);
                const double gpu = done - start;
                const double cpu = secs(r.ended - r.begun);
                sCost.push_back(std::max(gpu, cpu));
                if (sCost.size() > 45) sCost.pop_front();
                break;
            }
            sLastDone = std::max(sLastDone, done);
        }
    }
}

double costP90() {
    if (sCost.size() < 8) return 0.0;
    std::vector<double> v(sCost.begin(), sCost.end());
    const size_t k = std::min(v.size() - 1, size_t(double(v.size()) * 0.9));
    std::nth_element(v.begin(), v.begin() + k, v.end());
    return v[k];
}

// --- schedule ------------------------------------------------------------------------------------
enum class Mode { Off, Fifo, Vblank, Grid };
Mode sMode = Mode::Off;
Clock::time_point sNext{};       // scheduled start of the next frame (Grid)
Clock::time_point sFrameTime{};  // this frame's time
double sPeriod = 0;              // current frame period (s)
u32 sDivisor = 1;                // Vblank: refreshes per frame
u64 sStartVblank = 0;            // Vblank: refresh count at the previous frame's start
u32 sCalmFrames = 0;             // frames with the cost well below the period (to speed up again)
bool sVrrSeen = false;
float sVrrHz = 0;
char sStatus[128] = "";
std::deque<Clock::time_point> sStarts;
bool sPacing = false;

bool detectVrr(float hz) {
    if (hz != sVrrHz) {
        sVrrSeen = false;
        sVrrHz = hz;
    }
    if (sVrrSeen || hz <= 0) return sVrrSeen;
    std::lock_guard lk(sVb.m);
    if (sVb.intervals.size() < 120) return false;
    const double period = 1000.0 / hz;
    u32 odd = 0, n = 0;
    for (double d : sVb.intervals) {
        if (d > 60.0) continue;
        n++;
        const double k = std::max(1.0, std::round(d / period));
        if (std::fabs(d - k * period) > 0.8) odd++;
    }
    // Refreshes that are not multiples of the refresh period: the display follows the presents.
    if (n >= 100 && odd * 10 > n) {
        sVrrSeen = true;
        log("[pacing] variable refresh rate detected (%u of %u refresh intervals off the %.2f Hz grid)", odd, n, hz);
    }
    return sVrrSeen;
}

}  // namespace

bool vrrActive() {
    const int s = port::settings().video.vrr;
    return s == 1 || (s == 0 && sVrrSeen);
}

void beforeFrame() {
    pollTimings();
    vblankFollowWindow();
    const auto now = Clock::now();
    const Clock::time_point prevFrameTime = sFrameTime;
    sFrameTime = now;
    sPacing = frame::interpolating();
    if (!sPacing) {
        sMode = Mode::Off;
        sNext = now;
        sStatus[0] = 0;
        return;
    }
    const auto& v = port::settings().video;
    const float hz = frame::displayRefreshHz();
    const int fps = frame::targetFps();
    detectVrr(hz);
    if (v.unlimitedFps) {
        // Unlimited: no schedule, no cap and no wait for the GPU; each frame starts as soon as the
        // previous one is handed over (only the swap chain's VSync, if on, still limits it).
        sMode = Mode::Off;
        sNext = now;
        sStarts.push_back(now);
        while (sStarts.size() > 1 && sStarts.back() - sStarts.front() > std::chrono::seconds(1)) sStarts.pop_front();
        std::snprintf(sStatus, sizeof(sStatus), "%.0f fps (unlimited%s)", presentedFps(), v.vsync ? ", VSync on" : "");
        return;
    }
    const bool vrr = vrrActive();
    const double need = costP90() * 1.08 + 0.0003;  // what one frame takes, with a margin
    const double minPeriod = 1.0 / double(fps);
    Mode mode;
    double period;
    if (!v.vsync || hz <= 0) {
        mode = Mode::Grid;
        period = std::max(minPeriod, need);
    } else if (vrr) {
        // Inside the VRR range: a little below the refresh rate (at the ceiling the display falls
        // back to fixed refresh timing and the swap chain queue adds latency).
        const double cap = 1.0 / std::max(30.0, double(hz) - std::max(3.0, double(hz) * 0.03));
        mode = Mode::Grid;
        period = std::max({minPeriod, cap, need});
    } else {
        // Fixed refresh: every n-th refresh, never an uneven rate.
        const double refresh = 1.0 / double(hz);
        u32 n = u32(std::max(1.0, std::ceil(std::max(minPeriod, need) / refresh - 0.02)));
        // LM_PACING_DIVISOR=n (test aid): at least every n-th refresh.
        static const u32 sMinDiv = [] { const char* e = std::getenv("LM_PACING_DIVISOR"); return e ? u32(std::max(1, std::atoi(e))) : 1u; }();
        n = std::min<u32>(std::max(n, sMinDiv), 4);
        if (n < sDivisor) {  // speed up only after a while with room to spare
            if (need < double(n) * refresh * 0.92) sCalmFrames++;
            else sCalmFrames = 0;
            if (sCalmFrames < u32(2.0 * hz / sDivisor)) n = sDivisor;
            else sCalmFrames = 0;
        } else {
            sCalmFrames = 0;
        }
        sDivisor = n;
        period = double(n) * refresh;
        mode = n == 1 ? Mode::Fifo : vblankAvailable() ? Mode::Vblank : Mode::Grid;
    }
    if (mode == Mode::Grid) {
        // Speed up gently (2% per frame toward the target), slow down at once.
        if (sMode != Mode::Grid || period > sPeriod) sPeriod = period;
        else sPeriod += (period - sPeriod) * 0.02;
        if (sMode != Mode::Grid) sNext = now;
        sNext += dur(sPeriod);
        if (sNext < now - dur(sPeriod * 0.5)) sNext = now;  // fell behind (a load, a hitch): start over
        if (sNext > now) SDL_DelayPrecise(u64(std::chrono::duration_cast<std::chrono::nanoseconds>(sNext - now).count()));
        sFrameTime = sNext;
    } else if (mode == Mode::Vblank) {
        sPeriod = period;
        std::unique_lock lk(sVb.m);
        if (sMode != Mode::Vblank || sVb.count < sStartVblank || sVb.count > sStartVblank + sDivisor + 2)
            sStartVblank = sVb.count;  // (re)start: from the next refresh
        const u64 target = sStartVblank + sDivisor;
        // At most a period (+1 ms) after the previous start: a display that refreshes only when
        // a frame arrives (VRR active although set Off) would otherwise hold every frame until
        // its lowest refresh rate; then the start is the plain grid time.
        const auto deadline = (sMode == Mode::Vblank ? prevFrameTime : now) + dur(period + 0.001);
        const bool hit = sVb.cv.wait_until(lk, deadline, [&] { return sVb.count >= target; });
        sStartVblank = sVb.count;
        sFrameTime = hit ? sVb.last : deadline - dur(0.001);
    } else {  // Fifo: the swap chain paces at the refresh rate
        sPeriod = period;
        sFrameTime = now;
    }
    sMode = mode;
    // The GPU at most one frame behind: the time from a frame's start to its display stays constant.
    const u64 begun = aurora_frame_counter();
    if (mode != Mode::Fifo && begun > 1) aurora_wait_frame_done(begun - 1, 50'000'000);
    sStarts.push_back(Clock::now());
    while (sStarts.size() > 1 && sStarts.back() - sStarts.front() > std::chrono::seconds(1)) sStarts.pop_front();
    const float shown = presentedFps();
    char why[64] = "";
    if (need > minPeriod * 1.02)
        std::snprintf(why, sizeof(why), " (frame cost limits: %.1f ms > %.1f ms)", need * 1000.0, minPeriod * 1000.0);
    if (mode == Mode::Fifo)
        std::snprintf(sStatus, sizeof(sStatus), "%.0f fps (%.0f Hz)", shown, hz);
    else if (mode == Mode::Vblank)
        std::snprintf(sStatus, sizeof(sStatus), "%.0f fps = %.0f Hz / %u%s", shown, hz, sDivisor, why);
    else if (vrr && v.vsync)
        std::snprintf(sStatus, sizeof(sStatus), "%.0f fps (variable refresh)%s", shown, why);
    else
        std::snprintf(sStatus, sizeof(sStatus), "%.0f fps%s", shown, why);
}

void afterFrame() {
    if (!sPacing) return;
    FrameRec r;
    r.frame = aurora_frame_counter();
    r.begun = sFrameTime;
    r.ended = Clock::now();
    // (begun: the frame's start after any scheduling wait; the CPU part is begin..end)
    if (!sStarts.empty()) r.begun = sStarts.back();
    sFrames.push_back(r);
    while (sFrames.size() > 16) sFrames.pop_front();
    // Present timing records the GPU completion of every frame (one fence wait per frame on a
    // helper thread, Aurora patch 0197).
    static bool enabled = false;
    if (!enabled) aurora_present_timing_enable(enabled = true);
}

// (Fifo: the swap chain's wait happens in aurora_begin_frame, after beforeFrame: the time the
// replay starts is the better estimate there.)
Clock::time_point frameTime() { return sPacing && sMode != Mode::Fifo ? sFrameTime : Clock::now(); }
const char* status() { return sStatus; }
float presentedFps() {
    if (sStarts.size() < 2) return 0.0f;
    return float(double(sStarts.size() - 1) / secs(sStarts.back() - sStarts.front()));
}

}  // namespace recomp::pacing
