#include "Sato/SaEnGenUtility.hpp"

static float lbl_804D5FE8 = 0.73;
static float lbl_804D5FEC = 2500;
static char lbl_804D5FF0[] = "pnt0_x";
static char lbl_804D5FF8[] = "pnt0_y";
static char lbl_804D6000[] = "pnt0_z";

/**
 * Construct this vtable in 0x60 (the only diff match), same in dt
 */
SaEnZakoUtility::SaEnZakoUtility() {
    _04 = 0;
    _08 = 0;
    _0c = true;
    _41 = 0xff;
}


SaEnZakoUtility::~SaEnZakoUtility() {}

void SaEnZakoUtility::fn_80143810(void* param1, void* param2) {
    _04 = param1;
    _08 = param2;
}
