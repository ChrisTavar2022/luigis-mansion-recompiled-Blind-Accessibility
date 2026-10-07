#ifndef A_EN_NO_MOVE_2_HPP
#define A_EN_NO_MOVE_2_HPP

#include "Ajioka/Kagu/AEnKaguBase.hpp"
#include "Koga/CharacterEventObserver.hpp"

class AEnNoMove2 : public AEnKaguBase {
public:
    AEnNoMove2();
    virtual ~AEnNoMove2();

    // From AEnKaguBase -> EnemyStrategy
    virtual void doBehavior();
    virtual void doBehaviorInit();

    
    // From AEnKaguBase -> Koga::CharacterEventObserver
    virtual bool onSprayedWithWater(Koga::CharacterEvent* msg);
    virtual bool onSprayedWithFire(Koga::CharacterEvent* msg);
    virtual bool onSprayedWithIce(Koga::CharacterEvent* msg);

    // Ptmf table
    bool state_0_Init();
    bool state_0_Behavior();
    
    bool state_2_Init();
    bool state_2_Behavior();
};

#endif
