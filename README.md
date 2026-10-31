# Luigi's Mansion Recompiled

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
- The 2-player mode (Unused content) is unfinished. The plan is to finish it soon, but for now it's mostly in the state it was left in in the original code.

## Blind accessibility (this fork)

This fork makes the game playable by blind players with a screen reader and a controller. It is a beta, and the game text below is for the USA version of the game. A totally blind player has cleared all of section one independently with it.

Screen reader support turns on by itself when NVDA or another supported screen reader is running.

**Recommended:** play with the Sidestep control style. Luigi then moves and aims with the left stick alone, so you only have one stick to think about. Choose it when E. Gadd asks, or in the pause menu.

### Controls

- **Left stick press:** open or close the Go to menu.
- **Both sticks pressed in:** open or close the options menu.
- **Right stick press:** repeat the last announcement.

In the menus, the D-pad or left stick moves, A selects, and B goes back or closes. Opening the Go to menu or the options menu pauses the game.

### Go to menu (pathfinding)

The Go to menu lists what is in the room you are in: doors, coins and money, hearts, keys, chests, treasure and Toads. Doors are named after the room behind them, such as "Foyer door". A door to a hallway is "Hallway door".

1. Press the left stick in, then release it.
2. Use left and right to change category, and up and down to choose an object.
3. Press A. Luigi walks there by himself, around furniture, and collects it. He presses A for you at doors, chests and Toads.

Luigi takes the stairs when what you picked is on another level of the room. Moving the stick yourself stops the walk at any time. NVDA tells you how it ended. When a door is locked, it names the room behind it, such as "Anteroom is locked".

### Accessibility tab

The options menu opens on the Accessibility tab.

- **Screen reader support:** Auto, On or Off.
- **Object sounds:** chests, keys, hearts, treasure, money, doors and Toads repeat a tone, and banana peels repeat a warning tone.
- **Ghost sounds:** the nearest ghost that is showing repeats a low falling tone. It changes to a bright rising tone when Luigi is facing the ghost and close enough for the flashlight. NVDA says what you caught, such as "Neville caught" or "3 Shy Guy caught".
- **Announce rooms:** says each room as you enter it, using the game's own room names, and the floor when it changes.
- **Auto interact:** off by default. Presses A for you at chests and keys, and at doors you walk towards. It never acts on furniture.
- **Audio cue glossary:** press an entry to hear that sound.

Every sound gets faster as you get closer and pans left and right with the object, so keeping it centred walks you straight at it. Pitch gives the direction: normal when the object is straight left or right, higher when it is up or up to a side, and lower when it is down or down to a side.

### What NVDA reads

- Dialogue and choices.
- The title, file, game options, pause, control style, save, erase and copy screens.
- Every menu item with its position, such as "Save, 1 of 2".
- Health, coins, bills, gold bars and gems when they change.

On the file screen, NVDA says whether each file is empty, and for a saved file how far it has reached and its money. Y copies a saved file, and B on an Xbox controller erases it. The results after an area, and game over, are spoken too.

### Credits

- **Accessibility Mod Template** by Happy Starfish (CC BY 4.0): the guides, rules and workflow behind this mod's accessibility design. The original copies are in `docs/accessibility/template`.
- **Luigi's Mansion Recompiled** by rob646464: the PC port this fork is built on ([original repository](https://github.com/rob646464/luigis-mansion-recompiled)).
- **Tolk** by Davy Kager and contributors, and **NVDA's controller client** by NV Access: speech through screen readers.
- **Aurora**, **lm-decomp** and **Dusklight**: see `NOTICE.md`.
### Updates

Download the latest package from this repository's Releases page. When a newer version is out, NVDA says so at startup. Press both sticks in, and the update is at the top of the Accessibility tab: choose Download and install, then Install and restart. Your settings and saves are kept.
