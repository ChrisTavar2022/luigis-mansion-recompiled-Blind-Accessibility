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

// ImGui::Checkbox / Button / SmallButton for the menu's options: the same widgets, plus the
// LM_MENU_CLICK test aid (menu.cpp), which clicks an option by its label at a guest frame
// through the widget's own activation path, as a player's click would.
bool Checkbox(const char* label, bool* v);
bool Button(const char* label);
bool SmallButton(const char* label);
}  // namespace recomp::menu
