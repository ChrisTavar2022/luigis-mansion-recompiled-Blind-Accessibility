#ifndef A_EN_NO_MOVE_1_HPP
#define A_EN_NO_MOVE_1_HPP

#include "Ajioka/Kagu/AEnKaguBase.hpp"

class AEnNoMove1 : public AEnKaguBase {
public:
    AEnNoMove1();
    virtual ~AEnNoMove1();

    // From AEnKaguBase -> EnemyStrategy
    virtual void doBehavior();
    virtual void doBehaviorInit();

    bool state_0_Init();
    bool state_0_Behavior();
};

#endif
