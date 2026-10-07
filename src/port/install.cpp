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

bool InstallState::hiddenMansionPalAvailable() const {
    return running == GameVersion::PAL || hmOverlayReady;
}

bool InstallState::hiddenMansionUsAvailable() const {
    // On a PAL base the US rules would need the reverse overlay (docs/SETUP.md), not built yet.
    return running == GameVersion::USA;
}

InstallState& install() { return sInstall; }

std::filesystem::path dataDir() {
#ifdef _WIN32
    const wchar_t* base = _wgetenv(L"APPDATA");
    std::filesystem::path dir = base ? std::filesystem::path(base) / L"Luigi's Mansion PC" : std::filesystem::path(L".");
#else
    const char* base = std::getenv("XDG_CONFIG_HOME");
    std::filesystem::path dir = base ? std::filesystem::path(base) / "Luigi's Mansion PC" : std::filesystem::path(".");
#endif
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir;
}

std::filesystem::path hiddenMansionOverlayDir() { return dataDir() / "hidden_mansion"; }

void loadInstall() {
    std::ifstream in(dataDir() / "install.ini");
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
    std::ofstream out(dataDir() / "install.ini");
    out << "[install]\n";
    out << "us_disc=" << sInstall.usDisc << '\n';
    out << "pal_disc=" << sInstall.palDisc << '\n';
    out << "base=" << versionName(sInstall.base) << '\n';
}

void setLaunchArgs(int argc, char** argv) { sArgs.assign(argv, argv + argc); }

void relaunch() {
#ifdef _WIN32
    wchar_t exe[MAX_PATH * 4];
    GetModuleFileNameW(nullptr, exe, DWORD(std::size(exe)));
    // Re-use the original command line (minus the program name).
    std::wstring cmd = L"\"" + std::wstring(exe) + L"\"";
    int argc = 0;
    if (wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
        for (int i = 1; i < argc; i++) {
            const std::wstring a = argv[i];
            if (a == L"--setup") continue;
            cmd += L" \"" + a + L"\"";
        }
        LocalFree(argv);
    }
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(exe, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
#else
    std::vector<char*> argv;
    for (auto& a : sArgs) argv.push_back(a.data());
    argv.push_back(nullptr);
    if (fork() == 0) execv("/proc/self/exe", argv.data());
#endif
    std::_Exit(0);
}

}  // namespace port
