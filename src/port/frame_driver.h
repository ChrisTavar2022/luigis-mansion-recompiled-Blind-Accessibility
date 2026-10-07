#pragma once
// Runs the game's own blocking main loop on a dedicated thread and hands each
// presented frame to the host loop (Aurora). The game thread only executes
// between the host's aurora_begin_frame() and aurora_end_frame(), so all GX
// calls happen while a host frame is open; the two threads never run at once.
//
// Timing comes from an emulated VI: a timer thread fires retrace "interrupts"
// at 59.94 Hz (callbacks + VIWaitForRetrace wakeups), exactly like the console.
// LM blocks on those retraces to hold its 30 Hz tick, so the original speed is
// preserved without the host doing any pacing of its own.
//
// The handoff point is VISetNextFrameBuffer(): the game has finished a frame
// (GXCopyDisp issued), so the game thread parks and the host submits it.

namespace port::frame_driver {

using GameEntry = void (*)();

// Starts the game thread and the VI timer. The game stays parked until the
// first run_until_present().
void start(GameEntry entry);

// Called by the host inside an open Aurora frame. Resumes the game thread and
// blocks until it presents a frame (or exits). Returns false once the game
// thread has finished.
bool run_until_present();

// Unblocks and detaches the game thread during shutdown.
void stop();

// --- Called from the game thread (VI shims) ---
void present();

}  // namespace port::frame_driver
