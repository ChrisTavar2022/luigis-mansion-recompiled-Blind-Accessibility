// Shader/pipeline precaching: see precache.h and docs/RENDERING.md ("Pipeline precache").
#include "recomp/precache.h"

#include "recomp/gx_bridge.h"
#include "recomp/hle.h"

#include <aurora/pipeline_cache.h>
#include <imgui.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace recomp::precache {
namespace {

using Clock = std::chrono::steady_clock;

bool sLog = false;

std::filesystem::path exeDir() {
#ifdef _WIN32
    wchar_t buf[MAX_PATH * 4];
    const DWORD n = GetModuleFileNameW(nullptr, buf, DWORD(std::size(buf)));
    return std::filesystem::path(std::wstring(buf, n)).parent_path();
#else
    return std::filesystem::read_symlink("/proc/self/exe").parent_path();
#endif
}

}  // namespace

void configure() {
    // LM_PIPELINE_THREADS=<n>: pipeline compile threads (default: half the hardware threads, 1..8).
    if (const char* t = std::getenv("LM_PIPELINE_THREADS")) aurora_pipeline_cache_set_threads(uint32_t(std::atoi(t)));
}

void init() {
    // LM_PIPELINE_LOG=1: every pipeline creation (precache/demand, time), every draw that had to
    // wait for its pipeline, and a summary line every 5 s with the worst frame time.
    sLog = std::getenv("LM_PIPELINE_LOG") != nullptr;
    aurora_pipeline_cache_set_log(sLog);
    // LM_PIPELINE_WAIT=0 (testing): skip draws whose pipeline is not ready, as Aurora did before
    // patch 0013 (reproduces the first-run flicker).
    if (const char* w = std::getenv("LM_PIPELINE_WAIT"); w && std::atoi(w) == 0) aurora_pipeline_cache_set_wait(false);
    // The GX accuracy flags select shader variants; set them before the first frame builds the
    // known pipelines, or the first build is for the default flags and wasted.
    recomp::gx::applyAccuracySettings();
    // LM_PRECACHE=0: no shipped list (cold-cache testing). LM_PIPELINE_LIST=<file>: another list.
    if (const char* p = std::getenv("LM_PRECACHE"); p && std::atoi(p) == 0) return;
    std::string list;
    if (const char* p = std::getenv("LM_PIPELINE_LIST")) list = p;
    else list = (exeDir() / "pipelines.bin").string();
    const uint32_t n = aurora_pipeline_cache_seed(list.c_str());
    if (sLog) log("[precache] %u configs from %s", n, list.c_str());
}

Progress progress() {
    AuroraPipelineStats s{};
    aurora_pipeline_cache_get_stats(&s);
    return Progress{s.precacheDone, s.precacheTotal};
}

void frame() {
    static Clock::time_point start = Clock::now(), last = start, window = start;
    static float worstMs = 0.0f;
    static uint32_t frames = 0;
    const auto now = Clock::now();
    worstMs = std::max(worstMs, std::chrono::duration<float, std::milli>(now - last).count());
    last = now;
    frames++;

    AuroraPipelineStats s{};
    aurora_pipeline_cache_get_stats(&s);
    // The startup precompile time (fresh cache: the whole shipped list), logged once.
    static bool sReported = false;
    if (sLog && !sReported && s.precacheTotal > 0 && s.precacheDone >= s.precacheTotal) {
        sReported = true;
        log("[precache] all %u known pipelines ready after %.2f s (%u threads)", s.precacheTotal,
            std::chrono::duration<double>(now - start).count(), s.workerThreads);
    }
    if (sLog && now - window >= std::chrono::seconds(5)) {
        log("[precache] %.0fs: precache %u/%u, pending %u, demand-created %u, stalled draws %u (%.1f ms total, "
            "max %.1f ms), dropped draws %u, worst frame %.1f ms over %u frames, %u threads",
            std::chrono::duration<double>(now - start).count(), s.precacheDone, s.precacheTotal, s.pending,
            s.demandCreated, s.stalledDraws, s.stallNs / 1e6, s.maxStallNs / 1e6, s.droppedDraws, worstMs, frames,
            s.workerThreads);
        window = now;
        worstMs = 0.0f;
        frames = 0;
    }

    // Small corner note while the background compile is still running (first launch without
    // setup, after a driver update or a settings change that needs other shaders).
    if (s.precacheDone < s.precacheTotal && now - start > std::chrono::seconds(1)) {
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - 8, vp->WorkPos.y + vp->WorkSize.y - 8),
                                ImGuiCond_Always, ImVec2(1, 1));
        ImGui::SetNextWindowBgAlpha(0.4f);
        ImGui::Begin("##precache", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                         ImGuiWindowFlags_NoNav);
        ImGui::Text("Compiling shaders %u / %u", s.precacheDone, s.precacheTotal);
        ImGui::End();
    }
}

}  // namespace recomp::precache
