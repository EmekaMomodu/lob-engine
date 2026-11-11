# clang-format / clang-tidy integration.
#
# Homebrew's LLVM is keg-only (not on PATH), so we also look in its prefix.
# CI pins the LLVM major version (see .github/workflows/ci.yml); formatting
# output can differ between majors, so keep local and CI versions aligned.

set(_lob_llvm_hints /opt/homebrew/opt/llvm/bin /usr/local/opt/llvm/bin)

find_program(LOB_CLANG_FORMAT_EXE NAMES clang-format HINTS ${_lob_llvm_hints})
find_program(LOB_CLANG_TIDY_EXE NAMES clang-tidy HINTS ${_lob_llvm_hints})

file(
  GLOB_RECURSE
  _lob_format_sources
  CONFIGURE_DEPENDS
  "${PROJECT_SOURCE_DIR}/include/*.hpp"
  "${PROJECT_SOURCE_DIR}/reference/*.hpp"
  "${PROJECT_SOURCE_DIR}/reference/*.cpp"
  "${PROJECT_SOURCE_DIR}/tests/*.hpp"
  "${PROJECT_SOURCE_DIR}/tests/*.cpp"
  "${PROJECT_SOURCE_DIR}/bench/*.hpp"
  "${PROJECT_SOURCE_DIR}/bench/*.cpp"
  "${PROJECT_SOURCE_DIR}/tools/*.cpp")

if(LOB_CLANG_FORMAT_EXE)
  add_custom_target(
    format
    COMMAND ${LOB_CLANG_FORMAT_EXE} -i ${_lob_format_sources}
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    COMMENT "Formatting sources with clang-format")
  add_custom_target(
    format-check
    COMMAND ${LOB_CLANG_FORMAT_EXE} --dry-run --Werror ${_lob_format_sources}
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    COMMENT "Checking formatting with clang-format")
endif()

if(LOB_ENABLE_CLANG_TIDY)
  if(NOT LOB_CLANG_TIDY_EXE)
    message(FATAL_ERROR "LOB_ENABLE_CLANG_TIDY=ON but clang-tidy was not found")
  endif()
  message(STATUS "lob: clang-tidy: ${LOB_CLANG_TIDY_EXE}")
  # Applied per-target via lob_enable_tidy() so third-party code is never linted.
endif()

function(lob_enable_tidy target)
  if(LOB_ENABLE_CLANG_TIDY)
    set_target_properties(${target} PROPERTIES CXX_CLANG_TIDY "${LOB_CLANG_TIDY_EXE}")
  endif()
endfunction()
