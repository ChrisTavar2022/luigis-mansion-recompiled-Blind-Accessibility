// Host audio output: the AI DMA blocks the game hands to AIInitDMA are converted
// from big-endian s16 stereo and queued on an SDL3 audio stream (SDL resamples
// the 32/48 kHz DSP rate to the device). LM_AUDIO_WAV=<path> also records
// everything to a WAV file; LM_AUDIO=0 disables the device.
#include "port/settings.h"
#include "recomp/hidden_mansion.h"
#include "recomp/audio.h"
#include "recomp/hle.h"

#include <SDL3/SDL.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <algorithm>
#include <vector>

namespace recomp::audio {
namespace {

// Channel order of the AI DMA frames: JAudio interleaves the DSP's first output
// buffer (bus 0x0D00) into the first slot of each frame.
constexpr bool kRightFirst = false;

std::mutex sM;
SDL_AudioStream* sStream = nullptr;
bool sTried = false;
u32 sRate = 32000;
std::FILE* sWav = nullptr;
u32 sWavBytes = 0;
u32 sWavRate = 0;

void wavHeader() {
    auto le32 = [](u8* p, u32 v) { p[0] = u8(v); p[1] = u8(v >> 8); p[2] = u8(v >> 16); p[3] = u8(v >> 24); };
    u8 h[44] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 2, 0};
    le32(h + 4, 36 + sWavBytes);
    le32(h + 24, sWavRate);
    le32(h + 28, sWavRate * 4);
    h[32] = 4;
    h[34] = 16;
    std::memcpy(h + 36, "data", 4);
    le32(h + 40, sWavBytes);
    std::fseek(sWav, 0, SEEK_SET);
    std::fwrite(h, 1, sizeof h, sWav);
    std::fseek(sWav, 0, SEEK_END);
}

void openDevice() {
    sTried = true;
    if (const char* wav = std::getenv("LM_AUDIO_WAV"); wav && *wav) {
        sWav = std::fopen(wav, "wb");
        if (sWav) {
            sWavRate = sRate;
            wavHeader();
            log("[audio] recording to %s", wav);
        }
    }
    if (const char* e = std::getenv("LM_AUDIO"); e && *e == '0') return;
    if (!SDL_WasInit(SDL_INIT_AUDIO) && !SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        log("[audio] SDL audio init failed: %s", SDL_GetError());
        return;
    }
    const SDL_AudioSpec spec{SDL_AUDIO_S16, 2, int(sRate)};
    sStream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (!sStream) {
        log("[audio] no audio device: %s", SDL_GetError());
        return;
    }
    SDL_ResumeAudioStreamDevice(sStream);
    log("[audio] output started at %u Hz", sRate);
}

}  // namespace

void outStart(u32 rate) {
    std::lock_guard lk(sM);
    if (sTried) return;
    sRate = rate;
    openDevice();
}

void outSetRate(u32 rate) {
    std::lock_guard lk(sM);
    if (rate == sRate) return;
    sRate = rate;
    if (sStream) {
        const SDL_AudioSpec spec{SDL_AUDIO_S16, 2, int(rate)};
        SDL_SetAudioStreamFormat(sStream, &spec, nullptr);
    }
}

void outPushGuestBlock(u32 addr, u32 bytes) {
    const u32 frames = bytes / 4;
    if (!frames) return;
    static thread_local std::vector<s16> buf;
    buf.resize(frames * 2);
    const auto& as = port::settings().audio;
    const s32 gain = s32(std::clamp(as.masterVolume, 0.0f, 1.0f) * 32768.0f);
    const bool mono = as.output == port::AudioOutput::Mono;
    // PAL Hidden Mansion mirrors the picture, so it swaps the speakers too.
    const bool swap = (as.swapChannels != kRightFirst) != hm::mirrorActive();
    for (u32 i = 0; i < frames; i++) {
        const s16 a = s16(rd16(addr + i * 4)), b = s16(rd16(addr + i * 4 + 2));
        s32 l = swap ? b : a, r = swap ? a : b;
        if (mono) l = r = (l + r) / 2;
        buf[i * 2 + 0] = s16((l * gain) >> 15);
        buf[i * 2 + 1] = s16((r * gain) >> 15);
    }
    std::lock_guard lk(sM);
    if (sStream) SDL_PutAudioStreamData(sStream, buf.data(), int(frames * 4));
    if (sWav) {
        std::fwrite(buf.data(), 4, frames, sWav);
        sWavBytes += frames * 4;
        if ((sWavBytes & 0xFFFF) < frames * 4) wavHeader();  // keep the header valid while running
    }
}

double outQueuedSeconds() {
    std::lock_guard lk(sM);
    if (!sStream) return 0;
    return SDL_GetAudioStreamQueued(sStream) / (4.0 * sRate);
}

bool outHasDevice() {
    std::lock_guard lk(sM);
    return sStream != nullptr;
}

}  // namespace recomp::audio
