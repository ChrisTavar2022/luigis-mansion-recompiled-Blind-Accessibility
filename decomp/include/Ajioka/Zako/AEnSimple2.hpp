#ifndef A_EN_SIMPLE_2_HPP
#define A_EN_SIMPLE_2_HPP

#include "Ajioka/Zako/AEnZakoBase.hpp"

class AEnSimple2 : public AEnZakoBase {
public:
    AEnSimple2();
    virtual ~AEnSimple2();

    // From AEnZakoBase -> EnemyStrategy
    virtual bool vt_14();
    virtual void doBehavior();
    virtual void doBehaviorInit();

    // From AEnZakoBase -> CharacterEventObserver
    virtual bool onSprayedWithFire(Koga::CharacterEvent* msg);
    virtual bool onSprayedWithWater(Koga::CharacterEvent* msg);

    // From AEnZakoBase
    virtual void AEnZakoBase_fn_800F5FA8();

    // ptmf
    bool state_0_Init();
    bool state_0_Behavior();
    bool state_2_Init();
    bool state_2_Behavior();
    bool state_3_Init();
    bool state_3_Behavior();
};


#endif
