#pragma once
// LM's asynchronous file loader (retail: Unsorted/LMDvdFile, 80006DF0,
// LMDvdFileBlock). A ring of 64 file slots is serviced by a DVD reader thread;
// callers either block on the slot's message queue or receive a callback when
// the read (and optional Yay0 decompression / archive mount) completes.

#include <dolphin/dvd.h>
#include <dolphin/os.h>

class JKRMemArchive;

namespace lm {

struct DvdFileInfo;
using DvdFileCallback = void (*)(void* data, DvdFileInfo* info);

// Header of a loaded file buffer; Yay0 data starts with {'Yay0', decompSize}.
struct DvdFileBlock {
    u32 magic;   // big-endian in file data
    u32 length;  // big-endian in file data

    u32 magicHost() const;
    s32 decompressedSize() const;               // size() in retail: length rounded to 32
    void decompressYay0(DvdFileBlock* dst);     // fn_800071D0
};

struct DvdFileInfo : DVDFileInfo {
    OSMessageQueue doneQueue;  // 0x3C
    OSMessage doneMsg;         // 0x5C
    s32 readSize;              // 0x60 length rounded up to 32
    DvdFileBlock* data;        // 0x64 raw file contents
    DvdFileBlock* decompressed;// 0x68
    JKRMemArchive* archive;    // 0x6C
    s32 userArg;               // 0x70
    DvdFileCallback onLoaded;  // 0x74 user callback
    DvdFileCallback postLoad;  // 0x78 internal (decompress/mount)
    DvdFileInfo* next;         // 0x7C ring link
    u8 heapGroup;              // 0x80
    s32 ownsBuffer;            // 0x84
};

constexpr int kMaxFileInfos = 64;

void initDvdFiles();                       // fn_800065A8 (also starts the reader thread)
void* dvdFileOpenSync(const char* path, DvdFileBlock* dst);                      // LMDvdFileOpen
void* dvdFileOpenAsync(const char* path, DvdFileCallback cb, DvdFileBlock* dst, s32 arg);  // LMDvdFileOpen
void* dvdFileOpenYay0Archive(const char* path, DvdFileCallback cb, DvdFileBlock* dst, s32 arg);  // fn_800067D0
void* dvdFileOpenYay0Sync(const char* path, DvdFileBlock* dst);                   // fn_80006930
JKRMemArchive* openMemArchive(const char* path, void* dst);                       // LMOpenMemArchive
bool dvdFilesPending();                    // LMDvdFile::checkCurUnk0
void checkDvdStatus();                     // LMDvdFile::checkCurDVDStatus

extern BOOL gDvdFatalError;   // lbl_804D8028
extern s32 gDvdErrorShown;    // lbl_804D80AC

// --- Secondary loader thread (retail: Unsorted/80007018) ---
// Runs one function at a time in the background, e.g. a scene's init().
using LoaderFunc = void (*)();
void initLoaderThread();                   // fn_80007018
void runOnLoaderThread(LoaderFunc fn);     // fn_80007040
bool loaderThreadDone();                   // fn_80007090

}  // namespace lm
