cmake_minimum_required(VERSION 3.25)
project(lmport LANGUAGES C CXX VERSION 0.0.1)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(AURORA_ENABLE_DVD ON CACHE BOOL "" FORCE)
set(AURORA_ENABLE_CARD ON CACHE BOOL "" FORCE)
add_subdirectory(extern/aurora EXCLUDE_FROM_ALL)

# Platform layer (window, main loop, frame pacing, interpolation, settings).
# Decompiled game code from decomp/ is added to this target unit-by-unit as
# each unit is made to compile against Aurora (see docs/ROADMAP.md, Phase 1).
add_executable(lmport
    src/port/main.cpp
)
target_include_directories(lmport PRIVATE src)
target_link_libraries(lmport PRIVATE aurora::core aurora::gx aurora::main aurora::vi)

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
