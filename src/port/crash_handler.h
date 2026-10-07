#pragma once

namespace port {
// Installs abort/terminate/SEH handlers that print a symbolized stack trace to stderr.
void installCrashHandler();
}  // namespace port
