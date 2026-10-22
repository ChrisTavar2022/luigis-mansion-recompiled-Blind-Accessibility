# Options audit (October 2026, after v1.0)

Every optional toggle outside three areas handled elsewhere (Play as Mario / Mario as player 2 and
the model viewer, the Gameplay difficulty sliders, ambient occlusion) was played where it naturally
happens, switched on and off live through the F1 menu (the `LM_MENU_CLICK` test aid below clicks the
menu's own widgets), at a fresh start and mid-game, across map changes and a save/load cycle. Runs
used the user's real settings (Enhanced lighting Ultra, interpolated presentation, 16:9 window,
`out/play3/settings.ini`), with 4:3 and Enhanced lighting off as cross-checks where the result
depended on them. US disc. Evidence (frame dumps, window captures, logs) stayed in the agent's
scratch directory.

Status: **OK** works as described, nothing found; **fixed** a problem was found and fixed (commit);
**note** works, with a remark.

## Unused content

| Option | Status | What was checked / found |
|--------|--------|---------------------------|
| Game Boy Horror camera | OK | D-pad Up on/off in the nursery; menu off while the camera was up returns the retail screen; GBH map/Y afterwards fine |
| Game Boy Horror E3 clock | OK | D-pad Left on/off, exclusive with the camera |
| Game Boy Horror ghost tracker | OK | D-pad Down cycles off / far / close |
| Game Boy Horror map: full Luigi model | OK | loads from the old guide-map archive at the next map load |
| Early Luigi model | OK | nursery, lab events after a save load; reverts at the next map load |
| Air and water nozzles | OK | red `wpair` nozzle while blowing; turning it off mid-spray re-types the nozzle to retail at once |
| Early Poltergust 3000 | OK | E3 canister in the nursery and the lab; retail tank after off + map change |
| Ghost shading (Retail / Beta) | **fixed** | the Beta tooltip said ghosts are drawn directly, solid and opaque (the dev-only `LM_EARLY_GHOSTS` path); the option is the beta lighting cheat (brighter, flatter ghost lighting, glowing ghost layer kept). Tooltip and docs corrected (a5f3367). Checked with gold ghosts, switched live |
| Early Bogmire model | OK | map 13 fight with the early model, then off + warp back: retail Bogmire |
| Portrificationizer: each ghost's own model | OK (not re-played) | table patch written only over the retail value, restored when off (code check); verified earlier |
| Green Toad (kinopio2) | **fixed** | turned off mid-map, the character type kept the green texture set until the next map load (Toads set up later in that map came out green). The type follows the option at once now (158170e) |
| Beta skybox | OK | redirect fires at the mansion's sky load (map loads with all options on) |
| Heart door / Star door | OK | heart door in the Nursery hallway; off + warp into the Nursery: retail door |
| Unused ghost placements | OK | map 2 rebuilt (13 enemy rows, 1 character row), loads with all options on, after a save load too |
| Pink mice / Blue Ghost Guys | OK | type sets and `.prm` redirects applied at map setup |
| Extra Speedy Spirits 16-20 | OK | 5 enemy, observer and reward rows added; loads (catching one not played) |
| 2-player mode (unfinished) | OK (sanity) | on: second Luigi on port 2, shared letterboxed view, no crash; off + map change: one player, retail |
| Co-op improvements | not re-tested | covered by its own checklist (docs/UNUSED_CONTENT.md) |
| Bordered view | **fixed** | three problems. (1) At 16:9 the view's later passes (ghost layer, effects) set the bordered rectangle outside the HUD scope and drew into the raw one, so particles and the beam spilled into the right/left border (b1bb338). (2) The realistic flashlight, bloom and AO composite covered the whole screen, so the beam ran past the border even at 4:3 (Aurora 0225, 98fb840). (3) With the Game Boy Horror camera also on, the camera's view kept a stale rectangle and covered the whole screen, the HUD gone (51ca242). Now the 3D, particles and the beam stay inside the border at 4:3 and 16:9 with Enhanced lighting, and the GBH camera shows on the GBH screen |
| Ending pictures of Luigi | OK | picture loads for the rank (`LM_TEST_ENDING`); switched off live during the ending |
| Unused depth blur pass | OK | on/off live in the nursery |
| Unused areas (maps 0, 5, 7, 8) | OK | `Go` button of map 0 through the menu, then warps 5, 7, 8 and back to 2; each walkable |
| Unused rooms (6 leftovers) | OK | all six visited in sequence (lab E3, Portrificationizer pre-E3, breaker room, 1F/2F hallway duplicates, room01A); each restores the retail model on the next warp |

## Enhancements > Gameplay (unfinished Luigi states)

| Option | Status | What was checked / found |
|--------|--------|---------------------------|
| Poison mushrooms poison Luigi | OK | 25 HP over 5 s, controls held, ends normally; in a ghost room |
| Crawling fear | **fixed** | turning the option off while Luigi was panicking restored the retail mode-4 health factor (the cut code halves Luigi's health every frame in that mode) and the panic effect flood while he was still in mode 4. The fixes now stay until the panic ends (bbaa995). Real ghost startles from behind trigger it; health unchanged |
| Vacuum overheat | OK | meter fills, Luigi catches fire, vacuum locked; turned off while overheated: meter gone, vacuum works at once |

## Enhancements > General

| Option | Status | What was checked / found |
|--------|--------|---------------------------|
| Show FPS | OK | overlay on/off live (window capture) |
| Pause when window loses focus | not testable | automated runs never pause by design |
| Skippable cutscenes | OK | the "known broken" state could not be reproduced: the opening, the foyer entrance event and E. Gadd's GBH call were skipped with START; the skip suspends at the save prompt with the game visible and waits for the player. Snapshots of watched vs skipped runs had matched before (docs/ENHANCEMENTS.md §4). The interpolation scenario script still avoids it ("slow and can get stuck"); no stuck case was found here |
| Hold Tab to fast-forward | note | keyboard-only, not scriptable; the same fast path is used by the skip and `LM_FF` |
| Fix rooms vanishing near doorways | OK | switched off/on live during the foyer entrance cutscene |
| Hidden Mansion version | not re-tested | covered by docs/HIDDEN_MANSION.md |

## Enhancements > Debug

| Option | Status | What was checked / found |
|--------|--------|---------------------------|
| Particle manager report (JUTReport) | OK | lines drawn by the overlay (window capture), off live |
| Info overlay (F3) | OK | map/room/position shown |
| Debug mode (Luigi debug flags) | OK | on/off live, the game's value put back |
| Debug info display / GBH scan boxes / XYZ axes | OK | on/off live |
| HUD test (Icons / Numbers / Element meter) | OK | all three modes, then off |
| Font test + Latin last line | OK | kana/kanji lines, "alphabet 123" with the fix, off live |
| Skip entry cutscenes (warps) | OK | entry events held back on every warp of the map run |
| Visit area (14 maps, 10 entry points) | OK | used throughout the runs above |
| Gallery / Text archive | OK | opened and closed through their buttons |
| Spawn character | OK | four types spawned in the nursery |
| Particle test | OK | `kome_A`, `elh_fire_a`, `candle_yure` started |

## Visuals / Sound

| Option | Status | What was checked / found |
|--------|--------|---------------------------|
| CRT filter | OK | on (phosphor mask and scanlines in the window capture) and off live |
| CRT signal (S-Video / composite / RF) | OK | all four on a frozen title and foyer frame (`LM_CRT_CYCLE`, `LM_FREEZE_AT`; `out/crt_signals/`), 240 lines + comb and 480i; off unchanged |
| Jukebox | OK | Play / Stop through the menu buttons (`LM_JUKEBOX_UI`), music id follows |

## Visuals > General > Menu size (added after 1.1.2, issue #20)

"Menu size" (`[interface] menu_scale` in settings.ini: 0 = Auto, else 75-300 %) sizes the F1 menu and
every other ImGui window the port draws. Steps 75, 100, 125, 150, 175, 200, 250, 300 %. It sits at
the top of Visuals > General, the page the menu opens on, so a player who finds the menu too small
on a TV sees it first.

How it works (`src/recomp/ui_scale.cpp`): Dear ImGui here is 1.91.9 (fixed-size font atlas), so
each size rebuilds the atlas with the same ProggyClean font at round(13 x size) pixels and replaces
the style with ImGui's default style scaled once (`ScaleAllSizes` on a clean copy, never
cumulative). Text is rasterized at the size it is shown at, not a stretched 13 px atlas. Both
happen before `ImGui::NewFrame` through Aurora patch 0238 (`aurora_imgui_set_frame_callback`; the
atlas is locked during a frame; the backend's device objects are recreated only after the previous
frame's ImGui draw finished on the render worker). Auto: the largest of window height / 1080
(4K = 200 %), the display's scaling setting (Windows 150 % = 150 %) and 150 % on a Steam Deck
(`SteamDeck=1`), rounded down to a step; 1080p stays at 100 % (the old look). Any size is limited
to what still fits the window (560 x 330 units at 100 %; 1280x720 allows 200 %), with a note under
the slider. The setup screen's Auto is 600 lines = 100 % (as large as before, now sharp).

Windows: the menu's default (680 x 400) and minimum (520 x 300) size scale; a size saved in
imgui.ini is rescaled when the menu size changes (imgui.ini records the scale it was saved at,
section `[LMUiScale]`; sizes from older versions count as 100 %), and the window is kept on the
screen. The slider applies on release (dragging only changes its label); - and + apply at once, and
the menu is rescaled about the mouse so the button stays under it.

| Check | Status | What was checked / found |
|-------|--------|---------------------------|
| ID check (`LM_MENU_ID_CHECK=exit`) | OK | 0 conflicts at 100 % and at 300 % (1920x1080 window, all 12 pages) |
| Clipping / overlap | OK | Visuals, Controls (port table, input-test drawing scaled), Game at 1280x720 with 300 % chosen (200 % in effect, note shown); 1920x1080 at 100 / 150 / 250 %; 3840x2160 Auto = 200 % looks like 1080p at 100 % |
| Mouse (`LM_MENU_SIZE_TEST=1`) | OK | "+" clicked 100 -> 300 %: the mouse stays on "+" up to 200 %; at 250 / 300 % the 1700 / 2040 px wide menu is pushed back onto the 1920 px screen, so the button moves (unavoidable); "-" stays under the mouse; dragging the slider does not change the size until release |
| Other windows | OK | Show FPS, F3 info (4K Auto), "Preparing shaders" (large font, 1.5 x menu size: as large as before at 1080p, sharp), model viewer (F4) at 150 %, setup screen (1920x1080 Auto = 175 %); update / refresh-rate notices use the same font and scaled margins (not shown in a dev build) |
| Restore all defaults | OK (code check) | the settings go back to Auto; the size is read every frame, so it applies at the next frame |
| Keyboard / controller | note | not changed: the port does not enable ImGui's keyboard/gamepad navigation flags, and no input code was touched. Not exercised by the automated runs. A size change made without the mouse (e.g. Tab + arrow keys on the slider) applies when the slider is deactivated and rescales the menu about the slider |

Test aids: `LM_SETTINGS_AT=0:interface.menu_scale=<pct>`, `LM_MENU_SIZE_TEST=1` (with
`LM_MENU=Visuals`; logs `[ui-test]`), `[ui] menu size ...` log line on every change.

## Game > Updates (added after v1.1)

| Option | Status | What was checked / found |
|--------|--------|---------------------------|
| Check for updates at startup | OK | on: background check at startup, corner notice for a newer release; off: no request (`[update] startup check off`) |
| Check for updates | OK | manual check; shows a skipped version again; offline / rate limit messages |
| Download and install / Install and restart | OK | pretending 1.0: v1.0.1 downloaded, digest verified, installed, restarted into v1.0.1; bad digest, no hash, foreign URL refused; cancel; read-only folder offers the download page (docs/SETUP.md "Updates") |
| Skip this version / Later | OK | skip saved as `[updates] skip_version`, no notice at the next start; Later hides it for the session |

## Save / load and map changes

A save made with every option above on (`LM_TEST_SAVE`) loads with every option off (the lab
intro with the retail Luigi, Poltergust and E. Gadd: nothing from the options is stored in the save)
and with every option on (early Luigi and Poltergust, bordered view, training room ghosts). The map
run changed maps fourteen times with the options in use without a crash.

## Other observations (not changed)

- Arriving in the unused maps 5/7/8 Luigi faces the camera with the flashlight on; with the realistic
  flashlight the screen fills with the beam's haze for a few seconds. This is the beam seen from inside
  (looking into it), as in the foyer arrival; Enhanced lighting off shows the retail cone.
- One automated nursery run (out of five with the same input) was in Game Boy Horror search mode from
  the start of gameplay; the input never pressed X there. Not reproduced.
- Widescreen + Game Boy Horror search mode (X): the main view's later passes now follow its centered
  frame too (b1bb338), the same rule as the bordered view; the picture is the same apart from a few
  lighting pixels at the top of the GBH screen.

## Test aid: LM_MENU_CLICK

`LM_MENU_CLICK=<frame>:<action>[;<frame>:<action>...]` drives the F1 menu at guest frames
(`skip::guestFrames`): `open=<Tab>[/<Sub-tab>]`, `close` (saves like Esc), or the label of a checkbox /
button as passed to ImGui (`##` suffix included; a `###id` label matches by its id, so buttons whose
text changes can be clicked again). The widget is scrolled into view and activated through ImGui's
item activation, the same path as a mouse click; tree nodes are opened while a click waits.
`[menu-click]` lines log each step, `LM_MENU_CLICK_LOG=1` the labels seen while one waits. Implemented
in `src/recomp/menu.cpp` (`menu::Checkbox` / `Button` / `SmallButton` wrap the ImGui widgets of the
option menus). Example:
`LM_MENU_CLICK="2620:open=Enhancements/Unused content;2630:Game Boy Horror camera;2690:close"`.
