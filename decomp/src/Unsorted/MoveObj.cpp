#include "Unsorted/MoveObj.hpp"
#include "macros.h"

dummy_float_data();
enemies_float_data();

MoveObj::MoveObj() {
    _size = true;
}

MoveObj::~MoveObj() {
    
}

void MoveObj::fn_80067CB0(double var1, double var2, double var3) {
    _vec44.set(var1, var2, var3);
    _vec5c.set(_vec44);
    _vec50.set(_vec5c);

    Vec vec;
    vec.x = _vec44.x;
    vec.y = _vec44.y;
    vec.z = _vec44.z;
}
