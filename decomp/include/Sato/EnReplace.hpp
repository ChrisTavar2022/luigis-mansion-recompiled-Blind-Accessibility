#ifndef EN_REPLACE_HPP
#define EN_REPLACE_HPP
// vtable 8035f070

#include "Sato/EnemyStrategy.hpp"

class EnReplace : public EnemyStrategy {
public:
    EnReplace() { };
    /* 0x08 */ virtual ~EnReplace();
    /* 0x14 */ virtual bool vt_14();
    /* 0x1C */ virtual void doBehavior();
    /* 0x20 */ virtual void doBehaviorInit();


    bool state_0_Init();
    bool state_0_Behavior();
    
    bool state_1_Init();
    bool state_1_Behavior();
    
    bool state_256_Init();
    bool state_256_Behavior();
};

#endif
