// OS/VI functions not provided by Aurora: message queues backed by host
// synchronization, and VI retrace emulation that drives the frame handoff.

#include <dolphin/os.h>
#include <dolphin/vi.h>

#include "port/frame_driver.h"
#include "port/main.h"

#include <condition_variable>
#include <memory>
#include <mutex>
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
static u32 sRetraceCount;
static VIRetraceCallback sPreRetrace;
static VIRetraceCallback sPostRetrace;
static void* sNextFrameBuffer;
static void* sCurFrameBuffer;

u32 VIGetRetraceCount() { return sRetraceCount; }
u32 VIGetNextField() { return 0; }
u32 VIGetDTVStatus() { return 0; }
void VISetBlack(BOOL) {}

void VISetNextFrameBuffer(void* fb) {
    sNextFrameBuffer = fb;
    port::frame_driver::notify_frame_presented();
}

void* VIGetNextFrameBuffer() { return sNextFrameBuffer; }
void* VIGetCurrentFrameBuffer() { return sCurFrameBuffer; }

void VIWaitForRetrace() {
    ++sRetraceCount;
    sCurFrameBuffer = sNextFrameBuffer;
    if (sPreRetrace) {
        sPreRetrace(sRetraceCount);
    }
    if (sPostRetrace) {
        sPostRetrace(sRetraceCount);
    }
    port::frame_driver::on_retrace();
}

VIRetraceCallback VISetPreRetraceCallback(VIRetraceCallback cb) {
    VIRetraceCallback old = sPreRetrace;
    sPreRetrace = cb;
    return old;
}

VIRetraceCallback VISetPostRetraceCallback(VIRetraceCallback cb) {
    VIRetraceCallback old = sPostRetrace;
    sPostRetrace = cb;
    return old;
}

}  // extern "C"
