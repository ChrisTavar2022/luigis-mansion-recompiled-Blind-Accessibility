#include "Koga/EnManager.hpp"
#include "Koga/GameModeUtil.hpp"


int fn_800E85C8(s32) {}

// Does not match due to some ToolData inline shenanigans. Seems to match more as a u32 input though than u8
// https://decomp.me/scratch/PdZKG
ToolDataRef ToolDataRef::fn_800E82D8(u32 param_1) {
    ToolDataRef vRef;
    if (param_1 == -1) {
        vRef.setEntryIndex(-1);
        return vRef;
    }

    Koga::ToolData* treasureTable = Koga::GameModeUtil::getJmpResource("treasuretable");
    if (treasureTable == nullptr) {
        vRef.setEntryIndex(-1);
        return vRef;
    }

    int roomNo = treasureTable->findEntryByValue(treasureTable->searchItemInfo("room"), param_1 & 0xFF, 0);
    if (roomNo == -1) {
        vRef.setEntryIndex(-1);
        return vRef;
    }

    vRef.setToolData(treasureTable);
    vRef.setEntryIndex(roomNo);
    return vRef;
}


// Does not match due to some ToolData inline shenanigans
// https://decomp.me/scratch/iXsZ8
ToolDataRef ToolDataRef::fn_800E84CC(s32 param_1) {
    ToolDataRef vRef;
    Koga::ToolData* itemAppear = Koga::GameModeUtil::getJmpResource("itemappeartable");
    
    if (itemAppear == nullptr) {
        vRef.setToolData(nullptr);
        vRef.setEntryIndex(-1);
        return ToolDataRef(&vRef);
    }

    int entryIdx = fn_800E85C8(param_1);
    if (entryIdx > itemAppear->getDataEntryNum()) {
        vRef.setToolData(nullptr);
        vRef.setEntryIndex(-1);
        return ToolDataRef(&vRef);
    }

    // Calls upon fn_80009638() to get some value, which is probably the below fieldIdx
    int fieldIdx = 0;
    vRef.findInfoTableName(itemAppear->getStringValue(entryIdx, fieldIdx));
    return vRef;
}

// https://decomp.me/scratch/mcYqn
ToolDataRef ToolDataRef::fn_800E8658(s32 param_1, s32 param_2, s32 param_3) {
    ToolDataRef tmp;
    Koga::ToolData* itemFishing = Koga::GameModeUtil::getJmpResource("itemfishingtable");

    if (itemFishing == nullptr) {
        tmp.setToolData(nullptr);
        tmp.setEntryIndex(-1);
        return ToolDataRef(&tmp);
    }

    if (param_1 <= itemFishing->getDataEntryNum()) {
        tmp.setToolData(nullptr);
        tmp.setEntryIndex(-1);
        return ToolDataRef(&tmp);
    }

    int fieldCount = itemFishing->getJMapData()->mNumFields;
    if (fieldCount < param_3) {
        if (fieldCount <= param_2) {
            param_3 = fieldCount - 1;
        }
    } 
    
    else if (param_3 <= param_2) {
        param_2 = (param_2 - param_3) - (
            ((param_2 - param_3) / (fieldCount - param_3) * (fieldCount - param_3))) + param_3;
    }

    const char* itemName = itemFishing->getStringValue(param_2, param_3);
    if (strcmp(itemName, "-") == 0) {
        tmp.setToolData(nullptr);
        tmp.setEntryIndex(-1);
        return ToolDataRef(&tmp); 
    }

    return ToolDataRef::findInfoTableName(itemName);;
}

ToolDataRef::ToolDataRef(const ToolDataRef* pSrc) {
    setToolData(pSrc->mToolData);
    setEntryIndex(pSrc->mEntryIndex);
}

// https://decomp.me/scratch/qYZ9J
ToolDataRef ToolDataRef::findInfoTableName(const char* pName) {
    ToolDataRef temp;
    Koga::ToolData* itemInfo = Koga::GameModeUtil::getJmpResource("iteminfotable");
    int fieldIndex = itemInfo->searchItemInfo("name");
    int charFound = itemInfo->findEntryByValue(fieldIndex, pName, 0);

    if (charFound == -1) {
        temp.setToolData(nullptr);
        temp.setEntryIndex(-1);
    } else {
        temp.setToolData(itemInfo);
        temp.setEntryIndex(charFound);
    }

    return temp;
}

// Needed to match isNameValid/Money for now, otherwise it inlines this function.
// https://decomp.me/scratch/JdML7
const char* ToolDataRef::getName() {
    FORCE_DONT_INLINE;
    const char* name;

    if (isValid()) {
        getToolData()->getValue(mEntryIndex, "name", &name);
    }
    return name;
}

// Matches except one instruction. Also depends on the getName above to not inline.
// https://decomp.me/scratch/wZ68w
const BOOL ToolDataRef::isNameValid() {
    BOOL nameValid = true;

    if (isValid()) {
        if (strcmp(getName(), "nothing") != 0) {
            nameValid = false;
        }
    }

    return nameValid;
}

// Matches except one instruction. Also depends on the getName above to not inline.
// https://decomp.me/scratch/WT1gF
const BOOL ToolDataRef::isNameMoney() {
    BOOL nameValid = true;

    if (isValid()) {
        if (strcmp(getName(), "money") != 0) {
            nameValid = false;
        }
    }

    return nameValid;
}
