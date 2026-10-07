#include "Ajioka/Kagu/AEnNoMove2.hpp"

dummy_float_data()
enemies_float_data()

static EnemyStrategyState enemiesStates[2] = {
    {
        0, 
        (EnemyStrategyStateFn)&AEnNoMove2::state_0_Init, 
        (EnemyStrategyStateFn)&AEnNoMove2::state_0_Behavior
    },

    {
        2, 
        (EnemyStrategyStateFn)&AEnNoMove2::state_2_Init, 
        (EnemyStrategyStateFn)&AEnNoMove2::state_2_Behavior
    },
};


void AEnNoMove2::doBehavior() {

}
void AEnNoMove2::doBehaviorInit() {

}

bool AEnNoMove2::state_0_Init() {
    return false;
}
bool AEnNoMove2::state_0_Behavior() {
    return false;
}

bool AEnNoMove2::state_2_Init() {
    return true;
}
bool AEnNoMove2::state_2_Behavior() {
    return false;
}

bool AEnNoMove2::onSprayedWithWater(Koga::CharacterEvent* msg) {
    return false;
}

bool AEnNoMove2::onSprayedWithFire(Koga::CharacterEvent* msg) {
    return false;
}

bool AEnNoMove2::onSprayedWithIce(Koga::CharacterEvent* msg) {
    return false;
}


AEnNoMove2::~AEnNoMove2() {
    
}
