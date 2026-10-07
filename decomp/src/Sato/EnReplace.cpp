#include "Sato/EnReplace.hpp"
#include "Sato/EnemyStrategy.hpp"
#include "macros.h"

dummy_float_data();
enemies_float_data();



static EnemyStrategyState enemyStrategyStates[3] = {
    {
        0,
        (EnemyStrategyStateFn)&EnReplace::state_0_Init, 
        (EnemyStrategyStateFn)&EnReplace::state_0_Behavior
    },

    {
        1, 
        (EnemyStrategyStateFn)&EnReplace::state_1_Init, 
        (EnemyStrategyStateFn)&EnReplace::state_1_Behavior
    },

    {
        256, 
        (EnemyStrategyStateFn)&EnReplace::state_256_Init, 
        (EnemyStrategyStateFn)&EnReplace::state_256_Behavior
    },
};

void EnReplace::doBehavior() {
    EnemyStrategyState* end = enemyStrategyStates + ARRAY_COUNT(enemyStrategyStates);
    EnemyStrategyState* current = enemyStrategyStates;
    EnemyStrategyState* found = nullptr;

    while (enemyStrategyStates != end) {
        if (enemyStrategyStates->mStateIndex == mCurrentState) {
            found = enemyStrategyStates;
        }
        current++;
    }

    if (found != nullptr)
        (this->*found->mBehaviorFunc)();
}

void EnReplace::doBehaviorInit() {
    EnemyStrategyState* end = enemyStrategyStates + ARRAY_COUNT(enemyStrategyStates);
    EnemyStrategyState* current = enemyStrategyStates;
    EnemyStrategyState* found = nullptr;

    while (enemyStrategyStates != end) {
        if (enemyStrategyStates->mStateIndex == mCurrentState) {
            found = enemyStrategyStates;
        }
        current++;
    }

    if (found != nullptr)
        (this->*found->mBehaviorInitFunc)();
}

bool EnReplace::vt_14() {
    return false;
}

bool EnReplace::state_0_Init() { return true; }
bool EnReplace::state_0_Behavior() { return true; }
bool EnReplace::state_1_Init() { return true; }
bool EnReplace::state_1_Behavior() { return true; }
bool EnReplace::state_256_Init() { return true; }
bool EnReplace::state_256_Behavior() { return true; }
