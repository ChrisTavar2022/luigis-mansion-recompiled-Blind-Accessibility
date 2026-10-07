#pragma once
#include <string_view>

namespace setup::embedded {

std::string_view symbols(bool pal);     // decomp/config/<GLME01|GLMP01_00>/symbols.txt
std::string_view splits(bool pal);      // US only (the PAL config has no splits)
std::string_view versionMap(bool pal);  // tools/recomp/GLMP01_map.txt (PAL only)
std::string_view hooks();               // tools/recomp/hooks.txt
std::string_view ctxHeader();           // src/recomp/ctx.h
std::string_view moduleAbiHeader();     // src/recomp/module_abi.h
std::string_view moduleRt();            // src/recomp/module_rt.cpp
std::string_view recompilerSource();    // src/recompiler/recompiler.cpp (part of the cache key)

}  // namespace setup::embedded
