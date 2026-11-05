# Accessibility project status

## Release status

Beta, version 0.4.0, released 2026-11-05, on the user's word (0.3.0 was 2026-10-31). Commits use the GitHub private address 124943462+ChrisTavar2022@users.noreply.github.com, by the user's request: their email was removed from this branch's history and the v0.2.0 tag then. Keep using that address. This is not version 1. The user calls the game about 90 percent blind accessible as of 2026-10-31. Windows Git here has no GitHub login or commit identity: push with `wsl -e git -C "/mnt/c/Users/chris/Documents/Luigi's Mansion Blind Accessibility" push origin accessibility-beta`, and commit with that private address as author and committer. Windows has no `gh`; WSL's is logged in, and releases are uploaded with it. The work is on the `accessibility-beta` branch, the default branch of the public repository. The user made it public by choice and is not advertising it yet. Making it private earlier detached it from the upstream fork network, so GitHub lists it as a standalone repository; the `upstream` remote still pulls from the original port.

The updater and release script point at this repository, never at the original port, whose builds would remove the accessibility work. Numbering: 0.x while it is a beta. Publish releases as normal releases, not prereleases, or the updater ignores them. The update check runs only in a packaged copy, not a build folder.

Final silent tests for the Go to menu build: walks to a heart, a Toad, a locked door and an unlocked door, plus a stress run with coins, hits, a ghost, a chest and auto interact on. No corruption, crash or hang. The user has not played this build yet.

## Current milestone

Version 0.4.0, the user's call on 2026-11-01 ("it deserves a version 0.4.0"). Everything since 0.3.0 (commit 0be7514, on origin) is uncommitted and not released. The user is about a quarter of the way through the game and had reached the Mirror Room at the last session. Their direction: keep looking ahead through the guides and fill gaps unprompted. If they do not answer a question or raise a point, treat it as unnoticeable or solved; they only raise what breaks the experience for blind gamers.

## Milestone

2026-10-30: the user, a totally blind player, cleared all of section one independently. They call the mod a complete success at this point. They recommend the Sidestep control style for blind players: the left stick then moves and aims.

2026-10-30: the user, a totally blind player, cleared the first few rooms completely independently with NVDA, the object sounds and an Xbox controller.

User feedback from that session: the game's own ghost sounds showed a ghost was close, but were not accurate enough to always aim well. Ghost sounds and the aim cue were built for this.

## Done

- Forked port cloned, with the Aurora submodule.
- Template files copied to `docs/accessibility/template/` unmodified, with credit in `NOTICE.md`.
- Rules, mapping and build order written in `docs/accessibility/README.md`.
- First game notes written in `docs/accessibility/game-api.md`.
- Visual Studio Clang tools installed, and the unmodified game built and run with the user's USA disc.
- Local copy updated to upstream v1.1.5. Upstream is the `upstream` remote.
- Tolk and the NVDA controller client added to `extern/tolk`.
- Speech core with NVDA detection, Auto/On/Off setting and announcement log (`src/recomp/access.cpp`).
- Options menu driven by controller and spoken: tested with a simulated Xbox controller.
- Setup screen spoken: reading order tested by keyboard before the move to controller; controller use there is not yet tested.
- Dialogue and choices spoken (`src/recomp/access_game.cpp`), USA game: tested in E. Gadd's lab with the announcement log.
- Title, file screen, game Options and pause screens spoken, USA game: tested with scripted input and the announcement log, not yet by the user with NVDA.
- Automatic rebuilds after an update now announce their progress.
- Control style choice in E. Gadd's first talk spoken (`fn_80051380`). Reading "Standard" is tested; changing it to Sidestep there is not.
- Object sounds (`src/recomp/access_world.cpp`), on by default: chests, keys, hearts, treasure, spilled money and doors in Luigi's room, as tones on their own audio streams. Faster when closer, panned left and right, lower when behind.
- Ghost sounds, on by default, with their own setting: the nearest showing ghost in Luigi's room repeats a low falling tone on a third channel, faster than object sounds. Tested with spawned, stationary portrait ghosts plus coins and hits: no corruption, no crash. Not yet tested on moving ghosts, or on ghosts fading out or hiding.
- Banana peel warning, under the object sounds setting: peels on the floor in Luigi's room repeat a clashing tone on a fourth channel, however close he gets. Tested with a spawned peel, not yet one thrown by a ghost or by the user.
- Sapphires, emeralds and rubies added to the treasure sounds.
- Toad beacon on a fifth channel, under the object sounds setting, with a Go to menu category.
- Go to menu and pathfinding (`src/recomp/access_nav.cpp`): left stick press, categories, routes from `col.mp` inside Luigi's room, A at doors, chests and Toads. Tested with a simulated controller: walked to a heart and collected it, walked to a locked door and an unlocked one.
- Spilled money now counts only inside Luigi's room.
- Locked doors announced from `fn_8011496C`, by hand or through the Go to menu, with the name of the room behind them: "Anteroom is locked". A hallway behind the door gives "This door is locked". Tested: "Telephone Room is locked".
- Go to menu names doors after the room behind them: "Hallway door, ahead, far, 1 of 2", then "Going to Hallway door". Names exist only in the mansion, map 2; elsewhere it stays "door".
- File screen says each file: "File 2, Up to Area 1, money 0, 2 of 4", or "File 1, empty, 1 of 4". The Boo count and rank code paths exist, gated as the game gates them, but no save tested them yet: the user's save has neither. The rank letter assumes the ending's scale, 1 H to 8 A.
- Copy target has a position among the free files: "File 3, 2 of 2".
- Area-clear results spoken from `fn_800501EC`: ghosts caught, total, treasure and "Press A to continue". Tested with nothing caught, so a real count is not verified. It does not list the ghosts by name, and the per-item treasure list is read from Luigi's counters, not the screen's nine rows.
- "Game over" from the health setter `fn_800B9488` when Luigi's health goes to 0, replacing "Health 0". The test reached the title screen again; whether the game shows a further screen that its A presses skipped is not confirmed.
- Fixed: junk health, money and room names spoken during a map load, such as "Health 7382" and "Coins 64853". The actor keeps its address and is filled in stages, so values outside sane limits are ignored and the first 3 seconds of a map are only a baseline.
- Repository sweep, 2026-10-30: a code review of everything since upstream, plus checks for tracked game data, secrets, tool parsing, doc links and update targets. Fixed from it: a Go to walk cut short by an event now says "Walk interrupted" and the list empties during events; "Through the door" no longer cuts off the room name; a stick press during setup cannot leave Go to open; B closes only Go to when it is over the options menu, and Go to cannot open over options; a slightly diagonal stick no longer changes category; collision loads while the menu still holds the game. `core.filemode` is false in this checkout, so the Linux and macOS scripts keep their exec bits in commits.
- Beacon fixes after the user found ghosts hard to catch ("a bit farther away than the ghost is itself"): input reads run 30 times a second, so the 210 ms ghost tone overran its 6 read beat and every other tone was dropped within 150 units, halving the rate when closest. A busy channel now retries on the next read, the ghost tone is 110 ms with a 3 read beat, and a beat shortens at once when the target closes in. Measured in a gold ghost fight, not yet played by the user.
- Three beacon pitches, the user's design, on every beacon: level for straight left or right, higher for up and its diagonals, lower for down and its diagonals. Read as stick directions on screen, not height. The bands are 22.5 degrees.
- Ghost aim cue: a bright rising tone when Luigi faces the nearest ghost within about 20 degrees and 400 units (MaxLightBindRange), with a glossary entry. Seen in the same fight. The nozzle direction, which the C stick turns ahead of Luigi's body while vacuuming, is not read yet.
- Training room beacons: map 3 is one long event script with Luigi free through it, and the "not free" gate silenced everything. The gate now skips that map. Tested: the beacon sounds on the training ghost.
- Go to climbs stairs. Stairs are flat steps about 30 units high, walls to the flat grid. A target over 200 units from Luigi's height is routed over `col.mp` floors, a node per floor over each cell, joined within a 45 unit step; a same-level target that the flat route cannot reach tries this too. A target on no reachable floor now says "I can't find a way there" instead of walking at a wall. Tested: from the Foyer floor up to the landing at height 550. Only that one staircase is tested.
- Capture speech, from Luigi's message 21: "Neville caught", or "3 Shy Guy caught" for several within a second. The user's play showed that keys and hearts send the same message, so only types with the ghost flag speak; items keep the game's own text. Portrait names come from the game's plaques. Regular ghost names are from memory, and types not known for certain (`mapoo`, `mopoo`, `topoo`, `putcher`, `skul`, `poo`) say "Ghost caught". Fat, girl and denwa were matched to Miss Petunia, Sue Pea and Uncle Grimmly by elimination, and `yapoo` plus the small ghosts are unconfirmed.
- `lockedDoorText` and `scan` both walk the door table.
- Whole-map layout, 2026-10-31, the user's idea: `access_nav.cpp` builds every floor over every spot of the map once, on its own thread, about 1.2 seconds, and searches run on their own threads. This replaces the 240 cell grid, which reached only 2400 units from Luigi while the long halls run to 6725. Doors at height 275 above their floor now count as that floor. Tested: a walk of 2668 units down the hall into the Laundry Room, and up and down the Foyer stairs. Audit `LM_ACCESS_NAV_AUDIT=1`: 225 of 234 door to door routes work. The other 9 are in rooms 33, 34, 40, 72 and 73 (`game-api.md`); there Luigi goes as close as he can and says so, or says there is no way.
- Chests and Toads are routed round. Items out of reach get "As close as I can get". The Go to list marks doors upstairs and downstairs, since the Foyer has two doors one above the other.
- Right stick status, the user's request, after Spider-Man 2: health, then total money, then room and floor, starting over after 5 seconds without a press. It repeats the last announcement in menus, cutscenes and conversations. Total uses the game's own values (`fn_8004D688`), tested only at 0: no pickup was reached in a test.
- Furniture, the user's request: from `jmp/furnitureinfo`, 602 pieces in the mansion. A Furniture category in Go to says "At the candle stand" on arrival and presses nothing. A in play says the piece Luigi faces. No beacons, by choice. Tested in the Foyer. Words come from `kModelWords`, read from model names, so some may be wrong, and unknown models say "Furniture". Pieces that appear or disappear by flag (appear_flag, disappear_flag) are not filtered, and a broken vase stays listed.
- Game Boy Horror: Y is the map, X the search view. The map says the floor, the room and the visited rooms; zoom is not spoken, its state was not found. The search view names what is in the middle by left and right, and says look up or look down. Tested in the Foyer with the door, chest of drawers and candle stand. Ghosts in the view are not tested, and the game's A search may pick differently from this focus.
- Fixed: furniture matched only where a room's number and name index agree, as the Foyer's do. The furniture table uses the name index.
- Thrown object beacon, the user's request: `isThrown` in `access_world.cpp`, with its own channel and cue setting, under Ghost sounds. Boss arenas skip the room check. Seen working on Chauncey's balls and horses; Bowser's arena reports objects, not confirmed to be his spiked balls.
- Damage settings, the user's request, on by default: no damage from ghosts and bosses, and from thrown objects, through `fn_800B9124`. Tested on Chauncey: 28 hits cancelled, and health falls with the thrown setting off. Bowser's hits name no attacker, so all of them in map 13 fall under the thrown setting, flames included. Bogmire, Boolossus and King Boo are not tested. Each hit logs `[access] hit from type`.
- Further damage settings, the user's request, off by default as they asked for switches rather than for this damage to go: hazards, floors, lingering. The 15 point inhale falls under ghosts and bosses. Tested: ghosts on and off (23 of 24 hits cancelled on, health to 37 off), hazards on (all cancelled). Floors, lingering damage and the inhale are not tested: those routes were not reached.
- Health refills by itself, the user's request, on by default: 5 seconds after the last hurt, about a point a second, written directly to Luigi +0xFC up to +0xFFC, only while he is free. Tested from 90: "Health full" spoken once.
- Person beacon, the user's request: types 39 to 41, the professor, with a People category in Go to. Portrait ghosts who talk, such as Madame Clairvoya, keep the ghost beacon. Toad stays separate, as a safe point.
- Fixed: the opening went silent if the title demo played first. The demo is the same scene with `0x804CFFAC` = 1, and the once per run rule spent its lines. The real opening now always speaks, and the demo has no scene cues.
- Checked in silent runs with `LM_ACCESS_WORLD_LOG=1`: scene start, scene end and dialogue end cues fire, and the flashlight cue. A scene must stay over for 2 seconds before its end cue, so the opening handing to the Foyer does not say control is back. Notice cues queue instead of dropping. `damageCancelled` reads the right caller (800B9280 in `fn_800B9124`). The ghost hooked cue has not fired in a test: the bot did not hook a ghost. Luigi is not free in the lab during its conversations, so the person beacon rarely sounds.
- Added while the user was away: flashlight on ghost and ghost hooked cues from messages 2 and 9 (`fn_800C0F08`), an audio cue volume slider.
- Spoken money uses 10 counters again. Counting 12 stopped pickup speech in the user's play, or came with it: not separated. Coins and bills speak again in a test. All 12 still go into the status total.
- Ghost caught, reported missing in play, is not reproduced. `LM_ACCESS_TEST_CAUGHT=11@2950` speaks "Gold Ghost caught", and message 21 reaches the hook for keys and hearts. No test has caught a real ghost: `LM_GHOST_HUNT` needs `LM_GHOST_LIGHTS=1` and stalls at AI state 5 here. Every message 21 now logs `[access] message 21: type` to last_run.log, so the next report shows how far it got.
- To check: whether cancelling ghost hits changes how ghosts behave. Ghosts appear to repeat attacks with no state change. Also the override test with drive G gone: the user's disc is on an external drive G, with a copy at `E:\Downloads\Retro Games\Game Cube\`.
- Fixed, from the user's report: descriptions stopped on a second run through a scene, were cut off by dialogue, and were missed when one script followed another. They now reset at the title, dialogue waits for them, and a new script restarts them.
- Fixed, from the user's report: open zone boundaries were treated as doors, so Luigi pressed A and waited for nothing. They are now "Way to X" with no press, and a walk survives crossing into another zone.
- Corrected: map 13 is Bogmire's arena, map 9 Bowser's. Attacker -1 hits in those two go under the thrown setting.
- Milestone 2026-10-31: the user beat Chauncey, with the phases, lock and aim assist.
- Ghost caught, found from the user's log: the message's type reads 0 after the game's handler. It is now read before the call, and from Luigi's list at +0x11F0 when that grows. Message 21 also comes when the vacuum takes a ball. Boss messages seen: 9 hooked; 7, 10, 11 and 19 rare and not yet matched to events.
- Posters, wall buttons and spots to check, the user's request: the 97 furniture rows with no model, skipped until now, are in Furniture, the Go to menu and A, with the moving wall in the Storage Room. Their words are judged from size, so some may be wrong.
- Behind the Storage Room's moving wall, furniture stays off the lists until A at the wall button, or Luigi seen past x 2750. That is per run: with a wall opened in an earlier session they appear only once he walks past it.
- Key speech, the user's request: `kRoomKeys` names what the key found in a room opens, from the guide. A key that leaves the list from within 250 units counts as taken. The game's own key bits (`fn_80079E20`, door table at r13+0x2A4, 104 bytes each, +0 the key bit) are not used. The next release is to be 0.4.0, the user's call.
- Room hints, the user's request, on by default: `kRoomHints` in `access_game.cpp`, in own words from IGN's guide, read through Chrome. `guide-notes.md` keeps what else the guide says. Fake door warning, types 225 to 227, within 220 units, one per door.
- Boos, the user's request: `watchBoos` reads the game's Boo manager, captured by a hook on `fn_800C3B14`, and works in every room alike. Tested in the Fortune-teller's and Mirror rooms with flag 73 and rooms forced clear. Not tested: a Boo coming out, fleeing, or being caught. The furniture is re-rolled on each entry, so the tone follows it. The Go to menu does not name the piece, to stay level with the game's radar.
- Candles, the user's request: `candleStands` groups flames, `watchCandles` speaks. All speech tested through `LM_ACCESS_TEST_LIGHT`. Real fire never lit a flame in the silent tests, from five distances with the stick up, level and down, so the stop distance of 230 and the aiming line are unconfirmed. Go to Candles and Elements are built on the furniture walk, not walked in a test.
- Room hints now have a state (`When`): Dark while the ghosts are there, Cleared after, Fire with fire held. A boss or portrait ghost's advice stops once its room is lit. The Nursery's wake-up line and vacuum lock wait on Chauncey in the same way.
- Moving platform false alarms: a light push of the stick (under 25) counted as nobody moving Luigi. Now past 6 counts as steering, a hooked ghost counts too, and the notice runs only in the Ball Room and Rec Room, the two moving floors in the guide.
- Hidden items: 15 Speedy Spirits by `access_name` (three only in the blackout) and the five cheese spots from `schrat` generators.
- Ghost heart cue, the user's request: type 62 out of the park. Speech plus a repeating tone. The user heard a sound in the game itself at this moment, so the game may have its own cue too.
- Moving platforms, the user's request: `noticePlatform` watches for Luigi travelling with the stick at rest, since no game marker was looked for. Limited on 2026-11-01 to the Ball Room and Rec Room, see above. Not tested on a real moving floor.
- Easier boss hits, the user's request, on by default: `steerShot` moves a just-thrown object 45 units a read towards the boss once within 900 units, for 3 seconds. The boss's collision is unchanged, so the game's own hit fires. Not tested on a real hit.
- A thrown object's beacon stops while the vacuum is locked on to it (`sHeldId`).
- Keeping objects in the vacuum, the user's request, for blind and motor disabled players, on by default: `holdObject` locks the right trigger on when it is pressed with a thrown object within 300 units of the nozzle, until the left trigger, or 1.5 seconds with nothing in reach. It does not detect a held ball: Luigi +0x11A0 is the ghost system's (+0x28 holds its actor), and a spawned ball showed no value that marks a hold. The game's mode selector `fn_800B36B8` lets R win over L, so the lock lets go on the first read of L.
- Boss phases, the user's request: worked out in `bossSpeech` from what is in play, with the order from guides (`boss-notes.md`). Chauncey's real fight gives horses, balls, one ball left, in a loop. His jumping is judged by his height changing, a guess not seen on its own in a real jump phase. Bogmire, Boolossus and Bowser were heard only in idle arenas, so Boolossus splitting and King Boo coming out have not fired.
- Boss work, the user's request: a boss beacon on its own channel, a Thrown objects category in Go to that replots as the target moves, and `bossSpeech` with a setting. See `boss-notes.md`.
- Dropped items do not vanish, the user's request, on by default: coins, bills and hearts. Tested side by side in the Foyer: they stay with it on and are gone with it off.
- Hidden items, the user's request: a Go to category from the map's own tables, not guides. Nursery test: 12 pieces and 2 hidden items matched. Hall test: walked to "Heart in the vase", said "Here", A said "Vase", and the list went from 3 to 2. Pressing A at a piece takes it off until the map changes, because the game's own searched record is not found. A piece searched with the vacuum stays listed, and a row with random contents may give nothing.
- `boss-notes.md`: boss strategy from guides and what a blind player needs in each fight. Most valuable next: a cue when a heart is exposed.
- Audio cue choices, the user's request: one setting per cue in the Accessibility tab. Tested: the door cue off gave no tones in the Foyer.
- Accessibility tab in the options menu, with the settings moved there and an audio cue glossary.
- Save, erase and copy screens spoken, including the saved and erased confirmations. File screen says how to copy and erase.
- Every menu says its position: "Save, 1 of 2". Port menus count automatically, tabs in their own bar; game menus use `positioned()`.
- The press that closes a menu no longer reaches the game (`swallowPadPress`).
- Options and Go to menus stop the game for screen reader players, through the shader hold path in `main.cpp`.
- README section for players.
- Announce rooms setting, on by default: the room name on entering, from the game's own name cards, and the floor when it changes. Sub-room zones are not built: the user's wish for zones is read as floors for now.
- Auto interact, off by default, in the options menu's Accessibility tab: presses A at chests and keys, and at doors Luigi walks towards. At most 3 presses per object per room visit.
- Health, coins, bills, gold bars and gems spoken when they change.
- Stress test with chests, keys, hearts, gold, coins and hits together: no memory corruption, no crash.

- Advice follows the game's state, the user's rule from 2026-11-01: speak advice only when it applies now. `When` in `access_game.cpp` gained JustCleared (the room was seen dark and then clear on this visit, for jobs the mod cannot see done, such as the treadmill or washing machine), Seal, BoosIn (flag 73 unset) and Blackout (flag 61, the generator hint), and Fire needs unlit candles. A cleared room says so on entry: "Butler's Room, clear" (not in a blackout, not hallways). Tested with forced clears. Still not state-aware: `Always` hints such as the Pipe Room wheel, and JustCleared jobs left undone are not repeated.
- Boo speech and tone now need the room lit, health above 0 and record +4 at 0. The user heard a Boo in a dark room: `fn_800C46E4` gives a Boo a furniture row while Luigi is elsewhere, with no lit test; only `fn_800C4240` checks light. A Boo fleeing to a dark room is reported as not findable yet. Dark run: no speech. The lit path has the same code as before; a hidden Boo was not reached in these runs, as the record stayed at row -1.
- Continuing a game plays a short walk-up, not the forest: the same scene with script 0 at `0x804CFFA8` (76 is the opening). It now has its own lines (`kReturnWalk`), from the frames described in `docs/INTERPOLATION.md`. Tested on the lab route: event -2, then the Foyer.
- Shivers, the user's report of being stuck: type 14, class portrait, candle type 15, appear flag 35. Before fire he paces the north hallway (room 17, name index 18), x -300 to 935 at height 225, and the ghost tone already sounds there. `shiversSpeech` says when he is out, what to do in his hallway, and when he runs or returns, only until the Butler's Room is lit. His rails s0 to s7 run from there round the first floor to the Butler's Room. Scripted fire never fires in silent tests, so the fire hit, the running line and the seat are not tested. Hint corrected: he is not flashed. Guides disagree on detail, and his heart cue is unconfirmed.

- Elements, the user's request ("metal" in their dictation is medal): `pollElement` says "Fire Medal collected", "Fire collected", "Fire meter full" and "Fire used up", for fire, water and ice. A stop on the right stick says the element, its percent and the medals. Medal flags 43 fire, 44 water, 45 ice (`fn_800D27E0`); setting flag 43 played the real medal scene. Tested through `LM_ACCESS_TEST_ELEMENT=<read>`, not a real spirit.
- Medal chest beacon and spirit beacon, the user's request, each with a setting. The chest in the treasure table's rooms (Mirror Room, Kitchen, Tea Room) sounds until its medal flag is set, and is "Element medal chest" in Go to. Spirits leave the ghost tone: they have their own, and only once their medal is held. The chest tone tested at 200 units in the Mirror Room. The spirit tone is not tested: `LM_SPAWN=114` showed nothing.
- Conservatory count, the user's request: furniture rows 121 to 123 (drums), 131 double bass, 132 xylophone, 133 harp, 305 horn. A at one says "Drum playing. 1 of 7. Left: ...", and "Go to the piano" at the end. It counts A presses at the faced piece in a dark room, because no byte in the record or object changed in a test that was near the harp but not facing it. Flag 90 stops the instruments and flag 89 means the scene was seen (`event03`), and what starts event 3 is not found. Tested on a drum.
- Audio cues are one list, the user's request: A turns a cue on or off, Y plays a sample. ImGui's gamepad mapping activates items with Y too, so a Y change is undone. Tested with a virtual pad.

- Friend's Twitch feedback on the 0.3.0 beta, and the user's follow-ups, 2026-11-01:
  - Stun speech: "Gold Ghost stunned. Vacuum now.", per heart, named from the nearest ghost. Tested in a real gold ghost fight, 10 of 10 stuns.
  - Stun length setting, the user's request: Accessibility > Gameplay, default twice as long. `setNextState` is held for light-bind state 3. Logged: 29 game frames normal, 116 at four times. The heart actor still goes at about 0.87 seconds: `fn_800BFEA0` and `fn_800C0B18` also test +0x89 of the object from `fn_800AD39C`, set in `fn_800B84E4`, so it follows Luigi's state, not the stun, and the mod's stun line and tone follow the heart. Whether the vacuum hooks after the heart has gone is NOT tested: the bot never hooked in these runs. The hook covers strategies with the gold ghost's layout, +0x590; portrait ghosts and others are not checked.
  - Go to: the list was already nearest first in 0.3.0. Now another floor counts as further. Repeated names are numbered: furniture in table order so Box 2 stays Box 2, others by place. "visited" after a Go to arrival or a chest press, "searched" after A at furniture, "visited" on doors to rooms Luigi has stood in. All cleared on a map change, as the searched list always was, and not saved.
  - Parlor candles (name index 35): the only room with cond_type 10, all candles out brings the ghosts. Lit stands are listed under Candles, with vacuum wording. Six portrait flames need game flag 1, from event 01. Tested with flag 1: 3 stands listed and walked to. Putting them out with the real vacuum is not tested: `LM_ACCESS_TEST_SNUFF` only forces the flame state.
  - Gallery: `fn_8018712C` r3 is the plaque, 1 to 23, and it speaks name, title, frame and a description of the picture. New Go to category Portraits, from `jmp/eventinfo` event 79; uncaught says Blank canvas. Plaque order was matched by decoding the portrait pictures.
  - Accessibility tab split into General, Gameplay, Damage and Audio cues, the user's request. No ID conflicts in the menu sweep; spoken sub-tabs not yet heard by the user.
  - Object audit, the user's request: `docs/accessibility/object-audit.md`, generated from the furniture table. 15 more model words and 10 "what it is for" notes, from `guide-notes.md` and the room hints. 56 models are still unidentified, and only furniture is covered, not `objinfo` or characters. A cross-check past our own notes is still to do: IGN needs Chrome.
  - Shoulder button menu: the Game Boy Horror's Item and Ghost list, identified from a capture of the user's screen. Both Xbox bumpers are Z. `fn_8005232C` is hooked and `listScreen` speaks the Item page's treasure, each ghost tile and the profile. Scripted Z has never opened it, so the speech is NOT tested: the user's play is its first run. The profile's own age and other details are not read: `fn_80053454`, `fn_800540C8` and `list_2.blo` hold them.
  - Sound candidates: 70 short WAVs in `C:\Users\chris\Documents\LM Sound Candidates`, outside the repo, from Adobe's Cartoon, Horror, Multimedia and Science Fiction packs, picked by file name and not listened to. Adobe's terms allow distribution but not stand-alone. audiogames.net could not be read here (403), and its forum threads point to other sites. Nothing is wired into the mod.

- Later the same evening, the user's requests:
  - Ghost names: `ghostName` has every regular type, by matching `jmp/enemyinfo` rooms to the Super Mario Wiki: mapoo1 Purple Puncher, yapoo2 Temper Terror, topoo2 Mirror Ghost, topoo3 Cinema Ghost, putcher Bowling Ghost, tenjyo2 Purple Bomber, banaoba Garbage Can Ghost. Flash (mapoo2) and Blue Blaze (mopoo2) are inferred as the second of their kind. Tested: "Purple Puncher caught".
  - Spirit beacon bug, mine: types 114 to 116 have no ghost flag, so the beacon sat in code that could never run, which is why the spawn test showed nothing. Moved out of the ghost check. Not retested with a spirit.
  - First key: `lockedDoorText` adds that a key has appeared in the Foyer and opens the Parlor, until flag 14. Flags 3 and 14 are from `jmp/keyinfo`; what sets them was not traced. Tested at both hallway doors.
  - Zone edges: a Go to walk to an open boundary pushes on out of the room for 60 reads, then says which way to hold the stick. Tested crossing the Foyer's top stairs; the spoken fallback did not need to fire, so it is untested.
  - Parlor: only the two stands by the portrait (z below -2500) are listed and spoken. "One candle stand left", and nothing on the last, since the scene speaks for itself. Assumed from the room hint that those two start the fight: the condition's code was not found, and the test aid never put out both.
  - `build/windows-clang/LuigisMansionRecomp.running.exe` is the old build, moved aside while the user's game ran. Delete it once that game is closed.

- Boss lock and Go to, the user's request, 2026-11-02 ("the choncy boss fight is super fustrating"). The user cares about this fight most: it decides whether a blind player gets through or gives up.
  - Go to: new first category Bosses, the nearest boss, walked to on the floor with replotting, "Near Chauncey" at 350 units, "has gone out of sight" if it vanishes. Tested in the arena: said and arrived every time.
  - The old aim was wrong at its root: the "can throw" cue read Luigi's body (+0xF48), but a thrown object flies along the nozzle (Luigi +0x10FC, part +0x700), which the right stick turns ahead of him. +0x10FC is only fresh with something on the nozzle, so `lockAim` uses it then and the body otherwise.
  - Held for real: Luigi +0x10EC to +0x10F8, four slots, hold the actor on the nozzle. This replaces the old note that no value marks a hold. "Got it. Left trigger throws at Chauncey." now speaks at that moment.
  - `lockAim`, setting Lock on to bosses (Gameplay, `boss_lock`, on), works while R is held in a boss fight and yields to the player's own right stick: aims at the nearest ball within 900, at the boss with a ball held or the boss open.
  - Throws: all launch functions hooked (`fn_800A3460` is the ball's, seen in the logs; `fn_8009D050`, `fn_800A95C0` same shape, not seen). `aimThrow` points the nozzle value at the boss just before the game reads it, and `steerShot` reaches 5000 units with the lock on. Result over the last runs: 30 throws, and in every run each one stunned Chauncey, from up to 1253 units. There is no test of the old aim to compare with, since the bot never threw in the old setup. The last small fixes (left stick wording, shots need a held ball) were built but not rerun.
  - Open state: `bossOpen` reads the AI state (thought +0x800, +0xE). Actors have no pointer to their AI, so `noteStrategy` learns it from `setNextState`. Open states from the bot in `coop_test.cpp`: 64 state C (hooked D), 213 state 5, 220 state 7, 221 states 1 to 3. Chauncey's C lasts far longer than the heart. The heart tone now repeats through the whole opening, and "Chauncey has recovered." ends it.
  - Hooking Chauncey: in reach about 400, and only from his front (he faces +z, spot 188 across, 313 out, either side). Hooks came only as the vacuum started, so `lockAim` walks Luigi to the spot when the left stick is idle and restarts the vacuum every 40 reads for 5. Tilt 100, full tilt overshoots him. Balls keep the steep tilt. Last run: 2 hooks in 3 stuns, 100 to 32 health in about 6 minutes of bot play. Not yet won outright in a test, and hooks are not certain.
  - NOT verified: Sidestep. The style's stored value was not found. Testing ran in Standard, where the right stick turns him. If a full turn moves nothing the lock swaps to the left stick (`sOtherStick`), which also fired falsely in Standard for short spells. The user plays Sidestep, so this is the first thing to hear back on. The front spot, restart and tilt are Chauncey only or Chauncey-tested; other bosses are untested with the lock. `LM_BOSS_DRIVE=1` plus `LM_ACCESS_TEST_THROW=45` runs the fight: map 2, `LM_START_APPEAR=1`, `LM_SKIP_TEST=2600:flag:47`, about 400 seconds.
  - Pulling once hooked is not assisted yet.
- Game Boy Horror list: FIRST TESTED 2026-11-02. Scripted Z opens it; earlier tests pressed at the wrong time, since script frames are retraces, about three to a guest frame. Item page and Ghost page speech heard in the log. The left stick changes page, not the right stick or D-pad, so the spoken line was corrected. Moving between ghost tiles and the profile are not tested.
- Version: `CMakeLists.txt` raised to 0.4.0.

## Bug pass for 0.4.0, 2026-11-02 to 2026-11-03: all fixed
A code review of everything uncommitted found nine faults beyond the free hit from an unheld ball. All nine were fixed on 2026-11-03:
- Bowser fight: `pickBoss` in `access_world.cpp` is the one boss the lock, Go to and speech follow: an open boss first, then King Boo while he shows, then the suit's body over its head, then the nearest. A Go to boss walk follows whichever boss is listed, so a swap no longer ends it with "has gone out of sight". "Has recovered" names the boss that was open, so it says King Boo, not Bowser.
- `bossSpeech` counts 229, 230 and 221 as one fight (`bossFamily`): no repeated name, health or "King Boo is weak", and the quarter steps keep going. King Boo is named once when he first comes out, and his health starts the quarters if the suit had none.
- New game in the same session: `runCount()` rises each time the game goes back to the title or file screens (logged "back at the title: run N"). Rooms visited, room hints said, Shivers' lines, the Storage Room's wall and the searched lists clear when it changes.
- `lockedDoorText`: the sealed line is only in the Mirror Room, the one room whose candles break a seal. `candlesUnlit()` answers only for the room Luigi is in now.
- `watchBoos`: a Boo's flight is judged against the room it came out in, so walking away is no longer a flight.
- Speedy Spirits: empty and "(null)" `access_name` links are skipped on both sides.
- Cheese spots stay in Hidden items after A: they are found with the search view.
- Shivers counts as out only off the park.
- The candle chime goes through `noticeCue`, and the notice queue has a lock.
Silent tests, 2026-11-03: a game over returned to the title and logged run 1, and each title demo loop after it raised the count again, harmless with no game going. Bot fight with Chauncey: named once, health spoken in quarters from 100 to 25, 3 stuns and 3 hooks, 23 hits cancelled, no crash. The run then got event 67 and Luigi was back in the mansion; why the fight ended there is not known, and event 67's scene line, "Luigi is back in the graveyard", may be wrong for it. Bowser's fight, the Mirror Room seal, a Boo's flight and a new game after a game over were not reached in a test.

## Shivers, 2026-11-03
The user's report: "It's hard to find the butler in the hall way to get him to go back to his room." Two causes: the ghost tone needs a ghost's alpha, and he fades as he paces; and Go to could not take Luigi to him. Now his tone stays on while he is in Luigi's room, and he is in Go to under Bosses while he paces Luigi's hallway and the Butler's Room is dark. The walk follows him and stops 220 units off (a guess at fire's reach, not measured), then says "Near Shivers" and how to spray fire, or that Luigi has no fire. `nav::Object::arrive` holds that line. Log: "[access] Shivers is out at 313,225,-4132, room 17". Tested: 5 walks along the hallway as he paced, each arrived. Fire reaching his candle from there is not tested.

## Conservatory, 2026-11-03
The user's requests: the instruments as their own Go to section, and a beacon for the ghost who appears at the piano, since the piano's entry did not bring Luigi to where her talk starts.
- Go to has a new category, Instruments, between Portraits and Furniture: the three drums, double bass, xylophone, harp, horn and the piano, nearest first, "played" once A was pressed at one. Arrival says "At the drum. Press A to play it.", and at the piano just "At the piano". They are no longer under Furniture. Category numbers for `LM_ACCESS_TEST_WALK`: Instruments 13, Furniture 14, Hidden 15.
- Melody Pianissima is type 38 (`pianist`), seated at 1049,130,-4862 on the chair by the piano from the start, alpha 0 until the instruments have played (the user: "She shows up once all the instruments have been activated"). Seen and in her seat (within 60 units), she is a person: the person beacon, People in Go to as "Melody Pianissima", A pressed once on arrival, "At Melody Pianissima". Off her seat she is a ghost again. Test aid `LM_ACCESS_TEST_MELODY=1` forces her alpha. Tested: listed, walked to (four waypoints), arrived, one A press. Her real appearance, the talk starting from that spot and her fight are not tested.
- `LM_ACCESS_WORLD_LOG` now logs each actor the first time it is in or near Luigi's room, with name, flags and alpha, and Melody's state. `LM_ACCESS_DUMP_ROOM=<name index>` logs a room's furniture rows with positions.

## Chests and the Go to menu's categories, 2026-11-03
- Chests, the user's request ("If you approach it the wrong way and press A, the chest does not open"): a chest walk now goes to a spot 135 units out on one side, turns Luigi to face the chest (stick towards it, tilt 55, 10 reads), presses A once and waits 45 reads. A scene ends the walk through `cancel()`, as before; no scene means the next side. Order: its front, left, right, back. The front is from the chest's heading, +0x88 (u16, 0x10000 a turn), read as forward = (sin, cos): a guess. A side that cannot be reached is skipped. After four: "The chest did not open from any side". Tested on spawned chests in the Foyer: all four sides walked, turned and pressed, but spawned chests never open from any side, so the real opening rule, the front guess and that a real opening ends the walk are NOT tested. If a real chest opens without taking Luigi out of free play, the walk would go on round it.
- Found in the chest code (`SaEnTakara1`, vtable 0x8037CC48, functions 0x80153808 to 0x80154D44): it compares Luigi +0x80C +0x24 (his room record, not a target) with its own, and on opening calls `fn_800AD54C(0, 0x23, 0)`. The test for which side opens it was not found there.
- Go to menu, the user's request: changing category says "Furniture, 2 of 2" in front of the first item, in one announcement. Before, the item interrupted it and only the item was heard. Opening says "Go to menu. Doors, 1 of 2. Left and right change category. Hallway door, ahead, far, 1 of 2". `access::beforeNextItem` does this for any ImGui menu. Tested with a virtual pad.

## Review before the 0.4.0 release, 2026-11-05
The user asked for one more pass over the code before it went public. Found and fixed:
- Conservatory count: `instrumentCount` changed `sPlayed` on the input thread without `sM`, while the menu read it on the render thread: a possible crash. `pressedA` now takes `sM` first.
- Boos: `watchBoos` kept following a Boo's record across a map change, a new game or a catch, and could say "The Boo has fled" for nothing. It forgets the record then.
- Boss fights: message 21 comes every frame and was logged every frame near a boss. Excluded, and other repeats are logged at most every 2 seconds.
- A Go to chest walk now marks the chest "visited", as a press by hand does.
- The user's request: a chest that opens with no scene ended no walk, so Luigi went on round it pressing A. A chest walk now counts the money, hearts, keys and treasure within 450 units before its first A, and ends with "The chest is open" as soon as there are more. Tested on a spawned chest with thrown treasure: it ended on the first side and the menu said "visited". A real chest that opens with nothing countable, such as a ghost, still goes round.
Silent runs now also set `LM_AUDIO=0` and `SDL_AUDIO_DRIVER=dummy`: the user heard the test runs while working on other things.

## Decisions

- Accessibility is on automatically when a screen reader is detected (Auto, On, Off; default Auto).
- The original template folder is never modified.
- The US version comes first.

## Blocked on

Nothing at the moment.

## Disc image

The user's USA disc (GLME01 rev 0, verified from its header) stays outside this repo. It is never copied here, not even to the gitignored `orig/` folder. The developer build needs its `main.dol`, passed with `LM_DOL` from a location outside the repo.

## Reading the game code (2026-11-01)

The disc is now at `E:\Downloads\Retro Games\Game Cube\Luigi's Mansion (USA).iso` (`G:` is a removable drive, out for now). `main.dol` is at the offset in the disc header at 0x420. Generate into a scratch folder, never this repo: `lmrecompile --dol <main.dol> --symbols decomp/config/GLME01/symbols.txt --splits decomp/config/GLME01/splits.txt --hooks tools/recomp/hooks.txt --version GLME01 --out <dir>`. Map tables come from `/Map/map2.szp` with `tools/rarc.py` and field names by `tools/jmp_dump.py`. Class tables: find a class's name string in the DOL, then pointers to it, then pointers to those, which gives its vtable. Delete `main.dol`, generated code and guide text from scratch afterwards.

To build while the user's game runs: rename the running exe to `LuigisMansionRecomp.exe.running`, build, and the game stays up on the old file.

## Latest run (2026-10-30)

First real launch on the user's profile succeeded: game module built, game running, `Tolk.dll` and the NVDA client loaded in the process. The user confirmed it worked with NVDA and the controller. The `[access] screen reader:` line does not appear in `last_run.log` in a normal launch, though it does with redirected output; speech is not affected.

## Next steps

00. **Start here next (saved 2026-11-05).** Beta 0.4.0 is committed, pushed and released. Still waiting on the user's play for: whether a real chest opens from the first side tried, Melody's talk starting from the Go to spot, and fire reaching Shivers from 220 units. By their rule, silence means fine.

0. **Next session starts here (saved 2026-11-01).** The newest build is `build/windows-clang/LuigisMansionRecomp.exe`, and it adds the hook `fn_800C3B14`, so its first start rebuilds the game module. The user's game was still open on an older build at the last report: it must be restarted to get the 2026-11-01 work.
   **Newest (2026-11-01 evening):** the current build adds element and medal speech, medal and spirit beacons, the Conservatory count and the one list of cues (see Done). The user is stuck at Shivers, so his fire hit, running line and seat get their first real test next.
   The first real test is the Mirror Room with fire. Real fire never lit a flame in the silent tests, so check that Go to's stop distance (230) and the "aim up with the right stick" line are right, and that real fire gives the candle speech. If the flames cannot be lit from there, change `kCandleReach` or the arrival line. Also unconfirmed: that the sealed door says so when tried. `do_type` 11 and 12 in the observers are read as lock and unlock, not proven.
   Not yet seen in real play: a Boo coming out, fleeing and being caught; ghost catch speech with the ghost's name; the portrait heart cue; the moving floor notice; room key words in real rooms; a walk to Candles or Elements; a steered hit on a boss other than Chauncey. By the user's direction these are not questions for them: treat silence as fine.
   To release 0.4.0, when the user says so: bump `project(... VERSION 0.3.0)` in `CMakeLists.txt`, write `packaging/release_notes/v0.4.0.md`, commit with the private address, push with WSL, package, scan the package for the user's email, then `gh release create` from WSL as a normal release, not a prerelease. Do not commit `extern/aurora`: it shows modified only because the local patches are applied.
   Looking ahead: emblems in the Tea Room and Cellar in the Go to menu, a spoken count of Boos caught against the 5, 20 and 40 the doors want (the manager's records do not show it, try the capture list at Luigi +0x11F0 or the save record's bits at +0x368), bowling balls, plates and sheet music in the thrown list, a warning for sparks, the 9 failing routes (rooms 33, 34, 40, 72, 73), which furniture words are wrong.
1. The user tests ghost sounds and banana peel warnings on real ghosts, and says whether ghost aim is accurate enough.
2. Confirm auto interact and the Go to menu open a real chest, and that routes work in larger rooms, stairs and rooms with moved furniture.
3. Check the area-clear count and game over screens in real play.
4. Status on demand, then spoken navigation.
5. Cutscene audio descriptions (below).

## Cutscene audio descriptions

Started 2026-10-31: `kSceneLines` in `access_game.cpp`, by event number and seconds into the scene, with a setting, once per run. The opening and Foyer are written from frames. The other story scenes are written from the event scripts and the game's story, without seeing them, as the user asked: "if something doesn't look right, I'll let you know". Expect wording fixes. Scene start, scene end and dialogue end have cues.

## Plan for cutscene audio descriptions

The user asked for these, as in the Persona 4 accessibility mod. They are possible. The event interpreter at `0x803D2D40` shows which script runs and where its read cursor is, so a table can match a written description to an event and a point in its script, and NVDA speaks it between dialogue lines. Each description has to be written by hand from what the cutscene shows. Frame dumps can supply a first draft, and the user reviews the wording. The opening and the ending are not always event scripts, so they may need timing by scene or frame instead. Keep descriptions optional, behind their own setting.

## Known limits

- Choosing a disc opens the Windows file dialog, which needs a keyboard.
- Disabled options and plain notes in the menu are not read.
- The game's own text is USA only: the addresses are not in the version map yet, so the European game stays silent there.
- Dialogue does not say who is speaking: the name is not in the text.
- Game choice menus move with the control stick, not the D-pad.
- Object sounds are generated tones, not the game's sounds. Starting game sounds from the port crashed the game when coins were bouncing: it drains JAudio's fixed sound lists (`docs/accessibility/game-api.md`). Never call `fn_80189F78` or `fn_8018D6C0` for this.
- Spoken object cues do not name the object; each kind has its own tone.
- A locked door gets auto interact presses too; the game's locked animation plays and NVDA says it is locked.
- The Go to menu lists only what is scanned now, in Luigi's room.
- The Boo tone leads to the furniture, but the Go to menu does not name it, by choice: this keeps level with the game's radar, under the no-cheats rule.
- Boos have no names in speech: a Boo that fled carries no sign of its home room's name in the records used.
- Candle speech covers only flames waiting for fire (state 4). What each room's all-lit line says is in `candleResult`, for rooms 3, 4, 27 and 40; other rooms just say all lit.
- Speedy Spirits are listed by the furniture tied to them, and stay listed until A is pressed there, as other hidden items do.
- Cheese spots are the gold mouse generators' positions, near the cheese, not the cheese itself. Pressing A at the spot takes it off the list.
- The Game Boy Horror's map zoom is not spoken.
- Stick-press controls do not exist on a GameCube controller.
