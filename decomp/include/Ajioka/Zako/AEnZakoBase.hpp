#ifndef A_EN_ZAKO_BASE_HPP
#define A_EN_ZAKO_BASE_HPP

#include "Koga/CharacterEventObserver.hpp"
#include "Sato/EnemyStrategy.hpp"
#include "Sato/SaEnGenUtility.hpp"

// int a[] = {0, 0, 0, 0, 0, 0, 0, 0x10000, 0, 0, 0, 0, 0, 0};

class AEnZakoBase: public SaEnZakoUtility, public EnemyStrategy, public Koga::CharacterEventObserver {
public:    
    AEnZakoBase();
    virtual void vt_08(); // inherits from SaEnZakoUtility
    virtual ~AEnZakoBase() { }

    // From EnemyStrategy
    virtual void doBehavior();
    virtual void doBehaviorInit();
    virtual bool vt_14();
    virtual void init();

    // From Koga::CharacterEventObserver
    virtual bool onPlayerLeftRoom(Koga::CharacterEvent* msg);
    virtual bool onCollideWithPlayer(Koga::CharacterEvent* msg);
    virtual bool onSprayedWithWater(Koga::CharacterEvent* msg);
    virtual bool onSprayedWithFire(Koga::CharacterEvent* msg);
    virtual bool onSprayedWithIce(Koga::CharacterEvent* msg);

    // From this class, args/return types not provided
    virtual void AEnZakoBase_fn_800F5F4C();
    virtual void AEnZakoBase_fn_800F5FA8();
    virtual void AEnZakoBase_fn_800F6010();
    virtual void AEnZakoBase_fn_800F6048();
    virtual void AEnZakoBase_fn_800F6080();
    virtual void AEnZakoBase_fn_800F60B8();
    virtual void AEnZakoBase_fn_800F5CB8();

    bool state_0_Init();
    bool state_0_Behavior();
    
    bool state_1_Init();
    bool state_1_Behavior();
};

#endif
