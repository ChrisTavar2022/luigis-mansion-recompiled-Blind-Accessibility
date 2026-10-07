#ifndef A_EN_AT_UPPER_HPP
#define A_EN_AT_UPPER_HPP

#include "Koga/EnAttackBase.hpp"
class AEnAtUpper : public EnAttackBase {
public:
    AEnAtUpper();
    virtual ~AEnAtUpper() {};

    // From EnAttackbase -> EnemyStrategy
    virtual bool vt_14();
    virtual void doBehavior();
    virtual void doBehaviorInit();

    // From EnAttackBase -> Koga::CharacterEventObserver
    virtual bool onCollideWithPlayer(Koga::CharacterEvent* msg);

    // ptmf
    bool state_0_Init();
    bool state_0_Behavior();
    bool state_1_Init();
    bool state_1_Behavior();
    bool state_2_Init();
    bool state_2_Behavior();
    bool state_3_Init();
    bool state_3_Behavior();
    bool state_4_Init();
    bool state_4_Behavior();
    bool state_5_Init();
    bool state_5_Behavior();
    bool state_0x102_Init();
    bool state_0x102_Behavior();
    bool state_0x100_Init();
    bool state_0x100_Behavior();
    bool state_0x101_Init();
    bool state_0x101_Behavior();
};

#endif
