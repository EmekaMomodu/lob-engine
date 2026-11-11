# Global sanitizer flags.
#
# -fno-sanitize-recover=all makes UBSan abort on the first error instead of
# printing and continuing, so a UB report fails the test run.
# -fno-omit-frame-pointer gives usable stack traces.

if(NOT LOB_SANITIZERS)
  return()
endif()

if(MSVC)
  message(FATAL_ERROR "LOB_SANITIZERS is only supported with Clang/GCC")
endif()

list(JOIN LOB_SANITIZERS "," _lob_san_list)
message(STATUS "lob: sanitizers enabled: ${_lob_san_list}")

add_compile_options(-fsanitize=${_lob_san_list} -fno-omit-frame-pointer -fno-sanitize-recover=all)
add_link_options(-fsanitize=${_lob_san_list})
