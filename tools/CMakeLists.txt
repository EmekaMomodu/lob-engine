add_executable(lob_fuzz lob_fuzz.cpp)
target_link_libraries(lob_fuzz PRIVATE lob::lob lob::reference lob::workload lob_project_options)
lob_enable_tidy(lob_fuzz)
