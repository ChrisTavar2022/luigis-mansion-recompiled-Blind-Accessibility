# Blind accessibility

This fork makes Luigi's Mansion Recompiled playable with a screen reader. The method comes from the Accessibility Mod Template by Happy Starfish (CC BY 4.0). That template targets Unity games through MelonLoader; this port is native C++, so its rules apply here and its C# code is reference only.

Keep lists in everything under this folder: no pipe tables or layouts that depend on position.

## What is where

- `template/`: unmodified copies of the template files, for reference. Never edit them. `TEMPLATE_CLAUDE.md` and `TEMPLATE_README.md` were renamed so Claude Code does not load the template's setup interview as project instructions.
- `game-api.md`: this port's version of the template's game API notes: keys, hook points, game data. Only verified facts go in it.
- `project_status.md`: current milestone and next steps.

Left out of the copy: the German docs, `CLAUDE.de.md`, `scripts/` and `csproj.template`, which are duplicates or MelonLoader-only.

## Rules

From the template:

- Playability, not simplification: the game plays as it does for a sighted player. Never add a cheat or shortcut without asking the user first, and only when no other approach works.
- Never guess at game code. Check the symbols, decomp and docs first, and record findings in `game-api.md`.
- Never override keys the game or port uses, or keys screen reader users rely on.
- Keep announcements short and clear, and do not repeat them. Important events announce themselves, and the last announcement can be repeated.
- Separate input, text extraction, announcements and game state, with one handler per screen or feature.
- Every piece of text a menu shows must be a real, spoken label with its current value.
- All announcement text lives in one localization table, with English as the fallback.
- Handle missing data gracefully: log it and tell the player, do not fail silently.

Specific to this project:

- The game is played with a controller, and accessibility features are controller-driven too. Do not add keyboard controls for them. The port's existing keyboard bindings stay as they are.
- Accessibility turns on automatically when a screen reader is detected. The setting is Auto, On or Off, default Auto, and a player who chooses Off stays off.
- With no screen reader and the setting on Auto, the game must behave exactly as it does today.
- The first-run disc setup must work without sight, because it comes before everything else.
- Speech never runs on a guest thread. A guest thread that stays in a host call stalls the game (docs/RECOMPILER.md), so guest code only queues text and a separate thread speaks it.
- Hooks follow the port's rules: `tools/recomp/hooks.txt` plus `HLE(name)`, US first, with PAL added through `VA()`/`VOFF()` or gated by `port::usCodeFeature()`.
- Non-Windows builds compile without Tolk and the speech layer does nothing there until a Linux backend exists.
- Test with a scratch APPDATA and `LM_CARD_DIR`, `LM_OFFSCREEN=1`, and an announcement log so runs can be checked without listening. The user's NVDA testing has the final word.
- Every automated run sets `LM_ACCESS=1` (runs silently) or `LM_ACCESS=0` (off), because on Auto a test would speak over the user's NVDA. Use `LM_ACCESS_LOG=<file>` to read announcements and `LM_VIRTUAL_PAD` for a scripted controller.

## Controls

These work in the options menu and on the setup screens:

- D-pad or left stick: move between items.
- D-pad or left stick left and right on a slider: change its value.
- A: press, toggle or open.
- B: close a list, or close the options menu.
- Right stick press: repeat the last announcement.
- Left stick press: open or close the Go to menu. It acts on release, because it may be the start of pressing both.
- Both sticks pressed in: open or close the options menu.

## How the spoken menus work

`src/recomp/access.cpp` needs no code per option. ImGui's gamepad navigation moves the focus, ImGui's log holds the text every widget draws, and the item hooks in `src/imgui_hooks/` mark where the focused item and its tooltip are in that log. Text with no control, such as notes between options, cannot take focus and is not read. Disabled options cannot take focus either.

## Accessibility tab

The options menu opens on an Accessibility tab with the mod's settings and an audio cue glossary. Pressing a glossary entry plays that cue.

## Go to menu

`src/recomp/access_nav.cpp`. The left stick opens a list of what is in Luigi's room, by category: doors, coins and money, hearts, keys, chests, treasure, Toads. D-pad left and right change category, up and down move through it, A chooses, B closes.

Luigi then walks there by himself. The route comes from the map's own collision, `col.mp` on the disc, and stays inside the room. The walk replaces the stick input, so moving the stick takes control back. Doors, chests and Toads get an A press on arrival. If something blocks him he marks that spot and plots again, up to four times.

## Object sounds and auto interact

`src/recomp/access_world.cpp` scans for chests, keys, hearts, treasure, spilled money, doors, Toads, showing ghosts and banana peels on the floor in Luigi's room. The sounds and auto interact read only that list, and auto interact acts only on chests, keys and doors, so it cannot touch furniture or anything on walls. All are settings in the options menu's Accessibility tab: object sounds (which include banana peels) and ghost sounds default on, auto interact default off.

## How the game's own text works

`src/recomp/access_game.cpp` runs once per frame and only reads game memory. Dialogue and choices are real text there. Title, file, Options and pause screens are pictures, so their words are written in that file and matched to the game's cursor values. Addresses and test routes are in `game-api.md`.

## How the template maps to this port

- MelonLoader and Harmony become recompiler hooks and `callOriginal`.
- The `decompiled/` folder becomes `decomp/`, `decomp/config/<ver>/symbols.txt`, `tools/recomp/addr2sym.py` and the docs in `docs/`. Most game code is not decompiled, so runtime tracing (`LM_TRACE`, `LM_WATCH`) matters more than it would in a Unity game.
- `dotnet build` becomes `tools/build.ps1`.
- `ScreenReader.cs` and `DebugLogger.cs` became `src/recomp/access.cpp`, with `LM_ACCESS_LOG` as the announcement log. `Handler.cs` became the `poll...` functions in `src/recomp/access_game.cpp`. A central `Loc` table is still to do; announcement text is English only for now.
- `Main.cs` lifecycle calls become the port's frame loop and SDL event handling.
- The Unity UI sections do not apply. The port's own menus use ImGui, which screen readers cannot see, and the game's own screens are drawn as pictures and text through J2D.

## Navigation premise

The camera faces close to one fixed direction, which makes navigation practical in a way a free camera like Untitled Goose Game's does not. Stick up can stay "north" on the player's mental map, so spoken directions and stereo audio cues can stay stable from room to room.

`src/recomp/ghost_lights.cpp` already assumes this in its test bot: "stick up = -z, right = +x". Before relying on it, measure the camera heading in every room and cutscene and list any exceptions in `game-api.md`, since stick input goes through a camera reference (Luigi +0x798).

## Build order

Done:

1. Speech core, Auto/On/Off detection and an announcement log.
2. Disc setup and the options menu, driven by controller.
3. Dialogue, choices, and the title, file, game Options and pause screens.

To do:

4. Object sounds, auto interact, and health and money speech: built, awaiting the user's play test.
5. Go to menu and pathfinding, Accessibility tab and audio cue glossary, locked door speech: built, awaiting the user's play test.
6. The rest of the game's screens: saved files, save prompt, game over, area clear, map. Room names.
7. Status on demand: health, money, ghosts, equipment.
8. Spoken room descriptions and furniture.
9. Ghost capture, then bosses.
10. Optional cutscene audio descriptions (`project_status.md`).
11. European game support: add the addresses to the version map.

## After every change

- Keep this folder and the repo's `CLAUDE.md` up to date as the work happens.
- Build, test silently, then launch the build for the user to play, with no test settings.
