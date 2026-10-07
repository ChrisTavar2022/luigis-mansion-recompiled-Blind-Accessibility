cmake_minimum_required(VERSION 3.25)
project(lmport LANGUAGES C CXX VERSION 0.0.1)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(AURORA_ENABLE_DVD ON CACHE BOOL "" FORCE)
set(AURORA_ENABLE_CARD ON CACHE BOOL "" FORCE)
# Local fixes to Aurora, kept as patches so the submodule stays pristine.
file(GLOB AURORA_PATCHES ${CMAKE_SOURCE_DIR}/patches/aurora/*.patch)
foreach (_patch ${AURORA_PATCHES})
    execute_process(COMMAND git apply --reverse --check ${_patch}
                    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/extern/aurora
                    RESULT_VARIABLE _already OUTPUT_QUIET ERROR_QUIET)
    if (NOT _already EQUAL 0)
        execute_process(COMMAND git apply ${_patch}
                        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/extern/aurora
                        RESULT_VARIABLE _applied)
        if (NOT _applied EQUAL 0)
            message(FATAL_ERROR "Failed to apply Aurora patch ${_patch}")
        endif ()
        message(STATUS "Applied Aurora patch ${_patch}")
    endif ()
endforeach ()
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
    target_link_libraries(lmport PRIVATE dbghelp)
endif ()

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

# ---- Recompiled build (docs/RECOMPILER.md) -----------------------------------
# tools/recomp/recomp.py translates the user's own main.dol into C++ under the
# build directory at configure time. Nothing generated is ever committed.
set(LM_DOL "${CMAKE_SOURCE_DIR}/Luigis Mansion US/sys/main.dol" CACHE FILEPATH
    "Path to your Luigi's Mansion (USA) main.dol, extracted from your own disc")
set(LM_RECOMP_DIR ${CMAKE_BINARY_DIR}/recomp_gen)
set(_recomp_inputs ${CMAKE_SOURCE_DIR}/tools/recomp/recomp.py ${LM_DOL}
    ${CMAKE_SOURCE_DIR}/decomp/config/GLME01/symbols.txt ${CMAKE_SOURCE_DIR}/decomp/config/GLME01/splits.txt)
if (EXISTS ${LM_DOL})
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_recomp_inputs})
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(_stamp ${LM_RECOMP_DIR}/files.txt)
    set(_regen FALSE)
    if (NOT EXISTS ${_stamp})
        set(_regen TRUE)
    else ()
        foreach (_in ${_recomp_inputs})
            if (${_in} IS_NEWER_THAN ${_stamp})
                set(_regen TRUE)
            endif ()
        endforeach ()
    endif ()
    if (_regen)
        file(REMOVE_RECURSE ${LM_RECOMP_DIR})
        execute_process(
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/recomp/recomp.py
                --dol ${LM_DOL}
                --symbols ${CMAKE_SOURCE_DIR}/decomp/config/GLME01/symbols.txt
                --splits ${CMAKE_SOURCE_DIR}/decomp/config/GLME01/splits.txt
                --out ${LM_RECOMP_DIR}
            RESULT_VARIABLE _recomp_result)
        if (NOT _recomp_result EQUAL 0)
            message(FATAL_ERROR "Recompiling main.dol failed")
        endif ()
    endif ()
    file(STRINGS ${_stamp} _recomp_files)
    list(TRANSFORM _recomp_files PREPEND ${LM_RECOMP_DIR}/)

    file(GLOB RECOMP_RUNTIME_SOURCES CONFIGURE_DEPENDS src/recomp/*.cpp)
    add_executable(lmrecomp ${RECOMP_RUNTIME_SOURCES} ${_recomp_files}
        src/port/crash_handler.cpp src/port/options.cpp src/port/dol.cpp)
    target_include_directories(lmrecomp PRIVATE src ${LM_RECOMP_DIR}
        ${CMAKE_SOURCE_DIR}/extern/aurora/include/dolphin ${CMAKE_SOURCE_DIR}/extern/aurora/include)
    target_compile_definitions(lmrecomp PRIVATE TARGET_PC=1)
    target_link_libraries(lmrecomp PRIVATE aurora::main aurora::core aurora::gx aurora::vi aurora::pad
        aurora::si aurora::os aurora::dvd aurora::card)
    # Generated code: one huge function per guest function, lots of gotos.
    if (CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
        set_source_files_properties(${_recomp_files} PROPERTIES COMPILE_OPTIONS "/clang:-Wno-unused-label;/clang:-Wno-unused-variable")
    endif ()
    if (WIN32)
        target_link_libraries(lmrecomp PRIVATE dbghelp)
        add_custom_command(TARGET lmrecomp POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_RUNTIME_DLLS:lmrecomp> $<TARGET_FILE_DIR:lmrecomp>
            COMMAND_EXPAND_LISTS)
    endif ()
else ()
    message(STATUS "LM_DOL not found (${LM_DOL}); skipping the recompiled build")
endif ()
