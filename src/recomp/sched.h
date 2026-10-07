#pragma once
// Guest threads (OSThread) and interrupts.
//
// Every guest thread runs on its own host thread, but only one executes guest
// code at a time: the scheduler hands the "CPU" to the highest-priority ready
// thread, exactly like the console's OS. Switches happen when a thread blocks,
// when it readies a higher-priority thread, or after an interrupt.
//
// Interrupts (VI retrace, DVD/ARQ completion...) are queued by host threads and
// delivered on the running guest thread at its next safe point (loop back-edge
// or SDK call) with interrupts enabled, or by the poster itself when the CPU is
// idle. Handlers run on a dedicated interrupt context and stack.
#include "recomp/ctx.h"

#include <functional>

namespace recomp::sched {

struct ThreadExit {};

// Makes the calling host thread the boot guest thread (OSThread at `guest`).
GuestThread* adoptBootThread(u32 guest, const Ctx& init);
void setIrqContext(const Ctx& base, u32 stackTop);

GuestThread* current();
u32 currentGuest();
Ctx& ctxOf(GuestThread* t);

// OSThread operations; `guest` is the OSThread address.
bool create(u32 guest, u32 entry, u32 arg, u32 stackTop, u32 stackSize, s32 prio, u32 attr);
s32 resume(u32 guest);
s32 suspend(u32 guest);
void cancel(u32 guest);
[[noreturn]] void exitThread(u32 val);
bool isTerminated(u32 guest);
s32 priority(u32 guest);
void detach(u32 guest);

// Blocks the current thread on a thread queue (any guest address works as a key).
void sleepOn(u32 queue);
// Readies every thread blocked on `queue`; may switch to a higher-priority one.
void wakeup(u32 queue);

// Queues an interrupt handler (runs with the interrupt context).
void postIrq(std::function<void(Ctx&)> fn);
// Raises the interrupt after `seconds` of host time (models hardware latency).
void postIrqAfter(double seconds, std::function<void(Ctx&)> fn);
bool inIrq();

void shutdown();
void dumpThreads();
void serviceOnThread(Ctx& c);

}  // namespace recomp::sched
