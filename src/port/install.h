#pragma once
// What the user installed: their disc images and what setup derived from them
// (docs/SETUP.md). Persisted to %APPDATA%\Luigi's Mansion Recompiled\install.ini; the derived
// files live next to it. The F1 menu reads it to grey out features whose disc is missing.
#include <filesystem>
#include <string>
#include <vector>

namespace port {

enum class GameVersion { USA, PAL };  // GLME01 / GLMP01

const char* gameId(GameVersion v);         // "GLME01" / "GLMP01"
const char* versionName(GameVersion v);    // "USA" / "PAL"

struct InstallState {
    std::string usDisc;   // path to the user's US disc image (ISO/RVZ/...), empty if none
    std::string palDisc;  // path to the PAL disc image, empty if none
    GameVersion base = GameVersion::USA;  // which version runs (when both are installed)

    // Filled at startup from the setup results (not persisted).
    bool usReady = false;    // US game module built and current
    bool palReady = false;   // PAL game module built and current
    bool hmOverlayReady = false;  // PAL Hidden Mansion overlay for the US base (needs both discs)
    GameVersion running = GameVersion::USA;  // version of the game that is running

    bool staticBuild = false;  // lmrecomp: one version linked in, no setup

    bool both() const { return usReady && palReady; }
};

// Feature availability for the F1 menu (docs/SETUP.md, "Feature gating"). Only a
// single-disc install greys features out; with both discs everything stays selectable and
// options for the other version say where they apply.
enum class Avail {
    Yes,           // works in the running game
    OtherBase,     // works when the other version is the base (both discs installed)
    NeedsUsDisc,   // single-disc install without the USA disc
    NeedsPalDisc,  // single-disc install without the European disc
    NotYet,        // both discs installed, but not implemented for the running base yet
};
Avail usCodeFeature();      // enhancements written against the USA game's code/addresses
Avail palBaseFeature();     // 50/60 Hz, language: options of the European game itself
Avail hiddenMansionPal();   // the European Hidden Mansion rules
Avail hiddenMansionUs();    // the USA Hidden Mansion rules
const char* availNote(Avail a);  // short explanation, nullptr for Yes

InstallState& install();

// Where the port writes (created on demand; docs/SETUP.md, docs/LINUX.md):
//   Windows: all three are %APPDATA%\Luigi's Mansion Recompiled.
//   Linux:   configDir  $XDG_CONFIG_HOME/Luigi's Mansion Recompiled (~/.config/...): settings.ini, install.ini
//            dataDir    $XDG_DATA_HOME/Luigi's Mansion Recompiled (~/.local/share/...): game modules, Hidden
//                       Mansion overlays, memory cards, screenshots
//            cacheDir   $XDG_CACHE_HOME/Luigi's Mansion Recompiled (~/.cache/...): GPU pipeline/shader caches
//   Portable mode (both): all three are <program folder>/portable (the Windows layout).
std::filesystem::path configDir();
std::filesystem::path dataDir();
std::filesystem::path cacheDir();

// Portable mode (docs/SETUP.md "Portable mode"): a file portable.txt or a folder portable next to
// the program (or LM_PORTABLE=1) keeps every file of the port in <program folder>/portable.
// Decided once, at the first call (which also carries out a data move the F1 menu asked for
// before the restart); LM_PORTABLE=0 forces the system folders (testing).
struct DataLocation {
    bool portable = false;
    std::filesystem::path root;    // portable: <exeDir>/portable
    std::filesystem::path exeDir;  // folder of the running program
    bool requested = false;        // portable.txt / portable folder / LM_PORTABLE=1
    bool forcedByEnv = false;      // LM_PORTABLE=1: the menu cannot switch it off
    std::string problem;           // why requested portable mode is off (folder not writable)
    std::string moveResult;        // outcome of a data move carried out at this start ("" none)
    bool moveFailed = false;
};
const DataLocation& dataLocation();
std::filesystem::path exeDir();
// The system folders (Windows: one, Linux: config, data, cache), whatever mode runs.
std::vector<std::filesystem::path> systemDirs();
// Whether the program folder can be written (probe file).
bool exeDirWritable();
// F1 > Game: whether a move to (toPortable) or from portable mode can start now; "" or the reason.
std::string checkDataMove(bool toPortable);
// Saves the request next to the program and restarts; the next start moves the data before it
// opens any file (copy, verify, switch, then remove the old copy; nothing is lost on failure).
// Returns only on failure (the reason).
std::string requestDataMove(bool toPortable);
// A path inside the program folder (relative to it) that holds the player's data in portable
// mode: the updater never replaces or removes these (update.cpp).
bool isPortableDataPath(const std::filesystem::path& relToExeDir);
// An environment variable holding a path (read as UTF-16 on Windows, so any characters survive).
std::filesystem::path envPath(const char* name);

// PAL Hidden Mansion overlay built by setup for the US base: <dataDir>/hidden_mansion
std::filesystem::path hiddenMansionOverlayDir();

void loadInstall();
void saveInstall();

// Restarts the executable with the same arguments (after the user changed the install);
// `setup` opens the setup screen (add or change discs). `exe`: the program to start instead of
// the running one's file (the updater: on Linux /proc/self/exe names the replaced file); on Linux
// it is exec'd in place of this process (same PID, so a launcher script keeps waiting for it).
void relaunch(bool setup = false, const std::filesystem::path& exe = {});
void setLaunchArgs(int argc, char** argv);

}  // namespace port
