#include "port/options.h"

#include <cstring>

namespace port {

Options& options() {
    static Options sOptions;
    return sOptions;
}

void parseOptions(int argc, char* argv[]) {
    Options& o = options();
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--viewer-rest") == 0) {
            o.viewerRestPose = true;
            continue;
        }
        if (i + 1 >= argc) {
            break;
        }
        if (std::strcmp(argv[i], "--disc") == 0) {
            o.disc = argv[++i];
        } else if (std::strcmp(argv[i], "--model-viewer") == 0) {
            o.modelViewer = argv[++i];
        } else if (std::strcmp(argv[i], "--map-viewer") == 0) {
            o.mapViewer = argv[++i];
            o.modelViewer = "(map)";
        } else if (std::strcmp(argv[i], "--room-viewer") == 0) {
            o.roomViewer = argv[++i];
            o.modelViewer = "(room)";  // the viewer scene handles both
        }
    }
}

}  // namespace port
