# Accessibility project status

## Release status

Beta, version 0.3.0, released 2026-10-31. Commits use the GitHub private address 124943462+ChrisTavar2022@users.noreply.github.com, by the user's request: their email was removed from this branch's history and the v0.2.0 tag then. Keep using that address. This is not version 1. The user calls the game about 90 percent blind accessible as of 2026-10-31. Windows Git here has no GitHub login or commit identity: push with `wsl -e git -C "/mnt/c/Users/chris/Documents/Luigi's Mansion Blind Accessibility" push origin accessibility-beta`, and commit with that private address as author and committer. Windows has no `gh`; WSL's is logged in, and releases are uploaded with it. The work is on the `accessibility-beta` branch, the default branch of the public repository. The user made it public by choice and is not advertising it yet. Making it private earlier detached it from the upstream fork network, so GitHub lists it as a standalone repository; the `upstream` remote still pulls from the original port.

The updater and release script point at this repository, never at the original port, whose builds would remove the accessibility work. Numbering: 0.x while it is a beta. Publish releases as normal releases, not prereleases, or the updater ignores them. The update check runs only in a packaged copy, not a build folder.

Final silent tests for the Go to menu build: walks to a heart, a Toad, a locked door and an unlocked door, plus a stress run with coins, hits, a ghost, a chest and auto interact on. No corruption, crash or hang. The user has not played this build yet.

## Current milestone

Closing the small gaps: rooms, door names, save files, results and game over. Work after release 0.2.0 is not released yet.

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

## Decisions

- Accessibility is on automatically when a screen reader is detected (Auto, On, Off; default Auto).
- The original template folder is never modified.
- The US version comes first.

## Blocked on

Nothing at the moment.

## Disc image

The user's USA disc (GLME01 rev 0, verified from its header) stays outside this repo. It is never copied here, not even to the gitignored `orig/` folder. The developer build needs its `main.dol`, passed with `LM_DOL` from a location outside the repo.

## Latest run (2026-10-30)

First real launch on the user's profile succeeded: game module built, game running, `Tolk.dll` and the NVDA client loaded in the process. The user confirmed it worked with NVDA and the controller. The `[access] screen reader:` line does not appear in `last_run.log` in a normal launch, though it does with redirected output; speech is not affected.

## Next steps

0. **Next session starts here: the user plays the 2026-10-31 build** and reports on routes in big rooms, the right stick status, furniture names, the Game Boy Horror and cue choices. Then commit, and release 0.2.1 when they ask. Open: which furniture words are wrong, whether search view focus matches what the game's A finds, the 9 failing routes, the first real money total. The Go to list still covers Luigi's room only.
   Also open: 0.2.0 is the latest release, so the updater offers none of this. Item pickups after the "Ghost caught" fix and a real ghost capture still need confirming in play.
1. The user tests ghost sounds and banana peel warnings on real ghosts, and says whether ghost aim is accurate enough.
2. Confirm auto interact and the Go to menu open a real chest, and that routes work in larger rooms, stairs and rooms with moved furniture.
3. Check the area-clear count and game over screens in real play.
4. Status on demand, then spoken navigation.
5. Cutscene audio descriptions (below).

## Planned: cutscene audio descriptions

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
- The Game Boy Horror's map zoom is not spoken.
- Stick-press controls do not exist on a GameCube controller.
