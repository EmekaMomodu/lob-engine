# lob_add_benchmark(<name> SOURCES <files...>)
# Each benchmark is also registered as a CTest "smoke" test (label: bench)
# that runs every case briefly on a small workload. It proves the benchmarks
# build and run; its timings are meaningless and are never reported.
function(lob_add_benchmark name)
  cmake_parse_arguments(ARG "" "" "SOURCES" ${ARGN})
  add_executable(${name} ${ARG_SOURCES})
  target_link_libraries(${name} PRIVATE lob::lob lob::reference lob::workload lob_project_options
                                        benchmark::benchmark_main)
  lob_enable_tidy(${name})
  add_test(NAME ${name}_smoke COMMAND ${name} --benchmark_min_time=0.01s)
  set_tests_properties(${name}_smoke PROPERTIES LABELS bench ENVIRONMENT "LOB_BENCH_OPS=20000")
endfunction()

lob_add_benchmark(lob_replay_bench SOURCES micro/replay_bench.cpp)
lob_add_benchmark(lob_best_search_bench SOURCES micro/best_search_bench.cpp)
