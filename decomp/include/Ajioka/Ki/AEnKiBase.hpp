#ifndef A_EN_KI_BASE_HPP
#define A_EN_KI_BASE_HPP

#include "JSystem/JAudio/JAInterface/JAIAnimation.hpp"
#include "Sato/EnemyStrategy.hpp"

class AEnKiBase : public EnemyStrategy {
public:
    AEnKiBase();
    virtual ~AEnKiBase();

    // From EnemyStrategy
    virtual bool vt_14();
    virtual void doBehavior();
    virtual void doBehaviorInit();


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

protected:
    /* 0x14 */ void* m_14;
    /* 0x18 */ u32 m_18;
    /* 0x1c */ u32 m_1c[32];
    /* 0x9c */ JAIAnimeSound m_JAIAnimeSound;
    /* 0xfc */ // JAAAnimation ctor push its vtable here
};

#endif
