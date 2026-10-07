#pragma once
// Command-line options shared between the host and game code.
#include <string>

namespace port {

struct Options {
    std::string disc;         // --disc <path>
    std::string modelViewer;  // --model-viewer <name>: dev model viewer instead of the title screen
    bool viewerRestPose = false;  // --viewer-rest: ignore the model's animation
};

Options& options();
void parseOptions(int argc, char* argv[]);

}  // namespace port
