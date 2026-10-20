# Keep normal Release optimization and full, non-split DWARF in the raw Linux
# binaries. Only the release staging copy is stripped; never use -gsplit-dwarf.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang)$")
    add_compile_options("$<$<CONFIG:Release>:-O3>" "$<$<CONFIG:Release>:-g>")
    add_compile_definitions("$<$<CONFIG:Release>:NDEBUG>")
    add_link_options("$<$<CONFIG:Release>:LINKER:--build-id=sha1>")
endif()
