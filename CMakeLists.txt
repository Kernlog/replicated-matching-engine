cmake_minimum_required(VERSION 3.28)
project(replicated_matching_engine VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

option(RME_WERROR "Treat warnings as errors" ON)
option(RME_BUILD_BENCH "Build benchmarks" OFF)
option(RME_BUILD_FUZZ "Build libFuzzer targets (clang only)" OFF)

if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
  set(RME_ARCH "x86-64-v3" CACHE STRING "Value passed to -march; empty to disable")
else()
  set(RME_ARCH "" CACHE STRING "Value passed to -march; empty to disable")
endif()

add_library(rme_options INTERFACE)
target_compile_options(rme_options INTERFACE
  -Wall -Wextra -Wpedantic
  -Wconversion -Wsign-conversion -Wshadow
  -Wnon-virtual-dtor -Wold-style-cast -Wimplicit-fallthrough
  $<$<BOOL:${RME_WERROR}>:-Werror>
  $<$<BOOL:${RME_ARCH}>:-march=${RME_ARCH}>
  $<$<CONFIG:Release>:-fno-plt>)
target_include_directories(rme_options INTERFACE ${PROJECT_SOURCE_DIR})

include(CTest)
if(BUILD_TESTING)
  find_package(GTest CONFIG REQUIRED)
  include(GoogleTest)
endif()

add_subdirectory(core)
add_subdirectory(engine)
add_subdirectory(codec)
add_subdirectory(log)
add_subdirectory(consensus)
add_subdirectory(testing)
add_subdirectory(sim)

add_subdirectory(tools)

if(BUILD_TESTING)
  add_subdirectory(tests)
endif()

if(RME_BUILD_BENCH)
  add_subdirectory(bench)
endif()

if(RME_BUILD_FUZZ)
  add_subdirectory(fuzz)
endif()
