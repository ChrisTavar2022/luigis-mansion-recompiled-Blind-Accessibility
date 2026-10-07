cmake_minimum_required(VERSION 3.25)
project(lmport LANGUAGES C CXX VERSION 0.0.1)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(AURORA_ENABLE_DVD ON CACHE BOOL "" FORCE)
set(AURORA_ENABLE_CARD ON CACHE BOOL "" FORCE)
add_subdirectory(extern/aurora EXCLUDE_FROM_ALL)


# ---- Shared game/library compile environment -------------------------------
add_library(lm_game_headers INTERFACE)
target_include_directories(lm_game_headers INTERFACE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/libs/JSystem/include
    ${CMAKE_SOURCE_DIR}/extern/aurora/include/dolphin
    ${CMAKE_SOURCE_DIR}/extern/aurora/include
    ${CMAKE_SOURCE_DIR}/src)
target_compile_definitions(lm_game_headers INTERFACE TARGET_PC=1 AVOID_UB=1 VERSION=0 WIDESCREEN_SUPPORT=1)
# Game and JSystem headers assume global.h is the first include of every translation unit.
if (CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    target_compile_options(lm_game_headers INTERFACE "$<$<COMPILE_LANGUAGE:C,CXX>:/FIglobal.h>")
else ()
    target_compile_options(lm_game_headers INTERFACE "$<$<COMPILE_LANGUAGE:C,CXX>:-includeglobal.h>")
endif ()
set(GAME_LIBS lm_game_headers aurora::core aurora::gx aurora::gd aurora::si aurora::vi aurora::pad
    aurora::mtx aurora::os aurora::dvd aurora::card)

# ---- JSystem (ported base from Dusklight, CC0) ------------------------------
set(JSYSTEM_LIBRARIES)
foreach (_jsys JKernel JSupport JMath JUtility JGadget J2DGraph)
    file(GLOB _jsys_src CONFIGURE_DEPENDS libs/JSystem/src/${_jsys}/*.cpp)
    add_library(JSystem_${_jsys} STATIC ${_jsys_src})
    target_link_libraries(JSystem_${_jsys} PUBLIC ${GAME_LIBS})
    list(APPEND JSYSTEM_LIBRARIES JSystem_${_jsys})
endforeach ()

# Platform layer (window, main loop, frame pacing, interpolation, settings).
# Decompiled game code from decomp/ is added to this target unit-by-unit as
# each unit is made to compile against Aurora (see docs/ROADMAP.md, Phase 1).
file(GLOB PORT_SOURCES CONFIGURE_DEPENDS src/port/*.cpp src/port/os/*.cpp src/helpers/*.cpp)
file(GLOB_RECURSE GAME_SOURCES CONFIGURE_DEPENDS src/game/*.cpp)
add_executable(lmport ${PORT_SOURCES} ${GAME_SOURCES})
target_include_directories(lmport PRIVATE src)
target_link_libraries(lmport PRIVATE aurora::main ${GAME_LIBS} ${JSYSTEM_LIBRARIES})

if (WIN32)
    # Ship runtime DLLs (SDL3, Dawn, nod, libpng, zlib) next to the executable.
    # dxcompiler/dxil are loaded dynamically by Dawn's D3D12 backend, so they are
    # not part of TARGET_RUNTIME_DLLS and must be copied explicitly.
    add_custom_command(TARGET lmport POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_RUNTIME_DLLS:lmport> $<TARGET_FILE_DIR:lmport>
        COMMAND_EXPAND_LISTS)
    file(GLOB _dawn_extra_dlls "${CMAKE_BINARY_DIR}/_deps/dawn_prebuilt-src/bin/dx*.dll")
    if (_dawn_extra_dlls)
        add_custom_command(TARGET lmport POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_dawn_extra_dlls} $<TARGET_FILE_DIR:lmport>)
    endif ()
endif ()
