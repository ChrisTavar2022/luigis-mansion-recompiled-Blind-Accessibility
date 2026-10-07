#pragma once
// Widescreen presentation for the recompiled game (docs/ENHANCEMENTS.md).
//
// The game renders a 640x480 logical frame. Aurora's STRETCH viewport policy maps
// that frame onto the whole window, so everything is drawn stretched; the GX HLE
// then undoes the stretch per draw:
//   - 3D (perspective projection, full-screen viewport): Hor+. The projection's
//     x row is scaled by factor() so the vertical FOV is kept and the horizontal
//     FOV grows. The game's frustum culling is widened to match (hook on
//     fn_80008D58).
//   - 2D (orthographic projection, full-screen viewport): kept at 4:3 and centered
//     ("pillarboxed" inside the wide frame), except draws that cover the whole
//     logical screen width: fills (fades) and blits of EFB copies stay
//     stretched to the window so they line up with the stretched frame; 4:3
//     background pictures follow backgroundMode().
//   - Draws into a partial viewport (render-to-texture, picture-in-picture) are
//     left alone, so EFB copies of those regions keep matching what was drawn.
#include "recomp/ctx.h"

namespace recomp::ws {

void init();          // host thread, before the first frame: viewport policy from --aspect
void beginFrame();    // host thread, before the guest runs a frame: picks up window resizes

// (4:3) / display aspect: 1 when the display is 4:3 or narrower or the mode is
// off, < 1 when wider. Constant while the guest runs a frame.
float factor();
inline bool active() { return factor() < 0.9999f; }

// Full-screen 2D pictures (title backdrops, menu backgrounds), --ws-backgrounds.
enum class Background { Pillarbox, Stretch, Zoom };
Background backgroundMode();

}  // namespace recomp::ws
