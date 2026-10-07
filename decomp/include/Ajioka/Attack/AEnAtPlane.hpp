#ifndef A_EN_AT_PLANE_HPP
#define A_EN_AT_PLANE_HPP

#include "Koga/EnAttackBase.hpp"

// .data not complete and in the wrong order because of a dynamic Cast of AEnToy in fn_80100DC0
class AEnAtPlane : public EnAttackBase {
public:
    AEnAtPlane();
    virtual ~AEnAtPlane();

    // From EnAttackbase -> EnemyStrategy
    virtual void doBehavior();
    virtual void doBehaviorInit();

    // From EnAttackBase -> Koga::CharacterEventObserver

    // ptmf
    bool state_0_Init();
    bool state_0_Behavior();
    bool state_2_Init();
    bool state_2_Behavior();
    bool state_3_Init();
    bool state_3_Behavior();
    bool state_4_Init();
    bool state_4_Behavior();
    bool state_0x102_Init();
    bool state_0x102_Behavior();
    bool state_0x100_Init();
    bool state_0x100_Behavior();
    bool state_0x101_Init();
    bool state_0x101_Behavior();
};

#endif
