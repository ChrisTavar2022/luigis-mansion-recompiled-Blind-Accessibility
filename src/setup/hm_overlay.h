#pragma once
#include "setup/disc.h"

#include <filesystem>
#include <string>

namespace setup {

// Writes the PAL Hidden Mansion overlay for the US base into `out` (replacing it), and the
// USA Hidden Mansion overlay for the PAL base into `out`/usa.
bool buildHiddenMansionOverlay(const Disc& us, const Disc& pal, const std::filesystem::path& out, std::string& error);

}  // namespace setup
