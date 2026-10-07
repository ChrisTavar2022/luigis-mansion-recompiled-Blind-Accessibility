#ifndef A_EN_AT_CAR_HPP
#define A_EN_AT_CAR_HPP

#include "Koga/EnAttackBase.hpp"

// .data not complete and in the wrong order because of a dynamic Cast of AEnToy in fn_800FFE88
class AEnAtCar : public EnAttackBase {
public:
    AEnAtCar();
    virtual ~AEnAtCar();

    // From EnAttackbase -> EnemyStrategy
    virtual bool vt_14();
    virtual void doBehavior();
    virtual void doBehaviorInit();

    // From EnAttackBase -> Koga::CharacterEventObserver

    // ptmf
    bool state_0_Init();
    bool state_0_Behavior();
    bool state_1_Init();
    bool state_1_Behavior();
    bool state_2_Init();
    bool state_2_Behavior();
    bool state_0x102_Init();
    bool state_0x102_Behavior();
    bool state_0x100_Init();
    bool state_0x100_Behavior();
    bool state_0x101_Init();
    bool state_0x101_Behavior();
};

#endif
