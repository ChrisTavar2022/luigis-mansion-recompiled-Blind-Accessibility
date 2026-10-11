#pragma once

namespace port {
// Installs abort/terminate/SEH handlers that print a symbolized stack trace to stderr.
void installCrashHandler();
// LM_HANG_DUMP=<seconds> (debugging): when the main loop has not called heartbeat() for that
// long, every thread's stack is printed to stderr (three samples a second apart, per stall).
void startHangWatchdog();
void heartbeat();
}  // namespace port
