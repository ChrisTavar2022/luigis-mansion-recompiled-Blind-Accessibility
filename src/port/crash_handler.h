#pragma once

namespace port {
// Installs abort/terminate/SEH handlers that print a symbolized stack trace to stderr.
void installCrashHandler();
// Logs "[hang] ..." with the current phase (setPhase) when the main thread has not called
// heartbeat() for 15 s (then every 30 s). LM_HANG_DUMP=<seconds> (Windows, debugging): also every
// thread's stack (three samples a second apart, per stall).
void startHangWatchdog();
void heartbeat();
// What the main thread is doing, for the hang log (a string literal: kept by pointer); nullptr
// while it waits on purpose (paused in the background), which is never reported.
void setPhase(const char* phase);
}  // namespace port
