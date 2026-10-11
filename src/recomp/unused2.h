#pragma once
// Second batch of restored unused content (docs/UNUSED_CONTENT.md, "Second batch"): Mario as the
// player, the poison / crawling-fear / overheat states, the bordered view, the jukebox, the
// ending pictures, the text archive, unused placements and colour variants, the spawn list, the
// particle test and the gallery. Every option is off by default (retail unchanged).
//
// Wiring: guest-thread entry points are called from existing hooks (dev_tools.cpp map setup and
// end of frame, unused_models.cpp model setup); the menu sections run on the host thread while
// the guest is parked at its present (as dev_tools.h).
#include "recomp/ctx.h"

#include <vector>

namespace recomp::unused2 {

// Guest memory for host-built data (model copies, textures, strings): host-backed, above the 24
// MiB of MEM1 the game uses (0x81820000-0x81F00000; early_luigi.cpp uses 0x81F00000+, the model
// viewer 0x81810000-0x81812000 and 0x82000000+). Bump allocator, never freed.
u32 extraAlloc(u32 size, u32 align = 32);

// ---- guest thread ----
void onMapSetup(Ctx& c);                 // fn_8000C188 hook, before the original
void onFrame(Ctx& c);                    // LMChangeFrameBuffer hook, before the original
void onModelLoaded(Ctx& c, u32 entry);   // after fn_80061C94 (model registry entry filled)

// play_as_mario.cpp
void marioModelLoaded(Ctx& c, u32 entry);
void marioFrame(Ctx& c);
// luigi_states.cpp
void statesFrame(Ctx& c);
void statesMapSetup(Ctx& c);
bool playerHeld();  // player 1's pad is neutralised (poisoned); any thread
// jukebox.cpp
void jukeboxFrame(Ctx& c);
// spawn.cpp
void spawnFrame(Ctx& c);
void spawnSection(bool inGame);  // host
// gallery.cpp
void endingFrame(Ctx& c);
void galleryButton();   // host
void galleryWindow();   // host
// text_archive.cpp
void textArchiveButton();  // host
void textArchiveWindow();  // host
std::vector<u8> buildLabEvent26();
// Disc overlay (hle_dvd.cpp): files served rebuilt while an option needs them (virtual FST
// entries 0x50000000+).
s32 overlayEntry(const char* guestPath);  // -1 when not redirected
bool isOverlayEntry(s32 entry);
u32 overlayLength(s32 entry);
s32 overlayRead(s32 entry, void* dst, s32 length, s32 offset);
// placements.cpp
std::vector<u8> buildMapArchive2();  // /Map/map2.szp with the enabled placements / variants
u32 mapArchive2Key();                // 0 when no option needs a rebuilt map 2
std::vector<u8> buildMapArchive3();  // /Map/map3.szp (training room) with the unused generators
u32 mapArchive3Key();
u32 resourceRedirect(const char* path);  // guest path to load instead (colour variant .prm), or 0
void placementsMapSetup(Ctx& c);
void placementsFrame(Ctx& c);

// ---- host thread (menu.cpp / dev_tools.cpp) ----
void unusedSection();    // Enhancements > Unused content
void gameplaySection();  // Enhancements > Gameplay
void debugSection();     // Enhancements > Debug
void soundSection();     // Sound > Jukebox
void drawOverlays();     // windows that stay open (text archive, gallery)

}  // namespace recomp::unused2
