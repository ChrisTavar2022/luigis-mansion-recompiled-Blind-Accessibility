#include "Sato/EnZako.hpp"
#include "Koga/GameModeUtil.hpp"
#include "Koga/ToolData.hpp"
#include "types.h"

dummy_float_data();
enemies_float_data();

EnZako::EnZako() {

}

EnZako::~EnZako() {
    
}


void EnZako::vt_0C() {
    _958 = _954;
    getStrategy()->update();
    fn_800C1FFC();
}


void EnZako::vt_10() {
    getStrategy()->mpZako = this;
    _954 = nullptr;
    _93C = 0.0;
    _940 = 0.0;
    _944 = 0.0;
    _948 = 0.0;
    _94C = 0;
    _950 = nullptr;
}


void EnZako::fn_800C1F38(const Koga::ToolData::JMapData* pMapData) {
    _95C.attach(pMapData);
}


bool EnZako::fn_800C1F5C() {
    ToolDataRef temp = getToolDataRef();
    const char* pathName;

    if (temp.getToolData() == nullptr) {
        return true;
    }
    else if (!(temp.getToolData()->getValue(temp.getEntryIndex(), "path_name", &pathName))) {
        return false;
    }
    else {
        _95C.attach(reinterpret_cast<Koga::ToolData::JMapData*>(Koga::GameModeUtil::getPathResource(pathName)));
    }

    return true;
}


Koga::ToolData* EnZako::fn_800C1FF4() {
    return &_95C;
}
