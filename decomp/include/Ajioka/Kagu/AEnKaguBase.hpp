#ifndef A_EN_KAGU_BASE_HPP
#define A_EN_KAGU_BASE_HPP

#include "Sato/EnemyStrategy.hpp"
#include "Koga/CharacterEventObserver.hpp"

class AEnKaguBase: public EnemyStrategy, public Koga::CharacterEventObserver {
public:
    AEnKaguBase();
    virtual ~AEnKaguBase() { }

    // EnemyStrategy
    virtual void doBehavior();
    virtual void doBehaviorInit();

    // Koga::CharacterEventObserver
    virtual bool onPlayerLeftRoom(Koga::CharacterEvent* msg);
    virtual bool onSprayedWithWater(Koga::CharacterEvent* msg);
    virtual bool onSprayedWithFire(Koga::CharacterEvent* msg);
    virtual bool onSprayedWithIce(Koga::CharacterEvent* msg);
    virtual bool onPlayerTapped(Koga::CharacterEvent* msg);


    // From this class, args/return types not provided
    // PTMF for EnemyStrategy implementation
    bool state_0_Init();
    bool state_0_Behavior();
    bool state_1_Init();
    bool state_1_Behavior();
    bool state_2_Init();
    bool state_2_Behavior();
    bool state_3_Init();
    bool state_3_Behavior();

    bool isCurrentState2();
    bool fn_800F66AC();
    void fn_800F66E8();
    void fn_800F6724();
    void fn_800F6754();
    void fn_800F6778();
    void fn_800F6784();

protected:
    /* 0x18 */ int m_18;
    /* 0x1c */ int m_1c;
    /* 0x20 */ int m_20;
    /* 0x24 */ bool m_24;
    /* 0x25 */ bool m_25;
    /* 0x26 */ bool m_26;
    /* 0x27 */ bool m_27;
    /* 0x28 */ bool m_28;
};

#endif
