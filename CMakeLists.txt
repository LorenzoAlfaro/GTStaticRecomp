cmake_minimum_required(VERSION 3.20)
project(GTStaticRecomp C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_C_STANDARD 11)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# ---- SDL2 -------------------------------------------------------------------
set(SDL_SOURCE_DIR "" CACHE PATH "Local SDL2 source tree (optional; otherwise downloaded)")
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_TEST OFF CACHE BOOL "" FORCE)
if(SDL_SOURCE_DIR)
  add_subdirectory(${SDL_SOURCE_DIR} ${CMAKE_BINARY_DIR}/SDL EXCLUDE_FROM_ALL)
else()
  include(FetchContent)
  FetchContent_Declare(SDL2 GIT_REPOSITORY https://github.com/libsdl-org/SDL.git GIT_TAG release-2.30.9 GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(SDL2)
endif()

# ---- generated code ---------------------------------------------------------
set(GEN_DIR "${CMAKE_SOURCE_DIR}/build/gen" CACHE PATH "Output of gen/recomp.py")
if(NOT EXISTS "${GEN_DIR}/func_table.c")
  message(FATAL_ERROR "Run gen/recomp.py first (see build.ps1); expected ${GEN_DIR}/func_table.c")
endif()
file(GLOB GEN_SOURCES "${GEN_DIR}/funcs_*.c" "${GEN_DIR}/func_table.c")
add_library(gt_gen STATIC ${GEN_SOURCES})
target_include_directories(gt_gen PUBLIC runtime ${GEN_DIR})
target_compile_options(gt_gen PRIVATE -O2 -g1 -fwrapv -fno-strict-aliasing -w)
option(RECOMP_CHECK_CALLS "Report static calls that clobber callee-saved registers (debug)" OFF)
if(RECOMP_CHECK_CALLS)
  target_compile_definitions(gt_gen PUBLIC RECOMP_CHECK_CALLS)
endif()

# ---- runtime ----------------------------------------------------------------
add_executable(gt
  runtime/main.cpp runtime/cpu.cpp runtime/hw.cpp runtime/bios.cpp runtime/gte.cpp
  runtime/gpu.cpp runtime/spu.cpp runtime/cdrom.cpp runtime/sio.cpp runtime/mdec.cpp runtime/crash.cpp runtime/hooks.cpp runtime/pgxp.cpp runtime/jmp.c)
target_include_directories(gt PRIVATE runtime)
target_compile_options(gt PRIVATE -O2 -g -fwrapv -fno-strict-aliasing -Wall -Wno-unused-variable -Wno-unused-function)
target_link_libraries(gt PRIVATE gt_gen SDL2::SDL2-static SDL2::SDL2main)
if(MINGW)
  target_link_options(gt PRIVATE -static -mwindows)
endif()
