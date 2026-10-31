# Luigi's Mansion game API notes

The port's version of the template's `game-api.md`. Addresses are US (GLME01) unless marked otherwise. Only verified facts go here; open questions go in the last section.

## Overview

- Game: Luigi's Mansion (GameCube), USA GLME01 rev 0, PAL GLMP01.
- Engine: native PowerPC game recompiled to C++, on Aurora (WebGPU/Dawn, SDL3).
- Logic: 30 updates per second, whatever the display rate.

## Keys in use (do not override)

Keyboard defaults, from docs/CONTROLS.md. All game inputs can be rebound, so new mod keys must also be checked against the player's current bindings.

- Control stick: W, A, S, D
- C-stick: arrow keys
- A: E
- B: Q
- X: R
- Y: F
- Z: Left Ctrl, C
- START: Enter
- L: Left Shift, right mouse button
- R: Space, left mouse button
- D-pad up, down, left, right: 1, 2, 3, 4
- Port keys: Tab (fast-forward), F1 (menu), F3 and F4 (debug overlays), Esc (closes the menu)
- Gamepads: see docs/CONTROLS.md

## Controller buttons

Accessibility controls go on the controller. By default both Menu and View send START, and every button but the two stick presses is bound. So the stick presses are used:

- Right stick press: repeat the last announcement.
- Both sticks pressed in: options menu.

A GameCube controller has no stick presses, so this layout does not reach it yet. A player who binds a stick press to a game input gets both actions.

## Message windows and choices (verified, USA)

`src/recomp/access_game.cpp` reads these once per frame. No hook is needed.

- `r13` is `0x804D7F60` in the USA game.
- Text: `0x803CE7D0 + slot * 0x200`, four slots. The whole message is there as soon as the window opens.
- State: `0x803CF4C8 + slot * 4`: 0 closed, 1 opening, 2 writing, 4 waiting, 6 closing.
- Control codes: ESC, two letters and an optional `[argument]`, such as ESC `CC[ffffffff]`.
- Choice text: `0x803CEFD0 + index * 0x80`, up to four, set up by `fn_80043F14`.
- Choice state: `0x803CF4D8 + index * 4`, 4 means offered. `fn_80043F88` clears them.
- Selected choice: `0x804D850C` (`r13+0x5AC`), moved by `fn_80043FA8`. The control stick moves it, not the D-pad.
- Speaker names are not in the text. The script's `<SPEAK>` tags do not show who talks.

Test routes, with `LM_INPUT_CLOCK=frames` and a scratch profile:

- Dialogue: `LM_START_MAP=1`, input `300:START*10,500:A*6`, then A every 30 frames from 900.
- Choices: `LM_START_MAP=1`, `LM_SKIP_TEST` = flag 7 every 3 frames from 1200 to 1329, input `600:START*10,1000:A*6,1450:A,1550:A`, then `STICKDOWN*4` from 1750.

## Game menus (verified, USA)

These screens are pictures, so the words are written in `access_game.cpp`. The hooks in `tools/recomp/hooks.txt` only note that a screen runs and where its object is; the object sits on the heap, so never hard-code its address.

- Title: `fn_8003A6FC`, the "Press Start" screen.
- File screen and game Options: `fn_8004A004`, r3 = menu object.
- Object +1: file cursor: 0 to 2 files, 3 Options.
- Object +4: Options cursor: Sound, Rumble, Controls, Brightness, OK, View a Pikmin Movie.
- Object +5: Sound: 0 Mono, 1 Stereo, 2 Surround. +6: Rumble. +7: Controls: 0 Standard, 1 Sidestep.
- Object +8: screen: 0 files, 0x14 Options. Other values are not mapped.
- Pause: `fn_80038744`, r3 = menu object.
- Object +0x11A (byte): 1 Continue, 2 Quit, 3 No, 4 Yes. +0x124 (word): 1 Standard, 2 Sidestep.
- Sound 0x3801 is cursor move, 0x3802 confirm, 0x3803 cancel. Calls to `fn_8018A2A0` with these find a menu's cursor code quickly.

Finding a new screen: generate readable code with `lmrecompile` into a temp folder, list functions that play those sounds, use `LM_TRACE` to see which one runs, `LM_ACCESS_WATCH=<address>,<bytes>` to see its fields change, and `LM_FRAME_DUMP` to read the labels. Delete screenshots when finished, as the user asked.

The Start button skips cutscenes, which shortens test routes. In `--input` scripts D-pad and stick are separate: `DOWN` and `STICKDOWN`.

## World objects and Luigi (verified, USA)

- Character slots: `0x803D48A0`, 128 words, slot 0 is Luigi. Fields: +0x3C type, +0x44 position, +0xB4 room (top byte).
- Parked actors sit at about -32000: skip them.
- Ghosts: type table `0x8033B590`, 0x1C per type, +0x08 u16 flag 8. A ghost shows when actor +0x40 has none of 0x6100 and alpha +0xE3 is not 0.
- Types: 122 and 373 to 377 chests, 86 to 91 keys, 205 and 206 hearts, 194 to 203 treasure (pearls, sapphire, emerald, ruby, diamonds, gold), 168 banana peel. A peel on the floor sits about 10 units above Luigi's height.
- Doors: map table `0x802FF218[0x804D80C8]` + 0x18 gives a table of 0x1C byte records, ending at first byte 0. +8, +0xC, +0x10 are s32 x, y, z, and +0x1A, +0x1B are the two rooms.
- Spilled coins and bills are "grains", not actors: hooks `fn_80157030` and `fn_8015066C`, r5 = grain, +0x20 position, +0x74 state. State 3 is idle at a stale position.
- Luigi health: +0xFC s16. Counters from +0x12F4: coins, bills, gold bars, then gems. A hit can reduce them.
- Control style screen: `fn_80051380`, r3 object, +4 word: 1 Standard, 2 Sidestep.
- Luigi stops about 113 units from a chest's centre and 95 from a door's: they are solid.
- JAudio danger: each sound category has a fixed free list, and a start on an empty list writes through a null pointer (`fn_8018FF88`, seen as "low memory 80000028 overwritten" and then a crash). Do not start game sounds from the port.
- `LM_SKIP_TEST pos` did not move Luigi in the tested routes; use scripted stick input.
- `LM_SPAWN` places objects in front of Luigi: face open floor first, or they land behind a wall in another room.

Test aids: `LM_ACCESS_WORLD_LOG=1` logs targets and tones. `LM_SPAWN=<type>@<frame>`, `LM_SW_TREASURE=<frame>[:dx:dz]`, `LM_TEST_HIT=<frame>,<period>,<damage>`, `LM_WATCH=<addr>,<len>` with `LM_TRACE=ring` finds who writes an address. Always stress test sound work with treasure and spawns together.

## Text and hook points

- `fn_80044138`: draws every message window, already hooked in `src/recomp/tags.cpp`.
- `fn_800433B4`: draws a single message window (`tags.cpp`).
- `fn_80045BDC`: room name (`tags.cpp`).
- Event scripts: `/Event/eventNN.szp` holds `text/eventNN.txt` (Shift-JIS) and `message/messageN.csv`. `<SPEAK>(n)` shows message n and `<CHOICE>"label"(n)` offers one (`src/recomp/text_archive.cpp`). That file already parses these for the debug text archive.
- `src/recomp/skip.cpp`: event wait states at +0x1C: 3 is `<ANYKEY>` (window waits for A), 7 is `<CHOICE>`.
- Room name cards are pictures (`kawano/roomname/*.bti`), so room names need another source or a table.

## Player

- `Luigi +0x798`: the camera his stick input is relative to (`coop_fixes.cpp`, docs/UNUSED_CONTENT.md).
- `Luigi +0x792`: equipment bits; `|= 4|8` gives Poltergust and Game Boy Horror (`test_state.cpp`).
- `Luigi +0x1184`: element, 2, 3 or 4 for fire, water, ice. `+0x1188`: element meter.
- `fn_800B9488`: Luigi's health setter, called by every damage source (`coop_fixes.cpp`).

## World and ghosts

- `src/recomp/ghost_lights.cpp`: the ghost-hunt test bot tracks ghost world positions, AI state and held state, and turns a ghost's position into stick input. It is the best starting point for ghost cues.
- `fn_80018D78`: the game's room-lit test (`docs/RENDERING.md`).
- `LM_ACTOR_DUMP`, `LM_TEST_STATE`, `LM_START_MAP`, `LM_TELEPORT`: test aids in the repo's `CLAUDE.md`.

## Audio

- `src/recomp/audio_out.cpp`: SDL3 output, the likely place to mix accessibility cues. Not yet checked for that use.

## Not yet analysed

- Camera heading in every room and cutscene
- File screen with saves: empty or saved file, and the screens behind a saved file (object +8 values)
- Save prompt, game over and area-clear screens
- Map and Game Boy Horror screens
- Health, money and ghost-count addresses
- Room layout data: walls, doors, furniture
- Which objects can be interacted with, and how to detect them
- Boss fight mechanics
- Which of these work on PAL
