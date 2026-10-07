// Port of Unsorted/TitleScreenPane (0x8003A430..0x8003AF2C).
//
// Layout /scrn/testtitl.blo from res_titl.szp (staged in ARAM at boot).
// Animation states:
//   0  fade in the mansion background ('yski')
//   1  raise the logo ('logo') and fade in logo + copyright ('copy')
//   2..4  blink "PRESS START" ('strt') up and down forever
//   5  (unused by the title scene) toggle with the stick
// Two marker panes ('mpc1', 'mpc2') flash briefly on 330/430-frame cycles.
#include "game/scene/title_screen.h"
#include "game/system/aram_data.h"
#include "game/system/display.h"
#include "game/system/gx_util.h"
#include "game/system/pad.h"
#include "game/unported.h"

#include <dolphin/pad.h>
#include <dolphin/gx.h>

#include <JSystem/J2DGraph/J2DOrthoGraph.h>
#include <JSystem/J2DGraph/J2DPicture.h>
#include <JSystem/J2DGraph/J2DScreen.h>
#include <JSystem/JKernel/JKRHeap.h>
#include <JSystem/JKernel/JKRMemArchive.h>
#include <JSystem/JSupport/JSUMemoryStream.h>
#include <JSystem/JUtility/JUTGamePad.h>

namespace lm::TitleScreen {
namespace {

constexpr u64 tag(const char (&s)[5]) {
    return (u64(u8(s[0])) << 24) | (u64(u8(s[1])) << 16) | (u64(u8(s[2])) << 8) | u64(u8(s[3]));
}

// lbl_803CE278: four small color/texture holders shared with other front-end
// screens. The title only writes the alpha of #1 and #3.
struct ColorHolder {
    u32 _0, _4, _8;
    GXColor color;
    void* _10;
    void* _14;
};
ColorHolder sHolders[4];

struct TitlePane {
    s16 pending;       // 0x00
    s16 _2[9];         // 0x02..0x13 (layout constants, unused on PC)
    bool toggle;       // 0x14
    f32 logoX, logoY, logoX1, logoY1;  // 0x18 logo rect at load
    f32 logoW, logoH;  // 0x28
    bool showPressStart;  // 0x30
    s32 blink1;        // 0x34
    s32 blink2;        // 0x38
};

TitlePane* sPane;           // lbl_804D8404
JKRMemArchive* sArchive;    // lbl_804D83D8
J2DScreen* sScreen;         // lbl_804D83DC
J2DPane* sLogo;             // lbl_804D83E0 'logo'
J2DPicture* sPressStart;    // lbl_804D83E4 'strt'
J2DPane* sBackground;       // lbl_804D83E8 'yski'
J2DPane* sCopyright;        // lbl_804D83EC 'copy'
J2DPane* sMarker1;          // lbl_804D83F0 'mpc1'
J2DPane* sMarker2;          // lbl_804D83F4 'mpc2'
s32 sState;                 // lbl_804D83F8
s32 sCounter;               // lbl_804D83FC
f32 sUnused;                // lbl_804D8400

void initHolders() {  // fn_8003AEE0 (static constructor) / fn_8003BD88
    for (ColorHolder& h : sHolders) {
        h._0 = h._4 = h._8 = 0;
        h.color = {0xFF, 0xFF, 0xFF, 0xFF};
    }
}

void freeHolder(ColorHolder& h) {  // fn_8003C5E4
    (void)h;  // _10/_14 are owned objects on console; never set by the title screen.
}

void placeLogo(f32 x, f32 y, f32 w, f32 h) {
    sLogo->move(x, y);
    sLogo->resize(w, h);
}

void construct(TitlePane* pane, bool showPressStart) {  // fn_8003A430
    gResTitleAramData.beginLoad();
    sArchive = gResTitleAramData.finishLoadAndMount();
    if (sArchive != nullptr) {
        sScreen = JKR_NEW J2DScreen();
        void* blo = sArchive->getResource("/scrn/testtitl.blo");
        JSUMemoryInputStream stream(blo, sArchive->getResSize(blo));
        sScreen->setPriority(&stream, 0x20000, sArchive);

        sLogo = sScreen->search(tag("logo"));
        sPressStart = static_cast<J2DPicture*>(sScreen->search(tag("strt")));
        sPressStart->hide();
        sBackground = sScreen->search(tag("yski"));
        sCopyright = sScreen->search(tag("copy"));
        sMarker1 = sScreen->search(tag("mpc1"));
        sMarker2 = sScreen->search(tag("mpc2"));
        sMarker1->hide();
        sMarker2->hide();

        const auto& bounds = sLogo->getBounds();
        pane->logoX = bounds.i.x;
        pane->logoY = bounds.i.y;
        pane->logoX1 = bounds.f.x;
        pane->logoY1 = bounds.f.y;
        pane->logoW = bounds.f.x - bounds.i.x;
        pane->logoH = bounds.f.y - bounds.i.y;
    }

    pane->pending = 0;
    sState = 0;
    sCounter = 0;
    sUnused = 0.0f;
    pane->toggle = true;
    pane->blink1 = 0;
    pane->blink2 = 0;
    GXInvalidateTexAll();
    pane->showPressStart = showPressStart;
}

bool updatePane(TitlePane* pane, s32 mode) {  // fn_8003A6FC
    // "PRESS START" is drawn tinted red.
    sPressStart->setBlackWhite(JUtility::TColor(0, 0, 0, 0), JUtility::TColor(0xFF, 0x00, 0x00, 0xFF));

    if (mode == 0) {
        if (pane->pending == 1) {
            sState = 3;
            sCounter = 0xFF;
            pane->pending = 0;
        }
    } else if (mode == 1) {
        if (pane->pending == 0) {
            sState = 5;
            sHolders[1].color.a = 0xFF;
            sHolders[3].color.a = 0xFF;
            sLogo->setAlpha(0xFF);
            sCopyright->setAlpha(0xFF);
            placeLogo(pane->logoX, pane->logoY, pane->logoW, pane->logoH);
            pane->pending = 1;
        }
    }

    // Skip the intro animation with A or START.
    if ((gGamePad->getTrigger() & (PAD_BUTTON_A | PAD_BUTTON_START)) && sState < 2 && sState >= 0) {
        sState = 2;
        sBackground->setAlpha(0xFF);
        sLogo->setAlpha(0xFF);
        sCopyright->setAlpha(0xFF);
        placeLogo(pane->logoX, pane->logoY, pane->logoW, pane->logoH);
        sCounter = 0xF0;
    }

    switch (sState) {
    case 0:  // fade in the background
        if (++sCounter >= 0xFF) {
            sState = 1;
            sCounter = 0;
            sHolders[3].color.a = 0;
            sBackground->setAlpha(0xFF);
            sLogo->setAlpha(0);
        } else {
            sLogo->hide();
            sCopyright->hide();
            sPressStart->hide();
            sBackground->setAlpha(u8(sCounter));
        }
        break;
    case 1:  // raise the logo
        sCounter += 4;
        if (sCounter >= 0xFF) {
            sState = 2;
            sHolders[1].color.a = 0xFF;
            sHolders[3].color.a = 0xFF;
            sLogo->setAlpha(0xFF);
            sCopyright->setAlpha(0xFF);
            sCounter = 0;
        } else {
            sHolders[1].color.a = u8(sCounter);
            sHolders[3].color.a = u8(sCounter);
            sLogo->show();
            sLogo->setAlpha(u8(sCounter));
            sCopyright->show();
            sCopyright->setAlpha(u8(sCounter));
            sPressStart->hide();
        }
        break;
    case 2:
    case 4:  // "PRESS START" fading in
        if (++sCounter >= 0xFF) {
            sState = 3;
        }
        sLogo->show();
        placeLogo(pane->logoX, pane->logoY, pane->logoW, pane->logoH);
        sCopyright->show();
        if (pane->showPressStart) {
            sPressStart->show();
        }
        sPressStart->setAlpha(u8(sCounter));
        break;
    case 3:  // "PRESS START" fading out
        if (--sCounter <= 0) {
            sState = 4;
        }
        sLogo->show();
        sCopyright->show();
        if (pane->showPressStart) {
            sPressStart->show();
        }
        sPressStart->setAlpha(u8(sCounter));
        break;
    case 5:
        // Main stick up/down (retail button mask 0x0C000000).
        if (gGamePad->getTrigger() & 0x0C000000) {
            pane->toggle ^= 1;
            unported::fn_8018A2A0_playSystemSound(0x3801);
        }
        break;
    default:
        break;
    }

    if (sState >= 2 && sState < 5) {
        pane->blink1 = (pane->blink1 + 1) % 330;
        const bool on1 = pane->blink1 >= 300 && (pane->blink1 < 305 || pane->blink1 >= 325);
        on1 ? sMarker1->show() : sMarker1->hide();
        pane->blink2 = (pane->blink2 + 1) % 430;
        const bool on2 = pane->blink2 >= 400 && (pane->blink2 < 405 || pane->blink2 >= 425);
        on2 ? sMarker2->show() : sMarker2->hide();
    } else {
        pane->blink1 = 0;
        sMarker1->hide();
        pane->blink2 = 0;
        sMarker2->hide();
    }
    return sState >= 2 || sState < 0;
}

void drawPane(TitlePane* pane) {  // fn_8003ABF8
    if (sArchive == nullptr) {
        return;
    }
    defaultOrthoView();
    J2DOrthoGraph graph(0.0f, 0.0f, 640.0f, 480.0f, -1.0f, 1.0f);
    graph.setPort();

    if (sState == 1) {
        // The logo rises out of the ground: its visible height follows a quarter sine.
        const f32 height = pane->logoH * sinIdx(u16((sCounter & 0x3FF) << 6));
        const s32 h = s32(height);
        sLogo->move(pane->logoX, pane->logoY1 - f32(h));
        sLogo->resize(sLogo->getWidth(), f32(h));
    }
    sScreen->draw(0.0f, 0.0f, &graph);
}

void destroyPane() {  // fn_8003AD50
    if (sArchive == nullptr) {
        return;
    }
    for (ColorHolder& h : sHolders) {
        freeHolder(h);
    }
    if (sScreen != nullptr) {
        JKR_DELETE(sScreen);
        sScreen = nullptr;
    }
    sArchive->unmount();
    sArchive = nullptr;
}

struct StaticInit {
    StaticInit() { initHolders(); }
} sStaticInit;

}  // namespace

void create(bool showPressStart) {
    sPane = JKR_NEW TitlePane{};
    construct(sPane, showPressStart);
}

bool update() { return updatePane(sPane, 0); }

void draw() { drawPane(sPane); }

void destroy() {
    if (sPane != nullptr) {
        destroyPane();
        JKR_DELETE(sPane);
        sPane = nullptr;
    }
}

}  // namespace lm::TitleScreen
