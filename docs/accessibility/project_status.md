# Accessibility project status

## Release status

Beta, version 0.2.0. This is not version 1. The work is on the `accessibility-beta` branch, the default branch of the public repository. The user made it public by choice and is not advertising it yet. Making it private earlier detached it from the upstream fork network, so GitHub lists it as a standalone repository; the `upstream` remote still pulls from the original port.

The updater and release script point at this repository, never at the original port, whose builds would remove the accessibility work. Numbering: 0.x while it is a beta. Publish releases as normal releases, not prereleases, or the updater ignores them. The update check runs only in a packaged copy, not a build folder.

Final silent tests for the Go to menu build: walks to a heart, a Toad, a locked door and an unlocked door, plus a stress run with coins, hits, a ghost, a chest and auto interact on. No corruption, crash or hang. The user has not played this build yet.

## Current milestone

Spoken menus. The port's own setup screen and options menu speak and work with a controller. The game's own screens do not speak yet.

## Milestone

2026-10-30: the user, a totally blind player, cleared all of section one independently. They call the mod a complete success at this point. They recommend the Sidestep control style for blind players: the left stick then moves and aims.

2026-10-30: the user, a totally blind player, cleared the first few rooms completely independently with NVDA, the object sounds and an Xbox controller.

User feedback from that session: the game's own ghost sounds showed a ghost was close, but were not accurate enough to always aim well. Ghost sounds were built for this; the user has not played with them yet. If aim is still hard, add a distinct cue for when Luigi is facing the ghost.

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
- Ghost sounds, on by default, with their own setting: the nearest showing ghost in Luigi's room repeats a falling tone on a third channel, faster than object sounds. Tested with spawned, stationary portrait ghosts plus coins and hits: no corruption, no crash. Not yet tested on moving ghosts, or on ghosts fading out or hiding.
- Banana peel warning, under the object sounds setting: peels on the floor in Luigi's room repeat a clashing tone on a fourth channel, however close he gets. Tested with a spawned peel, not yet one thrown by a ghost or by the user.
- Sapphires, emeralds and rubies added to the treasure sounds.
- Toad beacon on a fifth channel, under the object sounds setting, with a Go to menu category.
- Go to menu and pathfinding (`src/recomp/access_nav.cpp`): left stick press, categories, routes from `col.mp` inside Luigi's room, A at doors, chests and Toads. Tested with a simulated controller: walked to a heart and collected it, walked to a locked door and an unlocked one.
- Spilled money now counts only inside Luigi's room.
- Locked doors announced from `fn_8011496C`, by hand or through the Go to menu.
- Accessibility tab in the options menu, with the settings moved there and an audio cue glossary.
- Save, erase and copy screens spoken, including the saved and erased confirmations. File screen says how to copy and erase.
- Every menu says its position: "Save, 1 of 2". Port menus count automatically, tabs in their own bar; game menus use `positioned()`.
- The press that closes a menu no longer reaches the game (`swallowPadPress`).
- Options and Go to menus stop the game for screen reader players, through the shader hold path in `main.cpp`.
- README section for players.
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

1. The user tests ghost sounds and banana peel warnings on real ghosts, and says whether ghost aim is accurate enough.
2. Confirm auto interact and the Go to menu open a real chest, and that routes work in larger rooms, stairs and rooms with moved furniture.
3. Read each file's progress and money on the file screen.
4. Speak the game over and area-clear screens.
5. Status on demand, then spoken navigation.
6. Cutscene audio descriptions (below).

## Planned: cutscene audio descriptions

The user asked for these, as in the Persona 4 accessibility mod. They are possible. The event interpreter at `0x803D2D40` shows which script runs and where its read cursor is, so a table can match a written description to an event and a point in its script, and NVDA speaks it between dialogue lines. Each description has to be written by hand from what the cutscene shows. Frame dumps can supply a first draft, and the user reviews the wording. The opening and the ending are not always event scripts, so they may need timing by scene or frame instead. Keep descriptions optional, behind their own setting.

## Known limits

- Choosing a disc opens the Windows file dialog, which needs a keyboard.
- Disabled options and plain notes in the menu are not read.
- The game's own text is USA only: the addresses are not in the version map yet, so the European game stays silent there.
- Dialogue does not say who is speaking: the name is not in the text.
- The file screen says "File 1" but not yet its progress and money, or whether it is empty.
- The copy target has no position: the game only offers free files, and their count is not read yet.
- Game choice menus move with the control stick, not the D-pad.
- Object sounds are generated tones, not the game's sounds. Starting game sounds from the port crashed the game when coins were bouncing: it drains JAudio's fixed sound lists (`docs/accessibility/game-api.md`). Never call `fn_80189F78` or `fn_8018D6C0` for this.
- Spoken object cues do not name the object; each kind has its own tone.
- A locked door gets auto interact presses too; the game's locked animation plays and NVDA says it is locked.
- Routes use one floor height: an object up or down stairs may be unreachable.
- The Go to menu lists only what is scanned now, in Luigi's room.
- Stick-press controls do not exist on a GameCube controller.
