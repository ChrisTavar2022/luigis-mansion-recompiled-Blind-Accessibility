#pragma once
// Overlay settings menu (F1). See menu.cpp.
union SDL_Event;

namespace recomp::menu {
void init();                              // loads settings, applies window options
void handleEvent(const SDL_Event& ev);    // host event loop
void draw();                              // host thread, inside an Aurora frame
bool isOpen();                            // game input is suppressed while open
bool quitRequested();                     // "Quit game" chosen in the menu
void shutdown();                          // saves changes not yet written (window closed with the menu open)
// LM_MENU_ID_CHECK sweep: tree nodes and collapsible sections in the menu call this right
// before their TreeNode() so the sweep sees (and checks) their contents too.
void openTreesForCheck();
}  // namespace recomp::menu
