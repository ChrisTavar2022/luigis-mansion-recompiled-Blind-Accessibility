#pragma once
// Developer tools in the F1 menu (Enhancements > Debug): the game's own leftover debug
// displays, a map warp list ("Visit area") and the model viewer. Findings behind them:
// docs/UNUSED_CONTENT.md ("Debug features", "Unused areas", "Model viewer").
//
// Wiring: hooks in dev_tools.cpp (tools/recomp/hooks.txt) run on the guest thread;
// unusedSection()/debugSection()/drawOverlays()/handleKey() are called from menu.cpp on the host thread,
// while the guest is parked at its present (frame.cpp), so the two never run at once.
union SDL_Event;

namespace recomp::dev {

struct MapInfo {
    int no;
    const char* name;
    bool used;        // reachable in normal play
    bool loads;       // the game can set it up (maps 7 and 8 crash while loading)
    const char* note;
};
const MapInfo* maps(int* count);

// Map 2 (the mansion) entry points: the game's "luige" appear points (SETLUIGI / sGlobalLuigiAppearPoint).
struct AppearPoint {
    int point;
    int room;
    const char* where;
};
const AppearPoint* mansionAppearPoints(int* count);

// Queues a warp through the game's own map change (the event script WARP command's path):
// applied by the scene manager once the game scene is idle. appearPoint picks the Luigi
// spawn in map 2 (0 = the map's default).
void requestWarp(int map, int appearPoint = 0);
bool inGameScene();  // the game scene is running (warps possible)
int currentMap();
void noteLuigi(unsigned guestActor);  // from Luigi's per-frame update (hidden_mansion_hooks.cpp)

void unusedSection();           // Enhancements > Unused content: depth blur, unused areas
void debugSection();            // Enhancements > Debug: debug switches, Visit area, model viewer
void drawOverlays();            // every frame: game debug report text, info overlay, viewer window
bool handleKey(const SDL_Event& ev);  // F3 info overlay, F4 model viewer; true if consumed
bool capturesInput();           // a dev window wants the pad neutralized (model viewer)

}  // namespace recomp::dev
