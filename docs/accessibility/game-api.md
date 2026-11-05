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
- Door records with byte +7 = 0xFF are not doors: they are open boundaries between zones, such as (0,825,-710) at the top of the Foyer stairs, between rooms 2 and 31. There is no door to press. Real doors carry a number there. 21 of the 72 records in map 2 are like this.
- Doors: map table `0x802FF218[0x804D80C8]` + 0x18 gives a table of 0x1C byte records, ending at first byte 0. +8, +0xC, +0x10 are s32 x, y, z, and +0x1A, +0x1B are the two rooms.
- Spilled coins and bills are "grains", not actors: hooks `fn_80157030` and `fn_8015066C`, r5 = grain, +0x20 position, +0x74 state. State 3 is idle at a stale position.
- Luigi health: +0xFC s16. Counters from +0x12F4: coins, bills, gold bars, then gems. A hit can reduce them.
- Control style screen: `fn_80051380`, r3 object, +4 word: 1 Standard, 2 Sidestep.
- Captures: Luigi message 21 (`fn_800AEEA8`), also sent for keys, hearts and other items, so check the type's ghost flag. Data +0x68 is the type, added to the list at Luigi +0x11F0 (count +0x11EC, up to 64). Type name: type table +0x18, class +0x14 (1 small, 2 normal, 3 portrait, 4 boss).
- Portrait names: `/Kawano/ENGLISH/res_acnt.szp` or a sibling, `timg/e_ad_ghostname_NN.bti`, I4, 160x22. Regular ghosts have no name pictures there.
- Stairs: flat steps about 30 units high in `col.mp`. Test aid `LM_ACCESS_GOTO=x,y,z`: any Go to pick walks there. Foyer landing: 0,550,-600.
- Training room is map 3: an event script runs throughout, with `kMenuOff` set, while Luigi is free. `LM_START_MAP=3`.
- Luigi's facing: +0xF48 (float x, y, z). The nozzle direction is Luigi +0x10FC, also reached by the pointer at `Luigi+0x80C+0x700`. It is written in `fn_800B36B8` from the joint at +0x7D8 only with something on the nozzle, and stale otherwise. Thrown objects fly along it (`fn_800A3460`, velocity set by `fn_80067C30`).
- On the nozzle: Luigi +0x10EC to +0x10F8, four actor slots, 0 when empty. +0x10E9 set while one is held, +0x10EA set when let go.
- AI objects: thought +0x800 strategy (+0xE state, +4 back to the thought), +0x808 actor, +0x950 player. No pointer from actor to thought within 0x800 bytes. Ball type 65 is `babyball`, held state 5; Chauncey `bbaby` open C, hooked D.
- List screen input (`fn_8005232C`): the left stick changes page. Script frames are retraces, about three to a guest frame.
- Pad reads, so beacon beats, run 30 times a second. A tone longer than its beat must retry, or it drops.
- Ghost fight route: `LM_START_MAP=2`, `LM_TELEPORT=0,550,-3300,100`, `LM_GHOST_HUNT=9`, `LM_GHOST_HUNT_BEAM=89`, `LM_GHOST_HUNT_DMAX=300`, `LM_GHOST_HUNT_R=0`, input `600:START*10,1000:A*6`, at least 135 seconds. Gold ghosts rush from about 700 units to 100 in 2.5 seconds and hover near 100. `LM_ACCESS_WORLD_LOG=1` logs distance, aim and tones.
- Luigi stops about 113 units from a chest's centre and 95 from a door's: they are solid.
- Actor heading: +0x88 and +0x8A, u16, 0x10000 a full turn (`coop_fixes.cpp`). Chests spawned by `LM_SPAWN=122` have 0 and never open. Chest class `SaEnTakara1`.
- Room table: pointer at `0x804D8170`, 0x30 per room, int min x, y, z at +8, max at +0x14. The box stops short of the walls.
- Door scenes: `fn_8011496C`, r3 is 1 for a locked door. Luigi's reported position does not change meaningfully during the scene, and his position jumps to a fixed spot, so do not read that as being stuck. An opening door takes about 13 seconds.
- Collision `col.mp` in `/Map/mapN.szp`, big endian: 0x00 grid cell sizes, 0x0C minimum, 0x18 size, 0x24 vertices (3 floats), 0x28 normals, 0x2C triangles (0x18 bytes: u16 vertices a, b, c, normal), 0x30 end of triangles. Static furniture is included; chests are not.
- `LM_SPAWN` objects inside each other block routes.
- Room sizes reach 5150 by 550 (room 6, the long hall) and 6725 by 950 (room 24). The old route grid, 2400 either way from Luigi, could not span them.
- Doors stand 150 above their floor, large double doors 275. Floors are 550 apart. Test aid `LM_ACCESS_NAV_AUDIT=1` plots between the doors of every room and logs failures: 225 of 234 work in the mansion. The 9 failures are in rooms 33 (door to room 56 at height 1375), 34 and 40 (the door between them, at height 830), 72 and 73. The cause there is not found: they are not stair faces, body width or step height, all tried. Nothing above tells whether they are real gaps or a door the game moves Luigi through.
- `LM_TELEPORT` during the Foyer's arrival scene leaves Luigi unable to walk: stick input has no effect until the scene is over.
- A `LM_VIRTUAL_PAD` run ignores `--input`: script everything on the pad. Its times are PADRead calls, about 40 a second. If the user's controller is on, it takes port 1 and only the stick presses reach the mod: hide it with `SDL_JOYSTICK_HIDAPI=0`, `SDL_JOYSTICK_RAWINPUT=0`, `SDL_JOYSTICK_WGI=0`, `SDL_XINPUT_ENABLED=0`, `SDL_JOYSTICK_DIRECTINPUT=0`.
- Money: `fn_8004D688` adds up 12 counters from Luigi +0x12F4, valued by the table at `0x80335B78`: 5000, 20000, 100000, 500000, 800000, 1000000, 2000000, 5000, 20000000, 50000, 100000, 1000000.
- Furniture: `jmp/furnitureinfo` in `/Map/mapN.szp`, 730 rows in map 2, 0xC4 bytes each. Fields (hash, offset): dmd_name 0x9BC8CD +84 (model, 32 bytes), pos 0x17BEFD9 +0, dir_y 0x17A0565 +16, room_no 0xC9952D +148 (the room's name index, byte 3 of its room table record, not the room number: the Foyer has the same for both, the Nursery is 26 and name index 24), furniture_x 0x1545CAA +176, furniture_z 0x1545CAC +184. A size of 0 is not a real piece. A model of "(null)" is a real one drawn into the room: 97 rows in map 2, with `move` 37, 0, 22, 23, 33 or 32. Their kind is judged by size, flat and small a wall button, flat a poster, otherwise something to check. `kabe14_1` is the Storage Room's moving wall. Other fields: item_table, generate, move, appear_flag, disappear_flag.
- Game Boy Horror map (Y): update `fn_80029C38`. `0x804D8314` (r13+0x3B4) signed byte, the floor shown: -1 basement, 0 first floor, range -1 to 3, moved by the stick only in `fn_8002A364`. The zoom state is not found: `0x804D8328` (r13+0x3C8) is 1 for the training maps' map, `0x804D834A` (r13+0x3EA) did not change on A, and `0x804D8354` (r13+0x3F4) is a running counter. `0x803CDF50 + room * 2`: u16, bit 0 set once a room is known to the map (`fn_8002A260`), bit 1 a further state not identified.
- Game Boy Horror search view (X): `0x804D8160` (r13+0x200) byte, set while it is up. Camera `0x803A3820` is the first-person view then: eye about 180 above Luigi. Stick left and right turn it quickly, about 90 degrees a second at 0.6. It rests tilted down about -0.51, and both sticks tilt it, inverted: down looks up to about 0.58, up looks down to about -0.71. How the game decides what A found is not traced.
- Xbox defaults: game X is the B button, game Y is the Y button.
- Hits on Luigi: `fn_800B9124(luigi, hit, r5)`, from his message handler and two others, returns nonzero when a hit lands. hit +0x14 attacker's actor slot (negative none), +0x18 damage, +0x1C flag, +0x20 kind (7 special). Then `fn_800B9680(luigi, amount)` and the setter `fn_800B9488`. Boss bodies: 64 Chauncey, 213 Bogmire, 220 Boolossus, 221 King Boo (message 21's portrait types), 229 and 230 the Bowser suit. `[access] hit from type` lines in the log name each attacker.
- Hidden items: furniture row item_table (hash 0x1023327) is a row of `jmp/itemappeartable`, 20 string columns, with the name kinds in `jmp/iteminfotable`. generate (0x1624539, mask 0xF0000) with generate_num (0x1A16608) gives coins and bills by count. 182 mansion rows hold something. The game's own searched record is not found: `fn_800316FC` is the burst, and the 12 byte table at r13+0x440 (index furniture +0xBC) holds light states, not this.
- Damage paths, USA: `fn_800B9124` hits; `fn_800B9680(luigi, amount)` called from `fn_800B41E0` the damaging floor, `fn_800B47B0` and `fn_800B4F2C` one point ticks on timers (+0x1130, +0x1348, +0x1340, not identified further), `fn_800B5110` 15 points after effects 0x2D and 0x2E, assumed to be Bowser's inhale from the guides. In `fn_800B9488`, called from `fn_800B9680`, the caller's return address is at the back chain + 4. `fn_800B61C8` only scales maximum health.
- Title demo: after about 30 seconds at the title, the opening scene plays with `0x804CFFAC` = 1. A new game's has 2.
- Scenes: `0x804CFF98` current scene, `0x8021DBD8` opening (about 48 seconds from file select), `0x8021DB68` main game. Event scripts load through `fn_800067D0(path, ...)`, path `/Event/eventNN.szp` built in `fn_8002B7BC`. Map event table: r13+0x410, 0x58 bytes each, +0x38 event number.
- Ghost hearts: actor type 62 `heart`, made for a stunned ghost by `fn_800C0B18` (pointer at EnThought +0x92C), parked at about -32000 otherwise, and not in the room's list. Its position out of the park is the signal. Stunned is the light-bind sub-strategy's state 3 (docs/RENDERING.md).
- Item timers: coin and bill grains +0x48 (s16, counts down in `fn_80157030` and `fn_8015066C`). Hearts +0x1AC (s32, `fn_800D1704`). Gems and other item actors are not checked.
- Boss arenas: map 10 Chauncey, 11 Boolossus, 13 Bogmire. By elimination Bowser is map 9, not yet seen. Hits with attacker -1, damage 10, kind 3 are from map 13 (Bogmire's shadows), not Bowser as first noted.
- Boolossus has no health at +0xFC. Small Boos in his fight are type 214, parked at about y -1000.
- Luigi +0x11A0: the ghost being fished, set from `fn_80083B60` in `fn_800B1BD4`, 0 when nothing, with its actor at [+0x28]. +0x24 bit 0x40 on it stops hits landing. Balls do not use it.
- Poltergust mode, Luigi +0x1180: 5 flashlight, 1 vacuum with R, 6 air with L and no element. Test: `LM_SPAWN=65@<frame>` puts a ball before Luigi, and it drifts towards the nozzle under R.
- Chauncey's arena: many actors of type 65, some waiting at heights up to about 1600.
- JAudio danger: each sound category has a fixed free list, and a start on an empty list writes through a null pointer (`fn_8018FF88`, seen as "low memory 80000028 overwritten" and then a crash). Do not start game sounds from the port.
- `LM_SKIP_TEST pos` did not move Luigi in the tested routes; use scripted stick input.
- `LM_SPAWN` places objects in front of Luigi: face open floor first, or they land behind a wall in another room.

Test aids: `LM_ACCESS_WORLD_LOG=1` logs targets and tones. `LM_SPAWN=<type>@<frame>`, `LM_SW_TREASURE=<frame>[:dx:dz]`, `LM_TEST_HIT=<frame>,<period>,<damage>`, `LM_WATCH=<addr>,<len>` with `LM_TRACE=ring` finds who writes an address. Always stress test sound work with treasure and spawns together.

- Boos: flag 73 lets them out (`telesa` generator's appear_flag). Manager update `fn_800C3B14`, r3 the manager, no global pointer. +0x5B8 record count (70), records of 12 bytes from +0x18: +0 room name index (0xFF none), +1 escapes, +2 s16 furniture table row or -1, +4 s8 must be 0 to hide, +5 Boo id, +6 health, +8 its body once out (body +0x808 is the actor). `fn_800C4240` hides the Boos of Luigi's room while it is lit, and sound 0x49C8 plays. `jmp/telesa` lists each room's health, `telesa_hide` in `jmp/furnitureinfo` is the furniture's weight. Actor type 222 stays parked at -32000 until out.
- Candles: objects behind emitters (+0x10 state, +0x14 position, +0x20 room number in the top byte, +0x30 emitter). Mirror Room: 3 flames at each end, high at y 311, start in state 4. `jmp/observerinfo` cond_type 9 is CondCandleAllOn, 10 CondCandleAllOff; do_type 11 and 12 appear to lock and unlock, unconfirmed.
- Elemental spirits: types 114 fire, 115 ice, 116 water, from `jmp/generatorinfo`. Elemental doors: types 264 to 273, `eldoorNN` in `jmp/objinfo`, arg0 the door. Speedy Spirits: types 239 to 258. Mario's items: 188 to 192. Gold mice: 107 to 111 and 259 to 263.
- Boo rows: `fn_800C46E4` places a Boo (row -1, room known) while Luigi is in another room, with no lit test. `fn_800C4240` re-rolls hidden Boos only while the room is lit.
- Game flags: `0x803D2D40 + 0x659 + n/8`, bit n%8. 61 blackout, 73 Boos out, 35 Shivers appears, 7 lab visited.
- Medal flags 43 fire, 44 water, 45 ice. `jmp/treasuretable` names the rooms: elffst 4, elwfst 8, elifst 47. Spirit fills the meter to the int at `0x804D2C2C`; Luigi +0x1184 is 1 when empty.
- Foyer scene holds Luigi: an early `LM_TELEPORT` leaves him stuck. Press A through it and teleport at about 2000 HUD updates; input must come after that.
- Stun: light-bind sub-strategy at strategy +0x504, pointer +0x590, +0xE state, 3 stunned for 29 frames, +0x10 frame count (not what ends it), +0xC next state. EnThought +0x908 stayed 0. `getEnemyDisappearFrame` did not change it. EnemyParam fields step 0x14 from LightBindFrame +0x94.
- Heart display: `fn_800C0B18` and `fn_800BFEA0` depend on +0x89 of the `fn_800AD39C` object as well as the ghost.
- Gallery: plaque (event 79 event_parameter) to ghost number at `0x80389A74 + plaque * 4`. Grades at `0x803CFA84`, 2 bits per ghost (number - 1). Models `/Nakamura/up/obaNNgpG.mdl`, NN the plaque, G the grade: 0 a blank canvas, 1 bronze, 2 silver, 3 gold. Plaques: 1 Neville, 2 Lydia, 3 Nana, 4 Chauncey, 5 Melody, 6 Petunia, 7 Clairvoya, 8 Shivers, 9 Sue Pea, 10 Whirlindas, 11 Luggs, 12 Jarvis, 13 Spooky, 14 twins, 15 soldiers, 16 Biff, 17 Slim, 18 Weston, 19 Bogmire, 20 Grimmly, 21 Van Gore, 22 Boolossus, 23 King Boo and Bowser.
- Melody Pianissima: type 38 `pianist`, in the Conservatory (room 22, name index 21) at 1049,130,-4862 from the map's start, alpha 0 until the instruments have played. Piano: furniture row 130 at 873,0,-5121.
- `objinfo` `sfire`: candle flames. The Parlor's six have appear_flag 1, set by event 01.
- Test aids: `LM_ACCESS_TEST_WALK=<reads>:<category number>` logs the menu's lines and walks to the first entry with no note. `LM_ACCESS_TEST_SNUFF=<reads>` puts out a flame that often. `LM_ACCESS_TEST_STUN=<0-3>` overrides the setting.
- Game Boy Horror list (Z): object at `0x804D8640` (r13+0x6E0), update `fn_8005232C`. +0 nonzero opens all tiles, +1 0 Item page, 1 Ghost page, 2 profile, +2 ghost cursor 0 to 21. Tile types at `0x8033804C`. Caught: bit by type in the bytes at [Luigi +0x86C] +4. `fn_800AD39C` returns Luigi +0x80C. Item page: `list_treasure_01` to `0c`, coins, bills, gold bars, sapphires, emeralds, rubies, diamonds, pink diamonds, gold diamonds, small, medium and large pearls, assumed in the order of the counters at Luigi +0x12F4.
- Element spirits, types 114 to 116, have no ghost flag in the type table.
- Opening script: `0x804CFFA8`, 76 the forest, 0 the walk-up after flag 7.
- Test aids: `LM_ACCESS_TEST_CLEAR=1` makes every room Luigi enters count as cleared. `LM_ACCESS_TEST_LIGHT=<reads>` lights one flame of his room that often, without fire. `LM_SKIP_TEST=<frame>:flag:73` lets the Boos out at run time; a Boo hides only after Luigi re-enters a cleared room, or after a while there.

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
