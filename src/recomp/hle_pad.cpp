// HLE: controllers, via Aurora's PAD (SDL gamepads + keyboard bindings), plus
// an optional scripted input for automated testing (--input).
#include "recomp/hle.h"

#include "port/options.h"
#include "recomp/menu.h"
#include "recomp/skip.h"

#include <dolphin/pad.h>

#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

namespace recomp {
u32 viRetraceCount();  // hle_vi.cpp

namespace {

struct ScriptedPress {
    u32 start, frames;
    u16 buttons;
    s8 stickX, stickY;
};

std::vector<ScriptedPress> parseScript(const std::string& script) {
    std::vector<ScriptedPress> out;
    std::stringstream ss(script);
    std::string item;
    while (std::getline(ss, item, ',')) {
        const auto colon = item.find(':');
        if (colon == std::string::npos) continue;
        ScriptedPress p{u32(std::strtoul(item.c_str(), nullptr, 10)), 6, 0, 0, 0};
        std::string rest = item.substr(colon + 1);
        if (const auto star = rest.find('*'); star != std::string::npos) {
            p.frames = u32(std::strtoul(rest.c_str() + star + 1, nullptr, 10));
            rest.resize(star);
        }
        std::stringstream bs(rest);
        std::string b;
        while (std::getline(bs, b, '+')) {
            if (b == "A") p.buttons |= PAD_BUTTON_A;
            else if (b == "B") p.buttons |= PAD_BUTTON_B;
            else if (b == "X") p.buttons |= PAD_BUTTON_X;
            else if (b == "Y") p.buttons |= PAD_BUTTON_Y;
            else if (b == "Z") p.buttons |= PAD_TRIGGER_Z;
            else if (b == "L") p.buttons |= PAD_TRIGGER_L;
            else if (b == "R") p.buttons |= PAD_TRIGGER_R;
            else if (b == "START") p.buttons |= PAD_BUTTON_START;
            else if (b == "UP") p.buttons |= PAD_BUTTON_UP;
            else if (b == "DOWN") p.buttons |= PAD_BUTTON_DOWN;
            else if (b == "LEFT") p.buttons |= PAD_BUTTON_LEFT;
            else if (b == "RIGHT") p.buttons |= PAD_BUTTON_RIGHT;
            else if (b == "STICKUP") p.stickY = 100;
            else if (b == "STICKDOWN") p.stickY = -100;
            else if (b == "STICKLEFT") p.stickX = -100;
            else if (b == "STICKRIGHT") p.stickX = 100;
        }
        out.push_back(p);
    }
    return out;
}

void applyScript(PADStatus& st) {
    static const std::vector<ScriptedPress> script = parseScript(port::options().inputScript);
    // LM_INPUT_CLOCK=frames: time the script in guest frames, which do not depend on
    // fast-forward (cutscene skipping) the way retraces do.
    static const bool byFrames = [] {
        const char* v = std::getenv("LM_INPUT_CLOCK");
        return v && std::string(v) == "frames";
    }();
    const u32 now = byFrames ? skip::guestFrames() : viRetraceCount();
    for (const auto& p : script) {
        if (now >= p.start && now < p.start + p.frames) {
            st.button |= p.buttons;
            if (p.stickX) st.stickX = p.stickX;
            if (p.stickY) st.stickY = p.stickY;
            st.err = PAD_ERR_NONE;
        }
    }
}

u16 sLastButtons = 0;  // port 1, as last handed to the game

}  // namespace

u16 padButtons() { return sLastButtons; }

HLE(PADInit) { c.r[3] = PADInit(); }
HLE(PADReset) { c.r[3] = PADReset(c.r[3]); }
HLE(PADRecalibrate) { c.r[3] = PADRecalibrate(c.r[3]); }
HLE(PADSetSpec) { PADSetSpec(c.r[3]); }
HLE(PADSetAnalogMode) { PADSetAnalogMode(c.r[3]); }
HLE(PADControlMotor) { PADControlMotor(c.r[3], c.r[4]); }
HLE(PADSetSamplingRate) {}  // PAL release; sampling is the host's business

// PADStatus (12 bytes): button, stickX/Y, substickX/Y, triggerL/R, analogA/B, err.
HLE(PADRead) {
    PADStatus st[PAD_MAX_CONTROLLERS] = {};
    const u32 motor = PADRead(st);
    if (menu::isOpen()) {  // the overlay menu owns input while open
        for (auto& s : st) {
            const s8 err = s.err;
            s = PADStatus{};
            s.err = err;
        }
    }
    if (!port::options().inputScript.empty()) {
        applyScript(st[0]);
        static u32 sLogged = 0;
        if (st[0].button && sLogged++ < 20)
            log("[pad] retrace %u frame %u: buttons %04X err %d", viRetraceCount(), skip::guestFrames(), st[0].button, st[0].err);
    }
    skip::filterPad(st, PAD_MAX_CONTROLLERS);
    sLastButtons = st[0].err == PAD_ERR_NONE ? st[0].button : 0;
    for (int i = 0; i < PAD_MAX_CONTROLLERS; i++) {
        const u32 p = c.r[3] + 12 * i;
        wr16(p, st[i].button);
        wr8(p + 2, u8(st[i].stickX));
        wr8(p + 3, u8(st[i].stickY));
        wr8(p + 4, u8(st[i].substickX));
        wr8(p + 5, u8(st[i].substickY));
        wr8(p + 6, st[i].triggerLeft);
        wr8(p + 7, st[i].triggerRight);
        wr8(p + 8, st[i].analogA);
        wr8(p + 9, st[i].analogB);
        wr8(p + 10, u8(st[i].err));
    }
    c.r[3] = motor;
}

}  // namespace recomp
