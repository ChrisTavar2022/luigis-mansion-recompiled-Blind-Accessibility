// HLE: DVD file system, on top of Aurora's disc reader. Reads complete
// synchronously; async completions are delivered as interrupts.
#include "recomp/hle.h"
#include "recomp/sched.h"

#include <dolphin/dvd.h>

#include <mutex>
#include <string>
#include <unordered_map>

namespace recomp {
namespace {

std::unordered_map<u32, DVDFileInfo> sFiles;  // guest DVDFileInfo -> host
std::unordered_map<u32, u32> sNames;          // FST entry -> guest copy of its name

DVDFileInfo* hostFile(u32 guest) {
    auto it = sFiles.find(guest);
    return it == sFiles.end() ? nullptr : &it->second;
}

void syncGuest(u32 guest, const DVDFileInfo& fi) {
    wr32(guest + 0x0C, 0);  // DVD_STATE_END
    wr32(guest + 0x30, fi.startAddr);
    wr32(guest + 0x34, fi.length);
}

bool openWith(u32 guest, BOOL ok, const DVDFileInfo& fi) {
    if (!ok) return false;
    sFiles[guest] = fi;
    syncGuest(guest, fi);
    return true;
}

}  // namespace

HLE(DVDInit) {}
HLE(DVDGetDriveStatus) { c.r[3] = 0; }
HLE(DVDCheckDisk) { c.r[3] = 1; }
HLE(DVDGetCommandBlockStatus) { c.r[3] = rd32(c.r[3] + 0x0C); }
HLE(DVDConvertPathToEntrynum) { c.r[3] = u32(DVDConvertPathToEntrynum(gstr(c.r[3]))); }

HLE(DVDOpen) {
    DVDFileInfo fi{};
    c.r[3] = openWith(c.r[4], DVDOpen(gstr(c.r[3]), &fi), fi);
}
HLE(DVDFastOpen) {
    DVDFileInfo fi{};
    c.r[3] = openWith(c.r[4], DVDFastOpen(s32(c.r[3]), &fi), fi);
}
HLE(DVDClose) {
    if (DVDFileInfo* fi = hostFile(c.r[3])) {
        DVDClose(fi);
        sFiles.erase(c.r[3]);
    }
    c.r[3] = 1;
}

HLE(DVDReadPrio) {
    DVDFileInfo* fi = hostFile(c.r[3]);
    c.r[3] = fi ? u32(DVDReadPrio(fi, hostp(c.r[4]), s32(c.r[5]), s32(c.r[6]), s32(c.r[7]))) : u32(-1);
}

HLE(DVDReadAsyncPrio) {
    const u32 guest = c.r[3], cb = c.r[7];
    DVDFileInfo* fi = hostFile(guest);
    if (!fi) {
        c.r[3] = 0;
        return;
    }
    const s32 result = DVDReadPrio(fi, hostp(c.r[4]), s32(c.r[5]), s32(c.r[6]), s32(c.r[8]));
    wr32(guest + 0x0C, 1);  // busy until the completion interrupt
    wr32(guest + 0x38, cb);
    wr32(guest + 0x20, u32(result));
    sched::postIrq([guest, cb, result](Ctx& ic) {
        wr32(guest + 0x0C, 0);
        if (cb) guestCall(ic, cb, u32(result), guest);
    });
    c.r[3] = 1;
}

HLE(DVDPrepareStreamAsync) {  // streamed audio is not emulated yet
    const u32 guest = c.r[3], cb = c.r[6];
    sched::postIrq([guest, cb](Ctx& ic) {
        if (cb) guestCall(ic, cb, 0, guest);
    });
    c.r[3] = 1;
}
HLE(DVDCancelStream) { c.r[3] = 0; }

HLE(DVDOpenDir) {
    DVDDir d{};
    const int ok = DVDOpenDir(gstr(c.r[3]), &d);
    if (ok) {
        wr32(c.r[4], d.entryNum);
        wr32(c.r[4] + 4, d.location);
        wr32(c.r[4] + 8, d.next);
    }
    c.r[3] = u32(ok);
}
HLE(DVDReadDir) {
    DVDDir d{rd32(c.r[3]), rd32(c.r[3] + 4), rd32(c.r[3] + 8)};
    DVDDirEntry e{};
    const int ok = DVDReadDir(&d, &e);
    if (ok) {
        wr32(c.r[3] + 4, d.location);
        u32& name = sNames[e.entryNum];
        if (!name) {
            const std::string s = e.name ? e.name : "";
            name = hleAlloc(u32(s.size() + 1), 4);
            std::memcpy(hostp(name), s.c_str(), s.size() + 1);
        }
        wr32(c.r[4], e.entryNum);
        wr32(c.r[4] + 4, u32(e.isDir));
        wr32(c.r[4] + 8, name);
    }
    c.r[3] = u32(ok);
}
HLE(DVDCloseDir) { c.r[3] = 1; }

}  // namespace recomp
