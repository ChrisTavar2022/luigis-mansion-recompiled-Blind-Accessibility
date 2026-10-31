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

- Right stick press: status while Luigi is free, otherwise repeat the last announcement.
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
- Object +8: screen: 0 files, 0x14 Options, 3 copy to, 6 copying, 4 copied, 7 erase question, 0x0A erasing, 8 erased. 0x0C, 0x0D, 0x0E and 0x11 are not mapped.
- Object +2: erase cursor: 0 Yes, 1 No, No is the default. Object +3: copy target: 1 File 2, 2 File 3. Only free files can be chosen, so there is no overwrite prompt.
- File screen buttons: game Y copies, game X erases. On an Xbox controller game X is B by default.
- Save screen: `fn_80054508`, r3 = menu object. +1 state: 0 question, 0x0B saving, 0x0C saved and waiting for A. +2 cursor: 0 Save, 1 Never mind. `0x804D80C4` (r13+0x164) = 5 requests it, and `LM_ACCESS_POKE=<frame>:804D80C4:5` opens it for tests.
- The arrays at `0x803CF9DC` on the file screen are pointers, not progress data.
- Save records: `fn_801859E0(file)` = `0x803ED36C` + file * 0x3B0. u16 +0 bit 1: saved. +0x338: flag bytes; +0x1B bit 0x10 gives progress text 3, +0x1A bit 0x20 text 2, +8 bit 1 text 1, else 0. Text pointers at `0x8033455C`. +0x368: 48 bits, counted, shown when +0x0B has bit 8. +0x3A7: rank byte, 0 none. +0x3A8: money. `fn_80049818` draws all this for the file under the cursor.
- Area-clear screens: `fn_800501EC`, r3 = account object, +1 state: 0, 3, 4, 5, 6, 7 in order. +0x7AC: 23 rows of 6 bytes, a nonzero first byte counts as caught. Route: `LM_START_MAP=4`, `LM_SKIP_TEST=2300:flag:66`, A every 90 frames.
- Game over: `LM_TEST_HIT=<frame>,<period>,<damage>,0` lets health reach 0. Luigi's actor holds junk during a map load: filter health and counters by range.
- Test saves live in `<LM_CARD_DIR>/USA/Card A/`.
- Pause: `fn_80038744`, r3 = menu object.
- Object +0x11A (byte): 1 Continue, 2 Quit, 3 No, 4 Yes. +0x124 (word): 1 Standard, 2 Sidestep.
- Sound 0x3801 is cursor move, 0x3802 confirm, 0x3803 cancel. Calls to `fn_8018A2A0` with these find a menu's cursor code quickly.

Finding a new screen: generate readable code with `lmrecompile` into a temp folder, list functions that play those sounds, use `LM_TRACE` to see which one runs, `LM_ACCESS_WATCH=<address>,<bytes>` to see its fields change, and `LM_FRAME_DUMP` to read the labels. Delete screenshots when finished, as the user asked.

The Start button skips cutscenes, which shortens test routes. In `--input` scripts D-pad and stick are separate: `DOWN` and `STICKDOWN`.

## World objects and Luigi (verified, USA)

- Character slots: `0x803D48A0`, 128 words, slot 0 is Luigi. Fields: +0x3C type, +0x44 position, +0xB4 room (top byte).
- Parked actors sit at about -32000: skip them.
- Ghosts: type table `0x8033B590`, 0x1C per type, +0x08 u16 flag 8. A ghost shows when actor +0x40 has none of 0x6100 and alpha +0xE3 is not 0.
- Room table record, first word: room number, floor, unknown, name index (Nursery `1A 01 01 18`). Hallways have an index with no card.
- Room names: `Luigi+0x80C+0x20` (low byte, a copy of that word) indexes the card table at `0x80305F20`, 72 entries, null for hallways. `kRoomNames` in `access_game.cpp` holds the words read off the cards. These are the mansion's, map 2.
- Toads: types 42 and 43. Types: 122 and 373 to 377 chests, 86 to 91 keys, 205 and 206 hearts, 194 to 203 treasure (pearls, sapphire, emerald, ruby, diamonds, gold), 168 banana peel. A peel on the floor sits about 10 units above Luigi's height.
- Doors: map table `0x802FF218[0x804D80C8]` + 0x18 gives a table of 0x1C byte records, ending at first byte 0. +8, +0xC, +0x10 are s32 x, y, z, and +0x1A, +0x1B are the two rooms.
- Spilled coins and bills are "grains", not actors: hooks `fn_80157030` and `fn_8015066C`, r5 = grain, +0x20 position, +0x74 state. State 3 is idle at a stale position.
- Luigi health: +0xFC s16. Counters from +0x12F4: coins, bills, gold bars, then gems. A hit can reduce them.
- Control style screen: `fn_80051380`, r3 object, +4 word: 1 Standard, 2 Sidestep.
- Captures: Luigi message 21 (`fn_800AEEA8`), also sent for keys, hearts and other items, so check the type's ghost flag. Data +0x68 is the type, added to the list at Luigi +0x11F0 (count +0x11EC, up to 64). Type name: type table +0x18, class +0x14 (1 small, 2 normal, 3 portrait, 4 boss).
- Portrait names: `/Kawano/ENGLISH/res_acnt.szp` or a sibling, `timg/e_ad_ghostname_NN.bti`, I4, 160x22. Regular ghosts have no name pictures there.
- Stairs: flat steps about 30 units high in `col.mp`. Test aid `LM_ACCESS_GOTO=x,y,z`: any Go to pick walks there. Foyer landing: 0,550,-600.
- Training room is map 3: an event script runs throughout, with `kMenuOff` set, while Luigi is free. `LM_START_MAP=3`.
- Luigi's facing: +0xF48 (float x, y, z). While vacuuming, the nozzle direction is at the pointer `Luigi+0x80C+0x700`.
- Pad reads, so beacon beats, run 30 times a second. A tone longer than its beat must retry, or it drops.
- Ghost fight route: `LM_START_MAP=2`, `LM_TELEPORT=0,550,-3300,100`, `LM_GHOST_HUNT=9`, `LM_GHOST_HUNT_BEAM=89`, `LM_GHOST_HUNT_DMAX=300`, `LM_GHOST_HUNT_R=0`, input `600:START*10,1000:A*6`, at least 135 seconds. Gold ghosts rush from about 700 units to 100 in 2.5 seconds and hover near 100. `LM_ACCESS_WORLD_LOG=1` logs distance, aim and tones.
- Luigi stops about 113 units from a chest's centre and 95 from a door's: they are solid.
- Room table: pointer at `0x804D8170`, 0x30 per room, int min x, y, z at +8, max at +0x14. The box stops short of the walls.
- Door scenes: `fn_8011496C`, r3 is 1 for a locked door. Luigi's reported position does not change meaningfully during the scene, and his position jumps to a fixed spot, so do not read that as being stuck. An opening door takes about 13 seconds.
- Collision `col.mp` in `/Map/mapN.szp`, big endian: 0x00 grid cell sizes, 0x0C minimum, 0x18 size, 0x24 vertices (3 floats), 0x28 normals, 0x2C triangles (0x18 bytes: u16 vertices a, b, c, normal), 0x30 end of triangles. Static furniture is included; chests are not.
- `LM_SPAWN` objects inside each other block routes.
- Room sizes reach 5150 by 550 (room 6, the long hall) and 6725 by 950 (room 24). The old route grid, 2400 either way from Luigi, could not span them.
- Doors stand 150 above their floor, large double doors 275. Floors are 550 apart. Test aid `LM_ACCESS_NAV_AUDIT=1` plots between the doors of every room and logs failures: 225 of 234 work in the mansion. The 9 failures are in rooms 33 (door to room 56 at height 1375), 34 and 40 (the door between them, at height 830), 72 and 73. The cause there is not found: they are not stair faces, body width or step height, all tried. Nothing above tells whether they are real gaps or a door the game moves Luigi through.
- `LM_TELEPORT` into a hallway left Luigi unable to walk there: stick input had no effect. Walk in through a door to test hallways.
- A `LM_VIRTUAL_PAD` run ignores `--input`: script everything on the pad. Its times are PADRead calls, about 40 a second.
- Money: `fn_8004D688` adds up 12 counters from Luigi +0x12F4, valued by the table at `0x80335B78`: 5000, 20000, 100000, 500000, 800000, 1000000, 2000000, 5000, 20000000, 50000, 100000, 1000000.
- Furniture: `jmp/furnitureinfo` in `/Map/mapN.szp`, 730 rows in map 2, 0xC4 bytes each. Fields (hash, offset): dmd_name 0x9BC8CD +84 (model, 32 bytes), pos 0x17BEFD9 +0, dir_y 0x17A0565 +16, room_no 0xC9952D +148, furniture_x 0x1545CAA +176, furniture_z 0x1545CAC +184. A model of "(null)" or a size of 0 is not a real piece. Other fields: item_table, generate, move, appear_flag, disappear_flag.
- Game Boy Horror map (Y): update `fn_80029C38`. `0x804D8314` (r13+0x3B4) signed byte, the floor shown: -1 basement, 0 first floor, range -1 to 3, moved by the stick only in `fn_8002A364`. The zoom state is not found: `0x804D8328` (r13+0x3C8) is 1 for the training maps' map, `0x804D834A` (r13+0x3EA) did not change on A, and `0x804D8354` (r13+0x3F4) is a running counter. `0x803CDF50 + room * 2`: u16, bit 0 set once a room is known to the map (`fn_8002A260`), bit 1 a further state not identified.
- Game Boy Horror search view (X): `0x804D8160` (r13+0x200) byte, set while it is up. Camera `0x803A3820` is the first-person view then: eye about 180 above Luigi. Stick left and right turn it quickly, about 90 degrees a second at 0.6. It rests tilted down about -0.51, and both sticks tilt it, inverted: down looks up to about 0.58, up looks down to about -0.71. How the game decides what A found is not traced.
- Xbox defaults: game X is the B button, game Y is the Y button.
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
- File screen: object +8 values 0x0C, 0x0D, 0x0E and 0x11
- Memory card error screens (`fn_80047798`, `fn_80054324`)
- Game over screen's own object, if it has one
- Health, money and ghost-count addresses
- Room layout data: walls, doors, furniture
- Which objects can be interacted with, and how to detect them
- Boss fight mechanics
- Which of these work on PAL
