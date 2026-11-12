# Reference order book: the test oracle. Header-only, test/tooling use only.
add_library(lob_reference INTERFACE)
add_library(lob::reference ALIAS lob_reference)
target_include_directories(lob_reference INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(lob_reference INTERFACE lob::lob)
