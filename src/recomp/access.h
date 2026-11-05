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
// Right stick press says Luigi's status, and repeats the last announcement where that does not apply.
// Left stick press opens the Go to menu (on release: it may be the start of pressing both). True when
// both sticks were pressed in: the control that opens and closes the options menu.
bool handleEvent(const SDL_Event& ev, bool goToAllowed = true);

// What to say for the item just drawn, instead of its label.
void itemSpeech(const std::string& text);
// Said in front of the next focused item, in one announcement, so the item does not cut it off. Said
// alone if no item takes focus soon. Render thread only (the ImGui frame).
void beforeNextItem(const std::string& text);
// For a focused slider: -1 / +1 when the D-pad or left stick went left / right, else 0. Takes them
// from ImGui's navigation so they change the value.
int sliderKeys();

// Speaks the game's own text. Call once per frame while the game runs.
void pollGame();
bool gamePaused();  // the game's pause screen is running
// Rises each time the game goes back to the title or file screens: what was learnt in one game (rooms
// visited, hints said, walls opened) is cleared when it changes. Safe from any thread.
std::uint32_t runCount();
// While Luigi is free: says the next of health, total money and location. False anywhere else.
bool sayStatus();
void luigiDown(std::uint32_t actor);  // his health was just set to 0
void ghostCaught(std::uint32_t type);  // Luigi's capture message: the type caught
std::string ghostWord(std::uint32_t type);  // what the capture speech calls this type
std::string plaqueName(int plaque);  // the gallery's plaques, 1 to 23: the ghost once caught
void galleryPlaque(int plaque);      // a plaque was opened: says the portrait
void listScreen(std::uint32_t screen);  // the Game Boy Horror's list screen, each update
std::string doorName(std::uint32_t room);  // "Foyer door", from the room behind it; empty off the mansion

// A menu closed on A or B: the game ignores those until they are released, or the press that closed
// the menu would also act in the game, such as opening the door Luigi stands at.
void swallowPadPress();

// The audio cues, in the order of the glossary in the options menu.
enum class Cue { Chest, Key, Heart, Treasure, Money, Door, Ghost, Banana, Toad, GhostAimed, Thrown, SceneStart, SceneEnd, DialogueEnd, Npc, GhostLit, GhostHooked, Boss, Holding, CanThrow, GhostHeart, Boo, Candle, Medal, Spirit };
void playCue(Cue cue);
void noticeCue(Cue cue);  // the scene and dialogue cues, on their own channel
void noticeTick();        // once per frame: plays the next one waiting

// Object sounds, auto interact and walking. Call from PADRead: button and the stick may change.
void worldFrame(std::uint16_t& button, std::int8_t& stickX, std::int8_t& stickY, std::int8_t& substickX, std::int8_t& substickY, std::uint8_t& triggerLeft, std::uint8_t& triggerRight);
void noteStrategy(std::uint32_t strategy);  // an AI's state is changing: the mod keeps the bosses'
void aimThrow(std::uint32_t strategy);  // an object is leaving the nozzle: its strategy, before the game reads the direction
// A hit on Luigi (fn_800B9124), its data at hit: true when the accessibility settings cancel it.
bool hitCancelled(std::uint32_t hit);
// A health change from fn_800B9680, called by callerLr: true when the settings cancel it.
bool damageCancelled(std::uint32_t lr, std::uint32_t callerLr);
void keyCollected(std::uint32_t room);  // a key just taken in this room: says what it opens
std::string bossStatus();  // "Chauncey, health 80" during a boss fight, else empty
void noteBooManager(std::uint32_t manager);  // the game's Boo manager, each time it updates
bool candlesUnlit();  // candles in Luigi's room wait for fire
bool hasMedal(std::uint32_t element);  // 2 fire, 3 water, 4 ice
std::string roomWord(std::uint32_t nameIndex);  // a room's name by its name index; empty for a hallway
// A message to a ghost's AI: 2 the flashlight touches it, 9 the vacuum has hooked it.
void ghostMessage(std::uint32_t msg);
// Spilled coins and bills, from their update hooks.
void noteMoney(float x, float y, float z);
// The same hooks, a grain in play: keeps its timer from running out, by the setting.
void keepMoney(std::uint32_t grain);

}  // namespace recomp::access
