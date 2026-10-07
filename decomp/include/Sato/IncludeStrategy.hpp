// Based on the previous decompilation effort: https://github.com/CoNesTra/zmansion/blob/main/include/Unsorted/IncludeStrategy.h

#ifndef INCLUDE_STRATEGY_HPP
#define INCLUDE_STRATEGY_HPP

#include <types.h>

#include "Sato/EnemyStrategy.hpp"

/*
.data has lbl_8035A1D8 as part of this split, maybe EnemyStrategy, IncludeStrategy and a couple others are actually a much bigger TU?
*/
class IncludeStrategyData
{
public:
    IncludeStrategyData() { mpStrategy = nullptr; }

    /* 0x000 - 0x7FF */ u8 mStrategyMemory[0x800];
    /* 0x800 */ EnemyStrategy* mpStrategy;
};

class IncludeStrategy : IncludeStrategyData
{
    
public:
    // /* 0x000 - 0x7FF */ u8 mStrategyMemory[0x800];
    // /* 0x800 */ EnemyStrategy mpStrategy;

    IncludeStrategy();
    /* 0x804 */ virtual ~IncludeStrategy();

    void setStrategy(int);
    void destroyStrategy();

    void* getBuffer() { return mStrategyMemory; }
    EnemyStrategy* getStrategy() const { return mpStrategy; }
};


#endif
