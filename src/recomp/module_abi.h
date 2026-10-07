#pragma once
// Boundary between LuigisMansionRecomp.exe (the runtime) and the game module DLL that
// setup compiles from the user's main.dol (docs/SETUP.md).
//
// The module has no imports: it is linked with no C runtime and gets every runtime
// service through this table, passed to its single export lm_module_init(). The hot
// paths (guest memory access, interrupt polling, direct calls between recompiled
// functions) stay inside the module: ctx.h inlines them against module-local copies of
// g_mem/g_traceOn/g_watchOn and the runtime's interrupt flag pointer.
//
// FREESTANDING like ctx.h. Bump kModuleAbiVersion (ctx.h) whenever this struct, Ctx or
// GameModule change; setup rebuilds modules whose ABI differs.
#include "recomp/ctx.h"

namespace recomp {

struct RuntimeApi {
    u32 abiVersion;
    u32 size;  // sizeof(RuntimeApi)

    // Shared state (values copied at init; g_mem and the flags never change afterwards).
    u8* mem;
    bool* irqPending;
    bool traceOn;
    bool watchOn;

    // Runtime services used by ctx.h / generated code. fatalMessage, callMissing and
    // unimplemented do not return.
    void (*watchStore)(u32 a, u32 v, int size);
    u32 (*mmioRead)(u32 a, int size);
    void (*mmioWrite)(u32 a, u32 v, int size);
    void (*serviceIrq)(Ctx& c);
    void (*traceEnter)(u32 addr, Ctx& c);
    void (*fatalMessage)(const char* message);
    HleFn (*hleLookup)(const char* name);
    void (*hleMissing)(const char* name, u32 addr, Ctx& c);
    u32 (*mfspr)(Ctx& c, u32 spr);
    void (*mtspr)(Ctx& c, u32 spr, u32 v);
    u32 (*mftb)(Ctx& c, u32 spr);
    void (*callMissing)(Ctx& c, u32 addr);  // indirect call to an unknown address
    void (*unimplemented)(Ctx& c, u32 pc, u32 insn);

    // C library functions the compiler may call for the builtins in ctx.h.
    double (*fma)(double, double, double);
    double (*trunc)(double);
    double (*nearbyint)(double);
    double (*sqrt)(double);
};

// Exported by the module (extern "C", __declspec(dllexport)): stores the API and returns
// the module's tables, or nullptr if the ABI does not match.
using ModuleInitFn = const GameModule* (*)(const RuntimeApi* api);
#define LM_MODULE_INIT_SYMBOL "lm_module_init"

}  // namespace recomp
