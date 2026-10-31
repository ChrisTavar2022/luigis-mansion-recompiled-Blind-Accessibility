// Screen reader support: speech through Tolk and spoken, controller-driven ImGui menus
// (docs/accessibility/README.md). With no screen reader and the setting on Auto, nothing here runs.
#pragma once

#include <SDL3/SDL.h>

#include <cstdint>
#include <string>

namespace recomp::access {

// Loads Tolk and looks for a screen reader. Call once, after the settings are loaded.
void init();
// Applies the setting to ImGui. Call after Aurora created the context, then once per frame.
void sync();
bool active();

// Safe from any thread: never waits for the screen reader. interrupt stops what is being said.
void say(const std::string& text, bool interrupt = true);
// Right stick press repeats the last announcement. True when both sticks were pressed in: the
// control that opens and closes the options menu.
bool handleEvent(const SDL_Event& ev);

// What to say for the item just drawn, instead of its label.
void itemSpeech(const std::string& text);
// For a focused slider: -1 / +1 when the D-pad or left stick went left / right, else 0. Takes them
// from ImGui's navigation so they change the value.
int sliderKeys();

// Speaks the game's own text. Call once per frame while the game runs.
void pollGame();
bool gamePaused();  // the game's pause screen is running

// Object sounds and auto interact. Call from PADRead: button may get A added.
void worldFrame(std::uint16_t& button, std::int8_t stickX, std::int8_t stickY);
// Spilled coins and bills, from their update hooks.
void noteMoney(float x, float y, float z);

}  // namespace recomp::access
