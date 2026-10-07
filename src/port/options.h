#pragma once
// Command-line options shared between the host and game code.
#include <string>

namespace port {

struct Options {
    std::string disc;         // --disc <path>
    std::string modelViewer;  // --model-viewer <name>: dev model viewer instead of the title screen
    bool viewerRestPose = false;  // --viewer-rest: ignore the model's animation
    std::string roomViewer;   // --room-viewer <map>/<room>: e.g. map2/room_01
    std::string mapViewer;
    // --input "<retrace>:<BUTTON>[+BUTTON...][*frames],...": scripted controller 1 input
    // for automated testing, e.g. "300:START,420:A*10,500:STICKUP*60".
    std::string inputScript;    // --map-viewer <map>: every room of a map with a fly camera (e.g. map2)
    // --hidden-mansion pal|us: which release's Hidden Mansion rules apply once the game's
    // hidden-mansion flag is set (default pal; docs/HIDDEN_MANSION.md).
    std::string hiddenMansion = "pal";
};

Options& options();
void parseOptions(int argc, char* argv[]);

}  // namespace port
