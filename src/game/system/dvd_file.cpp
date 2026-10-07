// Port of Unsorted/LMDvdFile, 80006DF0, 80007018 and LMDvdFileBlock.
// Based on lm-decomp's versions of these TUs.
#include "game/system/dvd_file.h"
#include "game/system/heap.h"

#include <JSystem/JKernel/JKRDecomp.h>
#include <JSystem/JKernel/JKRHeap.h>
#include <JSystem/JKernel/JKRMemArchive.h>
#include <JSystem/JUtility/JUTGamePad.h>

#include "game/system/pad.h"
#include "game/unported.h"
#include "helpers/endian.h"

namespace lm {

BOOL gDvdFatalError;
s32 gDvdErrorShown;

namespace {

constexpr u32 kMagicYay0 = 0x59617930;  // 'Yay0'
constexpr u32 kMagicRARC = 0x52415243;  // 'RARC'

struct DvdFileQueue {
    s32 pending;          // _0: reads queued but not yet completed
    s32 alignMultiplier;  // _4: buffer alignment in units of 32 bytes
    DvdFileInfo* cur;     // _8
};

DvdFileInfo sFileInfos[kMaxFileInfos];  // LMDvdFile::sFileInfoArray
DvdFileQueue sQueue;                    // LMDvdFile::sCurDvdFile

// --- Reader thread (80006DF0) ---
struct ReaderThread {
    OSThread thread;
    OSMessageQueue queue;
    OSMessage msgs[64];
    DvdFileInfo* cur;
};
ReaderThread sReader;
u8 sReaderStack[0x2000];

void* allocFileBuffer(u32 size, int align) {  // fn_80006258
    return allocFromCurrentHeap(size, align);
}

void closeFile(void* msg, DvdFileInfo* info) {  // LMDvdFile::closeFile
    sQueue.pending--;
    DVDClose(info);
    switch (DVDGetCommandBlockStatus(&info->cb)) {
    case 10:
    case -1:
        info->postLoad = nullptr;
        info->onLoaded = nullptr;
        return;
    default:
        break;
    }
    OSSendMessage(&info->doneQueue, msg, OS_MESSAGE_NOBLOCK);
}

void* readerThreadMain(void*) {
    while (true) {
        OSMessage msg;
        OSReceiveMessage(&sReader.queue, &msg, OS_MESSAGE_BLOCK);
        auto* info = static_cast<DvdFileInfo*>(msg);

        if (info->data == nullptr) {
            const u8 prevGroup = setHeapGroup(info->heapGroup);
            info->data = static_cast<DvdFileBlock*>(allocFileBuffer(info->readSize, sQueue.alignMultiplier * 32));
            setHeapGroup(prevGroup);
            info->ownsBuffer = 1;
        }

        const s32 result = DVDReadPrio(info, info->data, info->readSize, 0, 2);
        switch (result) {
        case DVD_RESULT_IGNORED:
        case -3:
        case DVD_RESULT_FATAL_ERROR:
        default:  // any successful byte count
        {
            closeFile(nullptr, info);
            const u8 prevGroup = setHeapGroup(info->heapGroup);
            if (info->postLoad != nullptr) {
                info->postLoad(info->data, info);
            }
            if (gDvdFatalError == FALSE) {
                if (info->onLoaded != nullptr) {
                    info->onLoaded(nullptr, info);
                }
                setHeapGroup(prevGroup);
                sReader.cur = sReader.cur->next;
            }
            break;
        }
        }
    }
}

void initReaderThread() {
    OSInitMessageQueue(&sReader.queue, sReader.msgs, 64);
    sReader.cur = sFileInfos;
    OSCreateThread(&sReader.thread, readerThreadMain, nullptr, sReaderStack + sizeof(sReaderStack),
                   sizeof(sReaderStack), 10, OS_THREAD_ATTR_DETACH);
    OSResumeThread(&sReader.thread);
}

void queueRead(DvdFileInfo* info) {  // LMDvdFileInfo::fn_80006E18
    OSSendMessage(&sReader.queue, info, OS_MESSAGE_BLOCK);
}

// Advances to the next free slot and resets it (LMDvdFile::init, often inlined in retail).
DvdFileInfo* nextSlot() {
    DvdFileInfo* info = sQueue.cur->next;
    while (DVDGetCommandBlockStatus(&info->cb) == DVD_STATE_BUSY) {
    }
    sQueue.cur = info;
    info->data = nullptr;
    info->decompressed = nullptr;
    info->archive = nullptr;
    info->postLoad = nullptr;
    info->onLoaded = nullptr;
    info->heapGroup = getHeapGroup();
    OSReceiveMessage(&info->doneQueue, nullptr, OS_MESSAGE_NOBLOCK);
    return info;
}

// LMDvdFile::open
void openSlot(const char* path, DvdFileBlock* dst) {
    DvdFileInfo* info = sQueue.cur;
    while (DVDGetCommandBlockStatus(&info->cb) == DVD_STATE_BUSY) {
    }
    if (!DVDOpen(path, info)) {
        OSReport("[dvd] file not found: %s\n", path);
        gDvdFatalError = TRUE;
    }
    info->readSize = (info->length + 0x1F) & ~0x1F;
    info->ownsBuffer = 0;
    info->data = dst;
    queueRead(info);
}

// fn_80006C84: replace the raw Yay0 buffer with its decompressed contents.
void decompressLoaded(void*, DvdFileInfo* info) {
    DvdFileBlock* header = info->data;
    if (header->magicHost() != kMagicYay0) {
        gDvdFatalError = TRUE;
        return;
    }
    const s32 size = header->decompressedSize();
    if (info->decompressed == nullptr) {
        setHeapGroup(info->heapGroup);
        info->decompressed = static_cast<DvdFileBlock*>(allocFileBuffer(size, 0x20));
    }
    header->decompressYay0(info->decompressed);
    JKRFreeToHeap(nullptr, header);
}

// fn_80006D80: decompress, then mount the result as an in-memory RARC archive
// into the storage that the caller pre-allocated in `info->archive`.
void decompressAndMount(void* data, DvdFileInfo* info) {
    decompressLoaded(data, info);
    if (gDvdFatalError != FALSE) {
        return;
    }
    if (info->decompressed->magicHost() != kMagicRARC) {
        gDvdFatalError = TRUE;
    } else if (info->archive != nullptr) {
        info->archive = new (info->archive)
            JKRMemArchive(info->decompressed, 0, static_cast<JKRMemBreakFlag>(info->ownsBuffer));
    }
}

}  // namespace

// --- LMDvdFileBlock ---
u32 DvdFileBlock::magicHost() const { return be32(magic); }

s32 DvdFileBlock::decompressedSize() const { return (be32(length) + 0x1F) & ~0x1F; }

void DvdFileBlock::decompressYay0(DvdFileBlock* dst) {
    JKRDecomp::decodeSZP(reinterpret_cast<u8*>(this), reinterpret_cast<u8*>(dst), be32(length), 0);
}

// --- LMDvdFile ---
void initDvdFiles() {
    initReaderThread();
    sQueue.alignMultiplier = 1;
    sQueue.pending = 0;
    sQueue.cur = sFileInfos;
    for (s32 i = 0; i < kMaxFileInfos; i++) {
        sFileInfos[i].next = &sFileInfos[(i + 1) % kMaxFileInfos];
        OSInitMessageQueue(&sFileInfos[i].doneQueue, &sFileInfos[i].doneMsg, 1);
    }
}

void* dvdFileOpenSync(const char* path, DvdFileBlock* dst) {
    DvdFileInfo* info = nextSlot();
    info->userArg = 0;
    openSlot(path, dst);
    sQueue.pending++;
    OSMessage msg;
    OSReceiveMessage(&info->doneQueue, &msg, OS_MESSAGE_BLOCK);
    return info->data;
}

void* dvdFileOpenAsync(const char* path, DvdFileCallback cb, DvdFileBlock* dst, s32 arg) {
    DvdFileInfo* info = nextSlot();
    info->onLoaded = cb;
    info->userArg = arg;
    openSlot(path, dst);
    sQueue.pending++;
    return info->data;
}

void* dvdFileOpenYay0Archive(const char* path, DvdFileCallback cb, DvdFileBlock* dst, s32 arg) {
    sQueue.alignMultiplier = -1;
    DvdFileInfo* info = nextSlot();
    if (dst != nullptr) {
        info->ownsBuffer = 0;
    }
    info->decompressed = dst;
    const u8 prevGroup = setHeapGroup(info->heapGroup);
    void* archiveStorage = allocFileBuffer(sizeof(JKRMemArchive), 8);
    setHeapGroup(prevGroup);
    info->archive = static_cast<JKRMemArchive*>(archiveStorage);
    info->postLoad = decompressAndMount;
    info->onLoaded = cb;
    info->userArg = arg;
    openSlot(path, nullptr);
    sQueue.pending++;
    sQueue.alignMultiplier = 1;
    if (dst != nullptr) {
        info->ownsBuffer = 0;
    }
    return archiveStorage;
}

void* dvdFileOpenYay0Sync(const char* path, DvdFileBlock* dst) {
    void* result = dvdFileOpenYay0Archive(path, nullptr, dst, 0);
    OSMessage msg;
    OSReceiveMessage(&sQueue.cur->doneQueue, &msg, OS_MESSAGE_BLOCK);
    return result;
}

JKRMemArchive* openMemArchive(const char* path, void* dst) {
    DvdFileInfo* info = nextSlot();
    info->onLoaded = nullptr;
    info->userArg = 0;
    openSlot(path, static_cast<DvdFileBlock*>(dst));
    sQueue.pending++;
    OSMessage msg;
    OSReceiveMessage(&info->doneQueue, &msg, OS_MESSAGE_BLOCK);
    return JKR_NEW JKRMemArchive(info->data, 0, JKRMEMBREAK_FLAG_UNKNOWN1);
}

bool dvdFilesPending() { return sQueue.pending != 0; }

void checkDvdStatus() {  // LMDvdFile::checkDVDStatus
    s32 status = DVDGetDriveStatus();
    if (gDvdFatalError != FALSE) {
        status = DVD_STATE_FATAL_ERROR;
    }
    switch (status) {
    case DVD_STATE_END:
        gDvdErrorShown = 0;
        break;
    case DVD_STATE_BUSY:
        if (DVDCheckDisk() != FALSE) {
            gDvdErrorShown = 0;
            break;
        }
        status = 3;
        [[fallthrough]];
    case DVD_STATE_FATAL_ERROR:
    case DVD_STATE_COVER_CLOSED:
    case DVD_STATE_NO_DISK:
    case DVD_STATE_COVER_OPEN:
    case DVD_STATE_WRONG_DISK:
    case DVD_STATE_RETRY:
        gGamePad->stopMotor();
        gDvdErrorShown = 1;
        break;
    default:
        break;
    }
    unported::fn_800473F8_setDiscErrorMessage(static_cast<s16>(status));
}

// --- Loader thread (80007018) ---
namespace {

struct LoaderThread {
    OSThread thread;
    OSMessageQueue startQueue;
    OSMessage startMsg;
    OSMessageQueue doneQueue;
    OSMessage doneMsg;
    LoaderFunc func;
};
LoaderThread sLoader;
u8 sLoaderStack[0x4000];

void* loaderThreadMain(void*) {
    while (true) {
        OSReceiveMessage(&sLoader.startQueue, nullptr, OS_MESSAGE_BLOCK);
        if (sLoader.func != nullptr) {
            sLoader.func();
        }
        OSSendMessage(&sLoader.doneQueue, nullptr, OS_MESSAGE_NOBLOCK);
    }
}

}  // namespace

void initLoaderThread() {
    OSInitMessageQueue(&sLoader.startQueue, &sLoader.startMsg, 1);
    OSInitMessageQueue(&sLoader.doneQueue, &sLoader.doneMsg, 1);
    OSCreateThread(&sLoader.thread, loaderThreadMain, nullptr, sLoaderStack + sizeof(sLoaderStack),
                   sizeof(sLoaderStack), 17, OS_THREAD_ATTR_DETACH);
    OSResumeThread(&sLoader.thread);
}

void runOnLoaderThread(LoaderFunc fn) {
    sLoader.func = fn;
    OSReceiveMessage(&sLoader.doneQueue, nullptr, OS_MESSAGE_NOBLOCK);
    OSSendMessage(&sLoader.startQueue, nullptr, OS_MESSAGE_NOBLOCK);
}

bool loaderThreadDone() {
    return OSReceiveMessage(&sLoader.doneQueue, nullptr, OS_MESSAGE_NOBLOCK) != FALSE;
}

}  // namespace lm
