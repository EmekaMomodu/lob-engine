include(GoogleTest)

# lob_add_test(<name> SOURCES <files...>)
# One executable per test area; tests are discovered individually by CTest.
function(lob_add_test name)
  cmake_parse_arguments(ARG "" "" "SOURCES" ${ARGN})
  add_executable(${name} ${ARG_SOURCES})
  target_link_libraries(${name} PRIVATE lob::lob lob::reference lob_project_options GTest::gtest_main)
  lob_enable_tidy(${name})
  # PRE_TEST: discover at ctest time rather than post-build, so a sanitizer
  # abort shows up as a failing test instead of a cryptic build error.
  gtest_discover_tests(${name} DISCOVERY_MODE PRE_TEST PROPERTIES LABELS unit)
endfunction()

lob_add_test(
  lob_unit_tests
  SOURCES
  unit/config_test.cpp
  unit/types_test.cpp
  unit/validation_test.cpp
  unit/resting_test.cpp
  unit/limit_matching_test.cpp
  unit/market_order_test.cpp
  unit/cancel_test.cpp
  unit/modify_test.cpp)
