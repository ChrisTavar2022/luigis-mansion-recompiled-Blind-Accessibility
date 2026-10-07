#ifndef A_EN_SIMPLE_1_HPP
#define A_EN_SIMPLE_1_HPP

#include "Ajioka/Zako/AEnZakoBase.hpp"

class AEnSimple1 : public AEnZakoBase {
public:
    AEnSimple1();
    virtual ~AEnSimple1();

    // From AEnZakoBase -> EnemyStrategy
    virtual bool vt_14();
    virtual void doBehavior();
    virtual void doBehaviorInit();


    bool state_0_Init();
    bool state_0_Behavior();
    bool state_3_Init();
    bool state_3_Behavior();
    bool state_4_Init();
    bool state_4_Behavior();
    bool state_5_Init();
    bool state_5_Behavior();
};


#endif
