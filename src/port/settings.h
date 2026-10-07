#pragma once
// User settings, edited in the in-game overlay menu (F1) and saved to
// %APPDATA%\Luigi's Mansion PC\settings.ini. Read them wherever they apply;
// changes take effect immediately unless noted.
#include <string>

namespace port {

enum class FrameRateMode { Original30, Interpolated60, InterpolatedUnlimited };
enum class AspectMode { Original4x3, Auto, Wide16x9, Wide16x10, Ultra21x9, Ultra32x9 };
enum class AudioOutput { Stereo, Mono, Surround };

struct VideoSettings {
    FrameRateMode frameRate = FrameRateMode::Original30;
    AspectMode aspect = AspectMode::Auto;
    int resolutionScale = 0;    // internal render resolution: 0 = match window, N = N x 480p (aspect-widened)
    float fovScale = 1.0f;      // multiplies the game's vertical field of view
    bool fullscreen = false;
    bool vsync = true;          // applied at next launch
    int msaa = 1;               // 1, 2, 4, 8; applied at next launch
    bool hudKeep4x3 = true;     // keep 2D/HUD at 4:3 (centered) in widescreen
    bool pal60 = false;         // PAL game only: 59.94 Hz retrace (PAL60, 20% faster) instead of 50 Hz
};

struct AudioSettings {
    float masterVolume = 1.0f;  // 0..1
    float musicVolume = 1.0f;   // streamed/sequenced music
    float sfxVolume = 1.0f;     // everything else
    AudioOutput output = AudioOutput::Stereo;
    bool swapChannels = false;
    bool reverb = true;
};

struct EnhancementSettings {
    bool showFps = false;
    bool pauseOnFocusLoss = true;
    bool skipIntroLogos = false;
    bool hiddenMansionPal = true;  // Hidden Mansion uses the PAL release's rules (until the lab menu offers both)
    int palLanguage = 1;           // PAL game only: 1 English, 2 German, 3 French, 4 Spanish, 5 Italian (next launch)
};

struct Settings {
    VideoSettings video;
    AudioSettings audio;
    EnhancementSettings enhancements;
};

Settings& settings();
void loadSettings();
void saveSettings();
std::string settingsPath();

}  // namespace port
