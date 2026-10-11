#include "port/install.h"

#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#else
#include <unistd.h>
#endif

namespace port {
namespace {

InstallState sInstall;
std::vector<std::string> sArgs;

}  // namespace

const char* gameId(GameVersion v) { return v == GameVersion::PAL ? "GLMP01" : "GLME01"; }
const char* versionName(GameVersion v) { return v == GameVersion::PAL ? "PAL" : "USA"; }

Avail usCodeFeature() {
    const auto& i = sInstall;
    if (i.running == GameVersion::USA) return Avail::Yes;
    return i.usReady ? Avail::OtherBase : Avail::NeedsUsDisc;
}

Avail palBaseFeature() {
    const auto& i = sInstall;
    if (i.running == GameVersion::PAL) return Avail::Yes;
    return i.palReady ? Avail::OtherBase : Avail::NeedsPalDisc;
}

// The other release's Hidden Mansion needs that release's data: the overlay setup builds from
// both discs (docs/SETUP.md; PAL rules on the USA game, USA rules on the European game).
Avail hiddenMansionPal() {
    const auto& i = sInstall;
    if (i.running == GameVersion::PAL || i.hmOverlayReady) return Avail::Yes;
    return Avail::NeedsPalDisc;
}

Avail hiddenMansionUs() {
    const auto& i = sInstall;
    if (i.running == GameVersion::USA || i.hmOverlayReady) return Avail::Yes;
    return Avail::NeedsUsDisc;
}

const char* availNote(Avail a) {
    switch (a) {
    case Avail::Yes: return nullptr;
    case Avail::OtherBase:
        return sInstall.running == GameVersion::USA ? "applies when playing the European version"
                                                    : "applies when playing the USA version";
    case Avail::NeedsUsDisc: return "needs the USA disc: F1 > Game > Add or change discs";
    case Avail::NeedsPalDisc: return "needs the European disc: F1 > Game > Add or change discs";
    case Avail::NotYet: return "not available on the European version yet; play the USA version";
    }
    return nullptr;
}

InstallState& install() { return sInstall; }

namespace {

#ifdef _WIN32
std::filesystem::path appDir() {
    const wchar_t* base = _wgetenv(L"APPDATA");
    std::filesystem::path dir = base ? std::filesystem::path(base) / L"Luigi's Mansion PC" : std::filesystem::path(L".");
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir;
}
#else
// XDG base directory: $<var> if set to an absolute path, else $HOME/<fallback>.
std::filesystem::path xdgDir(const char* var, const char* fallback) {
    std::filesystem::path base;
    if (const char* v = std::getenv(var); v && v[0] == '/') base = v;
    else if (const char* home = std::getenv("HOME"); home && home[0]) base = std::filesystem::path(home) / fallback;
    else base = ".";
    std::filesystem::path dir = base / "Luigi's Mansion PC";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir;
}
#endif

}  // namespace

std::filesystem::path configDir() {
#ifdef _WIN32
    return appDir();
#else
    return xdgDir("XDG_CONFIG_HOME", ".config");
#endif
}

std::filesystem::path dataDir() {
#ifdef _WIN32
    return appDir();
#else
    return xdgDir("XDG_DATA_HOME", ".local/share");
#endif
}

std::filesystem::path cacheDir() {
#ifdef _WIN32
    return appDir();
#else
    return xdgDir("XDG_CACHE_HOME", ".cache");
#endif
}

std::filesystem::path hiddenMansionOverlayDir() { return dataDir() / "hidden_mansion"; }

void loadInstall() {
    std::ifstream in(configDir() / "install.ini");
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto eq = line.find('=');
        if (eq == std::string::npos || line[0] == ';' || line[0] == '[') continue;
        const std::string key = line.substr(0, eq), value = line.substr(eq + 1);
        if (key == "us_disc") sInstall.usDisc = value;
        else if (key == "pal_disc") sInstall.palDisc = value;
        else if (key == "base") sInstall.base = value == "PAL" ? GameVersion::PAL : GameVersion::USA;
    }
}

void saveInstall() {
    std::ofstream out(configDir() / "install.ini");
    out << "[install]\n";
    out << "us_disc=" << sInstall.usDisc << '\n';
    out << "pal_disc=" << sInstall.palDisc << '\n';
    out << "base=" << versionName(sInstall.base) << '\n';
}

void setLaunchArgs(int argc, char** argv) { sArgs.assign(argv, argv + argc); }

void relaunch(bool setup) {
#ifdef _WIN32
    wchar_t exe[MAX_PATH * 4];
    GetModuleFileNameW(nullptr, exe, DWORD(std::size(exe)));
    // Re-use the original command line (minus the program name).
    std::wstring cmd = L"\"" + std::wstring(exe) + L"\"";
    int argc = 0;
    if (wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
        for (int i = 1; i < argc; i++) {
            const std::wstring a = argv[i];
            if (a == L"--setup" || a == L"--setup-auto") continue;
            cmd += L" \"" + a + L"\"";
        }
        LocalFree(argv);
    }
    if (setup) cmd += L" --setup";
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(exe, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
#else
    std::vector<char*> argv;
    for (auto& a : sArgs) argv.push_back(a.data());
    static char setupArg[] = "--setup";
    if (setup) argv.push_back(setupArg);
    argv.push_back(nullptr);
    if (fork() == 0) execv("/proc/self/exe", argv.data());
#endif
    std::_Exit(0);
}

}  // namespace port
