ruvia_collect_cpp_sources(ruvia_core_sources src)
add_library(ruvia-core STATIC ${ruvia_core_sources})

ruvia_configure_library(ruvia-core core)

target_link_libraries(ruvia-core
    PUBLIC
        ${RUVIA_CORE_DEPENDENCIES}
)
if(WIN32)
    target_link_libraries(ruvia-core PRIVATE $<IF:$<TARGET_EXISTS:mimalloc-static>,mimalloc-static,mimalloc>)
else()
    if(NOT TARGET mimalloc-static)
        message(FATAL_ERROR "Ruvia non-Windows core requires a static mimalloc build with override enabled")
    endif()
    get_target_property(_ruvia_mimalloc_type mimalloc-static TYPE)
    if(NOT _ruvia_mimalloc_type STREQUAL "STATIC_LIBRARY")
        message(FATAL_ERROR "Ruvia non-Windows core requires a real mimalloc-static STATIC_LIBRARY")
    endif()
    target_link_libraries(ruvia-core PRIVATE "$<LINK_LIBRARY:WHOLE_ARCHIVE,mimalloc-static>")
endif()
target_compile_definitions(ruvia-core PUBLIC ASIO_STANDALONE)

ruvia_install_target(ruvia-core core)

ruvia_install_public_header_closure(core include/ruvia/core)

if(WIN32)
    target_link_libraries(ruvia-core PUBLIC ws2_32 mswsock PRIVATE Advapi32 Bcrypt)
endif()

ruvia_install_export(core)
