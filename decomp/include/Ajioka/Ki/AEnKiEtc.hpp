#ifndef A_EN_KI_ETC_HPP
#define A_EN_KI_ETC_HPP

#include "JSystem/JAudio/JAInterface/JAIAnimation.hpp"
#include "Sato/EnemyStrategy.hpp"

class AEnKiEtc : public EnemyStrategy {
public:
    AEnKiEtc();
    virtual ~AEnKiEtc();

    // From EnemyStrategy
    virtual void doBehavior();
    virtual void doBehaviorInit();

private:
    /* 0x14 */ u32 m_14;
    /* 0x18 */ u32 m_18[32];
    /* 0x98 */ JAIAnimeSound m_JAIAnimeSound;
};

#endif
