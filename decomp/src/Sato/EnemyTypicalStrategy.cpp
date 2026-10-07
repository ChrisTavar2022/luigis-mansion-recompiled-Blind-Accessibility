#include "Sato/EnemyTypicalStrategy.hpp"
#include "macros.h"

dummy_float_data()
enemies_float_data()

bool EnemyTypicalStrategy::onFishingBegin(Koga::CharacterEvent* msg) {
    EnemyStrategy* tsuri; // = fn_800E410C(unk_20, mpZako->0x808->0x3C)

    tsuri->mpZako = mpZako;
    tsuri->mpUserData = mpUserData;
    tsuri->setNextState(0);

    setTsuriStrategy(tsuri);

    return true;
}
