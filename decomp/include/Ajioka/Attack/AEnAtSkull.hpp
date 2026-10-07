#ifndef A_EN_AT_SKULL_HPP
#define A_EN_AT_SKULL_HPP

#include "Koga/CharacterEventObserver.hpp"
#include "Koga/EnAttackBase.hpp"

class AEnAtSkull: public EnAttackBase {
public:
    AEnAtSkull();
    virtual ~AEnAtSkull();

    // From EnAttackbase -> EnemyStrategy
    virtual bool vt_14();
    virtual void doBehavior();
    virtual void doBehaviorInit();

    // From EnAttackBase -> Koga::CharacterEventObserver
    virtual bool onCollideWithPlayer(Koga::CharacterEvent* msg);
    virtual bool onSprayedWithWater(Koga::CharacterEvent* msg);
    virtual bool onSprayedWithFire(Koga::CharacterEvent* msg);
    virtual bool onSprayedWithIce(Koga::CharacterEvent* msg);
    virtual bool vt_18(Koga::CharacterEvent* msg);
    virtual bool onFishingBegin(Koga::CharacterEvent* msg);
    virtual bool onPlayerLeftRoom(Koga::CharacterEvent* msg);

    
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

private:
    /* 0x54 */ bool _54;
    /* 0x58 */ u8 _58[0xB];
    /* 0x60 */ u8 _60;
    /* 0x64 */ void* _64;
};


#endif
