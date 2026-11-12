include(GoogleTest)

# lob_add_test(<name> LABEL <label> SOURCES <files...>)
# One executable per test area; tests are discovered individually by CTest.
function(lob_add_test name)
  cmake_parse_arguments(ARG "" "LABEL" "SOURCES" ${ARGN})
  add_executable(${name} ${ARG_SOURCES})
  target_include_directories(${name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
  target_link_libraries(${name} PRIVATE lob::lob lob::reference lob::workload lob_project_options
                                        GTest::gtest_main)
  lob_enable_tidy(${name})
  # PRE_TEST: discover at ctest time rather than post-build, so a sanitizer
  # abort shows up as a failing test instead of a cryptic build error.
  gtest_discover_tests(${name} DISCOVERY_MODE PRE_TEST DISCOVERY_TIMEOUT 60
                       PROPERTIES LABELS ${ARG_LABEL} TIMEOUT 900)
endfunction()

lob_add_test(
  lob_unit_tests
  LABEL unit
  SOURCES
  unit/config_test.cpp
  unit/types_test.cpp
  unit/engine_components_test.cpp
  unit/validation_test.cpp
  unit/resting_test.cpp
  unit/limit_matching_test.cpp
  unit/market_order_test.cpp
  unit/cancel_test.cpp
  unit/modify_test.cpp
  unit/latency_histogram_test.cpp)

# Engines vs. the reference on random command streams (LOB_DIFF_OPS per case).
lob_add_test(lob_differential_tests LABEL differential SOURCES differential/differential_test.cpp)

# Separate binary: replaces global operator new/delete to count allocations.
lob_add_test(lob_alloc_tests LABEL alloc SOURCES alloc/no_alloc_test.cpp)
