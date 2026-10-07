#pragma once
// Overlay settings menu (F1). See menu.cpp.
union SDL_Event;

namespace recomp::menu {
void init();                              // loads settings, applies window options
void handleEvent(const SDL_Event& ev);    // host event loop
void draw();                              // host thread, inside an Aurora frame
bool isOpen();
bool quitRequested();                     // "Quit game" chosen in the menu                            // game input is suppressed while open
}  // namespace recomp::menu
