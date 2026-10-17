**No game data is included.** On first start the game asks for your own disc image (ISO, RVZ and or an extracted game folder) of the USA release (GLME01) and/or the European release (GLMP01). It is recommended to use both the USA and PAL versions of the game to experience every feature available in this port. Once the games are selected, it then builds the game for your PC with the bundled compiler, which takes about 20 to 60 seconds on a decent PC.


## Features
- **Any resolution, widescreen and ultrawide:** Hor+, with HUD elements anchored to the screen edges and widescreen fixes for full-screen scenes.
- **High frame rates:** game logic stays at the original 30 updates per second, and the frames in between are interpolated from source-tagged objects. Frame rate goes up to 320 fps, with "Match display refresh rate" and VRR/G-SYNC aware pacing. "Original" keeps the console's 30 fps.
- **Accurate GameCube rendering:** Better than Dolphin. EFB copies including copy-to-RAM, depth textures, real-time shadows, indirect effects, 6-bit blending and copy filters. A shader precache prepares everything before the game starts, so there are no compile stutters.
- **Optional Enhanced lighting:** per-pixel and colored lights, soft shadows, a realistic flashlight beam, bloom and ghosts cast a glowing light.
- **The European Hidden Mansion,** including its boss changes. With both discs, E. Gadd's lab offers the USA and the European Hidden Mansion.
- **Restored unused content,** all off by default: Toggles to re-enable various cut content, a model viewer, warps to unused rooms and more.
- **Options for EVERYTHING:** an F1 options menu for everything, difficulty settings, fun tweaks and toggles, rendering options, etc

## Known issues
- Enabling Ambient Occlusion under the enhanced lighting options causes the shading on some characters to appear polygonal and blocky, disable the AO for now. The rest of the enhanced lighting effects work as intended.
- The 2-player mode (Unused content) is unfinished. The plan is to finish it soon, but for now it's mostly in the state it was left in in the original code.
- A few things appear erratic while interpolating higher framerates: mostly water spray effects, sometimes the tube on the Poltergust 3000 jumps for a frame or two, working on resolving this.
- Some of the optional content is a little buggy, most of it works, but for example the Mario model when selecting "Play as Mario" is completely borked and sometimes the model viewer doesn't load properly. Some of the difficulty sliders have adverse side effects(Lowering Ghost HP will also lower Luigis HP next time you load into the Mansion). These will be fixed shortly.
