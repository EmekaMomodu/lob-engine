include(CheckCXXCompilerFlag)

# Strict warnings for project code. -Wconversion / -Wsign-conversion matter
# here: prices and quantities are integers of different widths, and silent
# narrowing is exactly the bug class we want the compiler to catch.
function(lob_set_warnings target)
  set(_common
      -Wall
      -Wextra
      -Wpedantic
      -Wconversion
      -Wsign-conversion
      -Wshadow
      -Wold-style-cast
      -Wcast-align
      -Wnon-virtual-dtor
      -Woverloaded-virtual
      -Wnull-dereference
      -Wdouble-promotion
      -Wfloat-equal # no floating point in the matching path; flag any comparison
      -Wimplicit-fallthrough
      -Wformat=2)

  set(_gcc -Wduplicated-cond -Wduplicated-branches -Wlogical-op -Wuseless-cast)
  set(_clang -Wextra-semi -Wunreachable-code)

  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    set(_flags ${_common} ${_clang})
  elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set(_flags ${_common} ${_gcc})
  else()
    message(WARNING "lob: unknown compiler '${CMAKE_CXX_COMPILER_ID}', no warnings configured")
    return()
  endif()

  if(LOB_WARNINGS_AS_ERRORS)
    list(APPEND _flags -Werror)
  endif()

  target_compile_options(${target} INTERFACE ${_flags})
endfunction()

# -march=native is the x86 spelling; Apple Silicon clang wants -mcpu=native.
function(lob_set_native_arch target)
  check_cxx_compiler_flag(-march=native LOB_HAS_MARCH_NATIVE)
  if(LOB_HAS_MARCH_NATIVE)
    target_compile_options(${target} INTERFACE -march=native)
    return()
  endif()
  check_cxx_compiler_flag(-mcpu=native LOB_HAS_MCPU_NATIVE)
  if(LOB_HAS_MCPU_NATIVE)
    target_compile_options(${target} INTERFACE -mcpu=native)
    return()
  endif()
  message(WARNING "lob: LOB_NATIVE_ARCH requested but compiler supports neither -march nor -mcpu=native")
endfunction()
