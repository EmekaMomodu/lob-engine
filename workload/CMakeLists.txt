# Workload generation, replay and differential-testing driver.
# Header-only; used by tests, benchmarks and tools (never by the engine).
add_library(lob_workload INTERFACE)
add_library(lob::workload ALIAS lob_workload)
target_include_directories(lob_workload INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(lob_workload INTERFACE lob::lob lob::reference)
