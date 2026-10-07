#include "port/settings.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

namespace port {
namespace {

Settings sSettings;

// One ini key bound to a setting, with text conversion both ways.
struct Field {
    const char* section;
    const char* key;
    std::function<std::string()> get;
    std::function<void(const std::string&)> set;
};

template <typename T>
Field num(const char* section, const char* key, T& v) {
    return {section, key, [&v] { std::ostringstream s; s << v; return s.str(); },
            [&v](const std::string& s) {
                std::istringstream in(s);
                T t{};
                if (in >> t) v = t;
            }};
}
Field flag(const char* section, const char* key, bool& v) {
    return {section, key, [&v] { return std::string(v ? "true" : "false"); },
            [&v](const std::string& s) { v = s == "true" || s == "1"; }};
}
template <typename E>
Field enm(const char* section, const char* key, E& v) {
    return {section, key, [&v] { return std::to_string(int(v)); },
            [&v](const std::string& s) { v = E(std::atoi(s.c_str())); }};
}

std::vector<Field> fields() {
    auto& v = sSettings.video;
    auto& a = sSettings.audio;
    auto& e = sSettings.enhancements;
    return {
        enm("video", "frame_rate", v.frameRate),
        enm("video", "aspect", v.aspect),
        num("video", "resolution_scale", v.resolutionScale),
        num("video", "fov_scale", v.fovScale),
        flag("video", "fullscreen", v.fullscreen),
        flag("video", "vsync", v.vsync),
        num("video", "msaa", v.msaa),
        flag("video", "hud_keep_4x3", v.hudKeep4x3),
        flag("video", "pal60", v.pal60),
        num("audio", "master_volume", a.masterVolume),
        num("audio", "music_volume", a.musicVolume),
        num("audio", "sfx_volume", a.sfxVolume),
        enm("audio", "output", a.output),
        flag("audio", "swap_channels", a.swapChannels),
        flag("audio", "reverb", a.reverb),
        flag("enhancements", "show_fps", e.showFps),
        flag("enhancements", "pause_on_focus_loss", e.pauseOnFocusLoss),
        flag("enhancements", "skip_intro_logos", e.skipIntroLogos),
        flag("enhancements", "hidden_mansion_pal", e.hiddenMansionPal),
        num("enhancements", "pal_language", e.palLanguage),
    };
}

}  // namespace

Settings& settings() { return sSettings; }

std::string settingsPath() {
#ifdef _WIN32
    const char* base = std::getenv("APPDATA");
#else
    const char* base = std::getenv("XDG_CONFIG_HOME");
#endif
    std::filesystem::path dir = base ? std::filesystem::path(base) / "Luigi's Mansion PC" : std::filesystem::path(".");
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return (dir / "settings.ini").string();
}

void loadSettings() {
    std::ifstream in(settingsPath());
    if (!in) return;
    auto table = fields();
    std::string line, section;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        if (line[0] == '[') {
            section = line.substr(1, line.find(']') - 1);
            continue;
        }
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq), value = line.substr(eq + 1);
        for (auto& f : table)
            if (section == f.section && key == f.key) f.set(value);
    }
}

void saveSettings() {
    std::ofstream out(settingsPath());
    if (!out) return;
    std::string section;
    for (auto& f : fields()) {
        if (section != f.section) {
            section = f.section;
            out << '[' << section << "]\n";
        }
        out << f.key << '=' << f.get() << '\n';
    }
}

}  // namespace port
