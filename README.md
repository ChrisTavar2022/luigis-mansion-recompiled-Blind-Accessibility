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
- **Right stick press:** your status. The first press says your health, the next your total money, the next your room and floor, then it starts again. After 5 seconds without a press it starts from health. In menus, cutscenes and conversations it repeats the last announcement instead.
- **A at furniture:** NVDA says what Luigi is facing, such as "Chair" or "Poster", as well as doing what A normally does.

In the menus, the D-pad or left stick moves, A selects, and B goes back or closes. Opening the Go to menu or the options menu pauses the game.

### Go to menu (pathfinding)

The Go to menu lists what is in the room you are in: doors, coins and money, hearts, keys, chests, treasure, Toads, people, thrown objects, unlit candles, element spirits, furniture and hidden items. Hidden items include Speedy Spirits in furniture and the cheese that lets out a gold mouse. Luigi follows a thrown object as it moves and stops within vacuum reach. Hidden items are furniture the game has put something in, such as "Money in the chair". Luigi walks you there, and getting it out is up to you: try A, or the vacuum. Pressing A at a piece takes it off the list until the map changes. Doors are named after the room behind them, such as "Foyer door". A door to a hallway is "Hallway door". Open areas are split into zones with no door between them, such as the top of the Foyer stairs. These are listed as "Way to Hallway", and Luigi just walks through.

1. Press the left stick in, then release it.
2. Use left and right to change category, and up and down to choose an object.
3. Press A. Luigi walks there by himself, around furniture, and collects it. He presses A for you at doors, chests and Toads. At furniture he stops and NVDA says "At the chair", and searching it is up to you.

Luigi takes the stairs when what you picked is on another level of the room. If he cannot get right up to something, he goes as close as he can and says so. Moving the stick yourself stops the walk at any time. NVDA tells you how it ended. When a door is locked, it names the room behind it, such as "Anteroom is locked".

### Game Boy Horror

- **Map (Y):** NVDA says the floor, the room you are in and the rooms you have visited on that floor. The stick up and down changes floor, and B or Y returns. A zooms the picture, which NVDA does not announce.
- **Search view (X):** as you look around with the stick, NVDA names what is in the middle of the view, such as "Chair", "Ghost" or "Foyer door". If it says "look up" or "look down", tilt that way first: the view is inverted, so pull the stick down to look up and push it up to look down. A searches it, and NVDA reads the answer.

### Keys

When you take a key, NVDA says what it opens, such as "This key opens the Ball Room."

### Room hints

The first time you enter a room with a puzzle or a portrait ghost, NVDA says what it asks of you, such as how to catch Neville or start the instruments in the Conservatory. Turn off Room hints in the Accessibility tab to work rooms out for yourself. NVDA also warns you when you are near a fake door, which would hurt Luigi.

### Boos

Once the Boos are out, NVDA tells you when one is hiding in a cleared room, with its health, in every room that has one. A wavering tone leads to the furniture it is in. NVDA says when it comes out, and which room it flees to, so you can follow.

### Candles and seals

The Go to menu lists unlit candle stands under Candles, and fire, water and ice spirits under Elements. NVDA says each flame you light, says when a stand needs no more fire, and says what lighting them all does. The Mirror Room's door back to the Fortune-teller's Room stays sealed until both of its stands are lit, and a sealed door says so when you try it.

### Moving platforms

In the Ball Room and on the Rec Room treadmill, NVDA says "On a moving platform" when something starts carrying Luigi while you are not moving him, and "Off the platform" when he is off it or it stops. There is a setting for this in the Accessibility tab.

### Cutscenes

Two low rising notes mean a scene has started and you have lost control of Luigi. Two low falling notes mean it has ended and you have control again. One soft note means dialogue is over. NVDA describes what is happening in the story's main scenes, once each time you play. These are first drafts. Each of these can be turned off in the Accessibility tab.

### Difficulty settings

These are in the Accessibility tab, so you can set the game as easy or as hard as you like.

- **Easier boss hits:** an object you throw is steered into the boss once it passes near. On by default.
- **No damage from ghosts and bosses:** on by default.
- **No damage from thrown objects:** on by default. In Bowser's fight this covers his flames too.
- **Dropped items do not vanish:** hearts, coins and bills. On by default.
- **Health refills by itself:** 5 seconds after Luigi was last hurt, about a point a second. NVDA says "Health full". On by default.
- **No damage from hazards:** fire, explosions and other hits. Off by default.
- **No damage from floors:** off by default.
- **No lingering damage:** damage that ticks away over time. Off by default.

### Accessibility tab

Audio cue volume sets how loud all the cues are, from 0 to 200 percent.


The options menu opens on the Accessibility tab.

- **Screen reader support:** Auto, On or Off.
- **Object sounds:** chests, keys, hearts, treasure, money, doors and Toads repeat a tone, and banana peels repeat a warning tone.
- **Ghost heart:** when a ghost shows its heart, NVDA says "Heart! Vacuum now." and a fast double rise repeats until the heart goes. That is the moment to hold the right trigger.
- **Flashlight on a ghost:** a quick high rise, each time the beam really touches one.
- **Ghost hooked:** a deep jump of an octave when the vacuum catches a ghost. Pull away from its sound.
- **Keeping objects in the vacuum:** press the right trigger with a ball or other object in reach, and two quick high notes play and NVDA says the vacuum has locked on. You can let go of the right trigger: it stays on until you press the left trigger to throw. While it is locked on and you face the boss, a fast bright pulse plays, so you know you can throw. The object's own sound stops while the vacuum has it, and comes back when you let go.
- **Bosses:** a heavy low rise, their own sound. In a boss fight NVDA says the boss's health as it drops, when it vanishes and returns, and each phase, such as "Rocking horses are charging" or "One ball is left. Vacuum it, then throw it at Chauncey." While a boss is about, the right stick has a fourth stop: its health.
- **People:** the professor repeats a soft rise and fall. Toad keeps his own sound, because a Toad means a safe point.
- **Thrown objects:** the nearest ball, book, bomb or other thrown object repeats a quick jump of an octave, faster than any other cue. It is under the Ghost sounds setting.
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
