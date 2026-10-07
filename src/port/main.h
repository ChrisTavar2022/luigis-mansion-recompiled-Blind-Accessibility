#pragma once
#include <atomic>

namespace port {
// Set when the host is exiting; blocking OS primitives (threads, message
// queues) observe it so the game thread can be torn down without deadlocking.
extern std::atomic_bool g_shuttingDown;
}  // namespace port
