// OS/VI functions not provided by Aurora: message queues backed by host
// synchronization, and VI retrace emulation that drives the frame handoff.

#include <dolphin/os.h>
#include <dolphin/vi.h>

#include "port/frame_driver.h"
#include "port/vi.h"
#include "port/main.h"

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace port {
std::atomic_bool g_shuttingDown{false};
}

// ---------------------------------------------------------------------------
// Message queues
// ---------------------------------------------------------------------------
namespace {

struct HostQueue {
    std::mutex mtx;
    std::condition_variable cvSpace;    // a slot became free
    std::condition_variable cvMessage;  // a message arrived
};

std::mutex& queue_map_mutex() {
    static std::mutex m;
    return m;
}

HostQueue& host_queue(OSMessageQueue* mq) {
    static std::unordered_map<OSMessageQueue*, std::unique_ptr<HostQueue>> map;
    std::lock_guard lock(queue_map_mutex());
    auto& slot = map[mq];
    if (!slot) {
        slot = std::make_unique<HostQueue>();
    }
    return *slot;
}

bool wait_for(std::condition_variable& cv, std::unique_lock<std::mutex>& lock, bool (*ready)(OSMessageQueue*),
              OSMessageQueue* mq, s32 flags) {
    if (ready(mq)) {
        return true;
    }
    if (flags != OS_MESSAGE_BLOCK) {
        return false;
    }
    cv.wait(lock, [&] { return ready(mq) || port::g_shuttingDown; });
    return !port::g_shuttingDown;
}

bool has_space(OSMessageQueue* mq) { return mq->usedCount < mq->msgCount; }
bool has_message(OSMessageQueue* mq) { return mq->usedCount > 0; }

}  // namespace

extern "C" {

void OSInitMessageQueue(OSMessageQueue* mq, OSMessage* msgArray, s32 msgCount) {
    mq->queueSend.head = mq->queueSend.tail = nullptr;
    mq->queueReceive.head = mq->queueReceive.tail = nullptr;
    mq->msgArray = msgArray;
    mq->msgCount = msgCount;
    mq->firstIndex = 0;
    mq->usedCount = 0;
    host_queue(mq);
}

int OSSendMessage(OSMessageQueue* mq, void* msg, s32 flags) {
    HostQueue& q = host_queue(mq);
    std::unique_lock lock(q.mtx);
    if (!wait_for(q.cvSpace, lock, has_space, mq, flags)) {
        return FALSE;
    }
    mq->msgArray[(mq->firstIndex + mq->usedCount) % mq->msgCount] = msg;
    ++mq->usedCount;
    q.cvMessage.notify_one();
    return TRUE;
}

int OSJamMessage(OSMessageQueue* mq, void* msg, s32 flags) {
    HostQueue& q = host_queue(mq);
    std::unique_lock lock(q.mtx);
    if (!wait_for(q.cvSpace, lock, has_space, mq, flags)) {
        return FALSE;
    }
    mq->firstIndex = (mq->firstIndex + mq->msgCount - 1) % mq->msgCount;
    mq->msgArray[mq->firstIndex] = msg;
    ++mq->usedCount;
    q.cvMessage.notify_one();
    return TRUE;
}

int OSReceiveMessage(OSMessageQueue* mq, OSMessage* msg, s32 flags) {
    HostQueue& q = host_queue(mq);
    std::unique_lock lock(q.mtx);
    if (!wait_for(q.cvMessage, lock, has_message, mq, flags)) {
        return FALSE;
    }
    if (msg) {
        *msg = mq->msgArray[mq->firstIndex];
    }
    mq->firstIndex = (mq->firstIndex + 1) % mq->msgCount;
    --mq->usedCount;
    q.cvSpace.notify_one();
    return TRUE;
}

// ---------------------------------------------------------------------------
// VI retrace emulation
// ---------------------------------------------------------------------------
}  // extern "C"

namespace {

std::mutex sViMutex;
std::condition_variable sViCv;
u32 sRetraceCount;
VIRetraceCallback sPreRetrace;
VIRetraceCallback sPostRetrace;
void* sNextFrameBuffer;
void* sCurFrameBuffer;
std::thread sViThread;
std::atomic_bool sViRunning{false};

void retrace_thread() {
    using clock = std::chrono::steady_clock;
    constexpr auto kPeriod = std::chrono::nanoseconds(16'683'350);  // 59.94 Hz
    auto next = clock::now();
    while (sViRunning) {
        next += kPeriod;
        std::this_thread::sleep_until(next);
        if (clock::now() - next > std::chrono::milliseconds(100)) {
            next = clock::now();  // debugger pause etc.: don't burst-fire missed retraces
        }
        u32 count;
        VIRetraceCallback pre, post;
        {
            std::lock_guard lock(sViMutex);
            count = ++sRetraceCount;
            sCurFrameBuffer = sNextFrameBuffer;
            pre = sPreRetrace;
            post = sPostRetrace;
        }
        // Like the hardware interrupt, callbacks run asynchronously to the game
        // thread. LM's callbacks only post to message queues (thread-safe here).
        if (pre) pre(count);
        if (post) post(count);
        sViCv.notify_all();
    }
}

}  // namespace

namespace port::vi {

void start_retrace_timer() {
    if (!sViRunning.exchange(true)) {
        sViThread = std::thread(retrace_thread);
    }
}

void stop_retrace_timer() {
    if (sViRunning.exchange(false) && sViThread.joinable()) {
        sViThread.join();
    }
    sViCv.notify_all();
}

}  // namespace port::vi

extern "C" {

u32 VIGetRetraceCount() {
    std::lock_guard lock(sViMutex);
    return sRetraceCount;
}
u32 VIGetNextField() { return 0; }
u32 VIGetDTVStatus() { return 0; }
void VISetBlack(BOOL) {}

void VISetNextFrameBuffer(void* fb) {
    {
        std::lock_guard lock(sViMutex);
        sNextFrameBuffer = fb;
    }
    // The game has finished a frame (GXCopyDisp precedes this); hand it to the host.
    port::frame_driver::present();
}

void* VIGetNextFrameBuffer() { return sNextFrameBuffer; }
void* VIGetCurrentFrameBuffer() { return sCurFrameBuffer; }

void VIWaitForRetrace() {
    std::unique_lock lock(sViMutex);
    const u32 start = sRetraceCount;
    sViCv.wait(lock, [start] { return sRetraceCount != start || port::g_shuttingDown || !sViRunning; });
}

VIRetraceCallback VISetPreRetraceCallback(VIRetraceCallback cb) {
    std::lock_guard lock(sViMutex);
    VIRetraceCallback old = sPreRetrace;
    sPreRetrace = cb;
    return old;
}

VIRetraceCallback VISetPostRetraceCallback(VIRetraceCallback cb) {
    std::lock_guard lock(sViMutex);
    VIRetraceCallback old = sPostRetrace;
    sPostRetrace = cb;
    return old;
}

}  // extern "C"
