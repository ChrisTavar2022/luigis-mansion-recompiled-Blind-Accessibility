#pragma once
// Runs the game's own blocking main loop on a dedicated thread and hands each
// presented frame to the host loop (Aurora). The game thread only executes
// between the host's aurora_begin_frame() and aurora_end_frame(), so all GX
// calls happen while a host frame is open; the two threads never run at once.
//
// The handoff point is VIWaitForRetrace(): once the game has presented a frame
// (VISetNextFrameBuffer) the next retrace wait yields to the host. Retrace waits
// without a new frame (LM waits two retraces per 30 Hz tick) are accounted as
// elapsed time so the host can pace the game at the original speed.

#include <cstdint>

namespace port::frame_driver {

using GameEntry = void (*)();

// Starts the game thread. It stays parked until the first run_until_present().
void start(GameEntry entry);

// Called by the host inside an open Aurora frame. Resumes the game thread and
// blocks until it presents a frame (or exits). Returns false once the game
// thread has finished.
bool run_until_present();

// Number of 60 Hz retraces the game waited during the last run_until_present().
uint32_t last_retrace_count();

// Unblocks and detaches the game thread during shutdown.
void stop();

// --- Called from the game thread (OS/VI shims) ---
void notify_frame_presented();
void on_retrace();

}  // namespace port::frame_driver
