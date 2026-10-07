// Based on the previous decompilation effort: https://github.com/CoNesTra/zmansion/blob/main/src/Unsorted/IncludeStrategy.cpp
#include "Sato/EnThought.hpp"
#include "Koga/ToolData.hpp"
#include "Sato/EnemyStrategy.hpp"
#include "Unsorted/MoveObj.hpp"
#include "macros.h"
#include "types.h"

static s32 lbl_804D2D5C = 0xff;
static s32 lbl_804D2D6C = 0xff;
static s32 lbl_804D2D70 = 0x0;
static float lbl_804DB698 = 0.f;
static s32 lbl_804D2D58 = 0x0;


IncludeStrategy::IncludeStrategy() {
    destroyStrategy();
}

IncludeStrategy::~IncludeStrategy() {
    destroyStrategy();
}

void IncludeStrategy::setStrategy(int entityIdx) {
    void* buffer = mStrategyMemory;
    destroyStrategy();
    EnemyStrategyInitFn* initFunc = *EnemyStrategy::getInitFunction(entityIdx);
    mpStrategy = reinterpret_cast<EnemyStrategy*>(initFunc(buffer, sizeof(mStrategyMemory)));
}

void IncludeStrategy::destroyStrategy() {
    EnemyStrategy* strategy = getStrategy();

    if (strategy != nullptr) {
        // This object was initialized via placement new, so we
        // can just call its destructor and invalidate its pointer.
        strategy->~EnemyStrategy();
        mpStrategy = nullptr;
    }
}


void MoveObjAlphaControl::vt_08() {
    void* obj = _18;
}

MoveObjAlphaControl::MoveObjAlphaControl() {
    _04 = lbl_804D2D58;
    _08 = lbl_804D2D5C;
    _0C.x = 1.0f;
    _0C.y = 1.0f;
    _0C.z = 1.0f;
    _18 = nullptr;
}


EnThought::EnThought() : _808(nullptr), _80C(nullptr) {
    reset();
}

void EnThought::reset() {
    float var1;
    EnemyStrategy* strategy;
    void* var3;

    _810 = 0;
    _814 = 0;
    _818 = 0;
    _928 = 0;
    _92c = 0;
    _930 = 0;
    _81c = 0;
    _820 = 0;
    _824 = 0;
    _934 = 0;
    _938 = 0;
    _89c = 0;
    _8a0 = 0;
    _890 = -1;
    _894 = 0;
    _898 = 0;
    array_828.resetSize();
    mAlphaControl._08 = lbl_804D2D6C;
    mAlphaControl._04 = lbl_804D2D70;;
    //var1 = lbl_804DB69C;
    mAlphaControl._0C.y = 1.0f;
    mAlphaControl._0C.z = 1.0f;
    //mAlphaControl._10 = lbl_804DB69C;
    //mAlphaControl._14 = var1;
    mAlphaControl._18 = _808;
    _8c8 = -1;
    _8cc = 0;
    _8d0 = 0;
    _8d4[0] = 0;
    _8e4 = lbl_804DB698;
    _8e8 = 0;
    _8EA = 0;
    _900 = 0;
    _904 = 0;
    _905 = 0;
    _906 = 0;
    _908 = 0;
    _90c = 0;
    _910 = 0;
    _914 = 0;
    _918 = 0;
    _91c = 0;
    _920 = 0;
    _924 = 0;

    destroyStrategy();

    void* var_this = _808;
    if (var_this != nullptr) {
        ToolDataRef ref = getToolDataRef();
        if (ref.isValid()) {
            u8 invisible = 0;
            ref.getToolData()->getValue(ref.getEntryIndex(), "invisible", &invisible);

            if (invisible != 0) {

            } else {

            }
        }
    }

    return;
}


EnThought::~EnThought() {
    if (getStrategy() != nullptr) {
        destroyStrategy();
    }
}


void EnThought::setToolDataRef(ToolDataRef ref) {
    mToolRef = ref;
}


ToolDataRef EnThought::getToolDataRef() const {
    ToolDataRef ref = mToolRef;
    return ref;
}
