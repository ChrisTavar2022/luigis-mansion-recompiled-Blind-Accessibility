#ifndef A_EN_BEAM_BASE_HPP
#define A_EN_BEAM_BASE_HPP

#include "Ajioka/Zako/AEnZakoBase.hpp"
#include "Sato/EnemyStrategy.hpp"

class AEnBeamBase: public AEnZakoBase {
public: 
    AEnBeamBase();
    virtual ~AEnBeamBase();

    // AEnZakoBase -> EnemyStrategy
    virtual bool vt_14();
    virtual void doBehavior();
    virtual void doBehaviorInit();

    // AEnZakoBase -> Koga::CharacterEventObserver
    // Nothing is override

    // AEnZakoBase
    virtual void AEnZakoBase_fn_800F5F4C();
    virtual void AEnZakoBase_fn_800F6010();

    // From this class. Args/return type not provided
    virtual void AEnBeamBase_fn_800F6DEC();
    virtual void AEnBeamBase_fn_800F6DF8();
    virtual void AEnBeamBase_fn_800F6EE8();
    virtual void AEnBeamBase_fn_800F700C();
    virtual void AEnBeamBase_fn_800F7120();

    
    bool state_0_Init();
    bool state_0_Behavior();
    
    bool state_1_Init();
    bool state_1_Behavior();

protected:
    /* 0x7c */ bool m_7c;
    /* 0x7d */ bool m_7d;
    /* 0x7e */ bool m_7e; // does not exist in this TU
    /* 0x7f */ bool m_7f;
    /* 0x80 */ bool m_80;
    /* 0x81 */ bool m_81;
    /* 0x82 */ bool m_82;
    /* 0x83 */ bool m_83;
    /* 0x84 */ bool m_84;
    /* 0x85 - 0xb3*/ u8 m_pad [0xb3 - 0x84];
    /* 0xb4 */ bool m_b4;
    /* 0xb5 - 0x1a7*/ u8 m_pad2 [0x1a7 - 0xb4];
    /* 0x1a8 */ bool m_1a8;
    /* 0x1a9 */ bool m_1a9;
    /* 0x1aa */ bool m_1aa;
    /* 0x1ab */ bool m_1ab;
    /* 0x1ac */ bool m_1ac;
    /* 0x1ad */ bool m_1ad; // does not exist in this TU
    /* 0x1ae */ bool m_1ae; // does not exist in this TU
    /* 0x1af */ bool m_1af; // does not exist in this TU
    /* 0x1b0 */ float m_1b0;
    /* 0x1b4 */ float m_1b4;
    /* 0x1b8 */ float m_1b8;
    /* 0x1bc */ float m_1bc;

    /* 0x1c0 */ int m_1c0;
    /* 0x1c4 */ int m_1c4;
    /* 0x1c8 */ int m_1c8;
    /* 0x1cc */ int m_1cc;
};

#endif
