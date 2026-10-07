#ifndef A_EN_KI_SENTAK_HPP
#define A_EN_KI_SENTAK_HPP


#include "JSystem/JAudio/JAInterface/JAIAnimation.hpp"
#include "Sato/EnemyStrategy.hpp"
#include "Koga/CharacterEventObserver.hpp"

// Sentaku means "laundry" or "washing" if google is right
class AEnKiSentak : public EnemyStrategy, public Koga::CharacterEventObserver {
public:
    AEnKiSentak();
    virtual ~AEnKiSentak();

    // From EnemyStrategy
    virtual bool vt_14();
    virtual void doBehavior();
    virtual void doBehaviorInit();

    // From Koga::CharacterEventObserver
    virtual bool vt_48(Koga::CharacterEvent* msg);

    // ptmf for EnemyStrategy
    bool state_0_Init();
    bool state_0_Behavior();
    bool state_1_Init();
    bool state_1_Behavior();
    bool state_2_Init();
    bool state_2_Behavior();
    bool state_3_Init();
    bool state_3_Behavior();

protected:
    /* 0x18 */ u32 m_18;
    /* 0x1c */ u32 m_1c[32];
    /* 0x9c */ JAIAnimeSound m_JAIAnimeSound;
    /* 0xfc */ // JAIAnimation, same as AEnKiBase
};

#endif
