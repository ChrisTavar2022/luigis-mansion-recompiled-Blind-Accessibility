#ifndef A_EN_BEAM_1_HPP
#define A_EN_BEAM_1_HPP

#include "Ajioka/Beam/AEnBeamBase.hpp"

class AEnBeam1: public AEnBeamBase {
public: 
    AEnBeam1() { };
    virtual ~AEnBeam1();

    // From EnemyStrategy
    virtual bool vt_14();
    virtual void doBehavior();
    virtual void doBehaviorInit();

    // From Koga::CharacterEventObserver
    // Nothing is override

    // From AEnZakoBase
    virtual void AEnZakoBase_fn_800F5FA8();
    virtual void AEnZakoBase_fn_800F6010();

    // From AEnBeamBase
    virtual void AEnBeamBase_fn_800F6DF8();
    virtual void AEnBeamBase_fn_800F700C();
    virtual void AEnBeamBase_fn_800F7120();


    // For ptmf for EnemyStrategy
    bool state_0_Init();
    bool state_0_Behavior();
    bool state_2_Init();
    bool state_2_Behavior();
    bool state_3_Init();
    bool state_3_Behavior();
    bool state_4_Init();
    bool state_4_Behavior();
    bool state_5_Init();
    bool state_5_Behavior();
    bool state_6_Init();
    bool state_6_Behavior();
};

#endif
