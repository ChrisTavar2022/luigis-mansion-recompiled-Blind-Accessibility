#ifndef A_EN_BEAM_2_HPP
#define A_EN_BEAM_2_HPP

#include "Ajioka/Beam/AEnBeamBase.hpp"

class AEnBeam2: public AEnBeamBase {
public: 
    AEnBeam2();
    virtual ~AEnBeam2();

    // From SaEnZakoUtility
    virtual void vt_08();

    // From EnemyStrategy
    virtual void doBehavior();
    virtual void doBehaviorInit();

    // From Koga::CharacterEventObserver
    virtual bool onPlayerLeftRoom(Koga::CharacterEvent* msg);

    // From AEnZakoBase
    virtual void AEnZakoBase_fn_800F5F4C();
    virtual void AEnZakoBase_fn_800F6010();

    // From AEnBeamBase
    virtual void AEnBeamBase_fn_800F6DF8();
    virtual void AEnBeamBase_fn_800F700C();
    
    // From this class
    virtual void AEnBeam2_fn_800FA11C();


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
};

#endif
