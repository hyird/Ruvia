# Preserve full DWARF, an 8 MiB worker/pool thread stack (musl defaults to 128 KiB),
# and a build ID alongside CMake's Release defaults.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang)$")
    add_compile_options("$<$<CONFIG:Release>:-g>")
    add_link_options("LINKER:-z,stack-size=8388608" "$<$<CONFIG:Release>:LINKER:--build-id=sha1>")
endif()
