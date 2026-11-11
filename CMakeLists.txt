cmake_minimum_required(VERSION 3.25)

project(
  lob_engine
  VERSION 0.1.0
  DESCRIPTION "Low-latency limit order book and matching engine"
  LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(LOB_BUILD_TESTS "Build unit and differential tests" ON)
option(LOB_BUILD_BENCHMARKS "Build benchmarks" ON)
option(LOB_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)
option(LOB_NATIVE_ARCH "Tune for the build machine (-march/-mcpu=native). Benchmarks only." OFF)
option(LOB_ENABLE_CLANG_TIDY "Run clang-tidy on project sources during the build" OFF)
set(LOB_SANITIZERS
    ""
    CACHE STRING "Semicolon-separated sanitizers to enable, e.g. 'address;undefined'")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE
      Release
      CACHE STRING "Build type" FORCE)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# Sanitizers are applied globally (before dependencies are added) so that
# GoogleTest/Benchmark are instrumented too; mixing instrumented and
# uninstrumented code produces false positives (e.g. container-overflow).
include(Sanitizers)
include(CompilerWarnings)
include(Tooling)
include(Dependencies)

# ---------------------------------------------------------------------------
# Project-wide options target: warnings + arch flags for *our* code only.
# Linked PRIVATE by every executable we build; never leaks to consumers.
# ---------------------------------------------------------------------------
add_library(lob_project_options INTERFACE)
lob_set_warnings(lob_project_options)
if(LOB_NATIVE_ARCH)
  lob_set_native_arch(lob_project_options)
endif()

# ---------------------------------------------------------------------------
# Core library (header-only: the hot path is templates, so the compiler can
# inline across engine / price levels / event sink).
# ---------------------------------------------------------------------------
add_library(lob INTERFACE)
add_library(lob::lob ALIAS lob)
target_include_directories(lob INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(lob INTERFACE cxx_std_20)

if(LOB_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(LOB_BUILD_BENCHMARKS)
  if(NOT LOB_BUILD_TESTS)
    enable_testing() # benchmark smoke tests are registered with CTest
  endif()
  add_subdirectory(bench)
endif()
