// HVQM4 movie start ("View a Pikmin(TM) Movie" in the options menu).
//
// The movie player (JAudio's hvqm_play: Jac_HVQM_Init / Jac_HVQM_Update / Jac_GetPicture,
// recompiled game code) decodes pictures on its own low-priority thread (priority 18,
// fn_800671E8 loops on Jac_HVQM_Update) into a ring of 17 picture buffers; the scene's draw
// takes the picture matching the audio stream's position (Jac_GetPicture, which also frees
// the buffers already due, buffer 0 included as soon as it is called). When the decoder
// reaches picture 17 it asks the stream whether it is ready (StreamSyncCheckReady), starts
// it (StreamSyncPlayAudio) and then needs buffer 0 back; if buffer 0 is still taken it
// returns and asks "ready?" again on the next try. Once the next DSP update has started the
// stream, the answer is "no" for good, and the decoder never gets past picture 17: the
// picture stays on the first frame (the Nintendo logo), the sound stops when the audio sent
// so far (~3.5 s) runs out, and the scene ends by itself after a while.
//
// On the console the 17 pictures take a while to decode (and the player's first disc read is
// slower), so the scene has started drawing - and freed buffer 0 - before picture 17. Here
// they are decoded well before the scene's first draw (it comes after the options menu has
// faded out, ~0.5 s after the player was set up). So the stream is reported not ready until
// the scene has taken its first picture, which restores the console's order of events
// without changing the player's timing otherwise.
#include "recomp/audio.h"
#include "recomp/hle.h"

#include <atomic>
#include <chrono>

namespace recomp {
namespace {
std::atomic<bool> sAwaitingFirstPicture{false};
}  // namespace

HLE(Jac_HVQM_Init) {
    sAwaitingFirstPicture = true;
    callOriginal(c, "Jac_HVQM_Init");
}

// LM_AUDIO_LOG=1: picture changes against host time while the movie plays (the scene shows
// the picture for the audio stream's position, so 29.97 changes a second = audio on time).
namespace {
struct MovieLog {
    using clock = std::chrono::steady_clock;
    u32 last = 0, changes = 0;
    clock::time_point start{};
    void picture(u32 r3) {
        if (!audio::g_log || !r3 || r3 == last) return;
        last = r3;
        if (changes++ == 0) start = clock::now();
        if (changes % 300 == 0) report("");
    }
    void report(const char* what) {
        if (!audio::g_log || !changes) return;
        const double s = std::chrono::duration<double>(clock::now() - start).count();
        log("[hvqm] %u picture changes in %.2f s (%.3f/s)%s", changes, s, s > 0 ? (changes - 1) / s : 0.0, what);
    }
} sMovieLog;
}  // namespace

HLE(Jac_GetPicture) {
    callOriginal(c, "Jac_GetPicture");
    sAwaitingFirstPicture = false;
    sMovieLog.picture(c.r[3]);
}

HLE(Jac_HVQM_ForceStop) {
    sMovieLog.report(", stopped");
    sMovieLog = {};
    sAwaitingFirstPicture = false;
    callOriginal(c, "Jac_HVQM_ForceStop");
}

// Only the movie player uses the synchronised stream (stream 0 here).
HLE(StreamSyncCheckReady) {
    if (sAwaitingFirstPicture) {
        c.r[3] = 0;
        return;
    }
    callOriginal(c, "StreamSyncCheckReady");
}

}  // namespace recomp
