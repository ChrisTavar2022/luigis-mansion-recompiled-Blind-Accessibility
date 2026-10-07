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
};

// CRT post-process filter (src/recomp/crt.cpp). Beam sigmas are fractions of the
// scanline spacing, convergence offsets are in game pixels / lines, mask size in
// output pixels, corner radius a fraction of the screen height.
enum class CrtPreset { ApertureGrille, SlotMask, ShadowMask, ConsumerTv, Custom };
enum class CrtScanlines { Progressive480, DoubleStrike240, Interlaced480i };
enum class CrtMask { ApertureGrille, SlotMask, ShadowMask, None };

struct CrtSettings {
    bool enabled = false;
    CrtPreset preset = CrtPreset::ApertureGrille;
    CrtScanlines scanlines = CrtScanlines::Progressive480;
    float beamMinSigma = 0.20f, beamMaxSigma = 0.36f, beamSpotPower = 0.6f;
    float beamMinShape = 2.0f, beamMaxShape = 3.0f, beamShapePower = 0.5f;
    float horizontalSoftness = 0.45f;
    CrtMask mask = CrtMask::ApertureGrille;
    float maskSize = 3.0f, maskStrength = 0.75f;
    float halation = 0.04f, diffusion = 0.06f, bloom = 0.15f;
    float convergence[6] = {0, 0, 0, 0, 0, 0};  // R x,y  G x,y  B x,y
    float gammaIn = 2.4f, gammaOut = 2.2f;
    float curvature = 0.0f, cornerRadius = 0.02f;
    float brightness = 1.2f;
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
    bool gbhCamera = false;        // unused Game Boy Horror live camera, toggled in game with D-pad Up (docs/UNUSED_CONTENT.md)
    bool earlyGhostLighting = false;  // "Ghost shading: Beta" = the known beta-shading cheat (fn_800581E8 channel setup)
    bool skipCutscenes = false;  // START skips cutscenes (src/recomp/skip.cpp)
    bool fastForwardKey = true;  // holding Tab runs the game at high speed (src/recomp/skip.cpp)
};

// Gameplay / difficulty options (F1 > Enhancements > Gameplay). Defaults are neutral: the
// original game is unchanged until one is moved. They apply in every mansion and variant
// (docs/GAMEPLAY_OPTIONS.md).
struct GameplaySettings {
    bool palDarknessEverywhere = false;  // PAL Hidden Mansion lighting (ambient x0.2) in every mansion
    float ambientScale = 1.0f;           // room ambient light multiplier, 0.05..1.5 (composes with the above)
    float damageScale = 1.0f;            // damage Luigi takes, 0..5
    float healthScale = 1.0f;            // Luigi's starting and maximum health, 0.25..5
    float ghostHealthScale = 1.0f;       // ghost HP, 0.25..5
    bool noHeartDrops = false;           // recovery hearts never appear (and any that do heal nothing)
    float vacuumStrength = 1.0f;         // Poltergust drain rate on ghost HP, 0.25..4
};

struct Settings {
    VideoSettings video;
    CrtSettings crt;
    AudioSettings audio;
    EnhancementSettings enhancements;
    GameplaySettings gameplay;
};

Settings& settings();
void loadSettings();
void saveSettings();
std::string settingsPath();

}  // namespace port
