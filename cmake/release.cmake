# Preserve full DWARF and a build ID alongside CMake's Release defaults.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang)$")
    add_compile_options("$<$<CONFIG:Release>:-g>")
    add_link_options("$<$<CONFIG:Release>:LINKER:--build-id=sha1>")
endif()
