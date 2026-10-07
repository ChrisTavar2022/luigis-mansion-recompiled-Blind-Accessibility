#pragma once
// What the user installed: their disc images and what setup derived from them
// (docs/SETUP.md). Persisted to %APPDATA%\Luigi's Mansion PC\install.ini; the derived
// files live next to it. The F1 menu reads it to grey out features whose disc is missing.
#include <filesystem>
#include <string>

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

// %APPDATA%\Luigi's Mansion PC (created on demand). Everything the port writes lives here.
std::filesystem::path dataDir();

// PAL Hidden Mansion overlay built by setup for the US base: <dataDir>/hidden_mansion
std::filesystem::path hiddenMansionOverlayDir();

void loadInstall();
void saveInstall();

// Restarts the executable with the same arguments (after the user changed the install);
// `setup` opens the setup screen (add or change discs).
void relaunch(bool setup = false);
void setLaunchArgs(int argc, char** argv);

}  // namespace port
